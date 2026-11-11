#include "windows_main.c"
int asphalt_windows_game_run(const unsigned char*i,size_t n,const char*s,const char*r){(void)i;(void)n;(void)s;(void)r;return 0;}
int main(int argc,char**argv){if(argc<3)return 2;wchar_t path[2048];if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,argv[2],-1,path,2048))return 2;int busy=0;HANDLE mutex=instance_lock(path,&busy);if(!mutex)return busy?7:8;if(!strcmp(argv[1],"hold")){puts("ready");fflush(stdout);getchar();}ReleaseMutex(mutex);CloseHandle(mutex);return 0;}
