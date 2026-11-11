/* Renderer-owned host wait; never used by guest/RTC timing. */
#ifndef PSPRECOMP_HOST_PACING_H
#define PSPRECOMP_HOST_PACING_H
#include <stdint.h>
#include <stdlib.h>
#if defined(_WIN32) && !defined(PSPRECOMP_PACING_TEST_POSIX)
#include "hle/host_thread.h"
typedef struct {host_cond condition;} host_pacing;
static int host_pacing_init(host_pacing *p){host_cond_init(&p->condition);return 0;}
static void host_pacing_destroy(host_pacing *p){host_cond_destroy(&p->condition);}
static void host_pacing_wake(host_pacing *p){host_cond_broadcast(&p->condition);}
static uint64_t host_pacing_now(void){return host_deadline_us();}
static int host_pacing_until(host_pacing *p,void *mutex,uint64_t deadline){return host_cond_until(&p->condition,(host_mutex *)mutex,deadline);}
#else
#ifndef PSPRECOMP_PACING_TEST_POSIX
#include <pthread.h>
#include <errno.h>
#include <time.h>
#endif
typedef struct {pthread_cond_t condition;} host_pacing;
static int host_pacing_init(host_pacing *p){
 pthread_condattr_t a;if(pthread_condattr_init(&a))return -1;
 int rc=pthread_condattr_setclock(&a,CLOCK_MONOTONIC);
 if(!rc)rc=pthread_cond_init(&p->condition,&a);
 if(pthread_condattr_destroy(&a))abort();
 return rc? -1:0;
}
static void host_pacing_destroy(host_pacing *p){if(pthread_cond_destroy(&p->condition))abort();}
static void host_pacing_wake(host_pacing *p){if(pthread_cond_broadcast(&p->condition))abort();}
static uint64_t host_pacing_now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))abort();return (uint64_t)t.tv_sec*1000000ull+(uint64_t)t.tv_nsec/1000;}
static int host_pacing_until(host_pacing *p,void *mutex,uint64_t deadline){
 struct timespec t;t.tv_sec=(time_t)(deadline/1000000);t.tv_nsec=(long)(deadline%1000000)*1000;
 int rc=pthread_cond_timedwait(&p->condition,(pthread_mutex_t *)mutex,&t);if(rc&&rc!=ETIMEDOUT)abort();return rc==ETIMEDOUT;
}
#endif
#endif
