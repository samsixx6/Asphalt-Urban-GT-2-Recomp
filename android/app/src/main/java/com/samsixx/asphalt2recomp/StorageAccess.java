package com.samsixx.asphalt2recomp;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import java.io.IOException;

/** OS access to the user's fixed visible save folder; called on Play only. */
final class StorageAccess {
    private static final String ASKED="storage_permission_requested";
    static boolean allowed(Context context) {
        if(Build.VERSION.SDK_INT>=30)return Environment.isExternalStorageManager();
        return context.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)==PackageManager.PERMISSION_GRANTED;
    }
    static boolean settingsRoute(Activity activity) {
        boolean asked=activity.getSharedPreferences(LauncherActivity.PREFERENCES,Context.MODE_PRIVATE).getBoolean(ASKED,false);
        boolean rationale=Build.VERSION.SDK_INT<30&&activity.shouldShowRequestPermissionRationale(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        return StorageLaunchGate.settingsRequest(Build.VERSION.SDK_INT>=30,asked,rationale);
    }
    static void request(Activity activity,int code,boolean settings) {
        if(settings&&Build.VERSION.SDK_INT>=30) {
            Intent intent=new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,Uri.parse("package:"+activity.getPackageName()));
            try{activity.startActivityForResult(intent,code);}
            catch(android.content.ActivityNotFoundException missing){activity.startActivityForResult(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),code);}
        } else if(settings) {
            activity.startActivityForResult(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,Uri.parse("package:"+activity.getPackageName())),code);
        } else {
            activity.getSharedPreferences(LauncherActivity.PREFERENCES,Context.MODE_PRIVATE).edit().putBoolean(ASKED,true).apply();
            activity.requestPermissions(new String[]{Manifest.permission.READ_EXTERNAL_STORAGE,Manifest.permission.WRITE_EXTERNAL_STORAGE},code);
        }
    }
    static FileDataTree open(Context context)throws IOException {
        if(!allowed(context))throw new IOException("Storage access is required. Press Play and allow storage access.");
        return new FileDataTree(Environment.getExternalStorageDirectory());
    }
    private StorageAccess() { }
}
