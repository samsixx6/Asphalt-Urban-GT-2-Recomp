package com.samsixx.asphalt2recomp;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.view.KeyEvent;
import android.view.Display;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.io.FileOutputStream;
import java.util.List;

/** One native lifetime per :game process; Home and surface loss suspend it. */
public final class GameActivity extends Activity implements SurfaceHolder.Callback, InputManager.InputDeviceListener {
    static final String IMAGE_PATH = "image_path";
    static final String EXTRA_MESSAGE = "session_message";
    private static final long STOP_TIMEOUT_MS = 2500;

    static native String nativeStart(Surface surface, int isoFd, String imagePath, String runtimeRoot);
    static native void nativeInput(int buttons, float ax, float ay);
    static native boolean nativeStop();
    static native void nativeSetResumed(boolean resumed);
    static native void nativeSetSurface(Surface surface);
    static native int nativeRecoverSaves(String root);
    static native int nativeValidateSaves(String root);
    static native boolean nativeFPSCounterEnabled();
    static native int nativeUILayout();
    static native boolean nativeRaceControlsHidden();
    static native String nativeStatus();
    static native boolean nativeDisplayRestartRequested();
    static native boolean nativeDisplayRecovered();
    static native void nativeDisplayInsets(int left, int top, int right, int bottom);
    static native long nativePresentedFrames();
    static native boolean nativeRaceControlsActive();

    private final Handler main = new Handler(Looper.getMainLooper());
    private static final Object lifecycle = new Object();
    private volatile boolean stopping;
    private boolean resumed;
    private static volatile boolean starting;
    private static volatile boolean nativeStarted;
    private static volatile boolean libraryLoaded;
    private static volatile GameActivity currentActivity;
    private boolean gameProcess;
    private boolean recoveryShown;
    private int safeLeft, safeTop, safeRight, safeBottom;
    private int[] drawableInsets = new int[4];
    private static ParcelFileDescriptor isoDescriptor;
    private SurfaceView surfaceView;
    private ControlsView controls;
    private PspInput input;
    private InputManager inputManager;
    private final FrameRateMeter frameRate = new FrameRateMeter();
    private float requestedRefreshRate;
    private static volatile File runtimeDirectory;
    private String lastDisplayReport;
    private String lastStorageWarning;
    private static volatile PersistentStorage storageSession;
    private static volatile String storageFailure;

    static void validateSavedata(File stagedRoot) throws IOException {
        if (nativeValidateSaves(stagedRoot.getCanonicalPath()) != 0)
            throw new IOException("The selected backup contains invalid save metadata or data.");
    }
    static int storageBeforeWrite() {
        PersistentStorage storage = storageSession;
        if (storage == null) return 1;
        try { storage.beforeWrite(); return 0; }
        catch (IOException | RuntimeException error) {
            storageFailure="Could not protect the next save: " + error.getMessage();
            return 1;
        }
    }
    static int storageAfterWrite() {
        PersistentStorage storage = storageSession;
        if (storage == null) return 1;
        try { storage.afterWrite(); storageFailure=null; }
        catch (IOException | RuntimeException error) {
            // The native commit is durable. Keep the coordinator and dirty marker
            // alive, report its warning, and retry without rejecting local progress.
            storageFailure="Folder sync pending: " + error.getMessage();
        }
        return 0;
    }

    private final Runnable pollControls = new Runnable() {
        @Override public void run() {
            if (stopping) return;
            try {
                int layout=nativeUILayout();
                controls.setUILayout(layout&1,(layout&2)!=0);
                controls.setRaceControls(nativeRaceControlsActive());
                controls.setButtonsVisible(!nativeRaceControlsHidden());
            } catch (RuntimeException | LinkageError error) {
                stopSession("The game could not update its controls. Tap Play to start again.");
                return;
            }
            main.postDelayed(this, 50);
        }
    };

    private final Runnable pollStatus = new Runnable() {
        @Override public void run() {
            if (stopping) return;
            try {
                if (!recoveryShown && nativeDisplayRecovered()) {
                    recoveryShown = true;
                    Toast.makeText(GameActivity.this, "Previous startup did not finish. Restored confirmed Recomp Settings.", Toast.LENGTH_LONG).show();
                }
                String message = nativeStatus();
                if (message != null) {
                    stopSession(message.isEmpty() ? "The game session ended. Tap Play to start again." : "The game stopped:\n\n" + message);
                    return;
                }
                controls.setFrameRateVisible(nativeFPSCounterEnabled());
                controls.setFrameRate(frameRate.sample(nativePresentedFrames(), System.nanoTime()));
                PersistentStorage storage = storageSession;
                String warning = storageFailure;
                if (warning == null && storage != null) warning=storage.warning();
                if (warning != null && !warning.equals(lastStorageWarning)) {
                    lastStorageWarning = warning;
                    Toast.makeText(GameActivity.this, warning, Toast.LENGTH_LONG).show();
                }
                writeDisplayReport();
            } catch (RuntimeException | LinkageError error) {
                stopSession("The game could not report its status. Tap Play to start again.");
                return;
            }
            main.postDelayed(this, 500);
        }
    };

    @Override public void onCreate(Bundle savedState) {
        super.onCreate(savedState);
        currentActivity = this;
        gameProcess = isGameProcess();
        setResult(RESULT_CANCELED, new Intent().putExtra(EXTRA_MESSAGE, "The game session ended. Tap Play to start again."));
        if (!gameProcess) {
            finish();
            return;
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // Physical refresh preference is separate from the unchanged native
        // 30/60 work cadence. Only modes with the current resolution qualify.
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode();
        Display.Mode[] modes = display.getSupportedModes();
        int[] widths = new int[modes.length], heights = new int[modes.length];
        float[] rates = new float[modes.length];
        for (int i = 0; i < modes.length; i++) {
            widths[i] = modes[i].getPhysicalWidth(); heights[i] = modes[i].getPhysicalHeight(); rates[i] = modes[i].getRefreshRate();
        }
        int preferred = RefreshRateChoice.choose(current.getPhysicalWidth(), current.getPhysicalHeight(), widths, heights, rates);
        if (preferred >= 0) {
            attributes.preferredDisplayModeId = modes[preferred].getModeId();
            requestedRefreshRate = rates[preferred];
        }
        attributes.preferredRefreshRate = requestedRefreshRate;
        if (android.os.Build.VERSION.SDK_INT >= 28) {
            attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        getWindow().setAttributes(attributes);
        input = new PspInput((buttons, x, y) -> {
            if (!stopping && libraryLoaded && nativeStarted) nativeInput(buttons, x, y);
        });
        inputManager = (InputManager) getSystemService(INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(this, main);
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(Color.BLACK);
        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        frame.addView(surfaceView, new FrameLayout.LayoutParams(-1, -1));
        controls = new ControlsView(this, buttons -> input.touch(buttons));
        frame.addView(controls, new FrameLayout.LayoutParams(-1, -1));
        frame.setOnApplyWindowInsetsListener((view, insets) -> {
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(android.view.WindowInsets.Type.systemBars());
                safeLeft=bars.left; safeTop=bars.top; safeRight=bars.right; safeBottom=bars.bottom;
            } else {
                boolean hidden=(getWindow().getDecorView().getSystemUiVisibility() & View.SYSTEM_UI_FLAG_FULLSCREEN)!=0;
                boolean navHidden=(getWindow().getDecorView().getSystemUiVisibility() & View.SYSTEM_UI_FLAG_HIDE_NAVIGATION)!=0;
                safeLeft=navHidden?0:insets.getSystemWindowInsetLeft();
                safeTop=hidden?0:insets.getSystemWindowInsetTop();
                safeRight=navHidden?0:insets.getSystemWindowInsetRight();
                safeBottom=navHidden?0:insets.getSystemWindowInsetBottom();
            }
            if (android.os.Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
                android.view.DisplayCutout cutout = insets.getDisplayCutout();
                safeLeft = Math.max(safeLeft, cutout.getSafeInsetLeft()); safeTop = Math.max(safeTop, cutout.getSafeInsetTop());
                safeRight = Math.max(safeRight, cutout.getSafeInsetRight()); safeBottom = Math.max(safeBottom, cutout.getSafeInsetBottom());
            }
            updateDisplayInsets();
            return insets;
        });
        surfaceView.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob) -> updateDisplayInsets());
        setContentView(frame);
        immersive();
    }

    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        immersive();
        frameRate.reset();
        maybeStart();
        if (nativeStarted) attachSession();
    }

    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    private void requestRefresh(Surface surface) {
        if (android.os.Build.VERSION.SDK_INT >= 30 && surface.isValid()) {
            try { surface.setFrameRate(requestedRefreshRate, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT); }
            catch (IllegalArgumentException | IllegalStateException ignored) {
                // A refresh hint is optional; native pacing remains in force.
            }
        }
    }
    private void writeDisplayReport() {
        File runtime = runtimeDirectory;
        if (runtime == null) return;
        Display display = getWindowManager().getDefaultDisplay();
        String report = "requested_refresh_hz=" + requestedRefreshRate
                + "\nobserved_refresh_hz=" + display.getRefreshRate()
                + "\nobserved_mode_id=" + display.getMode().getModeId()
                + "\nphysical_width=" + display.getMode().getPhysicalWidth()
                + "\nphysical_height=" + display.getMode().getPhysicalHeight()
                + "\ndrawable_width=" + surfaceView.getWidth() + "\ndrawable_height=" + surfaceView.getHeight()
                + "\nwindow_safe_edges=" + safeLeft + "," + safeTop + "," + safeRight + "," + safeBottom
                + "\ndrawable_safe_edges=" + drawableInsets[0] + "," + drawableInsets[1] + "," + drawableInsets[2] + "," + drawableInsets[3] + "\n";
        if (report.equals(lastDisplayReport)) return;
        try (FileOutputStream output = new FileOutputStream(new File(runtime, "display-timing.txt"))) {
            output.write(report.getBytes("UTF-8"));
            lastDisplayReport = report;
        } catch (IOException ignored) { }
    }
    private void updateDisplayInsets() {
        if (surfaceView == null || !libraryLoaded || !nativeStarted || stopping) return;
        View decor=getWindow().getDecorView();
        int[] location=new int[2], origin=new int[2];
        surfaceView.getLocationInWindow(location);decor.getLocationInWindow(origin);
        int[] relative=SurfaceInsets.relative(decor.getWidth(),decor.getHeight(),
                location[0]-origin[0],location[1]-origin[1],surfaceView.getWidth(),surfaceView.getHeight(),
                safeLeft,safeTop,safeRight,safeBottom);
        drawableInsets=relative;
        nativeDisplayInsets(relative[0],relative[1],relative[2],relative[3]);
        if (controls != null) controls.setSafeInsets(relative[0],relative[1],relative[2],relative[3]);
    }
    private void attachSession() {
        releaseInput();
        updateNativeWindow();
        main.removeCallbacks(pollStatus);main.removeCallbacks(pollControls);
        main.post(pollStatus);main.post(pollControls);
    }
    private void updateNativeWindow() {
        if (!libraryLoaded || !nativeStarted || stopping || surfaceView == null) return;
        Surface surface=surfaceView.getHolder().getSurface();
        boolean ready=resumed && surface.isValid();
        nativeSetSurface(ready?surface:null);
        nativeSetResumed(resumed);
        if (input != null) input.setEnabled(ready);
        frameRate.reset();
        updateDisplayInsets();
    }
    @Override public void surfaceCreated(SurfaceHolder holder) {
        requestRefresh(holder.getSurface());maybeStart();updateNativeWindow();
    }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        requestRefresh(holder.getSurface());updateDisplayInsets();
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        releaseInput();
        if (input != null) input.setEnabled(false);
        if (libraryLoaded && nativeStarted && !stopping) nativeSetSurface(null);
    }

    private void maybeStart() {
        if (!gameProcess || !resumed || nativeStarted || starting || stopping || surfaceView == null) return;
        if (!surfaceView.getHolder().getSurface().isValid()) return;
        starting = true;
        final Uri uri = getIntent().getData();
        final String imagePath = getIntent().getStringExtra(IMAGE_PATH);
        Thread startup = new Thread(() -> {
            ParcelFileDescriptor opened = null;
            try {
                if (uri == null || !"content".equals(uri.getScheme())) throw new IOException("Choose your ISO again from the launcher.");
                File expectedImage = VerifiedFiles.imageFile(this);
                if (imagePath == null || !expectedImage.getCanonicalPath().equals(new File(imagePath).getCanonicalPath())
                        || expectedImage.length() != VerifiedFiles.IMAGE_BYTES) {
                    throw new IOException("Game files are unavailable. Return to the launcher and try again.");
                }
                File runtime = new File(getFilesDir(), "runtime");
                if (!runtime.isDirectory() && !runtime.mkdirs()) throw new IOException("Could not create the game folder. Check device storage.");
                PerformanceReport.resetSessionLogs(runtime);
                runtimeDirectory = runtime;
                opened = getContentResolver().openFileDescriptor(uri, "r");
                if (opened == null) throw new IOException("The ISO could not be opened. Choose it again from the launcher.");
                VerifiedFiles.requireSeekable(opened);
                if (opened.getStatSize() >= 0 && opened.getStatSize() != VerifiedFiles.ISO_BYTES) {
                    throw new IOException("The ISO changed since verification. Choose the original ISO again from the launcher.");
                }
                synchronized (lifecycle) {
                    if (stopping) return;
                    System.loadLibrary("asphalt");
                    libraryLoaded = true;
                    PersistentStorage prepared = PersistentStorage.open(this, runtime);
                    storageSession = prepared;
                    if (nativeRecoverSaves(runtime.getAbsolutePath()) != 0)
                        throw new IOException("The private save files could not be recovered. Existing data was preserved.");
                    prepared.prepare();
                    isoDescriptor = opened;
                    opened = null;
                    // Teardown serializes with this call, so stop cannot overtake startup.
                    nativeStarted = true;
                    String error = nativeStart(null, isoDescriptor.getFd(), imagePath, runtime.getAbsolutePath());
                    if (error != null) throw new IOException(error);
                }
                main.post(() -> {
                    GameActivity activity=currentActivity;
                    if (activity != null && !activity.stopping) activity.attachSession();
                });
            } catch (IOException | RuntimeException | LinkageError error) {
                String message = error.getMessage();
                final String display = "The game could not start.\n\n" + (message == null ? "Return to the launcher and try again." : message);
                main.post(() -> {
                    GameActivity activity=currentActivity;
                    if(activity != null) activity.stopSession(display);
                    else stopSession(display);
                });
            } finally {
                close(opened);
            }
        }, "Start game");
        startup.start();
    }

    private void releaseInput() {
        if (controls != null) controls.release();
        if (input != null) input.release();
    }

    private void stopSession(String message) {
        if (stopping || !gameProcess) return;
        releaseInput();
        stopping = true;
        if (input != null) input.setEnabled(false);
        main.removeCallbacks(pollStatus);
        main.removeCallbacks(pollControls);
        setResult(RESULT_OK, new Intent().putExtra(EXTRA_MESSAGE, message));
        // Both paths finish before killing, so the launcher's Activity result is delivered.
        // Only this dedicated process may be killed; the launcher keeps running.
        final Runnable finishAndKill = () -> {
            finish();
            if (gameProcess) Process.killProcess(Process.myPid());
        };
        main.postDelayed(finishAndKill, STOP_TIMEOUT_MS);
        Thread teardown = new Thread(() -> {
            try {
                synchronized (lifecycle) {
                    boolean stopped = !nativeStarted || (libraryLoaded && nativeStop());
                    if (stopped) {
                        nativeStarted = false;
                        close(isoDescriptor); isoDescriptor = null;
                        PersistentStorage storage = storageSession;
                        if (storage != null) { try { storage.close(); } catch (IOException ignored) { } }
                        storageSession = null;
                    }
                    // A timed-out native owner may still be inside a storage callback.
                    // Retain its storage/lock until this dedicated process exits.
                }
            } catch (RuntimeException | LinkageError ignored) {
                // A process exit remains the final cleanup boundary after native failure.
            } finally {
                main.post(finishAndKill);
            }
        }, "Stop game");
        teardown.start();
    }

    @Override protected void onPause() {
        resumed = false;
        releaseInput();
        if (input != null) input.setEnabled(false);
        if (libraryLoaded && nativeStarted && !stopping) nativeSetResumed(false);
        super.onPause();
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus) releaseInput();
        else if (!stopping) immersive();
    }

    @Override public void onBackPressed() {
        stopSession("The game session ended. Tap Play to start again.");
    }

    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (!stopping && input != null && input.key(event, true)) return true;
        return super.onKeyDown(keyCode, event);
    }

    @Override public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (!stopping && input != null && input.key(event, false)) return true;
        return super.onKeyUp(keyCode, event);
    }

    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if (!stopping && input != null && input.motion(event)) return true;
        return super.onGenericMotionEvent(event);
    }

    @Override protected void onDestroy() {
        if (isFinishing()) stopSession("The game session ended. Tap Play to start again.");
        else if (libraryLoaded && nativeStarted && !stopping) { nativeSetResumed(false); nativeSetSurface(null); }
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        main.removeCallbacks(pollStatus);main.removeCallbacks(pollControls);
        if(currentActivity == this) currentActivity=null;
        super.onDestroy();
    }

    @Override public void onInputDeviceAdded(int deviceId) { }
    @Override public void onInputDeviceChanged(int deviceId) { releaseInput(); }
    @Override public void onInputDeviceRemoved(int deviceId) { releaseInput(); }

    private boolean isGameProcess() {
        String expected = getPackageName() + ":game";
        if (android.os.Build.VERSION.SDK_INT >= 28) return expected.equals(android.app.Application.getProcessName());
        ActivityManager manager = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (manager == null) return false;
        List<ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
        if (processes == null) return false;
        for (ActivityManager.RunningAppProcessInfo process : processes) {
            if (process.pid == Process.myPid()) return expected.equals(process.processName);
        }
        return false;
    }

    private static void close(ParcelFileDescriptor descriptor) {
        if (descriptor != null) {
            try { descriptor.close(); } catch (IOException ignored) { }
        }
    }
}
