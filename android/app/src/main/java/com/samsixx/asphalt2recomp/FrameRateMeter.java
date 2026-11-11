package com.samsixx.asphalt2recomp;

/** Measures completed game presentations, independent of display refresh and UI redraws. */
final class FrameRateMeter {
    private long windowFrames, windowNanos, lastFrames, lastNanos;
    private boolean initialized;
    private double rate = Double.NaN;

    void reset() {
        initialized = false;
        rate = Double.NaN;
    }

    double sample(long frames, long nowNanos) {
        if (!initialized || frames < lastFrames || nowNanos < lastNanos) {
            initialized = true;
            windowFrames = frames;
            windowNanos = nowNanos;
            rate = Double.NaN;
        }
        lastFrames = frames;
        lastNanos = nowNanos;
        long elapsed = nowNanos - windowNanos;
        if (elapsed >= 1_000_000_000L) {
            rate = (frames - windowFrames) * 1_000_000_000.0 / elapsed;
            windowFrames = frames;
            windowNanos = nowNanos;
        }
        return rate;
    }
}
