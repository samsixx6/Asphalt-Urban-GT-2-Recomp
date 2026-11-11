package com.samsixx.asphalt2recomp;

import java.io.*;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Local diagnostic export with a fixed file allowlist. Never includes the ISO or game assets. */
final class PerformanceReport {
    static final int MAX_LOG_BYTES = 2 * 1024 * 1024;
    private static final String[] FILES = {"performance.jsonl", "android-native.log", "display-settings.bin", "display-timing.txt"};

    /** Called before either native startup or display reporting can write. */
    static void resetSessionLogs(File runtime) throws IOException {
        File parent = runtime.getCanonicalFile();
        for (String name : new String[]{"performance.jsonl", "android-native.log", "display-timing.txt"}) {
            File file = new File(parent, name).getCanonicalFile();
            if (!parent.equals(file.getParentFile()) || (file.exists() && !file.delete())) {
                throw new IOException("Could not reset the previous game report. Check device storage.");
            }
        }
    }

    static boolean available(File runtime) {
        return new File(runtime, "performance.jsonl").isFile() || new File(runtime, "android-native.log").isFile();
    }

    static void write(File runtime, OutputStream destination, String metadata) throws IOException {
        if (!available(runtime)) throw new IOException("Play the game briefly before saving a performance report.");
        File parent = runtime.getCanonicalFile();
        try (ZipOutputStream zip = new ZipOutputStream(destination)) {
            zip.putNextEntry(new ZipEntry("device.txt"));
            zip.write(metadata.getBytes("UTF-8"));
            zip.closeEntry();
            byte[] buffer = new byte[8192];
            for (String name : FILES) {
                File file = new File(parent, name).getCanonicalFile();
                if (!parent.equals(file.getParentFile()) || !file.isFile()) continue;
                int limit = name.equals("android-native.log") ? MAX_LOG_BYTES : 512 * 1024;
                zip.putNextEntry(new ZipEntry(name));
                try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
                    long length = input.length(), start = Math.max(0, length - limit);
                    input.seek(start);
                    if (start > 0) zip.write("[Earlier log bytes omitted]\n".getBytes("UTF-8"));
                    long remaining = length - start;
                    while (remaining > 0) {
                        int read = input.read(buffer, 0, (int)Math.min(buffer.length, remaining));
                        if (read < 0) break;
                        zip.write(buffer, 0, read);
                        remaining -= read;
                    }
                }
                zip.closeEntry();
            }
        }
    }
}
