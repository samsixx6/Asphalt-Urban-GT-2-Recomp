package com.samsixx.asphalt2recomp;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;

/** Canvas presentation and Android event adapter for the independently tested pointer model. */
final class ControlsView extends View {
    interface Listener { void buttonsChanged(int mask); }
    private final Listener listener;
    private final RaceTouchModel touches;
    private final Bitmap[] icons = new Bitmap[RaceTouchLayout.Icon.values().length];
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF bounds = new RectF();
    private final Rect sourceBounds = new Rect();
    private int pressed;
    private int insetLeft, insetRight, insetTop, insetBottom;
    private boolean suppliedInsets;
    private boolean raceControls;
    private int uiLayout;
    private boolean pspAspect;
    private boolean buttonsVisible=true;
    private boolean frameRateVisible;
    private String frameRate = "— FPS";

    ControlsView(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        touches = new RaceTouchModel(RaceTouchLayout.psp(0, 0, 0, 0, 0, 0,
                getResources().getDisplayMetrics().density));
        loadIcon(context, RaceTouchLayout.Icon.NOS, "NOS.png");
        loadIcon(context, RaceTouchLayout.Icon.BRAKE, "Brake.png");
        loadIcon(context, RaceTouchLayout.Icon.PAUSE, "Pause.png");
        loadIcon(context, RaceTouchLayout.Icon.CAMERA, "Camera.png");
        paint.setFilterBitmap(true);
        setFocusable(false);
        describeControls();
    }

    private void loadIcon(Context context, RaceTouchLayout.Icon icon, String name) {
        try (InputStream stream = context.getAssets().open("controls/" + name)) {
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inScaled = false;
            icons[icon.ordinal()] = BitmapFactory.decodeStream(stream, null, options);
            if (icons[icon.ordinal()] == null) Log.w("AsphaltControls", "Cannot decode " + name);
        } catch (IOException error) {
            Log.w("AsphaltControls", "Cannot load " + name, error);
        }
    }

    void setRaceControls(boolean value) {
        if (raceControls == value) return;
        raceControls = value;
        layoutButtons(getWidth(), getHeight());
        describeControls();
    }
    void setUILayout(int value) {
        setUILayout(value,false);
    }
    void setUILayout(int value,boolean pspAspect) {
        int layout=value==1 ? 1 : 0;
        if(uiLayout==layout && this.pspAspect==pspAspect)return;
        uiLayout=layout;
        this.pspAspect=pspAspect;
        // A menu poll must not reset held PSP buttons. Race geometry changes
        // release and quarantine the current stream in RaceTouchModel.setLayout.
        if(raceControls)layoutButtons(getWidth(),getHeight());
    }
    void setButtonsVisible(boolean value) {
        if(buttonsVisible==value)return;
        buttonsVisible=value;
        touches.setControlsVisible(value);
        setPressedButtons(touches.buttons());
        describeControls();
        invalidate();
    }
    void setFrameRateVisible(boolean value) {
        if(frameRateVisible==value)return;
        frameRateVisible=value;
        invalidate();
    }
    void setSafeInsets(int left,int top,int right,int bottom) {
        suppliedInsets=true;
        left=Math.max(0,left);top=Math.max(0,top);right=Math.max(0,right);bottom=Math.max(0,bottom);
        if(insetLeft==left&&insetTop==top&&insetRight==right&&insetBottom==bottom)return;
        insetLeft=left;insetTop=top;insetRight=right;insetBottom=bottom;
        layoutButtons(getWidth(),getHeight());
    }

    private void describeControls() {
        if(!buttonsVisible){setContentDescription("Game cinematic");return;}
        setContentDescription(raceControls
                ? "Race controls: left half steer left, right half steer right, NOS, Brake, Pause and Camera"
                : "Game controls: direction pad, Cross accelerate, Square, Circle, Triangle, Select and Start");
    }

    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        if(suppliedInsets)return insets;
        insetLeft = insets.getSystemWindowInsetLeft();
        insetRight = insets.getSystemWindowInsetRight();
        insetTop = insets.getSystemWindowInsetTop();
        insetBottom = insets.getSystemWindowInsetBottom();
        if (android.os.Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
            android.view.DisplayCutout cutout = insets.getDisplayCutout();
            insetLeft = Math.max(insetLeft, cutout.getSafeInsetLeft());
            insetRight = Math.max(insetRight, cutout.getSafeInsetRight());
            insetTop = Math.max(insetTop, cutout.getSafeInsetTop());
            insetBottom = Math.max(insetBottom, cutout.getSafeInsetBottom());
        }
        layoutButtons(getWidth(), getHeight());
        return insets;
    }

    @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        layoutButtons(width, height);
    }

    private void layoutButtons(int width, int height) {
        float density = getResources().getDisplayMetrics().density;
        touches.setLayout(raceControls
                ? RaceTouchLayout.race(width, height, insetLeft, insetTop, insetRight, insetBottom, uiLayout, pspAspect)
                : RaceTouchLayout.psp(width, height, insetLeft, insetTop, insetRight, insetBottom, density));
        setPressedButtons(touches.buttons());
        invalidate();
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        paint.setTextAlign(Paint.Align.CENTER);
        RaceTouchLayout layout = touches.layout();
        for (int index = 0; buttonsVisible && index < layout.buttons.length; index++) {
            RaceTouchLayout.Button button = layout.buttons[index];
            bounds.set(button.left, button.top, button.right, button.bottom);
            if (button.icon != null) {
                drawIcon(canvas, button, touches.isButtonPressed(index));
                continue;
            }
            paint.setStyle(Paint.Style.FILL);
            paint.setColor((pressed & button.mask) != 0 ? Color.argb(175, 44, 133, 230) : Color.argb(85, 12, 18, 28));
            shape(canvas, button);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Math.max(1, getResources().getDisplayMetrics().density));
            paint.setColor(Color.argb(165, 235, 241, 255));
            shape(canvas, button);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.argb(225, 245, 247, 255));
            paint.setTextSize(bounds.height() * (button.label.length() > 1 ? 0.38f : 0.57f));
            Paint.FontMetrics metrics = paint.getFontMetrics();
            canvas.drawText(button.label, bounds.centerX(), bounds.centerY() - (metrics.ascent + metrics.descent) / 2, paint);
        }
        if(frameRateVisible)drawFrameRate(canvas);
    }

    private void drawIcon(Canvas canvas, RaceTouchLayout.Button button, boolean held) {
        paint.setStyle(Paint.Style.FILL);
        if (held) {
            paint.setColor(Color.argb(50, 44, 133, 230));
            shape(canvas, button);
        }
        Bitmap icon = icons[button.icon.ordinal()];
        if (icon != null) {
            // Half-opacity overlay, multiplied by the supplied PNG transparency.
            paint.setAlpha(128);
            sourceBounds.set(button.icon.sourceLeft, button.icon.sourceTop,
                    button.icon.sourceRight, button.icon.sourceBottom);
            canvas.drawBitmap(icon, sourceBounds, bounds, paint);
        } else {
            paint.setColor(Color.argb(113, 245, 247, 255));
            paint.setTextSize(bounds.height() * 0.25f);
            canvas.drawText(button.label, bounds.centerX(), bounds.centerY(), paint);
        }
    }

    void setFrameRate(double value) {
        String text = Double.isNaN(value) ? "— FPS" : String.format(Locale.US, "%.1f FPS", value);
        if (!text.equals(frameRate)) {
            frameRate = text;
            invalidate();
        }
    }

    private void drawFrameRate(Canvas canvas) {
        float scale = raceControls ? touches.layout().scale * 1.5f : Math.min(getResources().getDisplayMetrics().density,
                Math.max(1, getHeight() - insetTop - insetBottom) / 330f);
        if (scale <= 0 || touches.layout().fpsWidth <= 0) return;
        float padding = 3 * scale, left = touches.layout().fpsLeft, top = touches.layout().fpsTop;
        Typeface previous = paint.getTypeface();
        paint.setTypeface(Typeface.MONOSPACE);
        paint.setTextSize(10 * scale);
        float fit=Math.min(1,touches.layout().fpsWidth/(paint.measureText(frameRate)+2*padding));
        scale*=fit;
        padding*=fit;
        paint.setTextSize(10*scale);
        paint.setTextAlign(Paint.Align.LEFT);
        paint.setStyle(Paint.Style.FILL);
        Paint.FontMetrics metrics = paint.getFontMetrics();
        float height = metrics.descent - metrics.ascent + 2 * scale;
        paint.setColor(Color.argb(190, 0, 0, 0));
        canvas.drawRoundRect(left, top, left + paint.measureText(frameRate) + 2 * padding,
                top + height, 2 * scale, 2 * scale, paint);
        paint.setColor(Color.WHITE);
        canvas.drawText(frameRate, left + padding, top + scale - metrics.ascent, paint);
        paint.setTypeface(previous);
    }

    private void shape(Canvas canvas, RaceTouchLayout.Button button) {
        if (button.round) canvas.drawOval(bounds, paint);
        else canvas.drawRoundRect(bounds, 9, 9, paint);
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            touches.cancel();
        } else if (action == MotionEvent.ACTION_DOWN) {
            touches.beginGesture(event.getPointerId(0), event.getX(0), event.getY(0));
        } else {
            int excluded = action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_UP
                    ? event.getActionIndex() : -1;
            for (int index = 0; index < event.getPointerCount(); index++) {
                if (index != excluded) touches.pointerMove(event.getPointerId(index), event.getX(index), event.getY(index));
            }
            if (action == MotionEvent.ACTION_POINTER_DOWN) {
                int index = event.getActionIndex();
                touches.pointerDown(event.getPointerId(index), event.getX(index), event.getY(index));
            } else if (excluded >= 0) {
                touches.pointerUp(event.getPointerId(excluded));
            }
        }
        setPressedButtons(touches.buttons());
        invalidate();
        if (action == MotionEvent.ACTION_UP) performClick();
        return true;
    }

    @Override public void onWindowFocusChanged(boolean hasWindowFocus) {
        super.onWindowFocusChanged(hasWindowFocus);
        if (!hasWindowFocus) release();
    }

    @Override protected void onDetachedFromWindow() {
        release();
        super.onDetachedFromWindow();
    }

    @Override public boolean performClick() {
        super.performClick();
        return true;
    }

    void release() {
        touches.release();
        setPressedButtons(0);
        invalidate();
    }

    private void setPressedButtons(int value) {
        if (value != pressed) {
            pressed = value;
            listener.buttonsChanged(value);
            invalidate();
        }
    }
}
