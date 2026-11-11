package com.samsixx.asphalt2recomp;

import java.io.*;
import java.nio.channels.Channels;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.util.*;

/** Fixed, bounded public save subtree. Never accepts a user-selected path. */
final class FileDataTree implements PersistentDataStore.Tree {
    private final Path primary,root;
    FileDataTree(File primaryStorage) throws IOException {
        primary=primaryStorage.getCanonicalFile().toPath();
        if(!Files.isDirectory(primary))throw new IOException("Internal storage is unavailable.");
        Path app=primary.resolve("asphalt2recomp");directory(app);
        root=app.resolve("data");directory(root);
    }
    private static void directory(Path path)throws IOException {
        if(Files.isSymbolicLink(path))throw new IOException("A game data folder cannot be a symbolic link.");
        if(!Files.exists(path,LinkOption.NOFOLLOW_LINKS))Files.createDirectory(path);
        if(!Files.isDirectory(path,LinkOption.NOFOLLOW_LINKS))throw new IOException("A game data folder is blocked by a file.");
    }
    private Path resolve(String relative,boolean parents)throws IOException {
        if(relative==null)throw new IOException("A game data path is required.");
        // Recheck the fixed boundary for a replaced/deleted folder between saves.
        for(Path p:new Path[]{primary,root.getParent(),root})
            if(!Files.isDirectory(p,LinkOption.NOFOLLOW_LINKS))throw new IOException("The game data folder is unavailable.");
        if(relative.isEmpty())return root;
        String[] parts=relative.split("/",-1);
        if(parts.length>8)throw new IOException("The game data path is too deep.");
        for(String name:parts)
            if(name.isEmpty()||name.equals(".")||name.equals("..")||name.length()>128||!name.matches("[A-Za-z0-9_.-]+"))throw new IOException("Invalid game data filename.");
        Path p=root;
        for(int i=0;i<parts.length;i++) {
            String name=parts[i];
            p=p.resolve(name);
            if(!p.normalize().startsWith(root)||Files.isSymbolicLink(p))throw new IOException("Invalid game data location.");
            if(i<parts.length-1) {
                if(parents)directory(p);
                else if(Files.exists(p,LinkOption.NOFOLLOW_LINKS)&&!Files.isDirectory(p,LinkOption.NOFOLLOW_LINKS))throw new IOException("A game data path is not a folder.");
            }
        }
        return p;
    }
    @Override public List<PersistentDataStore.Entry> list(String relative)throws IOException {
        Path p=resolve(relative,false);List<PersistentDataStore.Entry> entries=new ArrayList<>();
        if(!Files.exists(p,LinkOption.NOFOLLOW_LINKS))return entries;
        if(!Files.isDirectory(p,LinkOption.NOFOLLOW_LINKS))throw new IOException("A game data path is not a folder.");
        try(DirectoryStream<Path> stream=Files.newDirectoryStream(p)) {
            for(Path child:stream) {
                if(entries.size()>=512)throw new IOException("The game data folder contains too many entries.");
                if(Files.isSymbolicLink(child))throw new IOException("A game data entry cannot be a symbolic link.");
                entries.add(new PersistentDataStore.Entry(child.getFileName().toString(),Files.isDirectory(child,LinkOption.NOFOLLOW_LINKS)));
            }
        }
        return entries;
    }
    @Override public InputStream read(String relative)throws IOException {
        Path p=resolve(relative,false);
        if(p.equals(root)||!Files.isRegularFile(p,LinkOption.NOFOLLOW_LINKS))throw new FileNotFoundException("Game data file is unavailable: "+relative);
        return Files.newInputStream(p,LinkOption.NOFOLLOW_LINKS);
    }
    @Override public OutputStream create(String relative)throws IOException { return output(relative,true); }
    @Override public OutputStream replace(String relative)throws IOException { return output(relative,false); }
    private OutputStream output(String relative,boolean exclusive)throws IOException {
        Path p=resolve(relative,true);
        if(p.equals(root)||(Files.exists(p,LinkOption.NOFOLLOW_LINKS)&&!Files.isRegularFile(p,LinkOption.NOFOLLOW_LINKS)))throw new IOException("The game data file is not writable.");
        Set<OpenOption> options=new HashSet<>();options.add(StandardOpenOption.WRITE);options.add(LinkOption.NOFOLLOW_LINKS);
        if(exclusive)options.add(StandardOpenOption.CREATE_NEW);
        else {options.add(StandardOpenOption.CREATE);options.add(StandardOpenOption.TRUNCATE_EXISTING);}
        final FileChannel channel=FileChannel.open(p,options);final OutputStream output=Channels.newOutputStream(channel);
        return new FilterOutputStream(output) {
            private boolean closed;
            @Override public synchronized void close()throws IOException {
                if(closed)return;closed=true;IOException error=null;
                try{flush();channel.force(true);}catch(IOException e){error=e;}
                try{super.close();}catch(IOException e){if(error==null)error=e;else error.addSuppressed(e);}
                if(error!=null)throw error;
            }
        };
    }
    @Override public void mkdirs(String relative)throws IOException { directory(resolve(relative,true)); }
    @Override public void delete(String relative)throws IOException {
        Path p=resolve(relative,false);if(!Files.exists(p,LinkOption.NOFOLLOW_LINKS))return;
        if(p.equals(root)||!Files.isRegularFile(p,LinkOption.NOFOLLOW_LINKS))throw new IOException("Only a game data file can be removed.");
        Files.delete(p);
    }
}
