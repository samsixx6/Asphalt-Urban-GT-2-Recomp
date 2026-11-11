package com.samsixx.asphalt2recomp;

import android.content.ContentResolver;
import android.content.Context;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import java.io.*;
import java.util.*;

/** SAF adapter restricted to the selected local primary-storage Asphalt folder. */
final class AndroidDataTree implements PersistentDataStore.Tree {
    private static final String AUTHORITY="com.android.externalstorage.documents";
    private static final String ROOT="primary:asphalt2recomp", DATA=ROOT+"/data";
    private static final String[] COLUMNS={DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_FLAGS};
    private final ContentResolver resolver;
    private final Uri tree;
    private final Doc data;
    private static final class Doc {
        final String id,name;final boolean directory;final int flags;
        Doc(String id,String name,boolean directory,int flags){this.id=id;this.name=name;this.directory=directory;this.flags=flags;}
    }
    private AndroidDataTree(ContentResolver resolver,Uri tree,Doc data){this.resolver=resolver;this.tree=tree;this.data=data;}
    static AndroidDataTree open(Context context,Uri tree)throws IOException{
        try{
            if(tree==null||!"content".equals(tree.getScheme())||!AUTHORITY.equals(tree.getAuthority())||!DocumentsContract.isTreeUri(tree))throw new IOException("Choose asphalt2recomp in Internal storage.");
            String id=DocumentsContract.getTreeDocumentId(tree);
            if(!DataTreeLocation.accepts(tree.getAuthority(),id))throw new IOException("Choose Internal storage/asphalt2recomp or its data folder.");
            ContentResolver resolver=context.getContentResolver();boolean granted=false;
            for(UriPermission permission:resolver.getPersistedUriPermissions())if(permission.getUri().equals(tree)&&permission.isReadPermission()&&permission.isWritePermission()){granted=true;break;}
            if(!granted)throw new IOException("Folder access expired. Choose the data folder again.");
            AndroidDataTree selected=new AndroidDataTree(resolver,tree,null);Doc root=selected.document(id);
            if(root==null||!root.directory)throw new IOException("The selected data folder was moved or deleted. Choose it again.");
            Doc data=DATA.equals(id)?root:selected.child(root,"data");
            if(data==null)data=selected.createChild(root,"data",true);
            if(!data.directory)throw new IOException("A file named data blocks the game folder.");
            AndroidDataTree adapter=new AndroidDataTree(resolver,tree,data);adapter.probe();return adapter;
        }catch(RuntimeException e){throw failure("Could not access the selected folder",e);}
    }
    private void probe()throws IOException{
        String name=".asphalt2-access-"+UUID.randomUUID().toString()+".tmp";IOException problem=null;
        try{try(OutputStream output=create(name)){output.write(new byte[]{65,83,50,68,65,84,65});}
            try(InputStream input=read(name)){byte[] expected={65,83,50,68,65,84,65};for(byte value:expected)if(input.read()!=(value&255))throw new IOException("The selected folder did not retain the test data.");if(input.read()!=-1)throw new IOException("The selected folder returned unexpected data.");}
        }catch(IOException e){problem=e;throw e;}finally{try{delete(name);}catch(IOException e){if(problem==null)throw e;problem.addSuppressed(e);}}
    }
    private Uri uri(Doc doc){return DocumentsContract.buildDocumentUriUsingTree(tree,doc.id);}
    private Doc document(String id)throws IOException{
        try(Cursor cursor=resolver.query(DocumentsContract.buildDocumentUriUsingTree(tree,id),COLUMNS,null,null,null)){
            if(cursor==null)throw new IOException("The Files provider is unavailable.");if(!cursor.moveToFirst())return null;return row(cursor);
        }catch(RuntimeException e){throw failure("Could not read the selected folder",e);}
    }
    private static Doc row(Cursor cursor)throws IOException{
        String id=cursor.getString(0),name=cursor.getString(1),type=cursor.getString(2);int flags=cursor.getInt(3);
        if(id==null||name==null||!(id.equals(ROOT)||id.equals(DATA)||id.startsWith(DATA+"/")))throw new IOException("The Files provider returned an invalid game-data location.");
        return new Doc(id,name,DocumentsContract.Document.MIME_TYPE_DIR.equals(type),flags);
    }
    private List<Doc> children(Doc parent)throws IOException{
        if(!parent.directory)throw new IOException("A game data path is not a folder.");
        List<Doc> out=new ArrayList<>();try(Cursor cursor=resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree,parent.id),COLUMNS,null,null,null)){
            if(cursor==null)throw new IOException("The Files provider could not list the folder.");while(cursor.moveToNext()){if(out.size()>=512)throw new IOException("The game data folder contains too many entries.");out.add(row(cursor));}
        }catch(RuntimeException e){throw failure("Could not list the selected folder",e);}return out;
    }
    private Doc child(Doc parent,String name)throws IOException{Doc found=null;for(Doc doc:children(parent))if(name.equals(doc.name)){if(found!=null)throw new IOException("Duplicate game data filenames are not supported.");found=doc;}return found;}
    private static String[] components(String path)throws IOException{
        if(path.isEmpty())return new String[0];String[] parts=path.split("/",-1);if(parts.length>8)throw new IOException("The game data path is too deep.");
        for(String part:parts)if(part.isEmpty()||part.equals(".")||part.equals("..")||part.length()>128||!part.matches("[A-Za-z0-9_.-]+"))throw new IOException("Invalid game data filename.");return parts;
    }
    private Doc find(String path,boolean createDirectories)throws IOException{Doc current=data;for(String part:components(path)){Doc next=child(current,part);if(next==null){if(!createDirectories)return null;next=createChild(current,part,true);}current=next;}return current;}
    private Doc createChild(Doc parent,String name,boolean directory)throws IOException{
        if((parent.flags&DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE)==0)throw new IOException("The selected folder does not allow creating game data.");
        try{
            Uri created=DocumentsContract.createDocument(resolver,uri(parent),directory?DocumentsContract.Document.MIME_TYPE_DIR:"application/octet-stream",name);
            if(created==null)throw new IOException("The Files provider could not create game data.");Doc doc=document(DocumentsContract.getDocumentId(created));
            if(doc==null||!name.equals(doc.name)||directory!=doc.directory){try{DocumentsContract.deleteDocument(resolver,created);}catch(Exception ignored){}throw new IOException("The Files provider changed the game data filename.");}return doc;
        }catch(RuntimeException e){throw failure("Could not create game data",e);}
    }
    @Override public List<PersistentDataStore.Entry> list(String path)throws IOException{
        try{Doc parent=find(path,false);List<PersistentDataStore.Entry> entries=new ArrayList<>();if(parent==null)return entries;for(Doc child:children(parent))entries.add(new PersistentDataStore.Entry(child.name,child.directory));return entries;}catch(RuntimeException e){throw failure("Could not list game data",e);}
    }
    @Override public InputStream read(String path)throws IOException{
        try{Doc file=find(path,false);if(file==null||file.directory)throw new FileNotFoundException("Game data file is unavailable: "+path);InputStream input=resolver.openInputStream(uri(file));if(input==null)throw new IOException("The Files provider could not open game data.");return input;}catch(RuntimeException e){throw failure("Could not read game data",e);}
    }
    @Override public OutputStream create(String path)throws IOException{return output(path,true);}
    @Override public OutputStream replace(String path)throws IOException{return output(path,false);}
    private OutputStream output(String path,boolean exclusive)throws IOException{
        try{
            String[] parts=components(path);if(parts.length==0)throw new IOException("A game data filename is required.");int slash=path.lastIndexOf('/');Doc parent=find(slash<0?"":path.substring(0,slash),true);String name=parts[parts.length-1];Doc file=child(parent,name);
            if(file!=null&&exclusive)throw new IOException("A recovery snapshot with this name already exists.");if(file==null)file=createChild(parent,name,false);
            if(file.directory||(file.flags&DocumentsContract.Document.FLAG_SUPPORTS_WRITE)==0)throw new IOException("The game data file is not writable.");
            final ParcelFileDescriptor descriptor=resolver.openFileDescriptor(uri(file),"rwt");if(descriptor==null)throw new IOException("The Files provider could not open game data for writing.");
            return new ParcelFileDescriptor.AutoCloseOutputStream(descriptor){
                private boolean closed;
                @Override public synchronized void close()throws IOException{
                    // FileOutputStream's finalizer can close this stream again.
                    // Never sync a descriptor already released by our first close.
                    if(closed)return;closed=true;
                    IOException failure=null;try{flush();descriptor.getFileDescriptor().sync();}catch(IOException e){failure=e;}
                    try{super.close();}catch(IOException e){if(failure==null)failure=e;else failure.addSuppressed(e);}if(failure!=null)throw failure;
                }
            };
        }catch(RuntimeException e){throw failure("Could not write game data",e);}
    }
    @Override public void mkdirs(String path)throws IOException{try{Doc dir=find(path,true);if(!dir.directory)throw new IOException("The game data folder is blocked by a file.");}catch(RuntimeException e){throw failure("Could not create the game data folder",e);}}
    @Override public void delete(String path)throws IOException{try{Doc file=find(path,false);if(file==null)return;if(file.directory)throw new IOException("Game data cleanup cannot delete a folder.");if(!DocumentsContract.deleteDocument(resolver,uri(file)))throw new IOException("The Files provider could not remove old game data.");}catch(RuntimeException e){throw failure("Could not remove old game data",e);}}
    private static IOException failure(String operation,RuntimeException error){String detail=error.getMessage();return new IOException(operation+(detail==null?".":": "+detail),error);}
}
