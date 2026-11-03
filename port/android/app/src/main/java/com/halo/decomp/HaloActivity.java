package com.halo.decomp;

import android.app.AlertDialog;
import android.annotation.TargetApi;
import android.content.Context;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Insets;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.provider.DocumentsContract;
import android.view.Display;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL3's activity, running libmain.so (port/android/host), which
 * loads the game image from the APK's assets.
 */
public class HaloActivity extends SDLActivity {
    /** lets system link's broadcasts in over Wi-Fi while the game runs */
    private WifiManager.MulticastLock multicastLock;
    /**
     * the latest system gesture insets {left, top, right, bottom}, in pixels;
     * written on the UI thread, read by the game's thread: replaced as a
     * whole, never changed in place
     */
    private volatile int[] gestureInsets = new int[] { 0, 0, 0, 0 };
    /** the on-screen touch controls, over SDL's surface */
    private TouchControls touchControls;
    private static final int EXPORT_LAYOUT = 401, IMPORT_LAYOUT = 402, EXPORT_REPLAYS = 403, IMPORT_REPLAYS = 404,
        IMPORT_MODS = 405, IMPORT_VK_DRIVERS = 406, IMPORT_MAPS = 407;
    private String pendingLayoutExport;
    /** the folder of the replays the Theater's EXPORT REPLAYS copies, while the folder picker is open */
    private String pendingReplaysFolder;
    /** the folder the Theater's IMPORT copies replays into, while the file picker is open */
    private String pendingImportFolder;
    /** the mods folder the Mods screen's IMPORT takes .zip files into, while the file picker is open */
    private String pendingModsFolder;
    /** the longest mod folder name the game takes (port/linux/src/port_mods.c's MAXIMUM_MOD_FOLDER_LENGTH less its
     * terminator), in UTF-8's bytes, and the most files a mod's .zip may hold (its MAXIMUM_MOD_FILES) */
    private static final int MOD_FOLDER_LENGTH = 63;
    private static final int MOD_MAXIMUM_FILES = 262144;
    /** the freedreno_drivers folder Video Setup's IMPORT DRIVER copies archives into, while the file picker is open */
    private String pendingDriversFolder;
    /** the most an archive and its entries may be (port/android/host/host_vk_driver.c's ARCHIVE_MAXIMUM and
     * ENTRY_COUNT_MAXIMUM), and the longest name Video Setup lists (menu_functions.c's VK_DRIVER_NAME_SIZE) */
    private static final long DRIVER_MAXIMUM_SIZE = 256L * 1024 * 1024;
    private static final int DRIVER_MAXIMUM_ENTRIES = 256;
    private static final int DRIVER_NAME_LENGTH = 127;
    /** the custom_maps folder the map lists' IMPORT copies maps into, while the file picker is open */
    private String pendingMapsFolder;
    /** the longest map name the map lists take (port/linux/game/custom_edition_maps.c's MAXIMUM_MAP_NAME_LENGTH), a
     * map file's header and the cache version of Custom Edition's (port/linux/src/server_rotation.c's
     * server_map_header), the largest map taken, and the largest description beside one (<name>.txt, which the lists
     * show) */
    private static final int MAP_NAME_LENGTH = 51;
    private static final int MAP_HEADER_SIZE = 0x800;
    private static final int MAP_VERSION = 609;
    private static final long MAP_MAXIMUM_SIZE = 1024L * 1024 * 1024;
    private static final int MAP_DESCRIPTION_MAXIMUM_SIZE = 65536;
    /** the longest folder name the lists show (custom_edition_maps.h's CUSTOM_EDITION_MAP_CATEGORY_LENGTH less its
     * terminator) */
    private static final int MAP_SET_NAME_LENGTH = 63;
    /** a replay's file name ends so, and the file begins so (port/linux/game/network_replay.c) */
    private static final String REPLAY_EXTENSION = ".ocreplay";
    private static final byte[] REPLAY_MAGIC = "OCREPLAY".getBytes(StandardCharsets.US_ASCII);
    /** the longest name the Theater lists (NETWORK_REPLAY_NAME_SIZE less its terminator), and the largest replay
     * taken (network_replay_page.c's REPLAY_PAGE_MAXIMUM_UPLOAD) */
    private static final int REPLAY_NAME_LENGTH = 95;
    private static final long REPLAY_MAXIMUM_SIZE = 256L * 1024 * 1024;

    /** a line into the game's log: logcat and halo.log (host_main.c) */
    private static native void nativeLog(String line);

    /** how an import or export went: in a Toast, and in the log, where a report sent in shows it */
    private void report(String what, String text) {
        try {
            nativeLog(what + ": " + text);
        } catch (UnsatisfiedLinkError e) {
            // (the game's library not loaded: the Toast alone)
        }
        runOnUiThread(() -> Toast.makeText(this, text, Toast.LENGTH_LONG).show());
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null) {
            pendingLayoutExport = savedInstanceState.getString("pending-layout-export");
            pendingReplaysFolder = savedInstanceState.getString("pending-replays-folder");
            pendingImportFolder = savedInstanceState.getString("pending-import-folder");
            pendingModsFolder = savedInstanceState.getString("pending-mods-folder");
            pendingDriversFolder = savedInstanceState.getString("pending-drivers-folder");
            pendingMapsFolder = savedInstanceState.getString("pending-maps-folder");
        }
        if (mLayout != null) {
            touchControls = new TouchControls(this);
            mLayout.addView(touchControls, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars();
        preferHighestRefreshRate();
        trackGestureInsets();
        acquireMulticastLock();
        // a new version looked for while the game starts
        Updater.start(this);
    }

    /** the system's file picker, for the touch layout; no storage permission */
    public void chooseLayoutFile(boolean export, String configuration) {
        String message = export
            ? "Choose the folder and the name of the touch layout file."
            : "Choose a touch layout file. It replaces your buttons, the look sensitivity and the General settings.";
        new AlertDialog.Builder(this)
            .setTitle(export ? "Export the layout" : "Import a layout")
            .setMessage(message)
            .setNegativeButton("Cancel", null)
            .setPositiveButton(export ? "Choose the place" : "Choose the file", (dialog, which) -> {
                Intent intent = new Intent(export ? Intent.ACTION_CREATE_DOCUMENT : Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType(export ? "text/plain" : "*/*");
                if (export) {
                    pendingLayoutExport = configuration;
                    intent.putExtra(Intent.EXTRA_TITLE, "halo-touch-layout.halolayout");
                }
                try {
                    startActivityForResult(intent, export ? EXPORT_LAYOUT : IMPORT_LAYOUT);
                } catch (android.content.ActivityNotFoundException e) {
                    pendingLayoutExport = null;
                    Toast.makeText(this, "This device has no file picker.", Toast.LENGTH_LONG).show();
                }
            })
            .show();
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putString("pending-layout-export", pendingLayoutExport);
        state.putString("pending-replays-folder", pendingReplaysFolder);
        state.putString("pending-import-folder", pendingImportFolder);
        state.putString("pending-mods-folder", pendingModsFolder);
        state.putString("pending-drivers-folder", pendingDriversFolder);
        state.putString("pending-maps-folder", pendingMapsFolder);
    }

    /**
     * The Theater's EXPORT REPLAYS (port/linux/game/menu_functions.c, through host_android_export_replays): the
     * system's folder picker, then each replay in folder that the folder chosen has not by its name copied there.
     * Called on the game's thread; no storage permission.
     */
    public void exportReplays(String folder) {
        runOnUiThread(() -> {
            File[] replays = replayFiles(folder);
            if (replays.length == 0) {
                Toast.makeText(this, "There are no replays to export yet.", Toast.LENGTH_LONG).show();
                return;
            }
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            pendingReplaysFolder = folder;
            try {
                startActivityForResult(intent, EXPORT_REPLAYS);
            } catch (android.content.ActivityNotFoundException e) {
                pendingReplaysFolder = null;
                Toast.makeText(this, "This device has no folder picker.", Toast.LENGTH_LONG).show();
            }
        });
    }

    private static File[] replayFiles(String folder) {
        File[] files = folder != null ? new File(folder).listFiles(
            (directory, name) -> name.endsWith(REPLAY_EXTENSION)) : null;
        return files != null ? files : new File[0];
    }

    /** the replays copied into the tree's folder, on a thread of their own; a Toast says how many */
    private void copyReplays(Uri tree, String folder) {
        ContentResolver resolver = getContentResolver();
        Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        Set<String> present = new HashSet<>();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        try (Cursor cursor = resolver.query(children,
                new String[] { DocumentsContract.Document.COLUMN_DISPLAY_NAME }, null, null, null)) {
            while (cursor != null && cursor.moveToNext())
                present.add(cursor.getString(0));
        } catch (Exception e) {
            // (then every replay is copied: the provider names a second copy itself)
        }
        int copied = 0, skipped = 0;
        String failed = null;
        for (File replay : replayFiles(folder)) {
            if (present.contains(replay.getName())) {
                skipped++;
                continue;
            }
            try {
                Uri document = DocumentsContract.createDocument(resolver, parent, "application/octet-stream",
                    replay.getName());
                if (document == null)
                    throw new IOException("the folder refused " + replay.getName());
                try (InputStream input = new FileInputStream(replay);
                     OutputStream output = resolver.openOutputStream(document, "w")) {
                    if (output == null)
                        throw new IOException(replay.getName() + " cannot be written");
                    byte[] buffer = new byte[65536];
                    int size;
                    while ((size = input.read(buffer)) != -1)
                        output.write(buffer, 0, size);
                }
                copied++;
            } catch (Exception e) {
                failed = e.getMessage();
                break;
            }
        }
        String text = failed != null ? "The export stopped: " + failed
            : copied + (copied == 1 ? " replay exported" : " replays exported")
                + (skipped > 0 ? " (" + skipped + " already there)" : "") + ".";
        report("export replays", text);
    }

    /**
     * The Theater's IMPORT (port/linux/game/menu_functions.c, through host_android_import_replays): the system's
     * file picker, several files at once, then each one chosen copied into folder, where the Theater's list finds
     * it. Called on the game's thread; no storage permission.
     */
    public void importReplays(String folder) {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            // (a replay has no type of its own: the providers call it anything)
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            pendingImportFolder = folder;
            try {
                startActivityForResult(intent, IMPORT_REPLAYS);
            } catch (android.content.ActivityNotFoundException e) {
                pendingImportFolder = null;
                Toast.makeText(this, "This device has no file picker.", Toast.LENGTH_LONG).show();
            }
        });
    }

    /**
     * the name a replay chosen is kept under: its own, with anything but letters, digits, '-', '_' and '.' made
     * '_' (as the consoles' replay page takes them), ending .ocreplay, short enough for the Theater to list
     */
    private static String replayName(String chosen) {
        String name = chosen != null ? chosen.replaceAll("[^A-Za-z0-9._-]", "_") : "";
        if (name.endsWith(REPLAY_EXTENSION))
            name = name.substring(0, name.length() - REPLAY_EXTENSION.length());
        if (name.isEmpty())
            name = "imported";
        if (name.length() > REPLAY_NAME_LENGTH - REPLAY_EXTENSION.length())
            name = name.substring(0, REPLAY_NAME_LENGTH - REPLAY_EXTENSION.length());
        return name + REPLAY_EXTENSION;
    }

    /** the document's name as its provider shows it, else its path's last part */
    private String documentName(Uri document) {
        try (Cursor cursor = getContentResolver().query(document,
                new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && cursor.getString(0) != null)
                return cursor.getString(0);
        } catch (Exception e) {
            // (the path's, then)
        }
        return document.getLastPathSegment();
    }

    /**
     * the documents chosen copied into folder, on a thread of their own; a Toast says how many. Each goes to
     * <name>.part, which becomes the replay only once it is whole and begins as a replay does; a name the folder has
     * already is left as it is.
     */
    private void copyReplaysIn(List<Uri> documents, String folder) {
        File directory = new File(folder);
        directory.mkdirs();
        int copied = 0, skipped = 0, refused = 0;
        String failed = null;
        for (Uri document : documents) {
            String name = replayName(documentName(document));
            File replay = new File(directory, name);
            File part = new File(directory, name + ".part");
            if (replay.exists()) {
                skipped++;
                continue;
            }
            try (InputStream input = getContentResolver().openInputStream(document)) {
                if (input == null)
                    throw new IOException(name + " cannot be opened");
                byte[] buffer = new byte[65536];
                int first = 0, size;
                while (first < REPLAY_MAGIC.length && (size = input.read(buffer, first, REPLAY_MAGIC.length - first)) != -1)
                    first += size;
                boolean isReplay = first == REPLAY_MAGIC.length;
                for (int index = 0; isReplay && index < REPLAY_MAGIC.length; index++)
                    isReplay = buffer[index] == REPLAY_MAGIC[index];
                if (!isReplay) {
                    refused++;
                    continue;
                }
                long total = first;
                try (OutputStream output = new FileOutputStream(part)) {
                    output.write(buffer, 0, first);
                    while ((size = input.read(buffer)) != -1) {
                        total += size;
                        if (total > REPLAY_MAXIMUM_SIZE)
                            throw new IOException(name + " is too large");
                        output.write(buffer, 0, size);
                    }
                }
                if (!part.renameTo(replay))
                    throw new IOException(name + " cannot be kept");
                copied++;
            } catch (Exception e) {
                part.delete();
                failed = e.getMessage();
                break;
            }
        }
        String text = failed != null ? "The import stopped: " + failed
            : copied + (copied == 1 ? " replay imported" : " replays imported")
                + (skipped > 0 ? " (" + skipped + " already there)" : "")
                + (refused > 0 ? " (" + refused + (refused == 1 ? " is" : " are") + " not a replay)" : "") + ".";
        report("import replays", text);
    }

    /**
     * The Mods screen's IMPORT (port/linux/game/menu_functions.c, through host_android_import_mods): the system's
     * file picker, several .zip files at once, then the mods in each taken into folder, the mods folder, where the
     * Mods screen finds them (copyModsIn). A mod's .zip holds its folder's files as they are laid out there (mod.toml
     * and tags/ at its top), and a pack's any number of mods, each in a folder of its own at the .zip's top. Called on the game's
     * thread; no storage permission.
     */
    public void importMods(String folder) {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            // (the providers call a .zip any of these)
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "application/zip", "application/x-zip-compressed",
                "application/x-zip", "application/octet-stream" });
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            pendingModsFolder = folder;
            try {
                startActivityForResult(intent, IMPORT_MODS);
            } catch (android.content.ActivityNotFoundException e) {
                pendingModsFolder = null;
                Toast.makeText(this, "This device has no file picker.", Toast.LENGTH_LONG).show();
            }
        });
    }

    /**
     * the folder a mod's .zip is taken into: the file's name without .zip, with what a folder's name cannot hold
     * (slashes, a leading dot, which the game passes over, and control characters) made '_', no longer than the game
     * takes
     */
    private static String modFolderName(String chosen) {
        String name = chosen != null ? chosen : "";
        if (name.toLowerCase(java.util.Locale.ROOT).endsWith(".zip"))
            name = name.substring(0, name.length() - 4);
        name = name.replaceAll("[\\\\/:*?\"<>|\\p{Cntrl}]", "_").trim();
        if (name.startsWith("."))
            name = "_" + name.substring(1);
        if (name.isEmpty())
            name = "mod";
        while (name.getBytes(StandardCharsets.UTF_8).length > MOD_FOLDER_LENGTH)
            name = name.substring(0, name.offsetByCodePoints(name.length(), -1));
        return name;
    }

    /** a file, or a folder and everything in it, deleted */
    private static void deleteTree(File file) {
        File[] children = file.isDirectory() ? file.listFiles() : null;
        if (children != null) {
            for (File child : children)
                deleteTree(child);
        }
        file.delete();
    }

    /** a .zip's entries written into folder, which none may leave */
    private void unzipInto(Uri document, File folder) throws IOException {
        String top = folder.getCanonicalPath() + File.separator;
        int files = 0;
        try (InputStream input = getContentResolver().openInputStream(document)) {
            if (input == null)
                throw new IOException("it cannot be opened");
            try (ZipInputStream zip = new ZipInputStream(input)) {
                byte[] buffer = new byte[65536];
                ZipEntry entry;
                while ((entry = zip.getNextEntry()) != null) {
                    String path = entry.getName().replace('\\', '/');
                    File target = new File(folder, path);
                    if (!target.getCanonicalPath().startsWith(top) && !target.getCanonicalPath().equals(folder.getCanonicalPath()))
                        throw new IOException("it names a file outside its folder (" + entry.getName() + ")");
                    if (entry.isDirectory()) {
                        target.mkdirs();
                        continue;
                    }
                    if (++files > MOD_MAXIMUM_FILES)
                        throw new IOException("it has more than " + MOD_MAXIMUM_FILES + " files");
                    File parent = target.getParentFile();
                    if (parent != null)
                        parent.mkdirs();
                    try (OutputStream output = new FileOutputStream(target)) {
                        int size;
                        while ((size = zip.read(buffer)) != -1)
                            output.write(buffer, 0, size);
                    }
                }
            }
        }
        if (files == 0)
            throw new IOException("it is not a .zip file, or it is empty");
    }

    /**
     * the mods a .zip unpacked into folder holds, by the name each is kept under: folder itself, named as the .zip
     * (zipName), when its mod.toml is at the top; else each folder at the top with a mod.toml at its own top (a pack
     * of mods), named as that folder. Mods further down are not looked for.
     */
    private static java.util.Map<String, File> modsIn(File folder, String zipName) {
        java.util.Map<String, File> found = new java.util.LinkedHashMap<>();
        if (new File(folder, "mod.toml").isFile()) {
            found.put(zipName, folder);
            return found;
        }
        File[] children = folder.listFiles(File::isDirectory);
        if (children == null)
            return found;
        java.util.Arrays.sort(children);
        for (File child : children) {
            if (new File(child, "mod.toml").isFile())
                found.putIfAbsent(modFolderName(child.getName()), child);
        }
        return found;
    }

    /**
     * the .zip files chosen taken into the mods folder, on a thread of their own; a Toast (and the log) says how it
     * went. Each is unpacked into .import/<name> (the game passes over a folder whose name begins with a dot), and
     * each mod in it (modsIn: one at its top, or one in each folder at its top) moved into the mods folder under its
     * name; a mod whose name the folder has already is left as it is, and a .zip that cannot be taken does not stop
     * the others.
     */
    private void copyModsIn(List<Uri> documents, String folder) {
        File mods = new File(folder);
        File staging = new File(mods, ".import");
        List<String> copied = new ArrayList<>();
        List<String> skipped = new ArrayList<>();
        List<String> failed = new ArrayList<>();
        runOnUiThread(() -> Toast.makeText(this, "Importing...", Toast.LENGTH_SHORT).show());
        for (Uri document : documents) {
            String chosen = documentName(document);
            String name = modFolderName(chosen);
            File part = new File(staging, name);
            try {
                deleteTree(part);
                if (!part.mkdirs())
                    throw new IOException("its folder cannot be made");
                unzipInto(document, part);
                java.util.Map<String, File> found = modsIn(part, name);
                if (found.isEmpty())
                    throw new IOException("it has no mod.toml at its top, nor folders at its top with one each");
                for (java.util.Map.Entry<String, File> mod : found.entrySet()) {
                    File target = new File(mods, mod.getKey());
                    if (target.exists())
                        skipped.add(mod.getKey());
                    else if (mod.getValue().renameTo(target))
                        copied.add(mod.getKey());
                    else
                        failed.add(mod.getKey() + ": its folder cannot be kept");
                }
            } catch (Exception e) {
                failed.add((chosen != null ? chosen : name) + ": " + e.getMessage());
            } finally {
                deleteTree(part);
            }
        }
        deleteTree(staging);
        String text = (copied.isEmpty() ? "" : "Imported " + String.join(", ", copied) + ". ")
            + (skipped.isEmpty() ? "" : String.join(", ", skipped) + (skipped.size() == 1 ? " was" : " were")
                + " already there (delete " + (skipped.size() == 1 ? "it" : "them") + " first to import again). ")
            + (failed.isEmpty() ? "" : "Not imported: " + String.join("; ", failed) + ".")
            + (copied.isEmpty() && skipped.isEmpty() && failed.isEmpty() ? "Nothing imported." : "");
        report("import mods", text.trim());
    }

    /**
     * Video Setup's IMPORT DRIVER (port/linux/game/menu_functions.c, through host_android_import_vk_drivers): the
     * system's file picker, several archives at once, then each one copied into folder, freedreno_drivers, where
     * VULKAN DRIVER lists it the next time Video Setup opens. An archive is an adrenotools zip: a meta.json naming
     * the driver's library, and the library, at its top. Called on the game's thread; no storage permission.
     */
    public void importVkDrivers(String folder) {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "application/zip", "application/x-zip-compressed",
                "application/x-zip", "application/octet-stream" });
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            pendingDriversFolder = folder;
            try {
                startActivityForResult(intent, IMPORT_VK_DRIVERS);
            } catch (android.content.ActivityNotFoundException e) {
                pendingDriversFolder = null;
                Toast.makeText(this, "This device has no file picker.", Toast.LENGTH_LONG).show();
            }
        });
    }

    /** the name a driver archive is kept under: its own, with anything but letters, digits, '-', '_' and '.' made
     * '_', ending .zip, short enough for Video Setup to list */
    private static String driverName(String chosen) {
        String name = chosen != null ? chosen.replaceAll("[^A-Za-z0-9._-]", "_") : "";
        if (name.toLowerCase(java.util.Locale.ROOT).endsWith(".zip"))
            name = name.substring(0, name.length() - 4);
        if (name.isEmpty() || name.startsWith("."))
            name = "driver" + name;
        if (name.length() > DRIVER_NAME_LENGTH - 4)
            name = name.substring(0, DRIVER_NAME_LENGTH - 4);
        return name + ".zip";
    }

    /** why an archive is not one the game can load (as host_vk_driver.c reads it), else null */
    private static String driverArchiveProblem(File archive) {
        try (ZipFile zip = new ZipFile(archive)) {
            if (zip.size() > DRIVER_MAXIMUM_ENTRIES)
                return "it has more than " + DRIVER_MAXIMUM_ENTRIES + " files";
            ZipEntry meta = zip.getEntry("meta.json");
            if (meta == null)
                return "it has no meta.json at its top (it is not a driver archive)";
            String library;
            try (InputStream input = zip.getInputStream(meta)) {
                ByteArrayOutputStream text = new ByteArrayOutputStream();
                byte[] buffer = new byte[4096];
                int size;
                while ((size = input.read(buffer)) != -1) {
                    if (text.size() + size > 65536)
                        return "its meta.json is too large";
                    text.write(buffer, 0, size);
                }
                library = new org.json.JSONObject(new String(text.toByteArray(), StandardCharsets.UTF_8))
                    .optString("libraryName", "");
            }
            if (library.isEmpty() || library.contains("/") || library.contains("\\") || library.contains(".."))
                return "its meta.json names no library";
            if (zip.getEntry(library) == null)
                return "it has no " + library;
            return null;
        } catch (Exception e) {
            return "it is not a zip file (" + e.getMessage() + ")";
        }
    }

    /**
     * the archives chosen copied into folder, on a thread of their own; a Toast says how it went. Each goes to
     * <name>.part, which becomes the archive only once it is whole and holds a driver; a name the folder has already
     * is left as it is.
     */
    private void copyVkDriversIn(List<Uri> documents, String folder) {
        File directory = new File(folder);
        directory.mkdirs();
        List<String> copied = new ArrayList<>();
        int skipped = 0;
        String failed = null;
        for (Uri document : documents) {
            String name = driverName(documentName(document));
            File archive = new File(directory, name);
            File part = new File(directory, name + ".part");
            if (archive.exists()) {
                skipped++;
                continue;
            }
            try {
                try (InputStream input = getContentResolver().openInputStream(document);
                     OutputStream output = new FileOutputStream(part)) {
                    if (input == null)
                        throw new IOException("it cannot be opened");
                    byte[] buffer = new byte[65536];
                    long total = 0;
                    int size;
                    while ((size = input.read(buffer)) != -1) {
                        total += size;
                        if (total > DRIVER_MAXIMUM_SIZE)
                            throw new IOException("it is larger than " + (DRIVER_MAXIMUM_SIZE >> 20) + " MB");
                        output.write(buffer, 0, size);
                    }
                }
                String problem = driverArchiveProblem(part);
                if (problem != null)
                    throw new IOException(problem);
                if (!part.renameTo(archive))
                    throw new IOException("it cannot be kept");
                copied.add(name.substring(0, name.length() - 4));
            } catch (Exception e) {
                failed = name + ": " + e.getMessage();
                break;
            } finally {
                part.delete();
            }
        }
        String text = (copied.isEmpty() ? "" : "Imported " + String.join(", ", copied)
                + ". Open Video Setup again and choose it as VULKAN DRIVER. ")
            + (skipped > 0 ? skipped + (skipped == 1 ? " archive was" : " archives were") + " already there. " : "")
            + (failed != null ? "The import stopped: " + failed : copied.isEmpty() && skipped == 0 ? "Nothing imported." : "");
        report("import driver", text.trim());
    }

    /**
     * The map lists' IMPORT (New Game's and the Map screen's: port/linux/game/menu_functions.c, through
     * host_android_import_maps): the system's file picker, several files at once, then each Custom Edition map chosen
     * copied into folder, custom_maps, where the lists find it. A .zip file gives the .map files in it, wherever
     * they are there: a set of maps goes to a folder of its own (copyMapsIn). Called on the game's thread; no storage
     * permission.
     */
    public void importMaps(String folder) {
        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            // (a .map has no type of its own: the providers call it anything)
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            pendingMapsFolder = folder;
            try {
                startActivityForResult(intent, IMPORT_MAPS);
            } catch (android.content.ActivityNotFoundException e) {
                pendingMapsFolder = null;
                Toast.makeText(this, "This device has no file picker.", Toast.LENGTH_LONG).show();
            }
        });
    }

    /**
     * a name in custom_maps, of a map (its level name's, custom_maps\<name>) or a folder of maps: the file's without
     * the folders before it or the extension given, with what the lists cannot take (anything but printable ASCII,
     * and what a file system refuses) made '_', and no dot or space at its ends; null when nothing is left
     */
    private static String mapName(String chosen, String extension) {
        String name = chosen != null ? chosen : "";
        name = name.substring(Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\')) + 1);
        if (name.toLowerCase(java.util.Locale.ROOT).endsWith(extension))
            name = name.substring(0, name.length() - extension.length());
        name = name.replaceAll("[^\\x20-\\x7E]|[\\\\/:*?\"<>|]", "_").replaceAll("^[. ]+|[. ]+$", "");
        return name.isEmpty() ? null : name;
    }

    /** the resource map type (cache_file_formats.h's resource_map_type) of Custom Edition's bitmaps.map, sounds.map
     * or loc.map, by its name; 0 for any other map */
    private static int resourceMapType(String name) {
        switch (name.toLowerCase(java.util.Locale.ROOT)) {
            case "bitmaps": return 1;
            case "sounds": return 2;
            case "loc": return 3;
            default: return 0;
        }
    }

    /**
     * why the file is not a map the lists take (as server_map_header reads it: a Custom Edition cache file), or for
     * a resource map's name, not that resource map (its type its first field), else null
     */
    private static String mapProblem(File map, String name) {
        byte[] header = new byte[MAP_HEADER_SIZE];
        int read = 0;
        try (InputStream input = new FileInputStream(map)) {
            int size;
            while (read < header.length && (size = input.read(header, read, header.length - read)) != -1)
                read += size;
        } catch (IOException e) {
            return "it cannot be read (" + e.getMessage() + ")";
        }
        int resource = resourceMapType(name);
        if (resource != 0) {
            if (read < 4 || header[0] != resource || header[1] != 0 || header[2] != 0 || header[3] != 0)
                return "it is not Custom Edition's " + name + ".map";
            return null;
        }
        if (read < header.length || header[0] != 'd' || header[1] != 'a' || header[2] != 'e' || header[3] != 'h'
                || header[0x7FC] != 't' || header[0x7FD] != 'o' || header[0x7FE] != 'o' || header[0x7FF] != 'f')
            return "it is not a map file";
        int version = (header[4] & 0xFF) | (header[5] & 0xFF) << 8 | (header[6] & 0xFF) << 16 | (header[7] & 0xFF) << 24;
        if (version == 5)
            return "it is an Xbox map, not a Custom Edition one";
        if (version != MAP_VERSION)
            return "it is not a Custom Edition map (its version is " + version + ", not " + MAP_VERSION + ")";
        return null;
    }

    /** the outcomes of one IMPORT: the maps taken, those already there, and why the others were not */
    private static final class MapImport {
        final List<String> copied = new ArrayList<>();
        final List<String> skipped = new ArrayList<>();
        final List<String> failed = new ArrayList<>();
        /** the .part files written so far (each one's name has its number) */
        int parts;
    }

    /** a map read and checked, not yet kept: its name, and the file it was written to */
    private static final class MapPart {
        final String name;
        final File part;

        MapPart(String name, File part) {
            this.name = name;
            this.part = part;
        }
    }

    /**
     * one map, read from input (left open), written to a .part file of folder (which the lists pass over) and
     * checked; null, its reason in outcome, when it is not a map the lists take
     */
    private static MapPart readMap(InputStream input, File folder, String chosen, MapImport outcome) {
        String name = mapName(chosen, ".map");
        if (name == null) {
            outcome.failed.add(chosen + ": it has no name");
            return null;
        }
        if (name.length() > MAP_NAME_LENGTH) {
            outcome.failed.add(name + ": its name is longer than " + MAP_NAME_LENGTH + " characters");
            return null;
        }
        // (the game looks for Custom Edition's resource maps by these names)
        if (resourceMapType(name) != 0)
            name = name.toLowerCase(java.util.Locale.ROOT);
        File part = new File(folder, ".import-" + (outcome.parts++) + ".map.part");
        try {
            try (OutputStream output = new FileOutputStream(part)) {
                byte[] buffer = new byte[65536];
                long total = 0;
                int size;
                while ((size = input.read(buffer)) != -1) {
                    total += size;
                    if (total > MAP_MAXIMUM_SIZE)
                        throw new IOException("it is larger than " + (MAP_MAXIMUM_SIZE >> 20) + " MB");
                    output.write(buffer, 0, size);
                }
            }
            String problem = mapProblem(part, name);
            if (problem != null)
                throw new IOException(problem);
            return new MapPart(name, part);
        } catch (IOException e) {
            part.delete();
            outcome.failed.add(name + ": " + e.getMessage());
            return null;
        }
    }

    /**
     * the maps read made maps of custom_maps (folder), each <name>.map: a set's in its folder (set, null for none),
     * Custom Edition's resource maps always at custom_maps' top, where the game looks for them; a name there already
     * is left as it is. A map's description (<name>.txt, which the lists show) goes beside it, unless one is there.
     */
    private static void keepMaps(List<MapPart> maps, File folder, String set, java.util.Map<String, byte[]> descriptions,
            MapImport outcome) {
        for (MapPart map : maps) {
            File directory = set != null && resourceMapType(map.name) == 0 ? new File(folder, set) : folder;
            File kept = new File(directory, map.name + ".map");
            String shown = directory == folder ? map.name : set + "/" + map.name;
            try {
                if (kept.exists()) {
                    outcome.skipped.add(shown);
                    continue;
                }
                directory.mkdirs();
                if (!map.part.renameTo(kept)) {
                    outcome.failed.add(shown + ": it cannot be kept");
                    continue;
                }
                outcome.copied.add(shown);
                byte[] text = descriptions.get(map.name.toLowerCase(java.util.Locale.ROOT));
                File description = new File(directory, map.name + ".txt");
                if (text != null && !description.exists()) {
                    try (OutputStream output = new FileOutputStream(description)) {
                        output.write(text);
                    } catch (IOException e) {
                        description.delete();
                    }
                }
            } finally {
                map.part.delete();
            }
        }
    }

    /**
     * the documents chosen taken into folder, custom_maps, on a thread of their own; a Toast says how it went. A .zip
     * gives its .map files, and the .txt of a map's name, its description; one of more than one map (the resource
     * maps aside) is a set of maps, which goes to a folder named like the .zip, as the lists show one. Any other
     * document is taken as a map.
     */
    private void copyMapsIn(List<Uri> documents, String folder) {
        File directory = new File(folder);
        directory.mkdirs();
        // (a .part an import cut short left)
        File[] left = directory.listFiles((parent, name) -> name.startsWith(".import-") && name.endsWith(".map.part"));
        if (left != null) {
            for (File part : left)
                part.delete();
        }
        MapImport outcome = new MapImport();
        runOnUiThread(() -> Toast.makeText(this, "Importing...", Toast.LENGTH_SHORT).show());
        for (Uri document : documents) {
            String chosen = documentName(document);
            List<MapPart> maps = new ArrayList<>();
            try (InputStream input = getContentResolver().openInputStream(document)) {
                if (input == null)
                    throw new IOException("it cannot be opened");
                java.util.Map<String, byte[]> descriptions = new java.util.HashMap<>();
                if (chosen == null || !chosen.toLowerCase(java.util.Locale.ROOT).endsWith(".zip")) {
                    MapPart map = readMap(input, directory, chosen, outcome);
                    if (map != null)
                        maps.add(map);
                    keepMaps(maps, directory, null, descriptions, outcome);
                    continue;
                }
                int found = 0, set = 0;
                try (ZipInputStream zip = new ZipInputStream(input)) {
                    ZipEntry entry;
                    while ((entry = zip.getNextEntry()) != null) {
                        String path = entry.getName();
                        String lower = path.toLowerCase(java.util.Locale.ROOT);
                        if (entry.isDirectory())
                            continue;
                        if (lower.endsWith(".map")) {
                            found++;
                            MapPart map = readMap(zip, directory, path, outcome);
                            if (map != null) {
                                maps.add(map);
                                if (resourceMapType(map.name) == 0)
                                    set++;
                            }
                        } else if (lower.endsWith(".txt")) {
                            ByteArrayOutputStream text = new ByteArrayOutputStream();
                            byte[] buffer = new byte[4096];
                            int size;
                            while ((size = zip.read(buffer)) != -1 && text.size() <= MAP_DESCRIPTION_MAXIMUM_SIZE)
                                text.write(buffer, 0, size);
                            String name = mapName(path, ".txt");
                            if (name != null && text.size() <= MAP_DESCRIPTION_MAXIMUM_SIZE)
                                descriptions.put(name.toLowerCase(java.util.Locale.ROOT), text.toByteArray());
                        }
                    }
                }
                if (found == 0)
                    outcome.failed.add(chosen + ": it has no .map file");
                String setName = mapName(chosen, ".zip");
                if (setName != null && setName.length() > MAP_SET_NAME_LENGTH)
                    setName = setName.substring(0, MAP_SET_NAME_LENGTH).replaceAll("[. ]+$", "");
                keepMaps(maps, directory, set > 1 && setName != null && !setName.isEmpty() ? setName : null,
                    descriptions, outcome);
            } catch (Exception e) {
                outcome.failed.add(chosen + ": " + e.getMessage());
            } finally {
                for (MapPart map : maps)
                    map.part.delete();
            }
        }
        String text = (outcome.copied.isEmpty() ? "" : "Imported " + String.join(", ", outcome.copied) + ". ")
            + (outcome.skipped.isEmpty() ? "" : String.join(", ", outcome.skipped) + (outcome.skipped.size() == 1
                ? " was" : " were") + " already there. ")
            + (outcome.failed.isEmpty() ? "" : "Not imported: " + String.join("; ", outcome.failed) + ".")
            + (outcome.copied.isEmpty() && outcome.skipped.isEmpty() && outcome.failed.isEmpty()
                ? "Nothing imported." : "");
        report("import maps", text.trim());
    }

    /** the documents a picker of several files chose */
    private static List<Uri> chosenDocuments(Intent data) {
        List<Uri> documents = new ArrayList<>();
        if (data.getClipData() != null) {
            for (int index = 0; index < data.getClipData().getItemCount(); index++)
                documents.add(data.getClipData().getItemAt(index).getUri());
        } else if (data.getData() != null) {
            documents.add(data.getData());
        }
        return documents;
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request == IMPORT_REPLAYS) {
            String folder = pendingImportFolder;
            pendingImportFolder = null;
            if (result != RESULT_OK || data == null || folder == null)
                return;
            List<Uri> documents = chosenDocuments(data);
            if (!documents.isEmpty())
                new Thread(() -> copyReplaysIn(documents, folder), "halo-import-replays").start();
            return;
        }
        if (request == IMPORT_VK_DRIVERS) {
            String folder = pendingDriversFolder;
            pendingDriversFolder = null;
            if (result != RESULT_OK || data == null || folder == null)
                return;
            List<Uri> documents = chosenDocuments(data);
            if (!documents.isEmpty())
                new Thread(() -> copyVkDriversIn(documents, folder), "halo-import-vk-drivers").start();
            return;
        }
        if (request == IMPORT_MAPS) {
            String folder = pendingMapsFolder;
            pendingMapsFolder = null;
            if (result != RESULT_OK || data == null || folder == null)
                return;
            List<Uri> documents = chosenDocuments(data);
            if (!documents.isEmpty())
                new Thread(() -> copyMapsIn(documents, folder), "halo-import-maps").start();
            return;
        }
        if (request == IMPORT_MODS) {
            String folder = pendingModsFolder;
            pendingModsFolder = null;
            if (result != RESULT_OK || data == null || folder == null)
                return;
            List<Uri> documents = chosenDocuments(data);
            if (!documents.isEmpty())
                new Thread(() -> copyModsIn(documents, folder), "halo-import-mods").start();
            return;
        }
        if (request == EXPORT_REPLAYS) {
            String folder = pendingReplaysFolder;
            pendingReplaysFolder = null;
            if (result == RESULT_OK && data != null && data.getData() != null && folder != null) {
                Uri tree = data.getData();
                new Thread(() -> copyReplays(tree, folder), "halo-export-replays").start();
            }
            return;
        }
        if (request != EXPORT_LAYOUT && request != IMPORT_LAYOUT) {
            super.onActivityResult(request, result, data);
            return;
        }
        String exported = pendingLayoutExport;
        pendingLayoutExport = null;
        if (result != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri document = data.getData();
        // (the file is read and written away from the UI thread)
        new Thread(() -> {
            try {
                if (request == EXPORT_LAYOUT)
                    writeLayoutFile(document, exported);
                else
                    readLayoutFile(document);
            } catch (Exception e) {
                runOnUiThread(() -> layoutFileError(e));
            }
        }, "halo-touch-layout-file").start();
    }

    private void writeLayoutFile(Uri document, String configuration) throws IOException {
        if (configuration == null)
            throw new IOException("the layout to export is gone");
        try (OutputStream output = getContentResolver().openOutputStream(document, "wt")) {
            if (output == null)
                throw new IOException("the file cannot be opened");
            output.write(configuration.getBytes(StandardCharsets.UTF_8));
        }
        runOnUiThread(() -> Toast.makeText(this, "The layout is exported.", Toast.LENGTH_SHORT).show());
    }

    private void readLayoutFile(Uri document) throws IOException {
        ByteArrayOutputStream contents = new ByteArrayOutputStream();
        try (InputStream input = getContentResolver().openInputStream(document)) {
            if (input == null)
                throw new IOException("the file cannot be opened");
            byte[] buffer = new byte[4096];
            int size;
            while ((size = input.read(buffer)) != -1) {
                if (contents.size() + size > 65536)
                    throw new IOException("the file is too large");
                contents.write(buffer, 0, size);
            }
        }
        String configuration = new String(contents.toByteArray(), StandardCharsets.UTF_8);
        // checked here, then applied to the overlay at once
        TouchLayout.importConfiguration(configuration);
        runOnUiThread(() -> {
            if (isFinishing() || isDestroyed() || touchControls == null)
                return;
            try {
                touchControls.importLayout(configuration);
                Toast.makeText(this, "The layout is imported and saved.", Toast.LENGTH_SHORT).show();
            } catch (IllegalArgumentException e) {
                layoutFileError(e);
            }
        });
    }

    private void layoutFileError(Exception error) {
        if (isFinishing() || isDestroyed())
            return;
        new AlertDialog.Builder(this)
            .setTitle("Touch layout file")
            .setMessage("The operation did not complete: " + error.getMessage())
            .setPositiveButton("OK", null)
            .show();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (touchControls != null && getWindow().getDecorView().hasWindowFocus())
            touchControls.startDeviceInput();
    }

    @Override
    protected void onPause() {
        if (touchControls != null)
            touchControls.stopDeviceInput();
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (touchControls != null) {
            if (hasFocus)
                touchControls.startDeviceInput();
            else
                touchControls.stopDeviceInput();
        }
        super.onWindowFocusChanged(hasFocus);
        // the system bars come back after a swipe or after the launcher was over them
        if (hasFocus)
            hideSystemBars();
    }

    @Override
    protected void onDestroy() {
        if (touchControls != null)
            touchControls.stopDeviceInput();
        if (multicastLock != null && multicastLock.isHeld())
            multicastLock.release();
        multicastLock = null;
        super.onDestroy();
    }

    /**
     * keeps gestureInsets current; Android sends the insets again when the
     * activity rotates (after the game's native code has started on a phone
     * launched from portrait), so a single read at startup would keep the
     * portrait values; the listener hands the insets on so SDL's own
     * handling still sees them
     */
    private void trackGestureInsets() {
        getWindow().getDecorView().setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override
            public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                gestureInsets = readGestureInsets(insets);
                return view.onApplyWindowInsets(insets);
            }
        });
    }

    @SuppressWarnings("deprecation") // getSystemGestureInsets is the only call on Android 10
    private static int[] readGestureInsets(WindowInsets insets) {
        Insets gesture;

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            gesture = insets.getInsets(WindowInsets.Type.systemGestures());
        else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            gesture = insets.getSystemGestureInsets();
        else
            return new int[] { 0, 0, 0, 0 };
        return new int[] { gesture.left, gesture.top, gesture.right, gesture.bottom };
    }

    /**
     * the edges of the screen where Android keeps its gestures; in sticky
     * full screen the first swipe from an edge only shows the system bars,
     * and Android hands that swipe to the game as an ordinary finger, so
     * the game (port/linux/src/touch_input.c) must ignore touches that
     * begin there; returns a copy of {left, top, right, bottom} in pixels:
     * all 0 before Android 10 (which has no insets) and until the first
     * insets arrive. The game's native code calls this through JNI by name
     * and signature (host_main.c GetMethodID(...,
     * "getSystemGestureInsetsPixels", "()[I")), at every finger down, so
     * it must not be renamed, retyped or removed as unused
     */
    public int[] getSystemGestureInsetsPixels() {
        return gestureInsets.clone();
    }

    /**
     * Many phones drop the Wi-Fi's broadcast and multicast datagrams to
     * save power unless an app holds this: without it they would not see
     * system link games on the local network, nor be seen hosting one.
     */
    private void acquireMulticastLock() {
        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifi == null)
                return;
            multicastLock = wifi.createMulticastLock("halo-system-link");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (RuntimeException e) {
            // (no Wi-Fi, or not allowed: the local network may miss games)
            multicastLock = null;
        }
    }

    /**
     * Keeps the status and navigation bars out of the way. The manifest's
     * fullscreen theme does this on older releases, but Android 15 and later
     * draw apps edge to edge regardless, so the bars have to be asked away.
     * Swiping brings them back transiently and they leave again on their own.
     *
     * The two ways of asking are split, because {@code WindowInsetsController}
     * arrived in Android 11: on 9 and 10 the call is a hidden one, which the
     * system refuses (and logs), and it is not the older call that hides the
     * bars there.
     */
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            hideSystemBarsWithInsets();
        else
            hideSystemBarsLegacy();
    }

    /** Android 11 and later */
    @TargetApi(Build.VERSION_CODES.R)
    private void hideSystemBarsWithInsets() {
        WindowInsetsController controller = getWindow().getInsetsController();

        if (controller == null) {
            hideSystemBarsLegacy();
            return;
        }
        controller.hide(WindowInsets.Type.systemBars());
        controller.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    /** Android 9 and 10, and anywhere the above has no controller */
    private void hideSystemBarsLegacy() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    /**
     * The game draws a frame at every display refresh, between its 30 Hz
     * ticks (port/linux/game/render_interpolation.c); Android otherwise
     * often keeps an app at 60 Hz on a faster display.
     */
    private void preferHighestRefreshRate() {
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode();
        Display.Mode best = current;

        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth() &&
                mode.getPhysicalHeight() == current.getPhysicalHeight() &&
                mode.getRefreshRate() > best.getRefreshRate()) {
                best = mode;
            }
        }
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(attributes);
    }
}
