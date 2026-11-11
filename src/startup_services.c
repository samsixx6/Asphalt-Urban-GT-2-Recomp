#include "startup_services.h"
#include "asphalt_media.h"
#include "psprecomp/hle.h"
#include <string.h>
#include <time.h>

/* Semantics checked against PPSSPP sceUmd.cpp at b69424e8f20e28ca48557011c5b992a265a115ae.
 * Independently implemented fixed-disc subset: no hot swap or hardware waits.
 * Presence is supplied only after the ISO filesystem has validated its PVD. */
static int mounted, activated;
static asphalt_service_stop unsupported;
static unsigned loaded_net_modules;
static int idle_reset_threshold, idle_back_threshold;
static int volatile_locked;
typedef struct { const char *path; uint32_t id; int loaded, media; } native_module;
static native_module modules[] = {
    {"disc0:/PSP_GAME/USRDIR/kmodule/sc_sascore.prx", 0x00100001u, 0, 0},
    {"disc0:/PSP_GAME/USRDIR/kmodule/videocodec.prx", 0x00100002u, 0, 1},
    {"disc0:/PSP_GAME/USRDIR/kmodule/audiocodec.prx", 0x00100003u, 0, 1},
    {"disc0:/PSP_GAME/USRDIR/kmodule/mpegbase.prx", 0x00100004u, 0, 1},
    {"disc0:/PSP_GAME/USRDIR/module/mpeg.prx", 0x00100005u, 0, 1},
    {"disc0:/PSP_GAME/USRDIR/module/libatrac3plus.prx", 0x00100006u, 0, 2}
};
static uint32_t state(void) { return mounted ? 0x12u | (activated ? 0x20u : 0) : 1u; }
static void check_medium(void) { psp_ret(mounted != 0); }
static void drive_stat(void) { psp_ret(state()); }
static void activate(void) {
    uint32_t mode = psp_arg(0), addr = psp_arg(1);
    const char *device = psp_mem_ptr(addr, 7);
    if ((mode != 1 && mode != 2) || !addr || addr & 0x80000000u || !device || memcmp(device, "disc0:", 7)) {
        psp_ret(0x80010016u); return;
    }
    if (!mounted) {
        if (unsupported) unsupported("sceUmdActivate", mode);
        psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); return;
    }
    activated = 1;
    psp_ret(0);
}
static void wait_stat(void) {
    uint32_t requested = psp_arg(0);
    if (!(requested & 0x3Bu)) { psp_ret(0x80010016u); return; }
    if (!(requested & state())) {
        if (unsupported) unsupported("sceUmdWaitDriveStat", requested);
        psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); return;
    }
    psp_ret(0);
}

/* The toolkit already implements this module's imported service surface.
 * Bind only this exact module; arbitrary PRX loading remains a hard stop.
 * A handle means native HLE services are available, not that PRX code ran. */
static void load_module(void) {
    char path[256];
    uint32_t path_addr = psp_arg(0), flags = psp_arg(1);
    psp_str(path_addr, path, sizeof path);
    fprintf(stderr, "Module request: %s\n", path);
    native_module *module = NULL;
    for (unsigned i = 0; i < sizeof modules / sizeof modules[0]; i++)
        if (!strcmp(path, modules[i].path)) module = &modules[i];
    if (!module || flags || module->loaded ||
        (module->media && (!asphalt_media_available() || !psp_hle_name(0x682A619Bu))) ||
        (module->media == 2 && !psp_hle_name(0x7A20E7AFu))) {
        if (unsupported) unsupported("sceKernelLoadModule", path_addr);
        psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); return;
    }
    static const uint32_t required_sas[] = {
        0x019B25EBu, 0x267A6DD2u, 0x2C8E6AB3u, 0x33D4AB37u, 0x42778A9Fu,
        0x440CA7D8u, 0x50A14DFCu, 0x5F9529F6u, 0x68A46B95u, 0x74AE582Au,
        0x76F01ACAu, 0x787D04D5u, 0x99944089u, 0x9EC3676Au, 0xA0CF2FA4u,
        0xA3589D81u, 0xAD84D37Fu, 0xB7660A23u, 0xCBCD4F79u, 0xD5A229C9u, 0xF983B186u
    };
    for (unsigned i = 0; !module->media && i < sizeof required_sas / sizeof required_sas[0]; i++) {
        if (!psp_hle_name(required_sas[i])) {
            if (unsupported) unsupported("sceKernelLoadModule", required_sas[i]);
            psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); return;
        }
    }
    uint32_t saved[8];
    memcpy(saved, &psp_cpu.r[PSP_REG_A0], sizeof saved);
    psp_cpu.r[PSP_REG_A1] = 1; /* PSP_O_RDONLY */
    psp_cpu.r[PSP_REG_A2] = 0;
    psp_hle_call(0x109F50BCu); /* Verify the requested module exists on the ISO. */
    uint32_t fd = psp_cpu.r[PSP_REG_V0];
    if ((int32_t)fd < 0) {
        memcpy(&psp_cpu.r[PSP_REG_A0], saved, sizeof saved);
        psp_ret(fd); return;
    }
    psp_cpu.r[PSP_REG_A0] = fd;
    psp_hle_call(0x810C4BC3u);
    memcpy(&psp_cpu.r[PSP_REG_A0], saved, sizeof saved);
    module->loaded = 1;
    fprintf(stderr, "Module bound to %s; id=%08X\n", module->media ? "native FFmpeg media services" : "21 native SAS handlers", module->id);
    psp_ret(module->id);
}
static void start_module(void) {
    uint32_t id = psp_arg(0), result_addr = psp_arg(3);
    native_module *module = NULL;
    for (unsigned i = 0; i < sizeof modules / sizeof modules[0]; i++)
        if (modules[i].loaded && id == modules[i].id) module = &modules[i];
    if (!module || psp_arg(1) || psp_arg(4)) {
        if (unsupported) unsupported("sceKernelStartModule", id);
        psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); return;
    }
    if (result_addr) psp_write32(result_addr, 0);
    psp_ret(id);
}
static void stop_module(void) {
    if (unsupported) unsupported("sceKernelStopModule", psp_arg(0));
    psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED);
}
static void unload_module(void) {
    if (unsupported) unsupported("sceKernelUnloadModule", psp_arg(0));
    psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED);
}
/* Utility module discovery is separate from network initialization and packet
 * transport. Match the two identities requested at startup; subsequent network
 * services retain their explicit missing-import guards. Multiplayer is unimplemented. */
static void load_net_module(void) {
    unsigned id = psp_arg(0);
    if (id != 1 && id != 2) {
        if (unsupported) unsupported("sceUtilityLoadNetModule", id);
        psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); return;
    }
    loaded_net_modules |= 1u << id;
    fprintf(stderr, "Utility network module %u registered; network calls remain diagnostic\n", id);
    psp_ret(0);
}
/* PSPSDK specifies NULL if profiler registers are unavailable; PPSSPP also
 * returns NULL. This is a pointer result, not a fabricated success status. */
static void refer_global_profiler(void) { psp_ret(0); }
/* PSP RTC units are microseconds since 0001-01-01, not Unix seconds. */
static int realtime_now(struct timespec *now) {
#ifdef __ANDROID__
    return clock_gettime(CLOCK_REALTIME, now);
#else
    return timespec_get(now, TIME_UTC) == TIME_UTC ? 0 : -1;
#endif
}
static void rtc_current_tick(void) {
    struct timespec now;
    uint32_t at = psp_arg(0);
    if (!psp_mem_ptr(at, 8)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    if (realtime_now(&now)) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    uint64_t tick = 62135596800000000ull + (uint64_t)now.tv_sec * 1000000ull + now.tv_nsec / 1000;
    psp_write32(at, (uint32_t)tick); psp_write32(at + 4, (uint32_t)(tick >> 32));
    psp_ret(0);
}
static void rtc_day_of_week(void) {
    unsigned y = psp_arg(0), m = psp_arg(1), d = psp_arg(2);
    static const unsigned offsets[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (!y || m < 1 || m > 12 || !d || d > 31) {
        if (unsupported) unsupported("sceRtcGetDayOfWeek", m);
        psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED); return;
    }
    if (m < 3) y--;
    psp_ret((y + y/4 - y/100 + y/400 + offsets[m-1] + d) % 7);
}
static void rtc_local_time(void) {
    struct timespec now;
    uint32_t at = psp_arg(0);
    if (!psp_mem_ptr(at, 16)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    if (realtime_now(&now)) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    const struct tm *local = localtime(&now.tv_sec);
    if (!local) { psp_ret(SCE_KERNEL_ERROR_ERROR); return; }
    psp_write16(at, (uint16_t)(local->tm_year+1900));
    psp_write16(at+2, (uint16_t)(local->tm_mon+1)); psp_write16(at+4, (uint16_t)local->tm_mday);
    psp_write16(at+6, (uint16_t)local->tm_hour); psp_write16(at+8, (uint16_t)local->tm_min);
    psp_write16(at+10, (uint16_t)local->tm_sec); psp_write32(at+12, (uint32_t)(now.tv_nsec/1000));
    psp_ret(0);
}
/* Prototype preferences: English, Cross confirm, 24-hour DD/MM/YYYY, UTC+5.
 * These are normal system settings, not success stubs for arbitrary IDs. */
static void system_param_int(void) {
    uint32_t id = psp_arg(0), at = psp_arg(1), value;
    switch (id) {
    case 2: case 3: case 5: case 7: value = 0; break;
    case 4: value = 2; break;
    case 6: value = 300; break;
    case 8: case 9: value = 1; break;
    default: psp_ret(0x80110103u); return;
    }
    if (!psp_mem_ptr(at, 4)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    psp_write32(at, value); psp_ret(0);
}
static void ctrl_idle_thresholds(void) {
    int reset=(int32_t)psp_arg(0), back=(int32_t)psp_arg(1);
    if (reset < -1 || reset > 128 || back < -1 || back > 128) { psp_ret(0x800001FEu); return; }
    idle_reset_threshold=reset; idle_back_threshold=back;
    psp_ret(0);
}
static void system_param_string(void) {
    static const char nickname[]="Player";
    uint32_t at=psp_arg(1);int capacity=(int32_t)psp_arg(2);
    if (psp_arg(0)!=1) { psp_ret(0x80110103u); return; }
    if (capacity < (int)sizeof nickname) { psp_ret(0x80110102u); return; }
    if (!psp_mem_ptr(at,(uint32_t)capacity)) { psp_ret(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
    psp_mem_write_block(at,nickname,sizeof nickname);psp_ret(0);
}
/* sceSuspendForUser's separately reserved 4 MB area is already backed by the
 * runtime's 32 MB RAM map. It lies below the game image and user allocator.
 * This is the nonblocking TryLock subset; no waiting clients are registered. */
static void volatile_try_lock(void) {
    if(psp_arg(0)!=0) { psp_ret(0x80000107u);return; }
    if(volatile_locked) { psp_ret(0x802B0200u);return; }
    if(!psp_mem_ptr(0x08400000u,0x00400000u)) {
        if(unsupported) unsupported("volatile RAM backing",0x08400000u);
        psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);return;
    }
    uint32_t addr=psp_arg(1),size=psp_arg(2);
    if(addr && psp_mem_ptr(addr,4)) psp_write32(addr,0x08400000u);
    if(size && psp_mem_ptr(size,4)) psp_write32(size,0x00400000u);
    volatile_locked=1;psp_ret(0);
}
static void volatile_unlock(void) {
    if(psp_arg(0)!=0) { psp_ret(0x80000107u);return; }
    if(!volatile_locked) { psp_ret(0x800201AEu);return; }
    volatile_locked=0;psp_ret(0);
}
void asphalt_startup_services_init(int iso_mounted, asphalt_service_stop stop) {
    mounted = iso_mounted != 0;
    activated = mounted;
    unsupported = stop;
    loaded_net_modules = 0;
    volatile_locked = 0;
    for (unsigned i = 0; i < sizeof modules / sizeof modules[0]; i++) modules[i].loaded = 0;
    psp_hle_register(0x46EBB729u, "sceUmdUser", "sceUmdCheckMedium", check_medium);
    psp_hle_register(0x6B4A146Cu, "sceUmdUser", "sceUmdGetDriveStat", drive_stat);
    psp_hle_register(0x8EF08FCEu, "sceUmdUser", "sceUmdWaitDriveStat", wait_stat);
    psp_hle_register(0xC6183D47u, "sceUmdUser", "sceUmdActivate", activate);
    psp_hle_register(0x977DE386u, "ModuleMgrForUser", "sceKernelLoadModule", load_module);
    psp_hle_register(0x50F0C1ECu, "ModuleMgrForUser", "sceKernelStartModule", start_module);
    psp_hle_register(0xD1FF982Au, "ModuleMgrForUser", "sceKernelStopModule", stop_module);
    psp_hle_register(0x2E0911AAu, "ModuleMgrForUser", "sceKernelUnloadModule", unload_module);
    psp_hle_register(0x1579A159u, "sceUtility", "sceUtilityLoadNetModule", load_net_module);
    psp_hle_register(0x8218B4DDu, "ThreadManForUser", "sceKernelReferGlobalProfiler", refer_global_profiler);
    psp_hle_register(0x3F7AD767u, "sceRtc", "sceRtcGetCurrentTick", rtc_current_tick);
    psp_hle_register(0x57726BC1u, "sceRtc", "sceRtcGetDayOfWeek", rtc_day_of_week);
    psp_hle_register(0xE7C27D1Bu, "sceRtc", "sceRtcGetCurrentClockLocalTime", rtc_local_time);
    psp_hle_register(0xA5DA2406u, "sceUtility", "sceUtilityGetSystemParamInt", system_param_int);
    psp_hle_register(0xA7144800u, "sceCtrl", "sceCtrlSetIdleCancelThreshold", ctrl_idle_thresholds);
    psp_hle_register(0x34B78343u, "sceUtility", "sceUtilityGetSystemParamString", system_param_string);
    psp_hle_register(0xA14F40B2u,"sceSuspendForUser","sceKernelVolatileMemTryLock",volatile_try_lock);
    psp_hle_register(0xA569E425u,"sceSuspendForUser","sceKernelVolatileMemUnlock",volatile_unlock);
}
