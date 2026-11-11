package com.samsixx.asphalt2recomp;

/** UI-owner-only permission handshake; a denied request has no launch action. */
final class StorageLaunchGate {
    enum Action { NONE, REQUEST, START }
    private boolean waiting;
    private boolean settings;
    StorageLaunchGate(boolean waiting) { this(waiting,false); }
    StorageLaunchGate(boolean waiting,boolean settings) { this.waiting=waiting;this.settings=settings; }
    boolean waiting() { return waiting; }
    boolean settings() { return settings; }
    static boolean settingsRequest(boolean modern,boolean askedBefore,boolean rationale) { return modern||(askedBefore&&!rationale); }
    Action play(boolean allowed) { return play(allowed,false); }
    Action play(boolean allowed,boolean settings) {
        if(waiting)return Action.NONE;
        if(allowed)return Action.START;
        waiting=true;this.settings=settings;return Action.REQUEST;
    }
    Action resumed(boolean allowed) { return settings?result(allowed):Action.NONE; }
    Action result(boolean allowed) {
        if(!waiting)return Action.NONE;
        waiting=false;return allowed?Action.START:Action.NONE;
    }
    void cancel() { waiting=false; }
}
