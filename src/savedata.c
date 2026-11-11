/* Original PSP utility ABI; own-recomp plaintext saves, not encrypted PSP imports.
 * PARAM.SFO is a standard PSF with explicit own-format size/CRC bookkeeping.
 * A private backup preserves the last committed payload across a two-file commit. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "savedata.h"
#include "psprecomp/hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/statvfs.h>
#endif

#define PATH_CAP 1400
#define FILE_LIMIT (4u*1024u*1024u)
#define SLOT_LIMIT (16u*1024u*1024u)
#define SECURE_LIMIT 32u
#define SFO_LIMIT 8192u
#define RW_ACCESS 0x80110325u
#define RW_BROKEN 0x80110326u
#define RW_NO_DATA 0x80110327u
#define RW_PARAM 0x80110328u
#define RW_NO_FILE 0x80110329u
#define TMP_PREFIX ".as2recomp-tmp-"
#define BACKUP_PREFIX ".as2recomp-backup-"
static const char format_name[]="AS2RECOMP_PLAIN_V1";
static const char *icon_names[]={"ICON0.PNG","ICON1.PMF","PIC1.PNG","SND0.AT3"};
typedef struct { char name[16]; uint32_t size,crc; } secure_file;
typedef struct {
    char title[128],savedata_title[128],detail[1024],directory[64];
    uint32_t parental,count;
    secure_file files[SECURE_LIMIT];
} save_info;
typedef struct { char name[256]; uint64_t size; time_t modified; } host_entry;
static char root[900];
static unsigned status;
static asphalt_service_stop stop;
static asphalt_savedata_persist before_write,after_write;
static void *persist_user;

static uint16_t le16(const unsigned char*p){return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t le32(const unsigned char*p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put16(unsigned char*p,uint16_t v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void put32(unsigned char*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t crc32(const unsigned char*p,size_t n){uint32_t c=~0u;for(size_t i=0;i<n;i++){c^=p[i];for(unsigned b=0;b<8;b++)c=(c>>1)^(0xEDB88320u&-(c&1u));}return ~c;}
static int memory(uint32_t p,uint32_t n){return p && n && n<=FILE_LIMIT && p<=UINT32_MAX-n && psp_mem_ptr(p,n)!=NULL;}
static int overlaps(uint32_t a,uint32_t an,uint32_t b,uint32_t bn){
    if(!an||!bn)return 0;
    /* Cache aliases and VRAM mirrors resolve to the same backing bytes. Guest
     * address arithmetic alone cannot detect those overlapping ABI buffers. */
    const void*ap=psp_mem_ptr(a,an),*bp=psp_mem_ptr(b,bn);if(!ap||!bp)return 0;
    uintptr_t aa=(uintptr_t)ap,bb=(uintptr_t)bp;
    return aa<bb?bb-aa<an:aa-bb<bn;
}
static int guest_string(uint32_t p,unsigned n,char*out){
    if(!memory(p,n))return -1;
    const char*s=psp_mem_ptr(p,n);const char*z=memchr(s,0,n);
    if(!z)return -1;
    memcpy(out,s,(size_t)(z-s)+1);return 0;
}
static int component(const char*s,int file){
    size_t n=strlen(s);if(!n||n>(file?12u:19u)||s[0]=='.'||s[n-1]=='.')return 0;
    for(size_t i=0;i<n;i++){unsigned c=(unsigned char)s[i];if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||(file&&c=='.')))return 0;}
    if(strstr(s,".."))return 0;
    char upper[20];for(size_t i=0;i<=n;i++)upper[i]=(s[i]>='a'&&s[i]<='z')?(char)(s[i]-32):s[i];
    char *dot=strchr(upper,'.');if(dot)*dot=0;
    if(!strcmp(upper,"CON")||!strcmp(upper,"PRN")||!strcmp(upper,"AUX")||!strcmp(upper,"NUL"))return 0;
    if((!strncmp(upper,"COM",3)||!strncmp(upper,"LPT",3))&&upper[3]>='1'&&upper[3]<='9'&&!upper[4])return 0;
    return 1;
}
static int system_file(const char*s){if(!strcmp(s,"PARAM.SFO"))return 1;for(unsigned i=0;i<4;i++)if(!strcmp(s,icon_names[i]))return 1;return 0;}
static int secure_name(const char*s){
    if(!component(s,1))return 0;
    char upper[16];size_t n=strlen(s);for(size_t i=0;i<=n;i++)upper[i]=(s[i]>='a'&&s[i]<='z')?(char)(s[i]-32):s[i];
    /* Keep one spelling across case-sensitive Android and case-insensitive hosts. */
    return !strcmp(s,upper)&&!system_file(upper);
}
static int join(char*out,const char*dir,const char*name){int n=snprintf(out,PATH_CAP,"%s/%s",dir,name);return n<0||(unsigned)n>=PATH_CAP?-1:0;}
/* 0 missing, 1 regular, 2 directory, -1 unsafe/error. Never accept links. */
static int info(const char*path,uint64_t*bytes,time_t*modified){
#ifdef _WIN32
    DWORD attr=GetFileAttributesA(path);
    if(attr==INVALID_FILE_ATTRIBUTES){DWORD e=GetLastError();return e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND?0:-1;}
    if(attr&FILE_ATTRIBUTE_REPARSE_POINT)return -1;
    if(attr&FILE_ATTRIBUTE_DIRECTORY)return 2;
    struct _stat64 st;if(_stat64(path,&st)||st.st_size<0)return -1;
#else
    struct stat st;if(lstat(path,&st))return errno==ENOENT?0:-1;
    if(S_ISLNK(st.st_mode))return -1;
    if(S_ISDIR(st.st_mode))return 2;
    if(!S_ISREG(st.st_mode)||st.st_size<0)return -1;
#endif
    if(bytes)*bytes=(uint64_t)st.st_size;
    if(modified)*modified=(time_t)st.st_mtime;
    return 1;
}
static int directory(const char*path,int create){
    int kind=info(path,NULL,NULL);if(kind==2)return 0;if(kind!=0||!create)return -1;
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path,0700);
#endif
}
static int slot_path(const char*base,const char*game,const char*save,char*out,int create){
    if(strcmp(game,"ULES00719")||!component(save,0)||strlen(game)+strlen(save)>=64||directory(base,0))return -1;
    char path[PATH_CAP];const char*parts[]={"ms","PSP","SAVEDATA"};
    if(strlen(base)>=sizeof path)return -1;
    strcpy(path,base);
    for(unsigned i=0;i<3;i++){char next[PATH_CAP];if(join(next,path,parts[i])||directory(next,create))return -1;strcpy(path,next);}
    char name[64];snprintf(name,sizeof name,"%s%s",game,save);if(join(out,path,name)||directory(out,create))return -1;return 0;
}
static int read_host(const char*path,unsigned char**data,uint32_t*size,uint32_t limit){
    uint64_t bytes;int kind=info(path,&bytes,NULL);if(kind!=1)return kind==0?1:-1;
    if(bytes>limit)return -1;
    unsigned char*d=malloc((size_t)bytes+1);if(!d)return -1;
    FILE*f=fopen(path,"rb");if(!f){free(d);return -1;}
    int ok=fread(d,1,(size_t)bytes,f)==bytes&&!ferror(f)&&fgetc(f)==EOF;
    if(fclose(f))ok=0;
    if(!ok){free(d);return -1;}
    *data=d;*size=(uint32_t)bytes;return 0;
}
static int flush_directory(const char*path){
#ifdef _WIN32
    (void)path;return 0; /* MoveFileEx WRITE_THROUGH flushes replacement. */
#else
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(fd<0)return -1;
    int result=fsync(fd);if(close(fd))result=-1;return result;
#endif
}
static int remove_owned(const char*dir,const char*name){char path[PATH_CAP];if(join(path,dir,name))return -1;int k=info(path,NULL,NULL);if(!k)return 0;if(k!=1)return -1;return remove(path)||flush_directory(dir)?-1:0;}
static int atomic_write(const char*dir,const char*name,const unsigned char*data,uint32_t size){
    char path[PATH_CAP],temp[PATH_CAP],tmpname[300];
    int n=snprintf(tmpname,sizeof tmpname,"%s%s",TMP_PREFIX,name);
    if(n<0||(unsigned)n>=sizeof tmpname||join(path,dir,name)||join(temp,dir,tmpname))return -1;
    int k=info(path,NULL,NULL),t=info(temp,NULL,NULL);if(k<0||k==2||t<0||t==2)return -1;
    /* Never truncate an existing temporary inode: it could be a hard link. */
    if(t==1&&remove_owned(dir,tmpname))return -1;
#ifdef _WIN32
    int fd=_open(temp,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
    FILE*f=fd<0?NULL:_fdopen(fd,"wb");if(fd>=0&&!f)_close(fd);
#else
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);FILE*f=fd<0?NULL:fdopen(fd,"wb");if(fd>=0&&!f)close(fd);
#endif
    if(!f)return -1;
    int ok=fwrite(data,1,size,f)==size&&fflush(f)==0;
#ifdef _WIN32
    if(ok&&_commit(_fileno(f)))ok=0;
#else
    if(ok&&fsync(fileno(f)))ok=0;
#endif
    if(fclose(f))ok=0;
    if(ok){
#ifdef _WIN32
        ok=MoveFileExA(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
        ok=rename(temp,path)==0;
#endif
    }
    if(!ok){remove(temp);return -1;}
    return flush_directory(dir);
}
static int visit(const char*dir,int(*fn)(const char*,const host_entry*,void*),void*user){
#ifdef _WIN32
    char pattern[PATH_CAP];if(join(pattern,dir,"*"))return -1;
    WIN32_FIND_DATAA fd;HANDLE h=FindFirstFileA(pattern,&fd);if(h==INVALID_HANDLE_VALUE)return -1;
    int result=0;
    do{if(!strcmp(fd.cFileName,".")||!strcmp(fd.cFileName,".."))continue;
        host_entry e={0};strncpy(e.name,fd.cFileName,sizeof e.name-1);char path[PATH_CAP];
        if(join(path,dir,e.name)){result=-1;break;}int k=info(path,&e.size,&e.modified);
        if(k<0){result=-1;break;}if(k==1&&(result=fn(dir,&e,user)))break;
    }while(FindNextFileA(h,&fd));
    if(!result&&GetLastError()!=ERROR_NO_MORE_FILES)result=-1;
    FindClose(h);return result;
#else
    DIR*d=opendir(dir);if(!d)return -1;int result=0;struct dirent*ent;
    while((ent=readdir(d))){if(!strcmp(ent->d_name,".")||!strcmp(ent->d_name,".."))continue;
        host_entry e={0};if(strlen(ent->d_name)>=sizeof e.name){result=-1;break;}strcpy(e.name,ent->d_name);
        char path[PATH_CAP];if(join(path,dir,e.name)){result=-1;break;}int k=info(path,&e.size,&e.modified);
        if(k<0){result=-1;break;}if(k==1&&(result=fn(dir,&e,user)))break;
    }if(closedir(d))result=-1;return result;
#endif
}
static int find_secure(const save_info*s,const char*name){for(unsigned i=0;i<s->count;i++)if(!strcmp(s->files[i].name,name))return (int)i;return -1;}

/* PSF encoder/decoder is independent of any emulator implementation. */
static int encode_sfo(const save_info*s,unsigned char*out,uint32_t*length){
    const char*keys[]={"AS2RECOMP_FILES","AS2RECOMP_FORMAT","CATEGORY","PARENTAL_LEVEL","SAVEDATA_DIRECTORY","SAVEDATA_DETAIL","SAVEDATA_FILE_LIST","SAVEDATA_PARAMS","SAVEDATA_TITLE","TITLE"};
    unsigned char registry[8+24*SECURE_LIMIT]={0},list[3168]={0},params[128]={0},parental[4];
    put32(registry,1);put32(registry+4,s->count);put32(parental,s->parental);
    for(unsigned i=0;i<s->count;i++){unsigned char*e=registry+8+24*i;memcpy(e,s->files[i].name,16);put32(e+16,s->files[i].size);put32(e+20,s->files[i].crc);memcpy(list+32*i,s->files[i].name,16);}
    const void*values[]={registry,format_name,"MS",parental,s->directory,s->detail,list,params,s->savedata_title,s->title};
    uint32_t maxs[]={sizeof registry,sizeof format_name,4,4,64,1024,sizeof list,sizeof params,128,128};
    uint16_t formats[]={0x0004,0x0204,0x0204,0x0404,0x0204,0x0204,0x0004,0x0004,0x0204,0x0204};
    uint32_t keybase=20+16*10,keybytes=0;for(unsigned i=0;i<10;i++)keybytes+=(uint32_t)strlen(keys[i])+1;
    uint32_t data=(keybase+keybytes+3)&~3u,key=0,at=0;memset(out,0,SFO_LIMIT);
    put32(out,0x46535000);put32(out+4,0x101);put32(out+8,keybase);put32(out+12,data);put32(out+16,10);
    for(unsigned i=0;i<10;i++){
        uint32_t n=formats[i]==0x0204?(uint32_t)strlen(values[i])+1:maxs[i];if(n>maxs[i]||data+at+maxs[i]>SFO_LIMIT)return -1;
        unsigned char*e=out+20+16*i;put16(e,(uint16_t)key);put16(e+2,formats[i]);put32(e+4,n);put32(e+8,maxs[i]);put32(e+12,at);
        memcpy(out+keybase+key,keys[i],strlen(keys[i])+1);key+=(uint32_t)strlen(keys[i])+1;
        memcpy(out+data+at,values[i],n);at+=(maxs[i]+3)&~3u;
    }*length=data+at;return 0;
}
static int decode_sfo(const unsigned char*d,uint32_t n,const char*directory_name,save_info*s){
    if(n<20||le32(d)!=0x46535000||le32(d+4)!=0x101)return -1;
    uint32_t keys=le32(d+8),data=le32(d+12),count=le32(d+16);if(count>32||keys<20+16*count||keys>=data||data>n)return -1;
    memset(s,0,sizeof *s);unsigned seen=0;const unsigned char*file_list=NULL;
    for(unsigned i=0;i<count;i++){
        const unsigned char*e=d+20+16*i;uint32_t key=le16(e),off=le32(e+12),len=le32(e+4),max=le32(e+8);uint16_t type=le16(e+2);
        if(key>=data-keys||!memchr(d+keys+key,0,data-keys-key)||len>max||off>n-data||max>n-data-off)return -1;
        const char*name=(const char*)d+keys+key;const unsigned char*v=d+data+off;
        unsigned bit=0;char*target=NULL;unsigned cap=0;
        if(!strcmp(name,"AS2RECOMP_FORMAT")){bit=1;if(type!=0x0204||len!=sizeof format_name||memcmp(v,format_name,len))return -1;}
        else if(!strcmp(name,"AS2RECOMP_FILES")){
            bit=2;if(type!=0x0004||len!=8+24*SECURE_LIMIT||le32(v)!=1||le32(v+4)>SECURE_LIMIT)return -1;s->count=le32(v+4);
            for(unsigned j=0;j<s->count;j++){const unsigned char*f=v+8+24*j;if(!memchr(f,0,16)||strlen((const char*)f)>12||!secure_name((const char*)f))return -1;strcpy(s->files[j].name,(const char*)f);s->files[j].size=le32(f+16);s->files[j].crc=le32(f+20);if(s->files[j].size>FILE_LIMIT)return -1;for(unsigned k=0;k<j;k++)if(!strcmp(s->files[k].name,s->files[j].name))return -1;}
        }else if(!strcmp(name,"SAVEDATA_FILE_LIST")){bit=4;if(type!=0x0004||len!=3168)return -1;file_list=v;}
        else if(!strcmp(name,"SAVEDATA_DIRECTORY")){bit=8;target=s->directory;cap=sizeof s->directory;}
        else if(!strcmp(name,"TITLE")){bit=16;target=s->title;cap=sizeof s->title;}
        else if(!strcmp(name,"SAVEDATA_TITLE")){bit=32;target=s->savedata_title;cap=sizeof s->savedata_title;}
        else if(!strcmp(name,"SAVEDATA_DETAIL")){bit=64;target=s->detail;cap=sizeof s->detail;}
        else if(!strcmp(name,"PARENTAL_LEVEL")){bit=128;if(type!=0x0404||len!=4||le32(v)>11)return -1;s->parental=le32(v);}
        if(bit&&(seen&bit))return -1;
        seen|=bit;
        if(target){if(type!=0x0204||!len||len>cap||v[len-1]||memchr(v,0,len-1))return -1;memcpy(target,v,len);}
    }
    if(seen!=255||strcmp(s->directory,directory_name))return -1;
    for(unsigned i=0;i<s->count;i++)if(memcmp(file_list+32*i,s->files[i].name,16))return -1;
    return 0;
}
static uint32_t load_sfo(const char*dir,save_info*s){
    char path[PATH_CAP];if(join(path,dir,"PARAM.SFO"))return RW_ACCESS;unsigned char*d=NULL;uint32_t n;
    int result=read_host(path,&d,&n,SFO_LIMIT);if(result)return result==1?RW_NO_DATA:RW_BROKEN;
    const char*name=strrchr(dir,'/');int invalid=decode_sfo(d,n,name?name+1:dir,s);free(d);return invalid?RW_BROKEN:0;
}
static int verified_payload(const char*dir,const secure_file*e,unsigned char**out){
    char path[PATH_CAP],backup[100];const char*names[2]={e->name,backup};snprintf(backup,sizeof backup,"%s%s",BACKUP_PREFIX,e->name);
    int absent=1;
    for(unsigned i=0;i<2;i++){if(join(path,dir,names[i]))return -1;unsigned char*d=NULL;uint32_t n;int r=read_host(path,&d,&n,FILE_LIMIT);if(r!=1)absent=0;if(!r){if(n==e->size&&crc32(d,n)==e->crc){*out=d;return (int)i;}free(d);}}
    return absent?-2:-1;
}
static int budget_entry(const char*dir,const host_entry*e,void*user){(void)dir;if(!strncmp(e->name,TMP_PREFIX,strlen(TMP_PREFIX))||!strncmp(e->name,BACKUP_PREFIX,strlen(BACKUP_PREFIX)))return 0;uint64_t*total=user;if(e->size>FILE_LIMIT||*total>SLOT_LIMIT-e->size)return -1;*total+=e->size;return 0;}
static int budget(const char*dir,uint64_t growth){uint64_t total=0;return visit(dir,budget_entry,&total)||growth>SLOT_LIMIT||total>SLOT_LIMIT-growth?-1:0;}
static int metadata_budget(const char*dir,const char**names,const uint32_t*sizes,unsigned count){
    uint64_t total=0;
    if(dir&&visit(dir,budget_entry,&total))return -1;
    for(unsigned i=0;i<count;i++){
        if(dir){char path[PATH_CAP];uint64_t old=0;if(join(path,dir,names[i]))return -1;int k=info(path,&old,NULL);if(k<0||k==2||old>total)return -1;if(k==1)total-=old;}
        if(sizes[i]>FILE_LIMIT||total>SLOT_LIMIT-sizes[i])return -1;
        total+=sizes[i];
    }
    return 0;
}
static int persist_before(void){return before_write?before_write(persist_user):0;}
static uint32_t persist_after(uint32_t result){if(after_write&&after_write(persist_user))return RW_ACCESS;return result;}

static uint32_t metadata_create(uint32_t p,const char*game,const char*save){
    if(psp_read8(p+100)||psp_read32(p+116)||psp_read32(p+124))return RW_PARAM;
    save_info s={0};if(guest_string(p+128,128,s.title)||guest_string(p+256,128,s.savedata_title)||guest_string(p+384,1024,s.detail)||psp_read8(p+1408)>11)return RW_PARAM;
    s.parental=psp_read8(p+1408);snprintf(s.directory,sizeof s.directory,"%s%s",game,save);
    for(unsigned i=0;i<4;i++){uint32_t q=p+1412+16*i,n=psp_read32(q+8),cap=psp_read32(q+4);if(n>FILE_LIMIT||cap>FILE_LIMIT||n>cap||(cap&&!memory(psp_read32(q),cap)))return RW_PARAM;}
    char dir[PATH_CAP];int exists=slot_path(root,game,save,dir,0)==0;
    if(exists){save_info old;uint32_t r=load_sfo(dir,&old);if(r!=RW_NO_DATA&&r)return r;if(!r){s.count=old.count;memcpy(s.files,old.files,sizeof s.files);}}
    unsigned char d[SFO_LIMIT];uint32_t n;if(encode_sfo(&s,d,&n))return RW_ACCESS;
    const char*names[5]={"PARAM.SFO"};uint32_t sizes[5]={n};unsigned count=1;
    for(unsigned i=0;i<4;i++){uint32_t size=psp_read32(p+1412+16*i+8);if(size){names[count]=icon_names[i];sizes[count++]=size;}}
    if(metadata_budget(exists?dir:NULL,names,sizes,count))return RW_ACCESS;
    if(persist_before())return RW_ACCESS;
    if(slot_path(root,game,save,dir,1))return RW_ACCESS;
    for(unsigned i=0;i<4;i++){uint32_t q=p+1412+16*i,n=psp_read32(q+8);if(n&&atomic_write(dir,icon_names[i],psp_mem_ptr(psp_read32(q),n),n))return RW_ACCESS;}
    if(atomic_write(dir,"PARAM.SFO",d,n))return RW_ACCESS;
    return persist_after(0);
}
static uint32_t payload_write(uint32_t p,const char*dir,const char*name,int erase){
    save_info s;uint32_t result=load_sfo(dir,&s);if(result)return result;
    int index=find_secure(&s,name);if(erase&&index<0)return RW_NO_FILE;
    uint32_t size=erase?0:psp_read32(p+124),cap=psp_read32(p+120),buffer=psp_read32(p+116);
    if(!erase&&(size>FILE_LIMIT||cap>FILE_LIMIT||size>cap||(cap&&(!memory(buffer,cap)||overlaps(buffer,cap,p,1536)))))return RW_PARAM;
    unsigned char*old=NULL;uint32_t old_size=0;int had_old=index>=0;
    if(had_old){int v=verified_payload(dir,&s.files[index],&old);if(v<0)return v==-2?RW_NO_FILE:RW_BROKEN;old_size=s.files[index].size;}
    if(!erase&&index<0){if(s.count==SECURE_LIMIT){free(old);return RW_ACCESS;}index=(int)s.count++;memset(&s.files[index],0,sizeof s.files[index]);strcpy(s.files[index].name,name);}
    if(erase){for(unsigned i=(unsigned)index;i+1<s.count;i++)s.files[i]=s.files[i+1];memset(&s.files[--s.count],0,sizeof s.files[0]);}
    else{s.files[index].size=size;s.files[index].crc=crc32(size?psp_mem_ptr(buffer,size):(const unsigned char*)"",size);}
    unsigned char metadata[SFO_LIMIT];uint32_t metadata_size;if(encode_sfo(&s,metadata,&metadata_size)||budget(dir,size>old_size?size-old_size:0)){free(old);return RW_ACCESS;}
    if(persist_before()){free(old);return RW_ACCESS;}
    char backup[100];snprintf(backup,sizeof backup,"%s%s",BACKUP_PREFIX,name);
    if(had_old&&atomic_write(dir,backup,old,old_size)){free(old);return RW_ACCESS;}
    int failed=erase?remove_owned(dir,name):atomic_write(dir,name,size?psp_mem_ptr(buffer,size):(const unsigned char*)"",size);
    if(!failed)failed=atomic_write(dir,"PARAM.SFO",metadata,metadata_size);
    if(failed){if(had_old)atomic_write(dir,name,old,old_size);else remove_owned(dir,name);free(old);return RW_ACCESS;}
    free(old);if(had_old&&remove_owned(dir,backup))return persist_after(RW_ACCESS);
    return persist_after(0);
}
static uint32_t payload_read(uint32_t p,const char*dir,const char*name){
    uint32_t cap=psp_read32(p+120),buffer=psp_read32(p+116);if(cap>FILE_LIMIT||!memory(buffer,cap)||overlaps(buffer,cap,p,1536))return RW_PARAM;
    save_info s;uint32_t r=load_sfo(dir,&s);if(r)return r;int index=find_secure(&s,name);if(index<0)return RW_NO_FILE;
    secure_file*e=&s.files[index];if(e->size>cap)return RW_PARAM;unsigned char*d=NULL;int v=verified_payload(dir,e,&d);if(v<0)return v==-2?RW_NO_FILE:RW_BROKEN;
    if(e->size)psp_mem_write_block(buffer,d,e->size);
    free(d);psp_write32(p+124,e->size);return 0;
}
static void date_time(unsigned char*out,time_t t){struct tm value;
#ifdef _WIN32
    if(gmtime_s(&value,&t))return;
#else
    if(!gmtime_r(&t,&value))return;
#endif
    put16(out,(uint16_t)(value.tm_year+1900));put16(out+2,(uint16_t)(value.tm_mon+1));put16(out+4,(uint16_t)value.tm_mday);put16(out+6,(uint16_t)value.tm_hour);put16(out+8,(uint16_t)value.tm_min);put16(out+10,(uint16_t)value.tm_sec);
}
typedef struct {uint32_t max[3],table[3],count[3];const save_info*s;} listing;
static int list_entry(const char*dir,const host_entry*e,void*user){
    listing*l=user;if(e->name[0]=='.')return 0;if(strlen(e->name)>15||(!component(e->name,1)&&!system_file(e->name)))return 0;
    int index=find_secure(l->s,e->name);unsigned kind=index>=0?0:system_file(e->name)?2:1;
    if(l->count[kind]>=l->max[kind])return 0;
    uint64_t size=e->size;if(index>=0){unsigned char*d=NULL;int valid=verified_payload(dir,&l->s->files[index],&d);free(d);if(valid<0)return -1;size=l->s->files[index].size;}
    unsigned char out[80]={0};put32(out,0x21FF);put32(out+8,(uint32_t)size);put32(out+12,(uint32_t)(size>>32));for(unsigned i=0;i<3;i++)date_time(out+16+16*i,e->modified);memcpy(out+64,e->name,strlen(e->name));psp_mem_write_block(l->table[kind]+80*l->count[kind]++,out,80);return 0;
}
static uint32_t files(uint32_t p,const char*dir){
    uint32_t f=psp_read32(p+1528);if(!memory(f,36)||overlaps(f,36,p,1536))return RW_PARAM;listing l={0};
    for(unsigned i=0;i<3;i++){l.max[i]=psp_read32(f+4*i);l.table[i]=psp_read32(f+24+4*i);if(l.max[i]>4096||(l.max[i]&&(!memory(l.table[i],80*l.max[i])||overlaps(l.table[i],80*l.max[i],p,1536)||overlaps(l.table[i],80*l.max[i],f,36))))return RW_PARAM;for(unsigned j=0;j<i;j++)if(l.max[i]&&l.max[j]&&overlaps(l.table[i],80*l.max[i],l.table[j],80*l.max[j]))return RW_PARAM;}
    save_info s;uint32_t r=load_sfo(dir,&s);if(r)return r;l.s=&s;for(unsigned i=0;i<3;i++)psp_write32(f+12+4*i,0);
    if(visit(dir,list_entry,&l))return RW_BROKEN;
    for(unsigned i=0;i<3;i++)psp_write32(f+12+4*i,l.count[i]);
    return 0;
}
static int free_space(uint64_t*bytes){
#ifdef _WIN32
    ULARGE_INTEGER available;if(!GetDiskFreeSpaceExA(root,&available,NULL,NULL))return -1;*bytes=available.QuadPart;
#else
    struct statvfs volume;if(statvfs(root,&volume))return -1;uint64_t blocks=volume.f_bavail,unit=volume.f_frsize?volume.f_frsize:volume.f_bsize;if(!unit||blocks>UINT64_MAX/unit)return -1;*bytes=blocks*unit;
#endif
    return 0;
}
static void size_info(uint32_t out,uint64_t bytes){
    uint64_t count=(bytes+32767)/32768;if(count>UINT32_MAX/32)count=UINT32_MAX/32;uint32_t kb=(uint32_t)count*32;psp_write32(out,(uint32_t)count);psp_write32(out+4,kb);psp_write32(out+16,kb);char text[32],short_text[8]={0};snprintf(text,sizeof text,"%u KB",kb);memcpy(short_text,text,strlen(text)<7?strlen(text):7);psp_mem_write_block(out+8,short_text,8);psp_mem_write_block(out+20,short_text,8);
}
static int used_entry(const char*dir,const host_entry*e,void*user){(void)dir;if(e->name[0]=='.')return 0;uint64_t*used=user;if(e->size>UINT64_MAX-32767||*used>UINT64_MAX-((e->size+32767)/32768)*32768)return -1;*used+=((e->size+32767)/32768)*32768;return 0;}
static uint32_t sizes(uint32_t p){
    uint32_t fr=psp_read32(p+1488),md=psp_read32(p+1492),ud=psp_read32(p+1496);if((fr&&(!memory(fr,20)||overlaps(fr,20,p,1536)))||(md&&(!memory(md,64)||overlaps(md,64,p,1536)))||(ud&&(!memory(ud,28)||overlaps(ud,28,p,1536))))return 0x801103C8u;
    uint64_t bytes;if(free_space(&bytes))return 0x801103C5u;
    if(fr){uint64_t clusters=bytes/32768,kb=bytes/1024;psp_write32(fr,32768);psp_write32(fr+4,clusters>UINT32_MAX?UINT32_MAX:(uint32_t)clusters);psp_write32(fr+8,kb>UINT32_MAX?UINT32_MAX:(uint32_t)kb);char text[32],short_text[8]={0};snprintf(text,sizeof text,"%llu MB",(unsigned long long)(bytes/1048576));memcpy(short_text,text,strlen(text)<7?strlen(text):7);psp_mem_write_block(fr+12,short_text,8);}
    uint32_t result=0;
    if(md){char game[17],save[21],dir[PATH_CAP];if(guest_string(md,13,game)||guest_string(md+16,20,save))return 0x801103C8u;if(!strcmp(save,"<>"))save[0]=0;if(strcmp(game,"ULES00719")||(!component(save,0)&&*save))return 0x801103C8u;uint64_t used=0;if(!*save||slot_path(root,game,save,dir,0))result=0x801103C7u;else if(visit(dir,used_entry,&used))return 0x801103C5u;size_info(md+36,used);}
    if(ud){uint64_t required=2*32768;if(psp_read8(p+100))required+=((uint64_t)psp_read32(p+124)+32767)/32768*32768;for(unsigned i=0;i<4;i++)required+=((uint64_t)psp_read32(p+1412+16*i+8)+32767)/32768*32768;size_info(ud,required);}
    return result;
}
static void init_start(void){
    uint32_t p=psp_arg(0);if(status&&status!=4){psp_ret(0x80110001u);return;}
    if(!memory(p,1536)||psp_read32(p)!=1536){psp_ret(0x80110004u);return;}
    unsigned mode=psp_read32(p+48);uint32_t result=RW_PARAM;
    if(mode==8)result=sizes(p);
    else if(mode==7)result=1; /* Interactive system delete dialog is cancelled. */
    else if(mode==12||mode==13||mode==15||mode==17||mode==19){
        char game[14],save[21],name[14],dir[PATH_CAP];
        if(!guest_string(p+60,13,game)&&!guest_string(p+76,20,save)&&!guest_string(p+100,13,name)&&!strcmp(game,"ULES00719")&&component(save,0)){
            if(mode==13)result=metadata_create(p,game,save);
            else if(mode!=12&&!secure_name(name))result=RW_PARAM;
            else if(slot_path(root,game,save,dir,0))result=RW_NO_DATA;
            else if(mode==12)result=files(p,dir);
            else if(mode==15)result=payload_read(p,dir,name);
            else result=payload_write(p,dir,name,mode==19);
        }
    }else{if(stop)stop("savedata mode/size request",mode);psp_ret(SCE_KERNEL_ERROR_NOTIMPLEMENTED);return;}
    psp_write32(p+28,result);status=1;fprintf(stderr,"savedata: mode=%u result=%08X\n",mode,result);psp_ret(0);
}
static void get_status(void){unsigned r=status;if(status==1)status=2;else if(status==2)status=3;else if(status==4)status=0;psp_ret(r);}
static void update(void){if(status==2)status=3;psp_ret(0);}
static void shutdown_start(void){if(status!=2&&status!=3){psp_ret(0x80110001u);return;}status=4;psp_ret(0);}
static int recover_slot(const char*dir){
    save_info s;uint32_t result=load_sfo(dir,&s);if(result)return result==RW_NO_DATA?0:-1;
    for(unsigned i=0;i<s.count;i++){
        unsigned char*d=NULL;int from=verified_payload(dir,&s.files[i],&d);if(from<0)return -1;
        if(from&&atomic_write(dir,s.files[i].name,d,s.files[i].size)){free(d);return -1;}free(d);
        char backup[100];snprintf(backup,sizeof backup,"%s%s",BACKUP_PREFIX,s.files[i].name);if(remove_owned(dir,backup))return -1;
    }return 0;
}
static int validate_slot(const char*dir){
    save_info s;if(load_sfo(dir,&s)||budget(dir,0))return -1;
    for(unsigned i=0;i<s.count;i++){
        char path[PATH_CAP];unsigned char*d=NULL;uint32_t n;
        if(join(path,dir,s.files[i].name)||read_host(path,&d,&n,FILE_LIMIT))return -1;
        int valid=n==s.files[i].size&&crc32(d,n)==s.files[i].crc;free(d);if(!valid)return -1;
    }
    return 0;
}
static int walk_saves(const char*runtime_root,int(*slot_action)(const char*)){
    if(!runtime_root||!*runtime_root||strlen(runtime_root)>=900||directory(runtime_root,0))return -1;
    char base[PATH_CAP],next[PATH_CAP];strcpy(base,runtime_root);const char*parts[]={"ms","PSP","SAVEDATA"};
    for(unsigned i=0;i<3;i++){if(join(next,base,parts[i]))return -1;int k=info(next,NULL,NULL);if(!k)return 0;if(k!=2)return -1;strcpy(base,next);}
    int result=0;
#ifdef _WIN32
    char pattern[PATH_CAP];if(join(pattern,base,"*"))return -1;WIN32_FIND_DATAA e;HANDLE h=FindFirstFileA(pattern,&e);if(h==INVALID_HANDLE_VALUE)return -1;
    do{if(strncmp(e.cFileName,"ULES00719",9))continue;char dir[PATH_CAP];if(!component(e.cFileName+9,0)||join(dir,base,e.cFileName)||info(dir,NULL,NULL)!=2||slot_action(dir)){result=-1;break;}}while(FindNextFileA(h,&e));FindClose(h);
#else
    DIR*d=opendir(base);if(!d)return -1;struct dirent*e;while((e=readdir(d))){if(strncmp(e->d_name,"ULES00719",9))continue;char dir[PATH_CAP];if(!component(e->d_name+9,0)||join(dir,base,e->d_name)||info(dir,NULL,NULL)!=2||slot_action(dir)){result=-1;break;}}closedir(d);
#endif
    return result;
}
int asphalt_savedata_recover(const char*runtime_root){return walk_saves(runtime_root,recover_slot);}
int asphalt_savedata_validate(const char*runtime_root){return walk_saves(runtime_root,validate_slot);}
void asphalt_savedata_set_persistence(asphalt_savedata_persist before,asphalt_savedata_persist after,void*user){before_write=before;after_write=after;persist_user=user;}
void asphalt_savedata_init(const char*runtime_root,asphalt_service_stop unsupported){
    snprintf(root,sizeof root,"%s",runtime_root?runtime_root:"");status=0;stop=unsupported;before_write=NULL;after_write=NULL;persist_user=NULL;(void)asphalt_savedata_recover(root);
    psp_hle_register(0x50C4CD57u,"sceUtility","sceUtilitySavedataInitStart",init_start);psp_hle_register(0x8874DBE0u,"sceUtility","sceUtilitySavedataGetStatus",get_status);psp_hle_register(0xD4B95FFBu,"sceUtility","sceUtilitySavedataUpdate",update);psp_hle_register(0x9790B33Cu,"sceUtility","sceUtilitySavedataShutdownStart",shutdown_start);
}
