#ifndef ASPHALT_ANDROID_LIFECYCLE_H
#define ASPHALT_ANDROID_LIFECYCLE_H
/* Host-only state, serialized by android_host's state_lock. No guest reset on
 * pause or window generation changes; stopping is irreversible. */
typedef struct { int resumed, have_window, stopping; unsigned generation; } asphalt_android_lifecycle;
static inline int asphalt_android_lifecycle_running(const asphalt_android_lifecycle *s) {
    return s->resumed && s->have_window && !s->stopping;
}
static inline void asphalt_android_lifecycle_resume(asphalt_android_lifecycle *s,int resumed) { s->resumed=!!resumed; }
static inline void asphalt_android_lifecycle_window(asphalt_android_lifecycle *s,int present) { s->have_window=!!present; ++s->generation; }
static inline void asphalt_android_lifecycle_stop(asphalt_android_lifecycle *s) { s->stopping=1; }
#endif
