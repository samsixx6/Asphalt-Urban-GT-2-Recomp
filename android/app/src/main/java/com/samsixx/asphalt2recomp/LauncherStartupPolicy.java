package com.samsixx.asphalt2recomp;

/** One automatic launch per launcher lifetime, including configuration recreation. */
final class LauncherStartupPolicy {
    private boolean consumed;
    LauncherStartupPolicy(boolean consumed) { this.consumed=consumed; }
    boolean ready(boolean iso, boolean folder) {
        if (consumed || !iso || !folder) return false;
        consumed=true; return true;
    }
    void returnedFromGame() { consumed=true; }
    boolean consumed() { return consumed; }
}
