/* Native GUI launcher. Player machines need only this build and their own ISO. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "windows_bootstrap.h"
#include "windows_game.h"
#define PATH_CAP 2048
static int utf8(const wchar_t*input,char*output,int capacity){return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,input,-1,output,capacity,NULL,NULL)>0?0:-1;}
static void message(const char*text){MessageBoxA(NULL,text,"Asphalt Urban GT 2",MB_OK|MB_ICONERROR);}
static int joined(wchar_t*out,size_t n,const wchar_t*root,const wchar_t*leaf){return swprintf(out,n,L"%ls\\%ls",root,leaf)>0&&wcslen(out)<n-1?0:-1;}
static int directory(const wchar_t*path){int n=SHCreateDirectoryExW(NULL,path,NULL);return n==ERROR_SUCCESS||n==ERROR_ALREADY_EXISTS?0:-1;}
static int absolute(wchar_t*path){wchar_t full[PATH_CAP];DWORD n=GetFullPathNameW(path,PATH_CAP,full,NULL);if(!n||n>=PATH_CAP)return -1;wcscpy(path,full);return 0;}
/* Lock the actual directory identity, so path aliases cannot open two writers. */
static HANDLE instance_lock(const wchar_t*root,int*already){
 *already=0;HANDLE directory=CreateFileW(root,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
 if(directory==INVALID_HANDLE_VALUE)return NULL;BY_HANDLE_FILE_INFORMATION info;
 int ok=GetFileInformationByHandle(directory,&info);CloseHandle(directory);if(!ok)return NULL;
 wchar_t name[128];swprintf(name,128,L"Local\\Samsixx.Asphalt2Recomp-%08lx-%08lx-%08lx",info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow);
 HANDLE mutex=CreateMutexW(NULL,TRUE,name);if(mutex&&GetLastError()==ERROR_ALREADY_EXISTS){*already=1;CloseHandle(mutex);return NULL;}return mutex;
}
static HANDLE session_mutex;
static int pick(wchar_t*path){OPENFILENAMEW dialog={0};path[0]=0;dialog.lStructSize=sizeof dialog;dialog.lpstrFilter=L"Asphalt Urban GT 2 ISO (*.iso)\0*.iso\0All files\0*.*\0";dialog.lpstrFile=path;dialog.nMaxFile=PATH_CAP;dialog.lpstrTitle=L"Select your original Asphalt Urban GT 2 Europe (ULES00719) ISO";dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT;return GetOpenFileNameW(&dialog)?1:CommDlgExtendedError()?-1:0;}
static int remembered(const wchar_t*file,wchar_t*path){char data[1024];FILE*f=_wfopen(file,L"rb");if(!f)return -1;size_t n=fread(data,1,sizeof data-1,f);int extra=fgetc(f);fclose(f);if(!n||extra!=EOF)return -1;data[n]=0;while(n&&(data[n-1]=='\r'||data[n-1]=='\n'))data[--n]=0;return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,data,-1,path,PATH_CAP)?0:-1;}
static int remember(const wchar_t*file,const char*path){wchar_t temp[PATH_CAP];if(swprintf(temp,PATH_CAP,L"%ls.tmp",file)<=0)return -1;FILE*f=_wfopen(temp,L"wb");if(!f)return -1;int ok=fwrite(path,1,strlen(path),f)==strlen(path)&&!fflush(f);if(fclose(f))ok=0;if(!ok){DeleteFileW(temp);return -1;}if(!MoveFileExW(temp,file,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp);return -1;}return 0;}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE previous,PWSTR command,int show){
 (void)instance;(void)previous;(void)command;(void)show;
 int argc=0;wchar_t**argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(!argv)return 2;
 wchar_t base[PATH_CAP],iso[PATH_CAP]={0},data[PATH_CAP],logs[PATH_CAP],config[PATH_CAP],log[PATH_CAP];int force_picker=0,explicit_iso=0;
 if(SHGetFolderPathW(NULL,CSIDL_LOCAL_APPDATA|CSIDL_FLAG_CREATE,NULL,SHGFP_TYPE_CURRENT,base)!=S_OK||wcscat_s(base,PATH_CAP,L"\\Asphalt2Recomp")){LocalFree(argv);return 2;}
 for(int i=1;i<argc;i++){
  if(!wcscmp(argv[i],L"--select-iso"))force_picker=1;
  else if((!wcscmp(argv[i],L"--iso")||!wcscmp(argv[i],L"--data-root"))&&i+1<argc){int is_iso=!wcscmp(argv[i],L"--iso");if(wcslen(argv[++i])>=PATH_CAP){LocalFree(argv);return 2;}wcscpy(is_iso?iso:base,argv[i]);if(is_iso)explicit_iso=1;}
  else{message("Unknown option. Use --iso <file>, --select-iso, or --data-root <folder>.");LocalFree(argv);return 2;}
 }
 LocalFree(argv);
 if(absolute(base)){message("The game data folder path is invalid or too long.");return 2;}
 if(directory(base)){message("Cannot create the game data folder.");return 2;}
 int already=0;session_mutex=instance_lock(base,&already);if(!session_mutex){if(already){MessageBoxA(NULL,"Asphalt Urban GT 2 is already running with this data folder.","Asphalt Urban GT 2",MB_OK|MB_ICONINFORMATION);return 0;}message("Cannot lock the game data folder.");return 2;}
 if(joined(data,PATH_CAP,base,L"data")||directory(data)||joined(logs,PATH_CAP,base,L"logs")||directory(logs)||joined(config,PATH_CAP,base,L"iso-path.txt")||joined(log,PATH_CAP,logs,L"session.log")){message("Cannot create the game data folder.");return 2;}
 FILE*logfile=NULL;_wfreopen_s(&logfile,log,L"w",stderr);setvbuf(stderr,NULL,_IONBF,0);
 char root8[1024],iso8[1024],error[256];if(utf8(data,root8,sizeof root8)){message("The game data path is too long.");return 2;}
 unsigned char*image=NULL;size_t image_size=0;int had_remembered=0;
 if(!explicit_iso&&!force_picker)had_remembered=remembered(config,iso)==0;
 for(;;){
  if(!iso[0]||force_picker){int result=pick(iso);force_picker=0;if(result<=0){if(result<0)message("Windows could not open the ISO selection dialog.");return result<0?2:0;}}
  if(absolute(iso)){message("The ISO path is invalid or too long.");return 2;}
  if(utf8(iso,iso8,sizeof iso8)){message("The ISO path is too long. Move it to a shorter folder path.");if(explicit_iso)return 2;iso[0]=0;continue;}
  if(!asphalt_windows_load_iso(iso,&image,&image_size,error,sizeof error))break;
  fprintf(stderr,"ISO import: %s\n",error);
  if(explicit_iso){message(error);return 2;}
  if(had_remembered){MessageBoxA(NULL,"The remembered ISO is missing or has changed. Select your original ISO again.","Asphalt Urban GT 2",MB_OK|MB_ICONINFORMATION);had_remembered=0;}
  else message(error);iso[0]=0;
 }
 if(remember(config,iso8)){free(image);message("Cannot save the selected ISO location.");return 2;}
 if(_wchdir(data)){free(image);message("Cannot open the game data folder.");return 2;}
 int result=asphalt_windows_game_run(image,image_size,iso8,root8);free(image);
 if(result)message(result==3?"Cannot start the GPU or audio device. Install a graphics driver supporting OpenGL 3.3 and make sure an audio output is available. See session.log for details.":"Cannot initialize the game. See session.log in the game's local data folder.");return result;
}
