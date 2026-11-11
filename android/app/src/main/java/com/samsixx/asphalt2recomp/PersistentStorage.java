package com.samsixx.asphalt2recomp;

import android.content.Context;
import java.io.Closeable;
import java.io.File;
import java.io.IOException;

/** One private working copy and its fixed persistent shared folder. */
final class PersistentStorage implements Closeable {
    private final PersistentDataStore store;
    private PersistentStorage(PersistentDataStore store) { this.store=store; }
    static PersistentStorage open(Context context, File runtime) throws IOException {
        FileDataTree tree=StorageAccess.open(context.getApplicationContext());
        return new PersistentStorage(new PersistentDataStore(runtime,tree,GameActivity::validateSavedata));
    }
    void prepare() throws IOException { store.prepare(); }
    void beforeWrite() throws IOException { store.beforeWrite(); }
    void afterWrite() throws IOException { store.afterWrite(); }
    String warning() { return store.warning(); }
    @Override public void close() throws IOException { store.close(); }
}
