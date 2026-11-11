/* Windows presentation/audio adapter, using the validated software renderer.
 * No UI or audio device thread may touch guest RAM or PSP CPU state. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop_host.h"
#include "audio_mixer.h"
#include "host_input.h"
#include "desktop_gl.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#ifdef ASPHALT_WINDOWS_RELEASE
#include "display_session.h"
#include "race_controls.h"
#endif
#define SW 480
#define SH 272
#define AUDIO_FRAMES 512
#define AUDIO_BUFFERS 4
static struct {
    int enabled, initialized, gpu;
    volatile LONG quitting, closed, failed, focused, capture_active;
    HANDLE ui_thread,audio_thread,ui_ready,audio_event;
    HWND window;
    CRITICAL_SECTION video_lock,audio_lock;
    uint32_t pixels[SW*SH];
    asphalt_host_keys keys;
#ifdef ASPHALT_WINDOWS_RELEASE
    asphalt_windows_keymap keyboard;
#endif
    uint64_t last_tick,last_flip,presented,input_changes;
    uint32_t last_buttons;
    uint8_t last_ax,last_ay;
    HMODULE xinput;
    DWORD (WINAPI *get_state)(DWORD,XINPUT_STATE *);
    HWAVEOUT device;
    WAVEHDR headers[AUDIO_BUFFERS];
    int16_t buffers[AUDIO_BUFFERS][AUDIO_FRAMES*2];
    unsigned prepared;
    asphalt_audio_mixer mixer;
    uint64_t submitted,completed;
    FILE *capture;
    uint32_t capture_bytes,capture_start,capture_duration;
    uint64_t captured_nonzero;
} host;
#ifdef ASPHALT_WINDOWS_RELEASE
static WINDOWPLACEMENT windowed={sizeof(WINDOWPLACEMENT)};
static LONG_PTR windowed_style;
static int fullscreen,last_width,last_height;
static uint64_t title_at,title_frames;
static void toggle_fullscreen(HWND window){
 if(!fullscreen){MONITORINFO monitor={sizeof monitor};if(!GetWindowPlacement(window,&windowed)||!GetMonitorInfo(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor))return;windowed_style=GetWindowLongPtr(window,GWL_STYLE);SetWindowLongPtr(window,GWL_STYLE,windowed_style&~WS_OVERLAPPEDWINDOW);SetWindowPos(window,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_FRAMECHANGED);fullscreen=1;}
 else{SetWindowLongPtr(window,GWL_STYLE,windowed_style);SetWindowPlacement(window,&windowed);SetWindowPos(window,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);fullscreen=0;}
}
#endif
static void wav_header(FILE *f,uint32_t bytes) {
    /* Windows targets are little endian. PCM16 stereo, 44.1 kHz. */
    unsigned char h[44]={'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,2,0,0x44,0xac,0,0,0x10,0xb1,2,0,4,0,16,0,'d','a','t','a',0,0,0,0};
    uint32_t size=bytes+36;memcpy(h+4,&size,4);memcpy(h+40,&bytes,4);
    fseek(f,0,SEEK_SET);fwrite(h,1,sizeof h,f);
}
static void audio_sink(int ch,const int16_t *pcm,uint32_t frames,int stereo,int l,int r) {
    EnterCriticalSection(&host.audio_lock);
    asphalt_audio_push(&host.mixer,ch,pcm,frames,stereo,l,r);
    LeaveCriticalSection(&host.audio_lock);
}
static DWORD WINAPI audio_loop(void *unused) {
    (void)unused;
    while(!InterlockedCompareExchange(&host.quitting,0,0)) {
        for(unsigned i=0;i<AUDIO_BUFFERS;i++) {
            WAVEHDR *h=&host.headers[i];
            if(h->dwFlags&WHDR_INQUEUE)continue;
            if(h->dwFlags&WHDR_DONE)host.completed+=AUDIO_FRAMES;
            EnterCriticalSection(&host.audio_lock);
            asphalt_audio_mix(&host.mixer,host.buffers[i],AUDIO_FRAMES);
            LeaveCriticalSection(&host.audio_lock);
            if(host.capture && InterlockedCompareExchange(&host.capture_active,0,0)) {
                size_t n=fwrite(host.buffers[i],4,AUDIO_FRAMES,host.capture);
                host.capture_bytes+=(uint32_t)n*4;
                for(unsigned k=0;k<n;k++)if(host.buffers[i][k*2] || host.buffers[i][k*2+1])host.captured_nonzero++;
                if(n!=AUDIO_FRAMES)InterlockedExchange(&host.failed,1);
            }
            if(waveOutWrite(host.device,h,sizeof *h)!=MMSYSERR_NOERROR) {
                InterlockedExchange(&host.failed,1);return 1;
            }
            host.submitted+=AUDIO_FRAMES;
        }
        WaitForSingleObject(host.audio_event,50);
    }
    return 0;
}
static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
    case WM_SETFOCUS:InterlockedExchange(&host.focused,1);return 0;
    case WM_KILLFOCUS:
        InterlockedExchange(&host.focused,0);
        EnterCriticalSection(&host.video_lock);asphalt_key_clear(&host.keys);LeaveCriticalSection(&host.video_lock);return 0;
    case WM_KEYDOWN:case WM_KEYUP:
#ifdef ASPHALT_WINDOWS_RELEASE
        if(w==VK_F11&&message==WM_KEYDOWN&&!(l&(1L<<30))){toggle_fullscreen(window);return 0;}
#else
        fprintf(stderr,"desktop key: %llu virtual=%u down=%d\n",(unsigned long long)GetTickCount64(),(unsigned)w,message==WM_KEYDOWN);
#endif
        if(w==VK_ESCAPE && message==WM_KEYDOWN){PostMessage(window,WM_CLOSE,0,0);return 0;}
        if(w<256) {EnterCriticalSection(&host.video_lock);asphalt_key_event(&host.keys,(unsigned)w,message==WM_KEYDOWN,GetTickCount64());LeaveCriticalSection(&host.video_lock);}return 0;
    case WM_CLOSE:InterlockedExchange(&host.closed,1);return 0; /* CPU owner joins GPU before HWND destruction */
    case WM_APP+1:DestroyWindow(window);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;HDC dc=BeginPaint(window,&paint);RECT rc;GetClientRect(window,&rc);
        if(host.gpu){EndPaint(window,&paint);return 0;}
        FillRect(dc,&rc,(HBRUSH)GetStockObject(BLACK_BRUSH));
        int width=rc.right,height=width*SH/SW;if(height>rc.bottom){height=rc.bottom;width=height*SW/SH;}
        BITMAPINFO bi={0};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=SW;
        bi.bmiHeader.biHeight=-SH;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
        EnterCriticalSection(&host.video_lock);
        StretchDIBits(dc,(rc.right-width)/2,(rc.bottom-height)/2,width,height,0,0,SW,SH,host.pixels,&bi,DIB_RGB_COLORS,SRCCOPY);
        LeaveCriticalSection(&host.video_lock);EndPaint(window,&paint);return 0;
    }
    }
    return DefWindowProc(window,message,w,l);
}
static DWORD WINAPI window_loop(void *unused) {
    (void)unused;HINSTANCE instance=GetModuleHandle(NULL);
    WNDCLASS wc={0};wc.lpfnWndProc=window_proc;wc.hInstance=instance;wc.lpszClassName="AsphaltNativeProbe";
    if(host.gpu)wc.style=CS_OWNDC;
    wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClass(&wc)) {InterlockedExchange(&host.failed,1);SetEvent(host.ui_ready);return 1;}
    RECT rect={0,0,960,544};
#ifdef ASPHALT_WINDOWS_RELEASE
    rect.right=1280;rect.bottom=720;
#endif
    AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
    host.window=CreateWindow(wc.lpszClassName,host.gpu?"Asphalt Urban GT 2 GPU prototype | Space: accelerate/select | Arrows: steer | Enter: pause":"Asphalt Urban GT 2 CPU prototype | Space: accelerate/select | Arrows: steer | Enter: pause",
        WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,NULL,NULL,instance,NULL);
    if(!host.window){InterlockedExchange(&host.failed,1);SetEvent(host.ui_ready);return 1;}
#ifdef ASPHALT_WINDOWS_RELEASE
    SetWindowTextA(host.window,"Asphalt Urban GT 2");
    SendMessage(host.window,WM_SETICON,ICON_BIG,(LPARAM)LoadIcon(GetModuleHandle(NULL),MAKEINTRESOURCE(101)));
    SendMessage(host.window,WM_SETICON,ICON_SMALL,(LPARAM)LoadIcon(GetModuleHandle(NULL),MAKEINTRESOURCE(101)));
#endif
    if(host.quitting){DestroyWindow(host.window);SetEvent(host.ui_ready);return 0;}
    ShowWindow(host.window,SW_SHOW);UpdateWindow(host.window);SetEvent(host.ui_ready);
    MSG msg;while(GetMessage(&msg,NULL,0,0)>0){TranslateMessage(&msg);DispatchMessage(&msg);}
    return 0;
}
int asphalt_desktop_start(void) {
    const char *option=getenv("ASPHALT_DESKTOP");
    if(!option || strcmp(option,"1"))return 0;
    host.enabled=host.initialized=1;host.last_ax=host.last_ay=128;
    const char *renderer=getenv("ASPHALT_RENDERER");
    if(renderer && *renderer && strcmp(renderer,"software") && strcmp(renderer,"gl")){host.initialized=0;return -1;}
    host.gpu=renderer && !strcmp(renderer,"gl");
    InitializeCriticalSection(&host.video_lock);InitializeCriticalSection(&host.audio_lock);
    host.ui_ready=CreateEvent(NULL,TRUE,FALSE,NULL);host.audio_event=CreateEvent(NULL,FALSE,FALSE,NULL);
    if(!host.ui_ready || !host.audio_event)goto fail;
    WAVEFORMATEX fmt={WAVE_FORMAT_PCM,2,44100,44100*4,4,16,0};
    MMRESULT error=waveOutOpen(&host.device,WAVE_MAPPER,&fmt,(DWORD_PTR)host.audio_event,0,CALLBACK_EVENT);
    if(error!=MMSYSERR_NOERROR){fprintf(stderr,"waveOutOpen failed: %u\n",error);goto fail;}
    for(unsigned i=0;i<AUDIO_BUFFERS;i++) {
        host.headers[i].lpData=(LPSTR)host.buffers[i];host.headers[i].dwBufferLength=sizeof host.buffers[i];
        if(waveOutPrepareHeader(host.device,&host.headers[i],sizeof(WAVEHDR))!=MMSYSERR_NOERROR)goto fail;
        host.prepared++;
    }
    const char *capture=getenv("ASPHALT_AUDIO_CAPTURE");
    if(capture && *capture) {
        host.capture=fopen(capture,"wb");if(!host.capture)goto fail;
        wav_header(host.capture,0);host.capture_start=85000;host.capture_duration=15000;
    }
    host.xinput=LoadLibrary("xinput1_4.dll");if(!host.xinput)host.xinput=LoadLibrary("xinput9_1_0.dll");
    if(host.xinput)host.get_state=(void *)GetProcAddress(host.xinput,"XInputGetState");
    host.audio_thread=CreateThread(NULL,0,audio_loop,NULL,0,NULL);
    host.ui_thread=CreateThread(NULL,0,window_loop,NULL,0,NULL);
    if(!host.audio_thread || !host.ui_thread || WaitForSingleObject(host.ui_ready,3000)!=WAIT_OBJECT_0 || host.failed)goto fail;
#ifdef ASPHALT_WINDOWS_RELEASE
    if(host.gpu && asphalt_gl_start_scaled(host.window,1,asphalt_display_session_config().scale))goto fail;
#else
    if(host.gpu && asphalt_gl_start(host.window,1))goto fail;
#endif
    psp_audio_set_sink(audio_sink);
    fprintf(stderr,"desktop: %s window, keyboard/XInput, PCM16 stereo 44100 Hz\n",host.gpu?"GPU OpenGL/GLES backend":"software framebuffer");return 0;
fail:InterlockedExchange(&host.failed,1);asphalt_desktop_stop();return -1;
}
int asphalt_desktop_tick(uint64_t elapsed,uint32_t *buttons,uint8_t *ax,uint8_t *ay) {
    if(!host.enabled)return 0;
    if(host.gpu && psp_render_gl_error()){fprintf(stderr,"GPU failure: %s\n",psp_render_gl_error());InterlockedExchange(&host.failed,1);}
    if(host.failed)return -1;if(host.closed)return 1;
    int resample=0;
#ifdef ASPHALT_WINDOWS_RELEASE
    int racing=asphalt_race_controls_racing();
    resample=host.keyboard.initialized&&host.keyboard.racing!=racing;
    RECT area={0};GetClientRect(host.window,&area);
    if(area.right>0&&area.bottom>0&&(area.right!=last_width||area.bottom!=last_height)){
        if(asphalt_display_session_geometry(area.right,area.bottom,NULL))return -1;
        last_width=area.right;last_height=area.bottom;
    }
    if(elapsed-title_at>=1000){uint64_t frames=0;psp_display_frame_stats(&frames,NULL);char title[128];
        if(asphalt_display_session_config().show_fps)snprintf(title,sizeof title,"Asphalt Urban GT 2 | %.1f FPS",(double)(frames-title_frames)*1000/(elapsed-title_at));else snprintf(title,sizeof title,"Asphalt Urban GT 2");
        SetWindowTextA(host.window,title);title_at=elapsed;title_frames=frames;
    }
#endif
    if(elapsed-host.last_tick<8&&!resample) {
        *buttons|=host.last_buttons;if(host.last_ax!=128)*ax=host.last_ax;if(host.last_ay!=128)*ay=host.last_ay;
        return 0;
    }
    host.last_tick=elapsed;
    unsigned char k[256];EnterCriticalSection(&host.video_lock);asphalt_key_sample(&host.keys,k,GetTickCount64());LeaveCriticalSection(&host.video_lock);
#ifdef ASPHALT_WINDOWS_RELEASE
    uint32_t live=asphalt_windows_keyboard(&host.keyboard,k,racing);
#else
    uint32_t live=(k[VK_UP]?0x10:0)|(k[VK_RIGHT]?0x20:0)|(k[VK_DOWN]?0x40:0)|(k[VK_LEFT]?0x80:0)|
        (k['Q']?0x100:0)|(k['E']?0x200:0)|((k[VK_SPACE]||k['X'])?0x4000:0)|
        (k['Z']?0x8000:0)|(k['C']?0x2000:0)|(k['V']?0x1000:0)|(k[VK_RETURN]?8:0)|(k[VK_BACK]?1:0);
#endif
#ifdef ASPHALT_WINDOWS_RELEASE
    uint8_t lx=128,ly=128; /* Keyboard driving is digital; the pad retains analog steering. */
#else
    uint8_t lx=k['A']?0:k['D']?255:128,ly=k['W']?0:k['S']?255:128;
#endif
    XINPUT_STATE pad={0};
    if(host.focused && host.get_state && host.get_state(0,&pad)==ERROR_SUCCESS) {
        WORD b=pad.Gamepad.wButtons;
        if(b&XINPUT_GAMEPAD_DPAD_UP)live|=0x10;if(b&XINPUT_GAMEPAD_DPAD_RIGHT)live|=0x20;
        if(b&XINPUT_GAMEPAD_DPAD_DOWN)live|=0x40;if(b&XINPUT_GAMEPAD_DPAD_LEFT)live|=0x80;
        if(b&XINPUT_GAMEPAD_LEFT_SHOULDER)live|=0x100;if(b&XINPUT_GAMEPAD_RIGHT_SHOULDER)live|=0x200;
        if(b&XINPUT_GAMEPAD_A)live|=0x4000;if(b&XINPUT_GAMEPAD_B)live|=0x2000;
        if(b&XINPUT_GAMEPAD_X)live|=0x8000;if(b&XINPUT_GAMEPAD_Y)live|=0x1000;
        if(b&XINPUT_GAMEPAD_START)live|=8;if(b&XINPUT_GAMEPAD_BACK)live|=1;
        int x=pad.Gamepad.sThumbLX,y=pad.Gamepad.sThumbLY;
        if(abs(x)>XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)lx=(uint8_t)((x+32768)/256);
        if(abs(y)>XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE)ly=(uint8_t)(255-(y+32768)/256);
    }
    *buttons|=live;if(lx!=128)*ax=lx;if(ly!=128)*ay=ly;
    if(live!=host.last_buttons || lx!=host.last_ax || ly!=host.last_ay) {
        host.input_changes++;fprintf(stderr,"desktop input: %llu ms buttons=%04X analog=%u,%u\n",(unsigned long long)elapsed,live,lx,ly);
        host.last_buttons=live;host.last_ax=lx;host.last_ay=ly;
    }
    InterlockedExchange(&host.capture_active,elapsed>=host.capture_start && elapsed<host.capture_start+host.capture_duration);
    if(host.gpu)return 0; /* GPU presents directly; no guest framebuffer copies or GDI blits. */
    uint64_t flips;psp_display_frame_stats(&flips,NULL);
    if(flips==host.last_flip)return 0;host.last_flip=flips;
    uint32_t addr,stride,format;psp_display_get(&addr,&stride,&format);
    if(!addr || stride<SW || stride>4096 || format>3)return 0;
    unsigned bpp=format==3?4:2;
    const unsigned char *p=psp_mem_ptr(addr,((SH-1)*stride+SW)*bpp);if(!p)return 0;
    EnterCriticalSection(&host.video_lock);
    for(unsigned y=0;y<SH;y++)for(unsigned x=0;x<SW;x++) {
        uint32_t raw=0;memcpy(&raw,p+(y*stride+x)*bpp,bpp);uint32_t c=psp_color_expand(raw,(int)format);
        host.pixels[y*SW+x]=((c&255)<<16)|(c&0xff00)|((c>>16)&255);
    }
    LeaveCriticalSection(&host.video_lock);host.presented++;InvalidateRect(host.window,NULL,FALSE);return 0;
}
void asphalt_desktop_stop(void) {
    if(!host.initialized)return;
    psp_audio_set_sink(NULL);InterlockedExchange(&host.quitting,1);
    if(host.gpu)asphalt_gl_stop();
    if(host.audio_event)SetEvent(host.audio_event);
    if(host.audio_thread){WaitForSingleObject(host.audio_thread,INFINITE);CloseHandle(host.audio_thread);host.audio_thread=NULL;}
    if(host.device) {
        waveOutReset(host.device);
        for(unsigned i=0;i<host.prepared;i++)waveOutUnprepareHeader(host.device,&host.headers[i],sizeof(WAVEHDR));
        waveOutClose(host.device);host.device=NULL;
    }
    if(host.window && IsWindow(host.window))PostMessage(host.window,WM_APP+1,0,0);
    if(host.ui_thread){WaitForSingleObject(host.ui_thread,INFINITE);CloseHandle(host.ui_thread);host.ui_thread=NULL;}
    if(host.capture){wav_header(host.capture,host.capture_bytes);fclose(host.capture);host.capture=NULL;}
    if(host.xinput)FreeLibrary(host.xinput);
    if(host.ui_ready)CloseHandle(host.ui_ready);if(host.audio_event)CloseHandle(host.audio_event);
    DeleteCriticalSection(&host.video_lock);DeleteCriticalSection(&host.audio_lock);host.initialized=0;
}
void asphalt_desktop_report(FILE *out) {
    uint64_t presented=host.presented,draws=0,batches=0,bytes=0,readbacks=0;
    if(host.gpu)psp_render_gl_stats(&draws,&batches,&bytes,&presented,&readbacks);
    fprintf(out,"{\"enabled\":%s,\"device_failed\":%s,\"presented_frames\":%llu,\"input_changes\":%llu,"
        "\"audio_submitted_frames\":%llu,\"audio_completed_frames\":%llu,\"audio_input_frames\":%llu,"
        "\"audio_nonzero_frames\":%llu,\"audio_peak\":%u,\"audio_channel_mask\":%u,\"audio_dropped_frames\":%llu,"
        "\"capture_bytes\":%u,\"capture_nonzero_frames\":%llu,\"gpu\":",host.enabled?"true":"false",host.failed?"true":"false",
        (unsigned long long)presented,(unsigned long long)host.input_changes,(unsigned long long)host.submitted,
        (unsigned long long)host.completed,(unsigned long long)host.mixer.input_frames,(unsigned long long)host.mixer.nonzero_frames,
        host.mixer.peak,host.mixer.channel_mask,(unsigned long long)host.mixer.dropped_frames,
        host.capture_bytes,(unsigned long long)host.captured_nonzero);
    if(host.gpu)asphalt_gl_report(out);else fputs("null",out);
    fprintf(out,",\"gpu_draws\":%llu,\"gpu_batches\":%llu,\"gpu_upload_bytes\":%llu,\"gpu_readbacks\":%llu}",
        (unsigned long long)draws,(unsigned long long)batches,(unsigned long long)bytes,(unsigned long long)readbacks);
}
