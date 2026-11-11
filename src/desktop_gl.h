#ifndef ASPHALT_DESKTOP_GL_H
#define ASPHALT_DESKTOP_GL_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
int asphalt_gl_start(HWND window,int vsync);
int asphalt_gl_start_scaled(HWND window,int vsync,int scale);
void asphalt_gl_stop(void);
void asphalt_gl_report(FILE *out);
#ifdef ASPHALT_GPU_TESTS
void asphalt_gl_test_set_swap_hook(void (*hook)(void));
#endif
#endif
