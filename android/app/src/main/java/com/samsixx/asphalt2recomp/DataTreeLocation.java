package com.samsixx.asphalt2recomp;

/** The exact local directory choices supported by this app. */
final class DataTreeLocation {
    private DataTreeLocation() { }
    static boolean accepts(String authority, String documentId) {
        return "com.android.externalstorage.documents".equals(authority)
                && ("primary:asphalt2recomp".equals(documentId) || "primary:asphalt2recomp/data".equals(documentId));
    }
}
