/* Throwaway native linking/import audit. This does not execute the game. */
#include <stdio.h>
#include <stdint.h>
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/hle.h"
#include "psprecomp/dispatch.h"
#include "asphalt_import_catalog.h"

void psp_recomp_register(void);

int main(void) {
    if (psp_mem_init() != 0) return 1;
    psp_cpu_reset();
    psp_hle_init();
    psp_recomp_register();
    int present = 0;
    for (unsigned i = 0; i < ASPHALT_IMPORT_COUNT; i++)
        present += psp_hle_name(asphalt_imports[i].nid) != NULL;
    printf("{\"phase\":\"native_preflight\",\"game_executed\":false,"
           "\"android_built\":false,\"registered_entries\":%u,"
           "\"imports_registered\":%d,\"imports_total\":%u,\"missing\":[",
           psp_dispatch_count(), present, ASPHALT_IMPORT_COUNT);
    int comma = 0;
    for (unsigned i = 0; i < ASPHALT_IMPORT_COUNT; i++) {
        if (psp_hle_name(asphalt_imports[i].nid)) continue;
        printf("%s{\"nid\":\"0x%08X\",\"library\":\"%s\",\"name\":\"%s\"}",
               comma++ ? "," : "", asphalt_imports[i].nid,
               asphalt_imports[i].library, asphalt_imports[i].name);
    }
    puts("]}");
    psp_mem_free();
    return 0;
}
