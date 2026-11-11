#include "windows_bootstrap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
int main(int argc,char**argv){
 if(argc!=4)return 2;unsigned char *out=NULL;size_t length=0;char error[256];int result;
 if(!strcmp(argv[1],"iso")){wchar_t path[2048];if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[2],-1,path,2048))return 2;result=asphalt_windows_load_iso(path,&out,&length,error,sizeof error);}
 else{FILE*f=fopen(argv[2],"rb");if(!f)return 2;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);unsigned char*data=malloc(n?n:1);if(!data)return 2;if(fread(data,1,n,f)!=(size_t)n)return 2;fclose(f);result=asphalt_windows_rebase_prx(data,n,0x08804000,&out,&length,error,sizeof error);free(data);}
 if(result){fprintf(stderr,"%s\n",error);return 1;}FILE*f=fopen(argv[3],"wb");if(!f)return 2;int ok=fwrite(out,1,length,f)==length;fclose(f);free(out);return ok?0:2;
}
