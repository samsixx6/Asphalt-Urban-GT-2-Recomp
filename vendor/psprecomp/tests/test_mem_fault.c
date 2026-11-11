/* Synthetic verification of the opt-in boot diagnostic hook. */
#include "psprecomp/mem.h"
#include <assert.h>
#include <stdio.h>
static unsigned calls;
static uint32_t address;
static int writing, width;
static void fault(uint32_t a, int w, int n) { calls++; address=a; writing=w; width=n; }
int main(void) {
    assert(psp_mem_init() == 0);
    psp_mem_set_fault_handler(fault);
    psp_write32(PSP_RAM_BASE, 123);
    assert(psp_read32(PSP_RAM_BASE) == 123 && calls == 0);
    assert(psp_read32(0) == 0);
    assert(calls == 1 && address == 0 && writing == 0 && width == 4);
    psp_write8(0, 1);
    assert(calls == 2 && writing == 1 && width == 1);
    psp_mem_set_fault_handler(NULL);
    psp_read8(0);
    assert(calls == 2 && psp_mem_bad_access == 3);
    psp_mem_free();
    puts("memory fault hook checks passed");
    return 0;
}
