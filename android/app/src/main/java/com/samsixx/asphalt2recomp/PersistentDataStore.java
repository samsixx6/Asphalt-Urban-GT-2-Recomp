package com.samsixx.asphalt2recomp;

import java.io.*;
import java.nio.channels.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;
import java.util.zip.*;

/** Platform-independent transaction coordinator; Android document access is injected. */
final class PersistentDataStore implements Closeable {
    private static final int MAX_FILE=4*1024*1024, MAX_TOTAL=16*1024*1024, MAX_FILES=256;
    private static final int MAX_ARCHIVE=MAX_TOTAL+1024*1024, MAGIC=0x41533250;
    private static final int MAX_RECOVERY=512; // Same bounded listing as the SAF adapter.
    private static final String MANIFEST="_asphalt2_manifest", SAVES="ms/PSP/SAVEDATA";
    private final File runtime, control;
    private final Tree tree;
    private final Validator validator;
    private final FileOutputStream lockStream;
    private final FileLock lock;
    private boolean prepared, closed;
    private volatile String warning;
    private long generation;
    static final class Entry {
        final String name; final boolean directory;
        Entry(String name, boolean directory) { this.name=name; this.directory=directory; }
    }
    interface Tree {
        List<Entry> list(String directory) throws IOException;
        InputStream read(String path) throws IOException;
        OutputStream create(String path) throws IOException;
        OutputStream replace(String path) throws IOException;
        void mkdirs(String directory) throws IOException;
        void delete(String path) throws IOException;
    }
    interface Validator { void validate(File stagedRuntime) throws IOException; }
    private static final class Snapshot {
        final long generation; final SortedMap<String,byte[]> files; final String name;
        Snapshot(long generation,SortedMap<String,byte[]> files,String name){this.generation=generation;this.files=files;this.name=name;}
    }
    PersistentDataStore(File runtime, Tree tree, Validator validator) throws IOException {
        if(runtime==null||tree==null||validator==null)throw new IOException("The game data folder is unavailable.");
        if(Files.isSymbolicLink(runtime.toPath()))throw new IOException("The private game data folder cannot be a symbolic link.");
        this.runtime=runtime.getCanonicalFile(); this.tree=tree;this.validator=validator;
        directory(this.runtime);control=privateFile(".persistent");directory(control);
        FileOutputStream stream=new FileOutputStream(privateFile(".persistent/session.lock"),true);FileLock acquired=null;
        try{acquired=stream.getChannel().tryLock();}catch(OverlappingFileLockException e){stream.close();throw new IOException("A game session is already using this data folder.",e);}catch(IOException e){stream.close();throw e;}
        if(acquired==null){stream.close();throw new IOException("A game session is already using this data folder.");}
        lockStream=stream;lock=acquired;
    }
    synchronized void prepare() throws IOException {
        requireOpen(); if(prepared)return;
        File restoring=privateFile(".persistent/restoring.zip");
        if(restoring.isFile()) {Snapshot pending=loadArchive(restoring,null);activate(pending);finishClean(pending.files,pending.generation);Files.delete(restoring.toPath());}
        SortedMap<String,byte[]> local=collectPrivate();
        String clean=readText(privateFile(".persistent/clean"));
        boolean dirty=privateFile(".persistent/pending").exists() || (clean!=null&&!clean.equals(fingerprint(local)));
        tree.mkdirs(".recovery");
        List<Snapshot> archives=snapshots(false,dirty);Snapshot latest=archives.isEmpty()?null:archives.get(0);
        if(latest!=null)generation=latest.generation;
        prepared=true;
        try{
        if(dirty){afterWrite();return;}
        if(latest!=null){
            if(!fingerprint(local).equals(fingerprint(latest.files))){
                if(!local.isEmpty())writeArchive(privateFile(".persistent/local-recovery.zip"),new Snapshot(generation,local,null));
                activate(latest);Files.delete(restoring.toPath());
            }
            mirror(latest.files);finishClean(latest.files,generation);warning=null;return;
        }
        SortedMap<String,byte[]> raw=collectShared();
        if(!raw.isEmpty()){
            // Settings and saves are independent scopes. Older internal preferences
            // must not cause an existing raw shared save to be overwritten by seeding.
            if(!raw.containsKey("display-settings.bin")&&local.containsKey("display-settings.bin"))raw.put("display-settings.bin",local.get("display-settings.bin"));
            if(!local.isEmpty())writeArchive(privateFile(".persistent/local-recovery.zip"),new Snapshot(0,local,null));
            Snapshot legacy=new Snapshot(0,raw,null);activate(legacy);Files.delete(restoring.toPath());
        }
        afterWrite();
        }catch(IOException e){prepared=false;throw e;}
    }
    synchronized void beforeWrite() throws IOException {
        requireOpen(); if(!prepared)throw new IOException("The data folder has not been prepared.");
        atomicBytes(privateFile(".persistent/pending"),new byte[]{'A','S','2','D','I','R','T','Y','\n'});
    }
    synchronized void afterWrite() throws IOException {
        requireOpen();if(!prepared)throw new IOException("The data folder has not been prepared.");
        beforeWrite();
        String publication=null;boolean created=false,verifiedPublication=false;
        try{
            SortedMap<String,byte[]> files=collectPrivate();
            tree.mkdirs(".recovery");
            // Resolve previous partial attempts before creating another file. If
            // cleanup is denied, retain pending local data and stop accumulation.
            snapshots(true,true);
            if(recoveryEntries().size()>=MAX_RECOVERY)throw new IOException("The recovery folder has no room for another snapshot.");
            if(generation==Long.MAX_VALUE)throw new IOException("The save generation limit was reached.");
            long next=++generation;String name=String.format(Locale.ROOT,"snapshot-%019d-%s.zip",next,UUID.randomUUID().toString().replace("-",""));
            File archive=privateFile(".persistent/publish.zip");writeArchive(archive,new Snapshot(next,files,name));
            publication=".recovery/"+name;
            try(InputStream input=new FileInputStream(archive);OutputStream output=tree.create(publication)){created=true;copy(input,output,MAX_ARCHIVE);}
            File checked=privateFile(".persistent/verify.zip");
            try(InputStream input=tree.read(".recovery/"+name);OutputStream output=new FileOutputStream(checked)){copy(input,output,MAX_ARCHIVE);}
            Snapshot verified=loadArchive(checked,name);
            if(verified.generation!=next||!fingerprint(verified.files).equals(fingerprint(files)))throw new IOException("The saved recovery snapshot did not verify.");
            verifiedPublication=true;
            generation=next;mirror(files);finishClean(files,generation);warning=null;
            Files.deleteIfExists(archive.toPath());Files.deleteIfExists(checked.toPath());
            // Cleanup is intentionally best effort and runs only after the new complete generation.
            try{snapshots(true,true);}catch(IOException ignored){}
        }catch(IOException e){
            // Only this attempt's exclusively created, unverified file is ours
            // to remove here. A complete verified generation survives mirror I/O.
            if(created&&!verifiedPublication)try{tree.delete(publication);}catch(IOException cleanup){e.addSuppressed(cleanup);}
            warning="Folder sync pending: "+message(e);throw e;
        }
    }
    String warning() { return warning; }
    @Override public synchronized void close() throws IOException {
        if(closed)return;closed=true;try{lock.release();}finally{lockStream.close();}
    }
    private void requireOpen()throws IOException{if(closed)throw new IOException("The data session has closed.");}
    private static String message(IOException e){return e.getMessage()==null?e.getClass().getSimpleName():e.getMessage();}
    private void finishClean(SortedMap<String,byte[]> files,long seq)throws IOException{
        atomicBytes(privateFile(".persistent/clean"),fingerprint(files).getBytes("UTF-8"));
        Files.deleteIfExists(privateFile(".persistent/pending").toPath());generation=seq;
    }
    private static String readText(File f)throws IOException{return f.isFile()?new String(Files.readAllBytes(f.toPath()),"UTF-8"):null;}
    private List<Entry> recoveryEntries()throws IOException{
        List<Entry> entries=tree.list(".recovery");if(entries.size()>MAX_RECOVERY)throw new IOException("Too many recovery files in the selected folder.");return entries;
    }
    private List<Snapshot> snapshots(boolean cleanup,boolean pendingLocal) throws IOException{
        List<Entry> entries=recoveryEntries();
        entries.sort((a,b)->b.name.compareTo(a.name));
        List<Snapshot> valid=new ArrayList<>();List<String> obsolete=new ArrayList<>();boolean invalid=false;File staging=privateFile(".persistent/read.zip");
        for(Entry entry:entries){
            if(entry.directory||!snapshotName(entry.name))continue;
            try{
                try(InputStream in=tree.read(".recovery/"+entry.name);OutputStream out=new FileOutputStream(staging)){copy(in,out,MAX_ARCHIVE);}
                Snapshot candidate=loadArchive(staging,entry.name);
                if(valid.size()<2)valid.add(candidate);else obsolete.add(entry.name);
            }catch(ZipException|InvalidData e){invalid=true;obsolete.add(entry.name);}
        }
        Files.deleteIfExists(staging.toPath());
        if(valid.isEmpty()&&invalid&&!pendingLocal)throw new IOException("No complete recovery snapshot could be read. Your existing files were preserved.");
        // Never delete either retained validated generation or unrelated names.
        // A durable pending local transaction can retry its first publication.
        if(cleanup)for(String name:obsolete)tree.delete(".recovery/"+name);
        valid.sort((a,b)->Long.compare(b.generation,a.generation));return valid;
    }
    private static boolean snapshotName(String name){return name.matches("snapshot-[0-9]{19}-[a-f0-9]{32}\\.zip");}
    private SortedMap<String,byte[]> collectPrivate()throws IOException{
        SortedMap<String,byte[]> files=new TreeMap<>();File settings=privateFile("display-settings.bin");
        if(settings.exists())add(files,"display-settings.bin",regularBytes(settings,128));
        File saves=privateFile(SAVES);if(!saves.exists())return files;directoryExisting(saves);
        File[] slots=saves.listFiles();if(slots==null)throw new IOException("Cannot list private saves.");
        for(File slot:slots){if(!slotName(slot.getName()))continue;directoryExisting(slot);File[] children=slot.listFiles();if(children==null)throw new IOException("Cannot read a save slot.");
            for(File child:children){String name=child.getName();if(privateTemporary(name))continue;if(!saveName(name))throw new InvalidData("Unexpected file in the private save slot: "+name);add(files,SAVES+"/"+slot.getName()+"/"+name,regularBytes(child,MAX_FILE));}}
        return files;
    }
    private SortedMap<String,byte[]> collectShared()throws IOException{
        SortedMap<String,byte[]> files=new TreeMap<>();
        for(Entry entry:tree.list(""))if("display-settings.bin".equals(entry.name)){if(entry.directory)throw new InvalidData("Settings must be a file.");try(InputStream in=tree.read(entry.name)){add(files,entry.name,bytes(in,128));}}
        for(Entry slot:tree.list(SAVES)){if(!slotName(slot.name))continue;if(!slot.directory)throw new InvalidData("The save slot must be a folder.");
            for(Entry file:tree.list(SAVES+"/"+slot.name)){if(privateTemporary(file.name))continue;if(file.directory||!saveName(file.name))throw new InvalidData("Unexpected file in the selected save slot.");String path=SAVES+"/"+slot.name+"/"+file.name;try(InputStream in=tree.read(path)){add(files,path,bytes(in,MAX_FILE));}}}
        return files;
    }
    private void mirror(SortedMap<String,byte[]> files)throws IOException{
        for(Map.Entry<String,byte[]> entry:files.entrySet()){
            String path=entry.getKey();int slash=path.lastIndexOf('/');if(slash>=0)tree.mkdirs(path.substring(0,slash));
            try(OutputStream out=tree.replace(path)){out.write(entry.getValue());}
            try(InputStream in=tree.read(path)){if(!Arrays.equals(bytes(in,MAX_FILE),entry.getValue()))throw new IOException("Visible save verification failed: "+path);}
        }
        // Remove only known durable files absent from this complete generation.
        for(Entry entry:tree.list(""))if(entry.name.equals("display-settings.bin")&&!entry.directory&&!files.containsKey(entry.name))tree.delete(entry.name);
        for(Entry slot:tree.list(SAVES))if(slot.directory&&slotName(slot.name))for(Entry entry:tree.list(SAVES+"/"+slot.name)){
            String path=SAVES+"/"+slot.name+"/"+entry.name;if(!entry.directory&&saveName(entry.name)&&!files.containsKey(path))tree.delete(path);
        }
    }
    private void activate(Snapshot snapshot)throws IOException{
        String id=UUID.randomUUID().toString();File stage=privateFile(".persistent/import-"+id);directory(stage);
        try{
        Set<String> slots=new TreeSet<>();
        for(Map.Entry<String,byte[]> e:snapshot.files.entrySet()){
            File destination=new File(stage,e.getKey());directory(destination.getParentFile());atomicBytes(destination,e.getValue());
            if(e.getKey().startsWith(SAVES+"/"))slots.add(e.getKey().split("/")[3]);
        }
        validator.validate(stage);
        // Only a fully validated staged runtime may become a pending activation.
        writeArchive(privateFile(".persistent/restoring.zip"),snapshot);
        File existingRoot=privateFile(SAVES);if(existingRoot.isDirectory()){File[] existing=existingRoot.listFiles();if(existing==null)throw new IOException("Cannot list saves before restore.");for(File slot:existing)if(slotName(slot.getName()))slots.add(slot.getName());}
        for(String slot:slots){File target=privateFile(SAVES+"/"+slot),source=new File(stage,SAVES+"/"+slot),old=privateFile(".persistent/previous-"+id+"-"+slot);
            if(target.exists()){directoryExisting(target);move(target,old);}
            if(source.exists()){directory(target.getParentFile());move(source,target);}
            if(old.exists())deletePrivateTree(old);
        }
        byte[] settings=snapshot.files.get("display-settings.bin");if(settings!=null)atomicBytes(privateFile("display-settings.bin"),settings);else Files.deleteIfExists(privateFile("display-settings.bin").toPath());
        }finally{deletePrivateTree(stage);}
    }
    private static void add(SortedMap<String,byte[]> files,String path,byte[] data)throws IOException{
        if(!allowed(path)||files.containsKey(path)||data.length>MAX_FILE)throw new InvalidData("Invalid or duplicate persistent file: "+path);
        if(path.equals("display-settings.bin"))validateSettings(data);
        files.put(path,data);long total=0;for(byte[] value:files.values())total+=value.length;if(files.size()>MAX_FILES||total>MAX_TOTAL)throw new InvalidData("The saved data exceeds the safe storage limit.");
    }
    private static boolean slotName(String s)throws InvalidData{
        if(!s.startsWith("ULES00719"))return false;
        String suffix=s.substring(9),upper=suffix.toUpperCase(Locale.ROOT);
        if(!suffix.matches("[A-Za-z0-9_-]{1,19}")||upper.equals("CON")||upper.equals("PRN")||upper.equals("AUX")||upper.equals("NUL")||upper.matches("(?:COM|LPT)[1-9]"))throw new InvalidData("Invalid owned save slot: "+s);
        return true;
    }
    private static boolean privateTemporary(String s){return s.startsWith(".as2recomp-tmp-")||s.startsWith(".as2recomp-backup-");}
    private static boolean saveName(String s){
        if(s.equals("PARAM.SFO")||s.equals("ICON0.PNG")||s.equals("ICON1.PMF")||s.equals("PIC1.PNG")||s.equals("SND0.AT3"))return true;
        if(s.length()>12||!s.matches("[A-Z0-9_-][A-Z0-9_.-]*")||s.endsWith(".")||s.contains(".."))return false;
        String base=s.split("\\.",2)[0];return !base.equals("CON")&&!base.equals("PRN")&&!base.equals("AUX")&&!base.equals("NUL")&&!base.matches("(?:COM|LPT)[1-9]");
    }
    private static boolean allowed(String path)throws InvalidData{if(path.equals("display-settings.bin"))return true;String[] p=path.split("/",-1);return p.length==5&&p[0].equals("ms")&&p[1].equals("PSP")&&p[2].equals("SAVEDATA")&&slotName(p[3])&&saveName(p[4]);}
    private static void validateSettings(byte[] b)throws IOException{
        if(b.length!=36&&b.length!=44&&b.length!=52&&b.length!=60&&b.length!=68)throw new InvalidData("Invalid display settings length.");int version=get(b,8);int expected=version==1?36:version==2?44:version==3?52:version==4?60:version==5?68:0;
        if(expected!=b.length||!new String(b,0,7,"US-ASCII").equals("AS2DSP"+version)||b[7]!=0)throw new InvalidData("Invalid display settings format.");
        int h=0x811c9dc5;for(int i=0;i<b.length-4;i++)h=(h^(b[i]&255))*16777619;if(h!=get(b,b.length-4))throw new InvalidData("Display settings checksum failed.");
        int pending=version==1?20:version==2?24:version==3?28:version==4?32:36;
        for(int start:new int[]{12,pending}){
            int scale=get(b,start),aspect=get(b,start+4);
            if(scale<1||scale>6||(aspect!=0&&aspect!=1))throw new InvalidData("Display settings contain an invalid resolution or aspect.");
            if(version>=2){int fps=get(b,start+8);if(fps!=30&&fps!=60)throw new InvalidData("Display settings contain an invalid frame rate.");}
            if(version>=3&&!bit(get(b,start+12)))throw new InvalidData("Display settings contain an invalid input option.");
            if(version>=4&&!bit(get(b,start+16)))throw new InvalidData("Display settings contain an invalid counter option.");
            if(version>=5&&!bit(get(b,start+20)))throw new InvalidData("Display settings contain an invalid UI layout.");
        }
        if(!bit(get(b,b.length-8)))throw new InvalidData("Display settings contain an invalid startup trial state.");
    }
    private static boolean bit(int value){return value==0||value==1;}
    private static int get(byte[] b,int p){return (b[p]&255)|(b[p+1]&255)<<8|(b[p+2]&255)<<16|(b[p+3]&255)<<24;}
    private static void writeArchive(File target,Snapshot snapshot)throws IOException{
        File part=new File(target.getPath()+".part");
        try(FileOutputStream file=new FileOutputStream(part);ZipOutputStream zip=new ZipOutputStream(file)){
            for(Map.Entry<String,byte[]> e:snapshot.files.entrySet()){zip.putNextEntry(new ZipEntry(e.getKey()));zip.write(e.getValue());zip.closeEntry();}
            ByteArrayOutputStream bytes=new ByteArrayOutputStream();try(DataOutputStream manifest=new DataOutputStream(bytes)){manifest.writeInt(MAGIC);manifest.writeInt(1);manifest.writeLong(snapshot.generation);manifest.writeInt(snapshot.files.size());for(Map.Entry<String,byte[]> e:snapshot.files.entrySet()){manifest.writeUTF(e.getKey());manifest.writeInt(e.getValue().length);manifest.write(hash(e.getValue()));}}
            zip.putNextEntry(new ZipEntry(MANIFEST));zip.write(bytes.toByteArray());zip.closeEntry();zip.finish();zip.flush();file.getFD().sync();
        }
        move(part,target);
    }
    private static Snapshot loadArchive(File file,String name)throws IOException{
        if(file.length()>MAX_ARCHIVE)throw new InvalidData("Recovery archive is too large.");
        try(ZipFile zip=new ZipFile(file)){
            Enumeration<? extends ZipEntry> entries=zip.entries();Set<String> names=new HashSet<>();SortedMap<String,byte[]> files=new TreeMap<>();byte[] manifestBytes=null;
            while(entries.hasMoreElements()){ZipEntry e=entries.nextElement();if(e.isDirectory()||!names.add(e.getName())||names.size()>MAX_FILES+1)throw new InvalidData("Invalid recovery entries.");
                int bound=e.getName().equals(MANIFEST)?65536:MAX_FILE;if(e.getSize()<0||e.getSize()>bound)throw new InvalidData("Invalid recovery entry size.");byte[] data;try(InputStream in=zip.getInputStream(e)){data=bytes(in,bound);}CRC32 crc=new CRC32();crc.update(data);if(data.length!=e.getSize()||crc.getValue()!=e.getCrc())throw new InvalidData("Recovery entry CRC failed.");
                if(e.getName().equals(MANIFEST))manifestBytes=data;else add(files,e.getName(),data);
            }
            if(manifestBytes==null)throw new InvalidData("Recovery manifest is missing.");
            try(DataInputStream in=new DataInputStream(new ByteArrayInputStream(manifestBytes))){if(in.readInt()!=MAGIC||in.readInt()!=1)throw new InvalidData("Unknown recovery format.");long seq=in.readLong();if(seq<0||in.readInt()!=files.size())throw new InvalidData("Invalid recovery generation.");
                Set<String> seen=new HashSet<>();for(int i=0;i<files.size();i++){String path=in.readUTF();int length=in.readInt();byte[] sha=new byte[32];in.readFully(sha);byte[] data=files.get(path);if(!seen.add(path)||data==null||length!=data.length||!Arrays.equals(sha,hash(data)))throw new InvalidData("Recovery manifest checksum failed.");}if(in.read()!=-1)throw new InvalidData("Trailing recovery metadata.");
                if(name!=null){long stated;try{stated=Long.parseLong(name.substring(9,28));}catch(NumberFormatException e){throw new InvalidData("Invalid recovery filename.");}if(stated!=seq)throw new InvalidData("Recovery filename generation mismatch.");}
                return new Snapshot(seq,files,name);
            }catch(EOFException|UTFDataFormatException e){throw new InvalidData("Malformed or truncated recovery manifest.");}
        }
    }
    private static byte[] hash(byte[] data){try{return MessageDigest.getInstance("SHA-256").digest(data);}catch(NoSuchAlgorithmException e){throw new AssertionError(e);}}
    private static String fingerprint(SortedMap<String,byte[]> files)throws IOException{ByteArrayOutputStream b=new ByteArrayOutputStream();try(DataOutputStream out=new DataOutputStream(b)){for(Map.Entry<String,byte[]> e:files.entrySet()){out.writeUTF(e.getKey());out.writeInt(e.getValue().length);out.write(hash(e.getValue()));}}StringBuilder hex=new StringBuilder();for(byte value:hash(b.toByteArray()))hex.append(String.format(Locale.ROOT,"%02x",value&255));return hex.toString();}
    private File privateFile(String path)throws IOException{Path root=runtime.toPath();Path result=root.resolve(path).normalize();if(!result.startsWith(root))throw new IOException("Private data path escaped its folder.");Path p=result;while(p!=null&&p.startsWith(root)){if(Files.isSymbolicLink(p))throw new IOException("A symbolic link cannot be used for game data.");p=p.getParent();}return result.toFile();}
    private static byte[] regularBytes(File file,int max)throws IOException{if(Files.isSymbolicLink(file.toPath())||!Files.isRegularFile(file.toPath(),LinkOption.NOFOLLOW_LINKS))throw new InvalidData("Persistent data must be a regular file.");try(InputStream input=new FileInputStream(file)){return bytes(input,max);}}
    private static byte[] bytes(InputStream in,int max)throws IOException{ByteArrayOutputStream out=new ByteArrayOutputStream();copy(in,out,max);return out.toByteArray();}
    private static void copy(InputStream in,OutputStream out,int max)throws IOException{if(in==null||out==null)throw new IOException("The storage provider did not open the file.");byte[] b=new byte[8192];int total=0;for(int n;(n=in.read(b))!=-1;){total+=n;if(total>max)throw new InvalidData("The persistent data exceeds its size limit.");out.write(b,0,n);}out.flush();}
    private static void directory(File dir)throws IOException{Files.createDirectories(dir.toPath());directoryExisting(dir);}
    private static void directoryExisting(File dir)throws IOException{if(Files.isSymbolicLink(dir.toPath())||!Files.isDirectory(dir.toPath(),LinkOption.NOFOLLOW_LINKS))throw new IOException("The game data path must be a real directory.");}
    private static void atomicBytes(File file,byte[] bytes)throws IOException{directory(file.getParentFile());File part=new File(file.getPath()+".part");if(Files.isSymbolicLink(part.toPath()))throw new IOException("Invalid temporary data path.");try(FileOutputStream out=new FileOutputStream(part)){out.write(bytes);out.flush();out.getFD().sync();}move(part,file);}
    private static void move(File from,File to)throws IOException{try{Files.move(from.toPath(),to.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING);}catch(AtomicMoveNotSupportedException e){Files.move(from.toPath(),to.toPath(),StandardCopyOption.REPLACE_EXISTING);}}
    private void deletePrivateTree(File file)throws IOException{File checked=privateFile(runtime.toPath().relativize(file.toPath()).toString());if(!checked.toPath().startsWith(control.toPath())||checked.equals(control))throw new IOException("Invalid recovery cleanup path.");if(checked.isDirectory()){File[] children=checked.listFiles();if(children==null)throw new IOException("Cannot clean private staging.");for(File child:children)deletePrivateTree(child);}Files.deleteIfExists(checked.toPath());}
    private static final class InvalidData extends IOException {private static final long serialVersionUID=1L;InvalidData(String message){super(message);}}
}
