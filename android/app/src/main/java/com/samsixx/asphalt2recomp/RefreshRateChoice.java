package com.samsixx.asphalt2recomp;

/** Prefer a supported high-refresh mode without changing the display resolution. */
final class RefreshRateChoice {
    static int choose(int width, int height, int[] widths, int[] heights, float[] rates) {
        if (widths.length != heights.length || widths.length != rates.length) return -1;
        int choice = -1;
        float best = 0;
        for (int i = 0; i < rates.length; i++) {
            float rate = rates[i];
            if (widths[i] == width && heights[i] == height && rate > best && rate <= 120.05f) {
                choice = i;
                best = rate;
            }
        }
        return choice;
    }
}
