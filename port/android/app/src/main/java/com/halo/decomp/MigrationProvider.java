package com.halo.decomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * The old package's side of the move to the new package name (the bridge's alone: the manifest enables it there),
 * read-only: content://com.halo.decomp.migration/files lists the player's files with their sizes, and
 * .../file?path=... opens one. Only the new package, signed with this app's own key, is answered: it is checked here,
 * rather than with a permission of our own, which two apps declaring can stop one installing.
 */
public class MigrationProvider extends ContentProvider {
    static final String PATH = "path";
    static final String SIZE = "size";

    private boolean callerAllowed() {
        String caller = getCallingPackage();

        return BuildConfig.HALO_NEW_PACKAGE.equals(caller) && getContext().getPackageManager()
            .checkSignatures(caller, getContext().getPackageName()) == PackageManager.SIGNATURE_MATCH;
    }

    private File preferencesFolder() {
        return new File(getContext().getApplicationInfo().dataDir, "shared_prefs");
    }

    private void list(File folder, String relative, MatrixCursor cursor) {
        File[] children = folder.listFiles();

        if (children == null)
            return;
        for (File child : children) {
            String path = relative + child.getName();

            if (child.isDirectory())
                list(child, path + "/", cursor);
            else if (MigrationFiles.copied(path))
                cursor.addRow(new Object[] { path, child.length() });
        }
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        if (!callerAllowed())
            throw new SecurityException("not the new package of this app's own key");
        MatrixCursor cursor = new MatrixCursor(new String[] { PATH, SIZE });
        File root = getContext().getExternalFilesDir(null);

        if (root != null)
            list(root, "", cursor);
        list(preferencesFolder(), MigrationFiles.PREFERENCES, cursor);
        return cursor;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!callerAllowed())
            throw new SecurityException("not the new package of this app's own key");
        String path = uri.getQueryParameter(PATH);

        if (!"r".equals(mode) || !MigrationFiles.copied(path))
            throw new FileNotFoundException(String.valueOf(path));
        File file = path.startsWith(MigrationFiles.PREFERENCES)
            ? new File(preferencesFolder(), path.substring(MigrationFiles.PREFERENCES.length()))
            : new File(getContext().getExternalFilesDir(null), path);
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }
}
