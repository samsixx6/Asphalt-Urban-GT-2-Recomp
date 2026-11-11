package com.samsixx.asphalt2recomp;

import android.util.SparseBooleanArray;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/** Merges independent touch, key and joystick state; releasing one source preserves the others. */
final class PspInput {
    static final int SELECT = 0x0001, START = 0x0008;
    static final int UP = 0x0010, RIGHT = 0x0020, DOWN = 0x0040, LEFT = 0x0080;
    static final int L = 0x0100, R = 0x0200;
    static final int TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000;

    interface Sink { void update(int buttons, float x, float y); }

    private final Sink sink;
    private final SparseBooleanArray keys = new SparseBooleanArray();
    private int touch;
    private int joystickButtons;
    private float x, y;
    private boolean enabled;

    PspInput(Sink sink) { this.sink = sink; }

    void setEnabled(boolean value) {
        if (!value) release();
        enabled = value;
        if (enabled) send();
    }

    void touch(int buttons) {
        touch = buttons;
        send();
    }

    boolean key(KeyEvent event, boolean down) {
        int mask = keyMask(event.getKeyCode());
        if (mask == 0) return false;
        if (down) keys.put(event.getKeyCode(), true);
        else keys.delete(event.getKeyCode());
        send();
        return true;
    }

    boolean motion(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
                || event.getActionMasked() != MotionEvent.ACTION_MOVE) return false;
        x = axis(event, MotionEvent.AXIS_X);
        y = axis(event, MotionEvent.AXIS_Y);
        joystickButtons = 0;
        float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X);
        float hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        if (hatX < -0.5f) joystickButtons |= LEFT;
        if (hatX > 0.5f) joystickButtons |= RIGHT;
        if (hatY < -0.5f) joystickButtons |= UP;
        if (hatY > 0.5f) joystickButtons |= DOWN;
        if (Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE)) > 0.5f) joystickButtons |= L;
        if (Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS)) > 0.5f) joystickButtons |= R;
        send();
        return true;
    }

    void release() {
        touch = joystickButtons = 0;
        x = y = 0;
        keys.clear();
        send();
    }

    private void send() {
        if (!enabled) return;
        int buttons = touch | joystickButtons;
        for (int i = 0; i < keys.size(); i++) buttons |= keyMask(keys.keyAt(i));
        sink.update(buttons, x, y);
    }

    private static float axis(MotionEvent event, int axis) {
        float value = event.getAxisValue(axis);
        InputDevice device = event.getDevice();
        InputDevice.MotionRange range = device == null ? null : device.getMotionRange(axis, event.getSource());
        float deadzone = range == null ? 0.15f : Math.max(0.15f, range.getFlat());
        if (Math.abs(value) <= deadzone) return 0;
        return Math.max(-1, Math.min(1, value));
    }

    static int keyMask(int key) {
        switch (key) {
            case KeyEvent.KEYCODE_DPAD_UP: return UP;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return RIGHT;
            case KeyEvent.KEYCODE_DPAD_DOWN: return DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return LEFT;
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_DPAD_CENTER:
            case KeyEvent.KEYCODE_SPACE:
            case KeyEvent.KEYCODE_W:
            case KeyEvent.KEYCODE_X: return CROSS;
            case KeyEvent.KEYCODE_BUTTON_B:
            case KeyEvent.KEYCODE_C: return CIRCLE;
            case KeyEvent.KEYCODE_BUTTON_X:
            case KeyEvent.KEYCODE_Z: return SQUARE;
            case KeyEvent.KEYCODE_BUTTON_Y:
            case KeyEvent.KEYCODE_V: return TRIANGLE;
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_L2:
            case KeyEvent.KEYCODE_Q: return L;
            case KeyEvent.KEYCODE_BUTTON_R1:
            case KeyEvent.KEYCODE_BUTTON_R2:
            case KeyEvent.KEYCODE_E: return R;
            case KeyEvent.KEYCODE_BUTTON_START:
            case KeyEvent.KEYCODE_ENTER: return START;
            case KeyEvent.KEYCODE_BUTTON_SELECT:
            case KeyEvent.KEYCODE_TAB: return SELECT;
            default: return 0;
        }
    }
}
