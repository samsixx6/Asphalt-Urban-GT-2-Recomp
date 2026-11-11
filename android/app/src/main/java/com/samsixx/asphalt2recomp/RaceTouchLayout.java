package com.samsixx.asphalt2recomp;

import java.util.ArrayList;

/** Android-free button geometry. Insets move icons, never the full-screen steering midpoint. */
final class RaceTouchLayout {
    enum Icon {
        NOS(0,0,328,328), BRAKE(0,0,328,328), PAUSE(10,15,92,108), CAMERA(10,10,92,92);
        // Pause/Camera source rectangles remove only supplied transparent padding.
        final int sourceLeft,sourceTop,sourceRight,sourceBottom;
        Icon(int left,int top,int right,int bottom) {
            sourceLeft=left; sourceTop=top; sourceRight=right; sourceBottom=bottom;
        }
    }
    // Verified original NOS action (device button index 5): PSP Circle, not L.
    static final int NOS_MASK = 0x2000;
    static final int BRAKE_MASK = 0x8000, PAUSE_MASK = 0x0008, CAMERA_MASK = 0x0001;
    static final int LEFT_MASK = 0x0080, RIGHT_MASK = 0x0020;

    static final class Button {
        final float left, top, right, bottom;
        final int mask;
        final Icon icon;
        final String label;
        final boolean round;

        Button(float left, float top, float right, float bottom, int mask,
               Icon icon, String label, boolean round) {
            this.left=left; this.top=top; this.right=right; this.bottom=bottom;
            this.mask=mask; this.icon=icon; this.label=label; this.round=round;
        }

        float centerX() { return (left+right)/2; }
        float centerY() { return (top+bottom)/2; }
        boolean contains(float x, float y, boolean circular) {
            if (!(x >= left && x < right && y >= top && y < bottom)) return false;
            if (!circular || !round) return true;
            float dx=(x-centerX())/((right-left)/2), dy=(y-centerY())/((bottom-top)/2);
            return dx*dx+dy*dy <= 1;
        }
    }

    final Button[] buttons;
    final float width, height, scale, fpsLeft, fpsTop, fpsWidth;
    final boolean race, usable;
    private final int uiLayout;
    private final boolean pspAspect;
    private final float insetLeft, insetTop, insetRight, insetBottom, density;

    private RaceTouchLayout(float width, float height, float left, float top, float right,
                            float bottom, float density, boolean race, int uiLayout, boolean pspAspect) {
        this.width=width; this.height=height; this.race=race;
        this.uiLayout=race && uiLayout==1 ? 1 : 0;
        this.pspAspect=race && pspAspect;
        insetLeft=left; insetTop=top; insetRight=right; insetBottom=bottom; this.density=density;
        float safeWidth=width-left-right, safeHeight=height-top-bottom;
        usable=width>0 && height>0 && safeWidth>0 && safeHeight>0;
        scale=usable ? (race ? Math.min(safeWidth/1284f,safeHeight/597f)
                            : Math.min(density,safeHeight/330f)) : 0;
        // Keep the original full-screen center unless a cutout would clip the
        // pair or its adjacent FPS badge. The complete row fits inside the insets.
        float pairLeft=Math.max(left,Math.min((width-121*scale)/2,width-right-237*scale));
        float pairTop=top+10*scale;
        boolean newLayout=this.uiLayout==1;
        float hudWidth=this.pspAspect ? 480*Math.min(width/480f,height/272f) : width;
        float hudLeft=(width-hudWidth)/2;
        float fpsLimit=Math.min(width-right,hudLeft+.875f*hudWidth);
        fpsLeft=race ? (newLayout ? Math.max(left,Math.min(hudLeft+.79f*hudWidth,fpsLimit))
                : pairLeft+129*scale) : left+4*scale;
        fpsTop=race ? (newLayout ? top+(18+40*93f/82f+8)*scale : pairTop) : top+scale;
        fpsWidth=usable ? (race ? (newLayout ? Math.max(0,Math.min(108*scale,fpsLimit-fpsLeft)) : 108*scale)
                : Math.max(0,safeWidth-4*scale)) : 0;
        ArrayList<Button> result=new ArrayList<>();
        if (usable) {
            float r=width-right, b=height-bottom, s=scale;
            if (race) {
                // Latest New reference: small Camera then Pause at safe top-right.
                // Preserve the source aspect ratios and each icon's action/index.
                if(newLayout) {
                    float newTop=top+18*s;
                    result.add(new Button(r-55*s,newTop,r-15*s,newTop+40*93f/82f*s,
                            PAUSE_MASK,Icon.PAUSE,"PAUSE",false));
                    result.add(new Button(r-107*s,newTop,r-67*s,newTop+40*s,
                            CAMERA_MASK,Icon.CAMERA,"CAMERA",false));
                } else {
                    result.add(new Button(pairLeft,pairTop,pairLeft+55*s,pairTop+55*93f/82f*s,
                            PAUSE_MASK,Icon.PAUSE,"PAUSE",false));
                    result.add(new Button(pairLeft+66*s,pairTop,pairLeft+121*s,pairTop+55*s,
                            CAMERA_MASK,Icon.CAMERA,"CAMERA",false));
                }
                // Follow the user's marked thumb positions within the safe
                // rectangle; all four circles share the enlarged diameter.
                float diameter=140*s, nosY=top+.635f*safeHeight, brakeY=top+.855f*safeHeight;
                add(result,left+.20f*safeWidth,nosY,diameter,diameter,NOS_MASK,Icon.NOS,"NOS",true);
                add(result,r-.20f*safeWidth,nosY,diameter,diameter,NOS_MASK,Icon.NOS,"NOS",true);
                add(result,left+.135f*safeWidth,brakeY,diameter,diameter,BRAKE_MASK,Icon.BRAKE,"BRAKE",true);
                add(result,r-.135f*safeWidth,brakeY,diameter,diameter,BRAKE_MASK,Icon.BRAKE,"BRAKE",true);
            } else {
                // Match the outer D-pad/Circle gaps even with an asymmetric cutout.
                float dx=right+104*s, dy=b-104*s;
                add(result,dx,dy-51*s,50*s,50*s,0x0010,null,"▲",false);
                add(result,dx+51*s,dy,50*s,50*s,RIGHT_MASK,null,"▶",false);
                add(result,dx,dy+51*s,50*s,50*s,0x0040,null,"▼",false);
                add(result,dx-51*s,dy,50*s,50*s,LEFT_MASK,null,"◀",false);
                float fx=r-108*s, fy=b-108*s;
                add(result,fx,fy+53*s,54*s,54*s,0x4000,null,"×",true);
                add(result,fx+53*s,fy,54*s,54*s,0x2000,null,"○",true);
                add(result,fx-53*s,fy,54*s,54*s,0x8000,null,"□",true);
                add(result,fx,fy-53*s,54*s,54*s,0x1000,null,"△",true);
                float middle=(left+r)/2;
                add(result,middle-58*s,b-21*s,90*s,34*s,CAMERA_MASK,null,"SELECT",false);
                add(result,middle+58*s,b-21*s,90*s,34*s,PAUSE_MASK,null,"START",false);
            }
        }
        buttons=result.toArray(new Button[0]);
    }

    static RaceTouchLayout race(float width,float height,float left,float top,float right,float bottom) {
        return race(width,height,left,top,right,bottom,0);
    }
    static RaceTouchLayout race(float width,float height,float left,float top,float right,float bottom,int layout) {
        return race(width,height,left,top,right,bottom,layout,false);
    }
    static RaceTouchLayout race(float width,float height,float left,float top,float right,float bottom,int layout,boolean pspAspect) {
        return new RaceTouchLayout(width,height,left,top,right,bottom,1,true,layout,pspAspect);
    }
    static RaceTouchLayout psp(float width,float height,float left,float top,float right,float bottom,float density) {
        return new RaceTouchLayout(width,height,left,top,right,bottom,density,false,0,false);
    }
    private static void add(ArrayList<Button> result,float x,float y,float width,float height,
                            int mask,Icon icon,String label,boolean round) {
        result.add(new Button(x-width/2,y-height/2,x+width/2,y+height/2,mask,icon,label,round));
    }
    boolean sameGeometry(RaceTouchLayout other) {
        return width==other.width && height==other.height && race==other.race && uiLayout==other.uiLayout
                && pspAspect==other.pspAspect && density==other.density
                && insetLeft==other.insetLeft && insetTop==other.insetTop
                && insetRight==other.insetRight && insetBottom==other.insetBottom;
    }
    boolean contains(float x,float y) {
        return usable && x>=0 && x<width && y>=0 && y<height;
    }
    int hitIcon(float x,float y) {
        for (int i=0;i<buttons.length;i++) if (buttons[i].contains(x,y,true)) return i;
        return -1;
    }
}
