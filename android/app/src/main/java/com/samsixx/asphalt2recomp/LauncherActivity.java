package com.samsixx.asphalt2recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ActivityManager;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.io.OutputStream;
import java.util.concurrent.atomic.AtomicBoolean;

public final class LauncherActivity extends Activity {
    static final String PREFERENCES = "launcher";
    static final String ISO_URI = "iso_uri";
    private static final int PICK_ISO = 100;
    private static final int PLAY_GAME = 101;
    private static final int SAVE_REPORT = 102;
    private static final int STORAGE_ACCESS = 103;
    private static final String STORAGE_WAITING = "storage_waiting";
    private static final String STORAGE_ACCEPTED = "storage_accepted";
    private static final String STORAGE_SETTINGS = "storage_settings";
    private static final String PENDING_REPORT = "pending_report";
    private static final String AUTO_CONSUMED = "auto_launch_consumed";

    private final Handler main = new Handler(Looper.getMainLooper());
    private final AtomicBoolean cancelled = new AtomicBoolean();
    private Button choose;
    private Button play;
    private Button report;
    private TextView status;
    private ProgressBar progress;
    private Uri verifiedUri;
    private Uri currentUri;
    private File verifiedImage;
    private boolean busy;
    private Uri pendingReport;
    private StorageLaunchGate storageGate;
    private boolean launchAfterPermission;
    private boolean autoEligible;
    private boolean launching;
    private LauncherStartupPolicy startup;

    @Override public void onCreate(Bundle savedState) {
        super.onCreate(savedState);
        storageGate = new StorageLaunchGate(savedState != null && savedState.getBoolean(STORAGE_WAITING, false),savedState != null && savedState.getBoolean(STORAGE_SETTINGS, false));
        launchAfterPermission = savedState != null && savedState.getBoolean(STORAGE_ACCEPTED, false);
        buildLayout();
        if (savedState != null && savedState.getString(PENDING_REPORT) != null)
            pendingReport = Uri.parse(savedState.getString(PENDING_REPORT));
        String savedUri = savedState != null && savedState.containsKey(ISO_URI)
                ? savedState.getString(ISO_URI) : preferences().getString(ISO_URI, null);
        startup = new LauncherStartupPolicy(savedState != null && savedState.getBoolean(AUTO_CONSUMED, false));
        autoEligible = savedUri != null;
        currentUri = savedUri == null ? null : Uri.parse(savedUri);
        if (foregroundExistingGame(savedUri)) return;
        verify(savedUri == null ? null : Uri.parse(savedUri));
    }

    private SharedPreferences preferences() {
        return getSharedPreferences(PREFERENCES, MODE_PRIVATE);
    }

    private void buildLayout() {
        getWindow().setStatusBarColor(Color.rgb(15, 20, 28));
        getWindow().setNavigationBarColor(Color.rgb(15, 20, 28));
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.rgb(15, 20, 28));
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setGravity(Gravity.CENTER_VERTICAL);
        final int padding = dp(28);
        content.setPadding(padding, padding, padding, padding);
        scroll.addView(content, new ScrollView.LayoutParams(-1, -1));
        scroll.setOnApplyWindowInsetsListener((view, insets) -> {
            content.setPadding(padding + insets.getSystemWindowInsetLeft(),
                    padding + insets.getSystemWindowInsetTop(),
                    padding + insets.getSystemWindowInsetRight(),
                    padding + insets.getSystemWindowInsetBottom());
            return insets;
        });
        TextView title = text("Asphalt Urban GT 2", 30, Color.WHITE);
        content.addView(title);
        TextView subtitle = text("Recomp Settings test", 16, Color.rgb(126, 187, 255));
        add(content, subtitle, 8);
        TextView introduction = text("Choose your original Asphalt Urban GT 2 Europe (ULES00719) ISO to begin.", 17, Color.rgb(225, 230, 239));
        add(content, introduction, 30);
        choose = new Button(this);
        choose.setText("Choose ISO");
        choose.setOnClickListener(view -> chooseIso());
        add(content, choose, 8);
        play = new Button(this);
        play.setText("Play");
        play.setEnabled(false);
        play.setOnClickListener(view -> playRequested());
        add(content, play, 8);
        report = new Button(this);
        report.setText("Save performance report");
        report.setOnClickListener(view -> saveReport());
        report.setEnabled(PerformanceReport.available(new File(getFilesDir(), "runtime")));
        add(content, report, 8);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        add(content, progress, 20);
        status = text("Preparing game files…", 15, Color.rgb(225, 230, 239));
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        add(content, status, 10);
        TextView note = text("Saves and Recomp Settings are created automatically in Internal storage/asphalt2recomp/data. Play asks for storage access when needed.\n\nYour ISO stays on this device. It is not included in this app. Minimize the game to resume it later.", 14, Color.rgb(162, 174, 192));
        add(content, note, 28);
        setContentView(scroll);
        scroll.requestApplyInsets();
    }

    private TextView text(String value, int size, int color) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        return view;
    }

    private void add(LinearLayout parent, View child, int margin) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.topMargin = dp(margin);
        parent.addView(child, params);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
    @Override protected void onResume() {
        super.onResume();
        refreshReportButton();
        storageReturned(true);
    }
    private void refreshReportButton() {
        if (report != null) report.setEnabled(!busy && PerformanceReport.available(new File(getFilesDir(), "runtime")));
    }
    private void refreshButtons() {
        choose.setEnabled(!busy);
        play.setEnabled(!busy && !launching && verifiedUri != null && verifiedImage != null && !storageGate.waiting());
        refreshReportButton();
    }
    private void maybeAutoLaunch() {
        if (autoEligible && !busy && !launching && startup.ready(verifiedUri != null && verifiedImage != null, StorageAccess.allowed(this))) launchGame();
    }
    private boolean foregroundExistingGame(String savedIso) {
        ActivityManager manager = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (manager == null) return false;
        java.util.List<ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
        if (processes == null) return false;
        String expected = getPackageName() + ":game";
        for (ActivityManager.RunningAppProcessInfo process : processes) {
            if (process.uid != Process.myUid() || !expected.equals(process.processName)) continue;
            startup.returnedFromGame();
            Intent intent = gameIntent(savedIso == null ? null : Uri.parse(savedIso), VerifiedFiles.imageFile(this));
            launching = true;
            status.setText("Returning to your game…");
            startActivityForResult(intent, PLAY_GAME);
            return true;
        }
        return false;
    }
    private void playRequested() {
        if (busy || launching || verifiedUri == null || verifiedImage == null) return;
        autoEligible = false; startup.returnedFromGame();
        boolean settings=StorageAccess.settingsRoute(this);
        StorageLaunchGate.Action action=storageGate.play(StorageAccess.allowed(this),settings);
        if (action==StorageLaunchGate.Action.START) { launchGame(); return; }
        if (action==StorageLaunchGate.Action.REQUEST) {
            refreshButtons();
            try { StorageAccess.request(this, STORAGE_ACCESS,settings); }
            catch (RuntimeException error) {
                storageGate.cancel();refreshButtons();
                showError("Android could not open storage access settings. Try Play again.");
            }
        }
    }
    private void storageReturned(boolean fromResume) {
        if (storageGate == null || !storageGate.waiting()) return;
        StorageLaunchGate.Action action=fromResume?storageGate.resumed(StorageAccess.allowed(this)):storageGate.result(StorageAccess.allowed(this));
        if(storageGate.waiting())return;
        launchAfterPermission=action==StorageLaunchGate.Action.START;
        refreshButtons();
        launchAfterStorage();
    }
    private void launchAfterStorage() {
        if (!launchAfterPermission || busy || launching || verifiedUri == null || verifiedImage == null) return;
        launchAfterPermission=false;launchGame();
    }
    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request==STORAGE_ACCESS) storageReturned(false);
    }
    private void exportPendingReport() {
        refreshReportButton();
        if (!busy && pendingReport != null) {
            Uri uri = pendingReport; pendingReport = null; exportReport(uri);
        }
    }
    private void saveReport() {
        if (busy) return;
        Intent picker = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("application/zip");
        picker.putExtra(Intent.EXTRA_TITLE, "Asphalt2-performance.zip");
        try { startActivityForResult(picker, SAVE_REPORT); }
        catch (RuntimeException error) { showError("The Files app could not save the report."); }
    }
    private void exportReport(Uri uri) {
        if (uri == null || !"content".equals(uri.getScheme())) return;
        if (busy) { pendingReport = uri; return; }
        busy = true;
        refreshButtons();
        status.setText("Saving performance report…");
        File runtime = new File(getFilesDir(), "runtime");
        String version = "unknown";
        try { version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName; }
        catch (android.content.pm.PackageManager.NameNotFoundException ignored) { }
        final String metadata = "app_version=" + version + "\nmodel=" + android.os.Build.MODEL
                + "\nmanufacturer=" + android.os.Build.MANUFACTURER + "\nandroid=" + android.os.Build.VERSION.RELEASE
                + "\nabi=" + java.util.Arrays.toString(android.os.Build.SUPPORTED_ABIS)
                + "\nprocessors=" + Runtime.getRuntime().availableProcessors() + "\n";
        new Thread(() -> {
            String message;
            try (OutputStream output = getContentResolver().openOutputStream(uri, "w")) {
                if (output == null) throw new IOException("The destination could not be opened.");
                PerformanceReport.write(runtime, output, metadata);
                message = "Performance report saved.";
            } catch (IOException | RuntimeException error) {
                message = "Could not save the report: " + error.getMessage();
            }
            final String result = message;
            main.post(() -> {
                if (cancelled.get()) return;
                busy = false; refreshButtons();
                status.setText(result); exportPendingReport(); maybeAutoLaunch();
            });
        }, "Save performance report").start();
    }

    private void chooseIso() {
        if (busy) return;
        autoEligible = false;
        Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        picker.addCategory(Intent.CATEGORY_OPENABLE);
        picker.setType("*/*");
        picker.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        try {
            startActivityForResult(picker, PICK_ISO);
        } catch (android.content.ActivityNotFoundException e) {
            showError("No document picker is available. Enable your device's Files app and try again.");
        }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == STORAGE_ACCESS) {
            storageReturned(false);return;
        }
        if (request == SAVE_REPORT) {
            if (result == RESULT_OK && data != null) exportReport(data.getData());
            return;
        }
        if (request == PLAY_GAME) {
            startup.returnedFromGame(); autoEligible = false; launching = false; refreshButtons();
            if (verifiedImage == null) verify(preferences().getString(ISO_URI, null) == null ? null : Uri.parse(preferences().getString(ISO_URI, null)));
            String message = data == null ? null : data.getStringExtra(GameActivity.EXTRA_MESSAGE);
            if (message != null) showError(message);
            return;
        }
        if (request != PICK_ISO || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        if (!"content".equals(uri.getScheme())) {
            showError("Choose the ISO from your device's Files app.");
            return;
        }
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            showError("This file provider could not keep access to the ISO. Save it to device storage and choose that local file.");
            return;
        }
        verify(uri);
    }

    private void verify(final Uri uri) {
        currentUri = uri;
        busy = true;
        verifiedUri = null;
        verifiedImage = null;
        choose.setEnabled(false);
        play.setEnabled(false);
        refreshReportButton();
        progress.setVisibility(View.VISIBLE);
        progress.setProgress(0);
        final android.content.Context app = getApplicationContext();
        Thread worker = new Thread(() -> {
            try {
                VerifiedFiles.Progress reporter = (message, bytes, total) -> {
                    if (cancelled.get()) throw new IOException("Verification cancelled.");
                    final int fraction = (int) (1000L * bytes / total);
                    final String display = message + "… " + (bytes * 100L / total) + "%";
                    main.post(() -> {
                        if (!cancelled.get()) {
                            status.setText(display);
                            progress.setProgress(fraction);
                        }
                    });
                };
                final File image = VerifiedFiles.prepareImage(app, reporter);
                if (uri != null) VerifiedFiles.verifyIso(app, uri, reporter);
                if (cancelled.get()) return;
                main.post(() -> {
                    if (cancelled.get()) return;
                    busy = false;
                    verifiedImage = image;
                    verifiedUri = uri;
                    progress.setVisibility(View.GONE);
                    refreshButtons();
                    status.setText(uri == null ? "Ready. Choose your ISO to continue." : "ISO verified. Tap Play.");
                    if (uri != null) {
                        String previous = preferences().getString(ISO_URI, null);
                        preferences().edit().putString(ISO_URI, uri.toString()).apply();
                        if (previous != null && !previous.equals(uri.toString())) {
                            try {
                                getContentResolver().releasePersistableUriPermission(Uri.parse(previous), Intent.FLAG_GRANT_READ_URI_PERMISSION);
                            } catch (SecurityException ignored) { }
                        }
                    }
                    refreshButtons();exportPendingReport();launchAfterStorage();maybeAutoLaunch();
                });
            } catch (final IOException | RuntimeException error) {
                main.post(() -> {
                    if (cancelled.get()) return;
                    busy = false;
                    startup.returnedFromGame(); autoEligible = false;launchAfterPermission=false;
                    refreshButtons();
                    play.setEnabled(false);
                    progress.setVisibility(View.GONE);
                    status.setText(error.getMessage() == null ? "The file could not be verified. Choose the ISO again." : error.getMessage());
                    if (uri != null && !uri.toString().equals(preferences().getString(ISO_URI, null))) {
                        try {
                            getContentResolver().releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                        } catch (SecurityException ignored) { }
                    }
                    exportPendingReport();
                });
            }
        }, "Verify game files");
        worker.start();
    }

    private void launchGame() {
        if (busy || launching || verifiedUri == null || verifiedImage == null || !StorageAccess.allowed(this)) return;
        startup.returnedFromGame(); launching = true; refreshButtons();
        Intent intent = gameIntent(verifiedUri, verifiedImage);
        startActivityForResult(intent, PLAY_GAME);
    }
    private Intent gameIntent(Uri iso, File image) {
        Intent intent = new Intent(this, GameActivity.class);
        intent.setData(iso);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        intent.putExtra(GameActivity.IMAGE_PATH, image.getAbsolutePath());
        return intent;
    }

    private void showError(String message) {
        new AlertDialog.Builder(this).setTitle("Asphalt Urban GT 2").setMessage(message).setPositiveButton("OK", null).show();
    }

    @Override protected void onDestroy() {
        cancelled.set(true);
        main.removeCallbacksAndMessages(null);
        super.onDestroy();
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        state.putString(ISO_URI, currentUri == null ? null : currentUri.toString());
        state.putString(PENDING_REPORT, pendingReport == null ? null : pendingReport.toString());
        state.putBoolean(AUTO_CONSUMED, startup != null && startup.consumed());
        state.putBoolean(STORAGE_WAITING, storageGate != null && storageGate.waiting());
        state.putBoolean(STORAGE_ACCEPTED, launchAfterPermission);
        state.putBoolean(STORAGE_SETTINGS, storageGate != null && storageGate.settings());
        super.onSaveInstanceState(state);
    }
}
