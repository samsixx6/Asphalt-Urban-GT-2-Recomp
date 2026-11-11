#include "../src/hle/host_thread.h"
#include <assert.h>
static HOST_TLS int value;
static void *worker(void *arg) {
    (void)arg;
    assert(value == 0); /* a new host thread must not see the caller's TLS */
    value = 99;
    return NULL;
}
int main(void) {
    value = 7;
    host_thread thread;
    assert(host_thread_create(&thread, 1024*1024, worker, NULL) == 0);
    host_thread_join(thread);
    assert(value == 7);
    puts("host thread-local isolation checks passed");return 0;
}
