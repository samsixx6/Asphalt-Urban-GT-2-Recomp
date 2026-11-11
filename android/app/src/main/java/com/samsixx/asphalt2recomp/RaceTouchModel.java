package com.samsixx.asphalt2recomp;

import java.util.HashMap;

/** Pointer IDs own race icons or steering until UP. Releases gate surviving old IDs. */
final class RaceTouchModel {
    private static final int PSP=-2;
    private static final class Pointer {
        final int owner;
        float x,y;
        boolean blocked;
        Pointer(int owner,float x,float y,boolean blocked) {
            this.owner=owner; this.x=x; this.y=y; this.blocked=blocked;
        }
    }

    private RaceTouchLayout layout;
    private final HashMap<Integer,Pointer> pointers=new HashMap<>();
    private boolean pauseRequested;
    private boolean releaseGate;
    private boolean controlsVisible=true;

    RaceTouchModel(RaceTouchLayout layout) { this.layout=layout; }
    void setLayout(RaceTouchLayout value) {
        if (layout.sameGeometry(value)) return;
        if (layout.race==value.race) release();
        else blockPointers();
        layout=value;
        pauseRequested=false;
    }
    RaceTouchLayout layout() { return layout; }
    void setControlsVisible(boolean value) {
        if(controlsVisible==value)return;
        controlsVisible=value;
        release();
    }

    int buttons() {
        if(!controlsVisible)return 0;
        // A short Pause tap must reach the CPU, and Circle must remain quarantined
        // until native state confirms the menu. Only acknowledgement/release clears it.
        if (pauseRequested) return RaceTouchLayout.PAUSE_MASK;
        int result=0;
        for (Pointer p:pointers.values()) {
            if (p.blocked) continue;
            if (layout.race) {
                if (p.owner>=0) result |= layout.buttons[p.owner].mask;
                else if (layout.contains(p.x,p.y)) result |= p.x<layout.width/2
                        ? RaceTouchLayout.LEFT_MASK : RaceTouchLayout.RIGHT_MASK;
            } else {
                for (RaceTouchLayout.Button b:layout.buttons)
                    if (b.contains(p.x,p.y,false)) result |= b.mask;
            }
        }
        int steering=RaceTouchLayout.LEFT_MASK | RaceTouchLayout.RIGHT_MASK;
        if (layout.race && (result & steering)==steering) result &= ~steering;
        return result;
    }

    boolean isButtonPressed(int index) {
        if (!controlsVisible || index<0 || index>=layout.buttons.length) return false;
        for (Pointer p:pointers.values()) {
            if (!p.blocked && (layout.race ? p.owner==index
                    : layout.buttons[index].contains(p.x,p.y,false))) return true;
        }
        return false;
    }

    void pointerDown(int id,float x,float y) {
        if (pointers.containsKey(id)) return;
        int owner=layout.race ? layout.hitIcon(x,y) : PSP;
        boolean blocked=!controlsVisible || pauseRequested || releaseGate || !layout.contains(x,y);
        if (!blocked && layout.race && owner>=0
                && layout.buttons[owner].icon==RaceTouchLayout.Icon.PAUSE) {
            // Circle is NOS in races and Back in menus: never leak it beside Start.
            for (Pointer old:pointers.values()) old.blocked=true;
            pauseRequested=true;
        }
        pointers.put(id,new Pointer(owner,x,y,blocked));
    }
    void pointerMove(int id,float x,float y) {
        Pointer p=pointers.get(id);
        if (p!=null) { p.x=x; p.y=y; }
    }
    void pointerUp(int id) {
        pointers.remove(id);
        if (pointers.isEmpty()) releaseGate=false;
    }

    /** Android ACTION_DOWN starts a new stream after CANCEL/focus loss without requiring stale UPs. */
    void beginGesture(int id,float x,float y) {
        pointers.clear();
        releaseGate=false;
        pointerDown(id,x,y);
    }
    private void blockPointers() { for (Pointer p:pointers.values()) p.blocked=true; }
    void release() {
        blockPointers();
        pauseRequested=false;
        releaseGate=!pointers.isEmpty();
    }
    void cancel() { release(); }
}
