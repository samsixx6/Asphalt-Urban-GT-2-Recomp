/* Runtime import of the one supported original disc. No game data is embedded. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "windows_bootstrap.h"

static uint16_t u16(const unsigned char*p){return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t u32(const unsigned char*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put16(unsigned char*p,uint16_t n){p[0]=(unsigned char)n;p[1]=(unsigned char)(n>>8);}
static void put32(unsigned char*p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(n>>(i*8));}
static int fail(char*e,size_t n,const char*s){if(e&&n)snprintf(e,n,"%s",s);return -1;}
static int range(size_t n,uint64_t off,uint64_t bytes){return off<=n&&bytes<=n-off;}

int asphalt_windows_rebase_prx(const unsigned char*src,size_t n,uint32_t base,
 unsigned char**output,size_t*out_size,char*error,size_t error_size){
 unsigned char*out=NULL,*seen=NULL;const char*why="Invalid original startup executable";
 if(!output||!out_size)return fail(error,error_size,why);*output=NULL;*out_size=0;
 if(!src||n<52||n>64*1024*1024||memcmp(src,"\177ELF\1\1\1",7)||u16(src+16)!=0xffa0||u16(src+18)!=8||u16(src+40)!=52||u16(src+42)!=32||u16(src+46)!=40)goto bad;
 uint32_t phoff=u32(src+28),shoff=u32(src+32),phnum=u16(src+44),shnum=u16(src+48),entry=u32(src+24);
 if(!phnum||shnum>4096||!range(n,phoff,phnum*32ull)||!range(n,shoff,shnum*40ull))goto bad;
 const unsigned char*ph=src+phoff;unsigned loads=0;
 for(unsigned i=0;i<phnum;i++)if(u32(ph+32*i)==1){if(i)goto bad;loads++;}
 uint32_t fileoff=u32(ph+4),filesz=u32(ph+16),memsz=u32(ph+20);
 if(loads!=1||u32(ph+8)||!range(n,fileoff,filesz)||filesz>memsz||entry>=filesz||(base&4095)||base<0x08800000u||(uint64_t)base+memsz>0x0a000000u)goto bad;
 out=malloc(n);seen=calloc((filesz+3ull)/4,1);if(!out||!seen){why="Cannot allocate startup image";goto bad;}memcpy(out,src,n);unsigned total=0;
 for(unsigned s=0;s<shnum;s++){
  const unsigned char*sh=src+shoff+40*s;uint32_t type=u32(sh+4),off=u32(sh+16),bytes=u32(sh+20),stride=u32(sh+36);
  if((type==9||type==0x700000a1u)&&bytes){why="Unsupported relocation encoding";goto bad;}
  if(type!=0x700000a0u)continue;
  if(!range(n,off,bytes)||(bytes&7)||(stride&&stride!=8))goto bad;
  int low=0,have_low=0;
  /* Reverse lookup uses the original LO16 even when several HI16 share it. */
  for(uint32_t k=bytes/8;k>0;k--){
   uint32_t pos=off+(k-1)*8,patch=u32(src+pos),info=u32(src+pos+4),kind=info&255;
   if(info>>8||(kind!=2&&kind!=4&&kind!=5&&kind!=6)||patch%4||!range(filesz,patch,4)){why="Unsupported or out-of-range relocation";goto bad;}
   if(seen[patch/4]){why="Duplicate relocation";goto bad;}seen[patch/4]=1;
   uint32_t word=u32(src+fileoff+patch),value;
   if(kind==6){low=(int16_t)(word&65535);have_low=1;}
   if(kind==2)value=word+base;
   else if(kind==4)value=(word&0xfc000000u)|(((word&0x03ffffffu)+(base>>2))&0x03ffffffu);
   else if(kind==5){if(!have_low){why="Unpaired HI16 relocation";goto bad;}int64_t address=((int64_t)(word&65535)<<16)+low+base;value=(word&0xffff0000u)|(uint32_t)((address+0x8000)>>16&65535);}
   else value=(word&0xffff0000u)|((word+base)&65535);
   put32(out+fileoff+patch,value);put32(out+pos,patch+base);total++;
  }
 }
 if(!total)goto bad;
 put16(out+16,2);put32(out+24,entry+base);put32(out+phoff+8,base);put32(out+phoff+12,base);
 for(unsigned s=0;s<shnum;s++){const unsigned char*sh=src+shoff+40*s;if(u32(sh+8)&2){if((uint64_t)u32(sh+12)+u32(sh+20)>memsz)goto bad;put32(out+shoff+40*s+12,u32(sh+12)+base);}}
 free(seen);*output=out;*out_size=n;return 0;
bad:free(seen);free(out);return fail(error,error_size,why);
}

static int hash(FILE*f,unsigned char digest[32]){
 BCRYPT_ALG_HANDLE alg=NULL;BCRYPT_HASH_HANDLE h=NULL;unsigned char*buffer=NULL;int result=-1;
 if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,NULL,0)<0)goto done;
 if(BCryptCreateHash(alg,&h,NULL,0,NULL,0,0)<0)goto done;
 buffer=malloc(1024*1024);if(!buffer)goto done;rewind(f);size_t n;
 while((n=fread(buffer,1,1024*1024,f)))if(BCryptHashData(h,buffer,(ULONG)n,0)<0)goto done;
 if(ferror(f)||BCryptFinishHash(h,digest,32,0)<0)goto done;result=0;
done:free(buffer);if(h)BCryptDestroyHash(h);if(alg)BCryptCloseAlgorithmProvider(alg,0);return result;
}
static int digest_matches(const unsigned char digest[32],const char*hex){char actual[65];for(unsigned i=0;i<32;i++)snprintf(actual+2*i,3,"%02x",digest[i]);return !strcmp(actual,hex);}
static int memory_matches(const unsigned char*data,size_t n,const char*hex){
 BCRYPT_ALG_HANDLE alg=NULL;unsigned char digest[32];int ok=0;
 if(n>0xffffffffu||BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,NULL,0)<0)return 0;
 if(BCryptHash(alg,NULL,0,(PUCHAR)data,(ULONG)n,digest,32)>=0)ok=digest_matches(digest,hex);BCryptCloseAlgorithmProvider(alg,0);return ok;
}
static int read_at(FILE*f,uint64_t length,uint64_t off,void*data,size_t n){return range((size_t)length,off,n)&&!_fseeki64(f,(long long)off,SEEK_SET)&&fread(data,1,n,f)==n;}
static int component(FILE*f,uint64_t length,uint32_t*lba,uint32_t*size,const char*name,int directory){
 uint64_t base=(uint64_t)*lba*2048;uint32_t count=*size;if(count>16*1024*1024||!range((size_t)length,base,count))return -1;
 for(uint32_t sector=0;sector<count;sector+=2048){unsigned char block[2048];size_t bytes=count-sector<2048?count-sector:2048;if(!read_at(f,length,base+sector,block,bytes))return -1;
  for(size_t off=0;off<bytes;){unsigned span=block[off];if(!span)break;if(span<34||span>bytes-off)return -1;const unsigned char*r=block+off;unsigned chars=r[32];if(chars>span-33)return -1;
   size_t actual=0;while(actual<chars&&r[33+actual]!=';')actual++;
   if(strlen(name)==actual&&!memcmp(r+33,name,actual)){if(!!(r[25]&2)!=directory||r[25]&0x80)return -1;*lba=u32(r+2);*size=u32(r+10);return range((size_t)length,(uint64_t)*lba*2048,*size)?0:-1;}off+=span;
  }
 }return -1;
}
int asphalt_windows_load_iso(const wchar_t*path,unsigned char**image,size_t*size,char*error,size_t error_size){
 const char*why="Cannot open the selected ISO";FILE*f=NULL;unsigned char*boot=NULL;int result=-1;if(!image||!size)return -1;*image=NULL;*size=0;
 f=_wfopen(path,L"rb");if(!f)goto done;if(_fseeki64(f,0,SEEK_END))goto done;long long length=_ftelli64(f);
 why="Select the supported original Asphalt 2 Europe (ULES00719) ISO";
 if(length!=884146176)goto done;unsigned char digest[32];if(hash(f,digest)||!digest_matches(digest,"64ab1ba788baade948b9b89d47145a653c063e85afed403b89d917d8a8703ec4"))goto done;
 unsigned char pvd[2048];if(!read_at(f,length,16*2048,pvd,sizeof pvd)||memcmp(pvd,"\1CD001\1",7)||pvd[156]<34)goto done;
 uint32_t lba=u32(pvd+158),n=u32(pvd+166);if(component(f,length,&lba,&n,"PSP_GAME",1)||component(f,length,&lba,&n,"SYSDIR",1)||component(f,length,&lba,&n,"BOOT.BIN",0)||n!=4343022)goto done;
 boot=malloc(n);if(!boot){why="Cannot allocate the startup image";goto done;}
 if(!read_at(f,length,(uint64_t)lba*2048,boot,n)||!memory_matches(boot,n,"d2a2e78ae0a19b4d7ac2eeb6386b8ae16cd7c35c624bdd59d0c6b4d241b31f9c"))goto done;
 if(asphalt_windows_rebase_prx(boot,n,0x08804000,image,size,error,error_size)){why=NULL;goto done;}
 if(!memory_matches(*image,*size,"6e52686f5f0b9e476a2481661b91ae241bd7b429e060f197d9728565a6317c3b")){free(*image);*image=NULL;*size=0;why="Startup image does not match this build";goto done;}result=0;
done:if(f)fclose(f);free(boot);if(result&&why)fail(error,error_size,why);return result;
}
