#include "analyze.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint32_t words[32]={0};
    const uint32_t base=0x08800000;
    words[0]=0x0E200018u; /* jal 0x08800060 */
    words[2]=0x03E00008u; /* jr ra */
    words[24]=0x03E00008u;
    a_analysis an;memset(&an,0,sizeof an);
    an.code=(uint8_t *)words;an.base=base;an.size=sizeof words;
    an.exec_lo[0]=base;an.exec_hi[0]=base+16;
    an.exec_lo[1]=base+0x60;an.exec_hi[1]=base+0x68;an.exec_ranges=2;
    assert(a_in_range(&an,base) && a_in_range(&an,base+0x60));
    assert(!a_in_range(&an,base+0x20) && !a_in_range(&an,base+0x68));
    assert(!a_in_range(&an,base+1));
    assert(a_discover(&an,&base,1)==0);
    assert(an.owner[24]!=A_NO_OWNER && an.owner[8]==A_NO_OWNER);
    assert(an.invalid==0 && an.vfpu==0);
    a_analysis_free(&an);
    puts("executable island checks passed");return 0;
}
