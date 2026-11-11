/* Bounded native first-boot experiment. Optional interactive desktop host. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/render.h"
#include "asphalt_import_catalog.h"
#include "startup_services.h"
#include "savedata.h"
#include "asphalt_media.h"
#include "host_interrupts.h"
#include "desktop_host.h"
#include "host_perf.h"
#include "display_session.h"

void psp_recomp_register(void);
static volatile LONG stopping;
static int game_started;
static uint64_t syscall_count, instruction_traps;
static unsigned missing_import_stops, service_state_stops;
static ULONGLONG probe_started_ms, last_progress_ms, last_capture_ms;
static unsigned probe_limit_ms=60000;
static uint64_t probe_call_budget=100000000;
static unsigned ge_dump_ms;
static unsigned capture_interval_ms=5000;
static uint64_t capture_us;
static const char *control_path;
static ULONGLONG last_control_poll, release_input_ms;
static unsigned control_sequence;
static uint32_t scripted_buttons;
static int read_probe_options(void) {
    const char *value=getenv("ASPHALT_PROBE_MS");
    if(value && *value) {
        char *end; unsigned long ms=strtoul(value,&end,10);
        if(end==value || *end || ms<1000 || ms>600000) return -1;
        probe_limit_ms=(unsigned)ms;
    }
    control_path=getenv("ASPHALT_PROBE_CONTROL");
    value=getenv("ASPHALT_PROBE_CALLS");
    if(value && *value) {
        char *end;unsigned long long calls=strtoull(value,&end,10);
        if(end==value || *end || calls<1000000 || calls>2000000000ull)return -1;
        probe_call_budget=calls;
    }
    value=getenv("ASPHALT_CAPTURE_INTERVAL_MS");
    if(value && *value) {
        char *end;unsigned long ms=strtoul(value,&end,10);
        if(end==value || *end || (ms && ms<1000) || ms>600000)return -1;
        capture_interval_ms=(unsigned)ms;
    }
    value=getenv("ASPHALT_GE_DUMP_MS");
    if(value && *value) {
        char *end; unsigned long ms=strtoul(value,&end,10);
        if(end==value || *end || !ms || ms>=probe_limit_ms) return -1;
        ge_dump_ms=(unsigned)ms;
    }
    return 0;
}
/* Optional local diagnostic input: sequence, hexadecimal PSP mask, hold ms.
 * Poll only while the current thread owns the emulated CPU. */
static void poll_control(ULONGLONG elapsed) {
    if(release_input_ms && elapsed>=release_input_ms) {
        scripted_buttons=0;release_input_ms=0;
        fprintf(stderr,"live input: %llu ms buttons=0000\n",(unsigned long long)elapsed);
    }
    if(!control_path || elapsed-last_control_poll<100) return;
    last_control_poll=elapsed;
    FILE *f=fopen(control_path,"r"); if(!f) return;
    unsigned sequence,buttons,hold;
    int count=fscanf(f,"%u %x %u",&sequence,&buttons,&hold);fclose(f);
    if(count!=3 || sequence==control_sequence || !sequence || buttons>0xffff || hold>10000) return;
    control_sequence=sequence;
    scripted_buttons=buttons;
    release_input_ms=buttons && hold ? elapsed+hold : 0;
    fprintf(stderr,"live input: %llu ms sequence=%u buttons=%04X hold=%u\n",(unsigned long long)elapsed,sequence,buttons,hold);
}
typedef struct { unsigned ms; uint32_t buttons; } input_event;
static input_event inputs[64];
static unsigned input_count, next_input;
static int read_inputs(void) {
    const char *text=getenv("ASPHALT_PROBE_INPUT");
    if(!text || !*text) return 0;
    while(*text && input_count<64) {
        char *end; unsigned long ms=strtoul(text,&end,10);
        if(end==text || *end!=':' || ms>=probe_limit_ms || (input_count && ms<inputs[input_count-1].ms)) return -1;
        text=end+1; unsigned long buttons=strtoul(text,&end,0);
        if(end==text || (*end && *end!=',') || buttons>0xffff) return -1;
        inputs[input_count++]=(input_event){(unsigned)ms,(uint32_t)buttons};
        text=*end?end+1:end;
    }
    return *text ? -1 : 0;
}

static void stop(const char *reason, uint32_t address, const char *detail, int code) {
    if (InterlockedCompareExchange(&stopping, 1, 0)) _Exit(code);
    if (psp_display_framebuffer() && (strcmp(psp_render_current()->name,"gl") || !psp_render_gl_error()))
        psp_display_capture("startup-frame.ppm");
    asphalt_desktop_stop();
    printf("{\"phase\":\"native_boot_probe\",\"reason\":\"%s\","
           "\"detail\":\"%s\",\"address\":\"0x%08X\","
           "\"game_executed\":%s,\"android_built\":false,\"menu_reached\":null,"
           "\"last_function\":\"0x%08X\",\"dispatch_calls\":%llu,\"dispatch_misses\":%llu,"
           "\"bad_memory_accesses\":%llu,\"unregistered_hle_calls\":%llu,"
           "\"guarded_import_stops\":%u,\"unsupported_service_states\":%u,"
           "\"threads_created\":%llu,\"threads_started\":%llu,\"thread_handovers\":%llu,"
           "\"disc_bytes_read\":%llu,\"ge_commands\":%llu,\"framebuffer\":\"0x%08X\","
           "\"decoded_video_frames\":%llu,\"decoded_audio_samples\":%llu,"
           "\"interrupt_handlers\":%d,\"interrupts_enabled\":%d,\"interrupt_deliveries\":%llu,"
           "\"ge_callbacks\":%llu,\"desktop\":",
           reason, detail, address, game_started ? "true" : "false", psp_trace_last(),
           (unsigned long long)psp_dispatch_calls(), (unsigned long long)psp_dispatch_misses(),
           (unsigned long long)psp_mem_bad_access, (unsigned long long)psp_hle_unimplemented_calls(),
           missing_import_stops, service_state_stops,
           (unsigned long long)psp_threadman_created(), (unsigned long long)psp_threadman_started(),
           (unsigned long long)psp_threadman_handovers(), (unsigned long long)psp_io_bytes_read(),
           (unsigned long long)psp_ge_command_count(), psp_display_framebuffer(),
           (unsigned long long)asphalt_mpeg_video_frames(), (unsigned long long)asphalt_mpeg_audio_samples(),
           psp_intr_handlers(), psp_intr_enabled(), (unsigned long long)psp_intr_deliveries(),
           (unsigned long long)psp_ge_callbacks_run());
    asphalt_desktop_report(stdout);puts("}");
    fprintf(stderr, "STOP %s: %s at 0x%08X\n", reason, detail, address);
    psp_trace_dump();
    psp_hle_dump_counts(stderr, 100);
    psp_threadman_dump(stderr);
    psp_hle_dump_recent(stderr);
    for (int i = 0; i < 32; i++)
        fprintf(stderr, "r%02d=%08X%s", i, psp_cpu.r[i], i % 4 == 3 ? "\n" : " ");
    fflush(NULL);
    _Exit(code);  /* guest worker threads may still exist */
}

static void missing_import(unsigned index) {
    missing_import_stops++;
    const asphalt_import *imp = &asphalt_imports[index];
    fprintf(stderr, "Missing %s: a0=%08X a1=%08X a2=%08X a3=%08X\n", imp->name,
            psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3));
    if (imp->nid == 0x977DE386u) {
        char path[512];
        fprintf(stderr, "Requested module: %s\n", psp_str(psp_arg(0), path, sizeof path));
    }
    if (imp->nid == 0x50C4CD57u) {
        uint32_t p=psp_arg(0);char game[16],save[24],file[16];
        fprintf(stderr,"Savedata request: size=%u mode=%u game=%s save=%s file=%s\n",psp_read32(p),psp_read32(p+48),
                psp_str(p+60,game,sizeof game),psp_str(p+76,save,sizeof save),psp_str(p+100,file,sizeof file));
    }
    stop("unsupported_import", imp->nid, imp->name, 10);
}
#include "asphalt_missing_guards.h"

static void memory_fault(uint32_t addr, int write, int width) {
    char detail[48];
    snprintf(detail, sizeof detail, "%s%d", write ? "write" : "read", width * 8);
    stop("unmapped_memory", addr, detail, 11);
}
static void miss(uint32_t addr) { stop("uncompiled_target", addr, "dispatch", 12); }
static void budget(void) { stop("call_budget", psp_trace_last(), "bounded probe", 14); }
static void service_state(const char *name, uint32_t argument) {
    service_state_stops++;
    stop("unsupported_service_state", argument, name, 10);
}
static void media_failure(const char *reason) { stop("media_decode_failure", psp_trace_last(), reason, 17); }
/* Runs at a guest wait point while this thread owns the emulated CPU. */
static void probe_tick(void) {
    if (!game_started) return;
    asphalt_interrupt_checkpoint();
    ULONGLONG elapsed = GetTickCount64() - probe_started_ms;
    while(next_input<input_count && elapsed>=inputs[next_input].ms) {
        input_event event=inputs[next_input++];
        fprintf(stderr,"input: %u ms buttons=%04X\n",event.ms,event.buttons);
        scripted_buttons=event.buttons;
    }
    poll_control(elapsed);
    uint32_t buttons=scripted_buttons;uint8_t ax=128,ay=128;
    int desktop=asphalt_desktop_tick(elapsed,&buttons,&ax,&ay);
    if(desktop)stop(desktop>0?"window_closed":"desktop_device_failure",psp_trace_last(),"desktop host",desktop>0?0:18);
    psp_ctrl_set(buttons,ax,ay);
    if(ge_dump_ms && elapsed>=ge_dump_ms) {
        ge_dump_ms=0;psp_ge_dump_next(1);
        fprintf(stderr,"GE diagnostic: capturing next list at %llu ms\n",(unsigned long long)elapsed);
    }
    if (elapsed - last_progress_ms >= 5000) {
        last_progress_ms = elapsed;
        fprintf(stderr, "progress: %llu ms, %llu disc bytes, %llu GE commands, %llu movie frames, framebuffer=%08X\n",
                (unsigned long long)elapsed, (unsigned long long)psp_io_bytes_read(),
                (unsigned long long)psp_ge_command_count(), (unsigned long long)asphalt_mpeg_video_frames(),
                psp_display_framebuffer());
    }
    asphalt_perf_tick(elapsed,capture_us);
    if (capture_interval_ms && psp_display_framebuffer() && elapsed - last_capture_ms >= capture_interval_ms) {
        LARGE_INTEGER capture_start,capture_end,frequency;QueryPerformanceCounter(&capture_start);
        char path[80]; snprintf(path, sizeof path, "startup-frame-%02u.ppm", (unsigned)(elapsed / capture_interval_ms));
        psp_display_capture(path); last_capture_ms = elapsed;
        QueryPerformanceCounter(&capture_end);QueryPerformanceFrequency(&frequency);
        capture_us+=(uint64_t)((capture_end.QuadPart-capture_start.QuadPart)*1000000ull/frequency.QuadPart);
    }
    if (elapsed >= probe_limit_ms) stop("cooperative_time_limit", psp_trace_last(), "bounded probe", 15);
}

/* Override the donor's counting-and-continuing instruction traps. */
void psp_syscall(uint32_t id) { syscall_count++; stop("unsupported_syscall", id, "syscall", 13); }
void psp_unimplemented(uint32_t addr, const char *what) {
    instruction_traps++; stop("unsupported_instruction", addr, what ? what : "unknown", 13);
}
uint64_t psp_syscall_total(void) { return syscall_count; }
uint64_t psp_unimplemented_total(void) { return instruction_traps; }
void psp_trap_reset(void) { syscall_count = instruction_traps = 0; }

static DWORD WINAPI watchdog(void *unused) {
    (void)unused;
    Sleep(probe_limit_ms+5000);
    /* Do not inspect mutable guest state from the watchdog thread. */
    if (!InterlockedCompareExchange(&stopping, 1, 0)) {
        puts("{\"phase\":\"native_boot_probe\",\"reason\":\"wall_time_limit\",\"game_executed\":true,\"menu_reached\":null,\"android_built\":false}");
        fflush(NULL);
    }
    _Exit(15);
    return 0;
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static int load_image(const char *path, uint32_t *entry, uint32_t *gp) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    long length = ftell(f);
    if (length != 4343022 || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    uint8_t *d = malloc((size_t)length);
    if (!d) { fclose(f); return -1; }
    int ok = fread(d, 1, (size_t)length, f) == (size_t)length;
    fclose(f);
    if (!ok || memcmp(d, "\177ELF\1\1\1", 7) || u16(d + 16) != 2 || u16(d + 18) != 8 ||
        u16(d + 42) != 32 || u16(d + 44) != 1 || u32(d + 28) > (uint32_t)length - 32) {
        free(d); return -1;
    }
    const uint8_t *ph = d + u32(d + 28);
    uint32_t off = u32(ph + 4), base = u32(ph + 8), filesz = u32(ph + 16), memsz = u32(ph + 20);
    *entry = u32(d + 24);
    /* This spike accepts only the exact, hash-checked Asphalt image layout. */
    if (u32(ph) != 1 || base != 0x08804000u || filesz != 3382256 || memsz != 3449456 ||
        off > (uint32_t)length || filesz > (uint32_t)length - off ||
        *entry != 0x08AD3A98u || base + memsz > 0x08C00000u) {
        free(d); return -1;
    }
    void *dest = psp_mem_ptr(base, memsz);
    if (!dest) { free(d); return -1; }
    memset(dest, 0, memsz);
    memcpy(dest, d + off, filesz);
    free(d);
    *gp = psp_read32(0x08AD4608u + 32);
    return *gp == 0x08B45BE0u ? 0 : -1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc != 4) { fprintf(stderr, "usage: boot_probe rebased.elf disc.iso runtime-root\n"); return 2; }
    if(read_probe_options() || read_inputs()) { fprintf(stderr,"invalid probe limit or ASPHALT_PROBE_INPUT\n"); return 2; }
    if (psp_mem_init()) return 2;
    psp_cpu_reset();
    psp_hle_init();
    psp_recomp_register();
    uint32_t entry, gp;
    if (load_image(argv[1], &entry, &gp)) { fprintf(stderr, "image validation failed\n"); return 2; }
    psp_io_set_root(argv[3]);
    if(asphalt_display_session_start(argv[3])){fprintf(stderr,"display settings initialization failed\n");return 2;}
    const char *dw=getenv("ASPHALT_DRAWABLE_W"),*dh=getenv("ASPHALT_DRAWABLE_H");
    if(asphalt_display_session_geometry(dw?atoi(dw):480,dh?atoi(dh):272,NULL))return 2;
    if (psp_io_set_disc_image(argv[2])) { fprintf(stderr, "disc mount failed\n"); return 2; }
    asphalt_mpeg_register(media_failure);
    asphalt_atrac_register(media_failure);
    if (!asphalt_media_available()) { fprintf(stderr, "required media decoders unavailable\n"); return 2; }
    asphalt_startup_services_init(1, service_state);
    asphalt_savedata_init(argv[3], service_state);
    install_missing_guards();
    const uint32_t stack_size = 0x40000u;
    uint32_t stack = psp_sysmem_alloc(stack_size, 1);
    if (!stack) return 2;
    uint32_t k0 = stack + stack_size - 0x100u;
    static const char boot_path[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    uint32_t argp = (k0 - sizeof boot_path) & ~15u;
    psp_mem_write_block(argp, boot_path, sizeof boot_path);
    psp_cpu.r[PSP_REG_A0] = sizeof boot_path;
    psp_cpu.r[PSP_REG_A1] = argp;
    psp_cpu.r[PSP_REG_K0] = k0;
    psp_cpu.r[PSP_REG_GP] = gp;
    psp_cpu.r[PSP_REG_SP] = (argp - 0x40u) & ~15u;
    psp_cpu.r[PSP_REG_RA] = 0;
    psp_mem_set_fault_handler(memory_fault);
    psp_set_miss_handler(miss);
    psp_dispatch_set_budget(probe_call_budget, budget);
    psp_intr_set_host_tick(probe_tick);
    psp_trace_set_progress_hook(65536, probe_tick);
    if(asphalt_desktop_start()) { fprintf(stderr,"desktop initialization failed\n");return 2; }
    fprintf(stderr, "Asphalt first-boot probe: entry=%08X gp=%08X stack=%08X\n", entry, gp, stack);
    probe_started_ms = GetTickCount64();
    asphalt_perf_tick(0,0); /* exclude initialization CPU time from the first sample */
    HANDLE timer = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    if (!timer) return 2;
    CloseHandle(timer);
    game_started = 1;
    psp_call_from_host(entry);
    fprintf(stderr, "Module entry returned: v0=%08X\n", psp_cpu.r[PSP_REG_V0]);
    uint64_t last = psp_threadman_handovers();
    for (int idle = 0; idle < 200 && !psp_exit_requested();) {
        psp_threadman_yield();
        uint64_t now = psp_threadman_handovers();
        idle = now == last ? idle + 1 : 0;
        last = now;
    }
    stop(psp_exit_requested() ? "guest_exit" : "threads_idle", entry, "startup stopped", 16);
    return 0;
}
