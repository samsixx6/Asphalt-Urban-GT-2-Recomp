package com.samsixx.asphalt2recomp;

/** Converts window-safe edges into the actual drawable's coordinate system. */
final class SurfaceInsets {
    static int[] relative(int windowWidth, int windowHeight, int x, int y, int width, int height,
                          int left, int top, int right, int bottom) {
        return new int[]{Math.max(0,left-x), Math.max(0,top-y),
                Math.max(0,x+width-(windowWidth-right)),
                Math.max(0,y+height-(windowHeight-bottom))};
    }
    private SurfaceInsets() { }
}
