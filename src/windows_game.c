/* Player entry for the native Windows host, sharing Android's game/runtime. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "windows_game.h"
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
#include "display_session.h"
void psp_recomp_register(void);
static int started,stopping;
static ULONGLONG started_ms,last_progress;
static unsigned test_limit;
static uint64_t syscalls,traps;
static _Noreturn void finish(const char *reason,uint32_t address,const char *detail,int code){
 if(stopping)_Exit(code);stopping=1;
 fprintf(stderr,"windows-stop: %s at %08X: %s; dispatch_misses=%llu bad_memory=%llu unimplemented=%llu\n",reason,address,detail,(unsigned long long)psp_dispatch_misses(),(unsigned long long)psp_mem_bad_access,(unsigned long long)psp_hle_unimplemented_calls());
 asphalt_desktop_stop();
 if(code){char message[768];snprintf(message,sizeof message,"%s\n%s\n\nSee session.log in %%LOCALAPPDATA%%\\Asphalt2Recomp\\logs.",reason,detail);MessageBoxA(NULL,message,"Asphalt Urban GT 2",MB_OK|MB_ICONERROR);}
 fflush(NULL);exit(code);
}
static void missing_import(unsigned i){finish("Unsupported game service",asphalt_imports[i].nid,asphalt_imports[i].name,10);}
#include "asphalt_missing_guards.h"
static void fault(uint32_t address,int write,int width){char text[80];snprintf(text,sizeof text,"%s of %d bits",write?"Memory write":"Memory read",width*8);finish("Invalid memory access",address,text,11);}
static void miss(uint32_t address){finish("Unsupported game target",address,"Dispatch target is not compiled",12);}
static void service(const char*name,uint32_t argument){finish("Unsupported service state",argument,name,10);}
static void media_failure(const char*reason){finish("Media decoding failed",psp_trace_last(),reason,17);}
void psp_syscall(uint32_t id){syscalls++;finish("Unsupported syscall",id,"syscall",13);}
void psp_unimplemented(uint32_t address,const char*what){traps++;finish("Unsupported instruction",address,what?what:"unknown",13);}
uint64_t psp_syscall_total(void){return syscalls;}
uint64_t psp_unimplemented_total(void){return traps;}
void psp_trap_reset(void){syscalls=traps=0;}
static void tick(void){
 if(!started)return;
 asphalt_interrupt_checkpoint();uint32_t buttons=0;uint8_t ax=128,ay=128;
 ULONGLONG elapsed=GetTickCount64()-started_ms;int result=asphalt_desktop_tick(elapsed,&buttons,&ax,&ay);
 if(result)finish(result>0?"Window closed":"Desktop device failed",psp_trace_last(),"Windows host",result>0?0:18);
 psp_ctrl_set(buttons,ax,ay);
 if(elapsed-last_progress>=5000){last_progress=elapsed;fprintf(stderr,"windows-progress: %llu ms disc=%llu ge=%llu movie=%llu framebuffer=%08X\n",(unsigned long long)elapsed,(unsigned long long)psp_io_bytes_read(),(unsigned long long)psp_ge_command_count(),(unsigned long long)asphalt_mpeg_video_frames(),psp_display_framebuffer());}
 /* Opt-in bounded automated startup check. Normal player sessions have no limit. */
 if(test_limit&&elapsed>=test_limit)finish("Test completed",psp_trace_last(),"Explicit test duration",0);
}
static uint16_t u16(const unsigned char*p){return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t u32(const unsigned char*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int image_load(const unsigned char*d,size_t n,uint32_t*entry,uint32_t*gp){
 if(!d||n!=4343022||memcmp(d,"\177ELF\1\1\1",7)||u16(d+16)!=2||u16(d+18)!=8||u16(d+42)!=32||u16(d+44)!=1||u32(d+28)>n-32)return -1;
 const unsigned char*ph=d+u32(d+28);uint32_t off=u32(ph+4),base=u32(ph+8),filesz=u32(ph+16),memsz=u32(ph+20);*entry=u32(d+24);
 if(u32(ph)!=1||base!=0x08804000||filesz!=3382256||memsz!=3449456||off>n||filesz>n-off||*entry!=0x08ad3a98||base+memsz>0x08c00000)return -1;
 void*dest=psp_mem_ptr(base,memsz);if(!dest)return -1;memset(dest,0,memsz);memcpy(dest,d+off,filesz);*gp=psp_read32(0x08ad4608+32);return *gp==0x08b45be0?0:-1;
}
int asphalt_windows_game_run(const unsigned char*image,size_t image_size,const char*iso,const char*root){
 const char*limit=getenv("ASPHALT_WINDOWS_TEST_MS");if(limit){char*end;unsigned long value=strtoul(limit,&end,10);if(end==limit||*end||value<100||value>180000)return 2;test_limit=(unsigned)value;}
 if(psp_mem_init())return 2;psp_cpu_reset();psp_hle_init();psp_recomp_register();uint32_t entry,gp;
 if(image_load(image,image_size,&entry,&gp))return 2;
 psp_io_set_root(root);if(psp_io_set_disc_image(iso)||asphalt_savedata_recover(root)||asphalt_savedata_validate(root)||asphalt_display_session_start(root)||asphalt_display_session_geometry(1280,720,NULL))return 2;
 asphalt_mpeg_register(media_failure);asphalt_atrac_register(media_failure);if(!asphalt_media_available())return 2;
 asphalt_startup_services_init(1,service);asphalt_savedata_init(root,service);install_missing_guards();
 const uint32_t stack_size=0x40000;uint32_t stack=psp_sysmem_alloc(stack_size,1);if(!stack)return 2;uint32_t k0=stack+stack_size-0x100;
 static const char boot_path[]="disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";uint32_t argp=(k0-sizeof boot_path)&~15u;psp_mem_write_block(argp,boot_path,sizeof boot_path);
 psp_cpu.r[PSP_REG_A0]=sizeof boot_path;psp_cpu.r[PSP_REG_A1]=argp;psp_cpu.r[PSP_REG_K0]=k0;psp_cpu.r[PSP_REG_GP]=gp;psp_cpu.r[PSP_REG_SP]=(argp-0x40)&~15u;psp_cpu.r[PSP_REG_RA]=0;
 psp_mem_set_fault_handler(fault);psp_set_miss_handler(miss);psp_intr_set_host_tick(tick);psp_trace_set_progress_hook(65536,tick);
 _putenv_s("ASPHALT_DESKTOP","1");_putenv_s("ASPHALT_RENDERER","gl");_putenv_s("ASPHALT_GPU_API","gl");
 if(asphalt_desktop_start())return 3;
 fprintf(stderr,"Asphalt Urban GT 2 Windows v1.0: entry=%08X gp=%08X\n",entry,gp);started_ms=GetTickCount64();started=1;psp_call_from_host(entry);
 uint64_t last=psp_threadman_handovers();for(int idle=0;idle<200&&!psp_exit_requested();){psp_threadman_yield();uint64_t now=psp_threadman_handovers();idle=now==last?idle+1:0;last=now;}
 finish(psp_exit_requested()?"Game exited":"Game threads idle",entry,"Session finished",psp_exit_requested()?0:16);
}
