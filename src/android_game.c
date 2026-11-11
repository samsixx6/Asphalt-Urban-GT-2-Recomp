/* Strict guest startup shared in behavior with boot_probe.c. Android has no
 * probe time limit, synthetic controls, screenshots or automatic save writes. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#include "android_host.h"

void psp_recomp_register(void);
static int game_started;
static uint64_t syscall_count, instruction_traps;
static unsigned missing_import_stops, service_state_stops;
static uint64_t started_ms, last_progress_ms;

static _Noreturn void stop(const char *reason, uint32_t address, const char *detail) {
    char message[512];
    snprintf(message, sizeof message, "%s at 0x%08X: %s", reason, address, detail);
    fprintf(stderr, "STOP %s\n", message);
    fprintf(stderr, "android-guest: started=%d last=%08X calls=%llu misses=%llu "
        "bad_memory=%llu unregistered_hle=%llu guarded_imports=%u service_stops=%u "
        "threads=%llu started_threads=%llu handovers=%llu disc_bytes=%llu ge=%llu "
        "video_frames=%llu audio_samples=%llu interrupts=%llu ge_callbacks=%llu\n",
        game_started, psp_trace_last(), (unsigned long long)psp_dispatch_calls(),
        (unsigned long long)psp_dispatch_misses(), (unsigned long long)psp_mem_bad_access,
        (unsigned long long)psp_hle_unimplemented_calls(), missing_import_stops, service_state_stops,
        (unsigned long long)psp_threadman_created(), (unsigned long long)psp_threadman_started(),
        (unsigned long long)psp_threadman_handovers(), (unsigned long long)psp_io_bytes_read(),
        (unsigned long long)psp_ge_command_count(), (unsigned long long)asphalt_mpeg_video_frames(),
        (unsigned long long)asphalt_mpeg_audio_samples(), (unsigned long long)psp_intr_deliveries(),
        (unsigned long long)psp_ge_callbacks_run());
    psp_trace_dump(); psp_hle_dump_counts(stderr, 100);
    psp_threadman_dump(stderr); psp_hle_dump_recent(stderr);
    for (int i = 0; i < 32; i++)
        fprintf(stderr, "r%02d=%08X%s", i, psp_cpu.r[i], i % 4 == 3 ? "\n" : " ");
    asphalt_android_host_report(stderr);
    asphalt_android_finish(message);
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
        uint32_t p = psp_arg(0); char game[16], save[24], file[16];
        fprintf(stderr, "Savedata request: size=%u mode=%u game=%s save=%s file=%s\n",
            psp_read32(p), psp_read32(p + 48), psp_str(p + 60, game, sizeof game),
            psp_str(p + 76, save, sizeof save), psp_str(p + 100, file, sizeof file));
    }
    stop("unsupported_import", imp->nid, imp->name);
}
#include "asphalt_missing_guards.h"
static void memory_fault(uint32_t address, int write, int width) {
    char detail[48];
    snprintf(detail, sizeof detail, "%s%d", write ? "write" : "read", width * 8);
    stop("unmapped_memory", address, detail);
}
static void miss(uint32_t address) { stop("uncompiled_target", address, "dispatch"); }
static void service_state(const char *name, uint32_t argument) {
    service_state_stops++;
    stop("unsupported_service_state", argument, name);
}
static void media_failure(const char *reason) {
    stop("media_decode_failure", psp_trace_last(), reason);
}
/* Both entry-trace checkpoints and guest waits run on the current CPU owner.
 * Check device/lifecycle failure before delivering more guest callbacks. */
static void game_tick(void) {
    if (!game_started) return;
    uint32_t buttons; uint8_t ax, ay; char error[512];
    int result = asphalt_android_host_tick(&buttons, &ax, &ay, error, sizeof error);
    if (result) stop(result > 0 ? "activity_stopped" : "android_device_failure", psp_trace_last(), error);
    psp_ctrl_set(buttons, ax, ay);
    asphalt_interrupt_checkpoint();
    uint64_t elapsed = asphalt_android_now_ms() - started_ms;
    if (elapsed - last_progress_ms >= 5000) {
        last_progress_ms = elapsed;
        fprintf(stderr, "progress: %llu ms disc=%llu ge=%llu movie=%llu framebuffer=%08X\n",
            (unsigned long long)elapsed, (unsigned long long)psp_io_bytes_read(),
            (unsigned long long)psp_ge_command_count(), (unsigned long long)asphalt_mpeg_video_frames(),
            psp_display_framebuffer());
        asphalt_android_host_report(stderr);
    }
}
/* Never link vendor src/traps.c into this target: those defaults count and
 * continue, whereas an unsupported instruction/import must stop this session. */
void psp_syscall(uint32_t id) { syscall_count++; stop("unsupported_syscall", id, "syscall"); }
void psp_unimplemented(uint32_t address, const char *what) {
    instruction_traps++;
    stop("unsupported_instruction", address, what ? what : "unknown");
}
uint64_t psp_syscall_total(void) { return syscall_count; }
uint64_t psp_unimplemented_total(void) { return instruction_traps; }
void psp_trap_reset(void) { syscall_count = instruction_traps = 0; }

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/* Java verifies SHA-256 before calling nativeStart. Retain the independent
 * exact-length/ELF/entry/GP/layout checks used by the validated PC startup. */
static int load_image(const char *path, uint32_t *entry, uint32_t *gp) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return -1; }
    long length = ftell(file);
    if (length != 4343022 || fseek(file, 0, SEEK_SET)) { fclose(file); return -1; }
    uint8_t *data = malloc((size_t)length);
    if (!data) { fclose(file); return -1; }
    int ok = fread(data, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!ok || memcmp(data, "\177ELF\1\1\1", 7) || u16(data + 16) != 2 || u16(data + 18) != 8 ||
        u16(data + 42) != 32 || u16(data + 44) != 1 || u32(data + 28) > (uint32_t)length - 32) {
        free(data); return -1;
    }
    const uint8_t *ph = data + u32(data + 28);
    uint32_t off = u32(ph + 4), base = u32(ph + 8), filesz = u32(ph + 16), memsz = u32(ph + 20);
    *entry = u32(data + 24);
    if (u32(ph) != 1 || base != 0x08804000u || filesz != 3382256 || memsz != 3449456 ||
        off > (uint32_t)length || filesz > (uint32_t)length - off ||
        *entry != 0x08AD3A98u || base + memsz > 0x08C00000u) {
        free(data); return -1;
    }
    void *destination = psp_mem_ptr(base, memsz);
    if (!destination) { free(data); return -1; }
    memset(destination, 0, memsz); memcpy(destination, data + off, filesz); free(data);
    *gp = psp_read32(0x08AD4608u + 32);
    return *gp == 0x08B45BE0u ? 0 : -1;
}
void asphalt_android_game_run(const char *image_path, const char *iso_path, const char *runtime_root) {
    if (psp_mem_init()) asphalt_android_finish("Cannot allocate PSP guest memory");
    psp_cpu_reset(); psp_hle_init(); psp_recomp_register();
    uint32_t entry, gp;
    if (load_image(image_path, &entry, &gp)) asphalt_android_finish("Game startup image validation failed");
    psp_io_set_root(runtime_root);
    if (psp_io_set_disc_image(iso_path)) asphalt_android_finish("Cannot mount the selected owned ISO");
    asphalt_mpeg_register(media_failure); asphalt_atrac_register(media_failure);
    if (!asphalt_media_available()) asphalt_android_finish("Required FFmpeg media decoders unavailable");
    asphalt_startup_services_init(1, service_state);
    asphalt_savedata_init(runtime_root, service_state);
    asphalt_android_install_persistence();
    install_missing_guards();
    const uint32_t stack_size = 0x40000u;
    uint32_t stack = psp_sysmem_alloc(stack_size, 1);
    if (!stack) asphalt_android_finish("Cannot allocate the guest startup stack");
    uint32_t k0 = stack + stack_size - 0x100u;
    static const char boot_path[] = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    uint32_t argp = (k0 - sizeof boot_path) & ~15u;
    psp_mem_write_block(argp, boot_path, sizeof boot_path);
    psp_cpu.r[PSP_REG_A0] = sizeof boot_path; psp_cpu.r[PSP_REG_A1] = argp;
    psp_cpu.r[PSP_REG_K0] = k0; psp_cpu.r[PSP_REG_GP] = gp;
    psp_cpu.r[PSP_REG_SP] = (argp - 0x40u) & ~15u; psp_cpu.r[PSP_REG_RA] = 0;
    psp_mem_set_fault_handler(memory_fault); psp_set_miss_handler(miss);
    psp_dispatch_set_budget(0, NULL);
    psp_intr_set_host_tick(game_tick); psp_trace_set_progress_hook(65536, game_tick);
    if (asphalt_android_host_start()) asphalt_android_finish("Android graphics/audio initialization failed; see native log");
    fprintf(stderr, "Asphalt Android: entry=%08X gp=%08X stack=%08X\n", entry, gp, stack);
    started_ms = asphalt_android_now_ms(); game_started = 1;
    game_tick();
    psp_call_from_host(entry);
    fprintf(stderr, "Module entry returned: v0=%08X\n", psp_cpu.r[PSP_REG_V0]);
    uint64_t last = psp_threadman_handovers();
    for (int idle = 0; idle < 200 && !psp_exit_requested();) {
        game_tick(); psp_threadman_yield();
        uint64_t now = psp_threadman_handovers();
        idle = now == last ? idle + 1 : 0; last = now;
    }
    stop(psp_exit_requested() ? "guest_exit" : "threads_idle", entry, "guest execution stopped");
}
