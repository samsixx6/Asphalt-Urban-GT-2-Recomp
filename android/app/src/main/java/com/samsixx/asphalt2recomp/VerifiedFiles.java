package com.samsixx.asphalt2recomp;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/** Only the inspected, user-owned edition is supported by this test build. */
final class VerifiedFiles {
    static final long ISO_BYTES = 884146176L;
    static final long IMAGE_BYTES = 4343022L;
    static final String ISO_SHA256 = "64ab1ba788baade948b9b89d47145a653c063e85afed403b89d917d8a8703ec4";
    static final String IMAGE_SHA256 = "6e52686f5f0b9e476a2481661b91ae241bd7b429e060f197d9728565a6317c3b";

    interface Progress {
        void update(String message, long bytes, long total) throws IOException;
    }

    static File imageFile(Context context) {
        return new File(new File(context.getFilesDir(), "startup"), "asphalt-image.elf");
    }

    static synchronized File prepareImage(Context context, Progress progress) throws IOException {
        File image = imageFile(context);
        if (image.isFile() && image.length() == IMAGE_BYTES) {
            try (InputStream input = new FileInputStream(image)) {
                if (IMAGE_SHA256.equals(hash(input, IMAGE_BYTES, null, progress, "Checking game files"))) {
                    return image;
                }
            }
        }
        File directory = image.getParentFile();
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("Could not create the game files folder. Free some device storage and try again.");
        }
        File temporary = new File(directory, "asphalt-image.elf.part");
        try {
            String digest;
            try (InputStream input = context.getAssets().open("asphalt-image.elf");
                 FileOutputStream output = new FileOutputStream(temporary)) {
                digest = hash(input, IMAGE_BYTES, output, progress, "Preparing game files");
                output.getFD().sync();
            }
            if (!IMAGE_SHA256.equals(digest)) {
                throw new IOException("The installed game files failed verification. Reinstall this test APK.");
            }
            // Same-filesystem atomic replacement; never leave a partially written startup image.
            try {
                Os.rename(temporary.getAbsolutePath(), image.getAbsolutePath());
            } catch (ErrnoException e) {
                throw new IOException("Could not finish preparing game files. Check device storage.", e);
            }
            return image;
        } finally {
            if (temporary.exists()) temporary.delete();
        }
    }

    static void verifyIso(Context context, Uri uri, Progress progress) throws IOException {
        if (!"content".equals(uri.getScheme())) {
            throw new IOException("Choose the ISO again with the document picker.");
        }
        ParcelFileDescriptor descriptor;
        try {
            descriptor = context.getContentResolver().openFileDescriptor(uri, "r");
        } catch (SecurityException e) {
            throw new IOException("Access to this ISO has expired. Choose it again.", e);
        }
        if (descriptor == null) throw new IOException("The selected ISO could not be opened. Choose it again.");
        try (ParcelFileDescriptor ownedDescriptor = descriptor) {
            requireSeekable(ownedDescriptor);
            long size = ownedDescriptor.getStatSize();
            if (size >= 0 && size != ISO_BYTES) {
                throw new IOException("This ISO has a different size. Select the original Asphalt Urban GT 2 Europe (ULES00719) ISO (884,146,176 bytes).");
            }
            // AutoCloseInputStream owns the FD; closing the PFD again is harmless.
            try (InputStream input = new ParcelFileDescriptor.AutoCloseInputStream(ownedDescriptor)) {
                String digest = hash(input, ISO_BYTES, null, progress, "Verifying your ISO");
                if (!ISO_SHA256.equals(digest)) {
                    throw new IOException("This ISO does not match the supported Asphalt Urban GT 2 Europe (ULES00719) copy. Select the original, unmodified ISO.");
                }
            }
        }
    }

    static void requireSeekable(ParcelFileDescriptor descriptor) throws IOException {
        try {
            Os.lseek(descriptor.getFileDescriptor(), 0, OsConstants.SEEK_SET);
        } catch (ErrnoException e) {
            throw new IOException("This provider cannot give the game random access to the ISO. Save the ISO to device storage, then choose that local file.", e);
        }
    }

    private static String hash(InputStream input, long expectedBytes, FileOutputStream output,
                               Progress progress, String message) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException("SHA-256 is unavailable on this device.", e);
        }
        byte[] buffer = new byte[256 * 1024];
        long count = 0;
        long lastReport = 0;
        progress.update(message, 0, expectedBytes);
        int length;
        while ((length = input.read(buffer)) != -1) {
            if (length == 0) continue;
            count += length;
            if (count > expectedBytes) throw new IOException("The selected file has a different size than the supported original.");
            digest.update(buffer, 0, length);
            if (output != null) output.write(buffer, 0, length);
            long now = android.os.SystemClock.uptimeMillis();
            if (now - lastReport >= 150 || count == expectedBytes) {
                progress.update(message, count, expectedBytes);
                lastReport = now;
            }
        }
        if (count != expectedBytes) throw new IOException("The selected file is incomplete or has a different size than the supported original.");
        progress.update(message, count, expectedBytes);
        StringBuilder hex = new StringBuilder(64);
        for (byte value : digest.digest()) {
            hex.append(Character.forDigit((value >>> 4) & 15, 16));
            hex.append(Character.forDigit(value & 15, 16));
        }
        return hex.toString();
    }

    private VerifiedFiles() {}
}
