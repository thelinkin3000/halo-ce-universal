package com.halo.decomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.util.TypedValue;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * The move from the old package name (com.halo.decomp) to the new (ar.com.slipstreamce), which Android treats as two
 * apps with nothing in common.
 *
 * The old side is the bridge, the app built with HALO_ANDROID_BRIDGE=1: every release's halo-android-release.zip, the
 * file every installed build's updater asks for, so that a phone that comes back at any time reaches it. It has no
 * game: only a notice (BridgeActivity), which downloads the new app and hands it to the installer, then offers to
 * open it. Its MigrationProvider hands over the player's files, which stay where they are through the update to it.
 *
 * The new side: at its first start with the old app installed (LauncherActivity), the files are copied here, each to a
 * .part then into place, and only once all have arrived is .migrated written and the old app's removal offered. The
 * old app's data is never touched, so a failure loses nothing and can be tried again. Android asks the player to allow
 * each install and each removal; nothing here can do either unasked.
 */
final class Migration {
    static final int UNINSTALL_OLD = 7301;
    private static final String DONE = ".migrated";
    private static final String BEGUN = ".migrating";
    /** room kept free past what is copied */
    private static final long SPARE_BYTES = 64L << 20;

    private Migration() {
    }

    static boolean installed(Context context, String name) {
        try {
            context.getPackageManager().getPackageInfo(name, 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    /* ---------- the new app's side */

    /** whether this start should offer to bring the old app's files here */
    static boolean importWanted(Context context, File root) {
        return !BuildConfig.HALO_BRIDGE && BuildConfig.APPLICATION_ID.equals(BuildConfig.HALO_NEW_PACKAGE) && root != null
            && !new File(root, DONE).exists() && installed(context, BuildConfig.HALO_OLD_PACKAGE);
    }

    private static final class Entry {
        final String path;
        final long size;

        Entry(String path, long size) {
            this.path = path;
            this.size = size;
        }
    }

    private static Uri providerUri(String segment) {
        return Uri.parse("content://" + BuildConfig.HALO_OLD_PACKAGE + ".migration/" + segment);
    }

    private static void mark(File root, String name) throws IOException {
        try (OutputStream output = new FileOutputStream(new File(root, name))) {
            output.write('1');
        }
    }

    /** brings the files over, then calls done (after the old app's removal, if it is offered and taken: the activity) */
    static void begin(Activity activity, File root, Runnable done) {
        boolean played = new File(root, "config.toml").isFile() && !new File(root, BEGUN).exists();

        if (!played) {
            copyAll(activity, root, done);
            return;
        }
        // (this app has been played already: its files are not replaced unasked)
        new AlertDialog.Builder(activity)
            .setTitle("Your old Halo's data")
            .setMessage("This app already has data of its own. Replace it with the saves, profiles and settings of your "
                + "old Halo app?")
            .setCancelable(false)
            .setPositiveButton("Replace", (dialog, which) -> copyAll(activity, root, done))
            .setNegativeButton("Keep this app's", (dialog, which) -> {
                try {
                    mark(root, DONE);
                } catch (IOException e) {
                    android.util.Log.i("halo", "migration: " + e);
                }
                done.run();
            })
            .show();
    }

    private static List<Entry> listOld(Context context) throws IOException {
        List<Entry> entries = new ArrayList<>();

        try (Cursor cursor = context.getContentResolver().query(providerUri("files"), null, null, null, null)) {
            if (cursor == null)
                throw new IOException("your old Halo app needs its last update first: open it once, then try again");
            while (cursor.moveToNext())
                entries.add(new Entry(cursor.getString(0), cursor.getLong(1)));
        }
        return entries;
    }

    /** what is to be copied: all there is room for, the maps the ISO can remake given up first */
    private static List<Entry> chosen(List<Entry> all, long room, List<String> leftOut) {
        List<Entry> entries = new ArrayList<>(all);
        long total = 0;

        for (Entry entry : entries)
            total += entry.size;
        if (total > room) {
            for (int index = entries.size() - 1; index >= 0; index--) {
                if (MigrationFiles.remade(entries.get(index).path))
                    total -= entries.remove(index).size;
            }
            leftOut.add("the game's maps (pick your ISO again)");
        }
        if (total > room) {
            for (int index = entries.size() - 1; index >= 0; index--) {
                if (MigrationFiles.large(entries.get(index).path))
                    total -= entries.remove(index).size;
            }
            leftOut.add("your custom maps (they stay in the old app until you remove it)");
        }
        // (the small ones first: a phone that runs out, or a player who gives up, has the saves)
        Collections.sort(entries, (first, second) -> Long.compare(first.size, second.size));
        return entries;
    }

    private static void copyOne(Activity activity, File root, Entry entry, byte[] buffer, Runnable progress)
        throws IOException {
        File target = entry.path.startsWith(MigrationFiles.PREFERENCES)
            ? new File(new File(activity.getApplicationInfo().dataDir, "shared_prefs"),
                entry.path.substring(MigrationFiles.PREFERENCES.length()))
            : new File(root, entry.path);
        File part = new File(target.getPath() + ".part");
        File folder = target.getParentFile();
        long copied = 0;

        if (folder != null && !folder.isDirectory() && !folder.mkdirs())
            throw new IOException("the folder of " + entry.path + " cannot be made");
        Uri uri = providerUri("file").buildUpon().appendQueryParameter(MigrationProvider.PATH, entry.path).build();
        try (InputStream input = activity.getContentResolver().openInputStream(uri);
             OutputStream output = new FileOutputStream(part)) {
            if (input == null)
                throw new IOException(entry.path + " cannot be read");
            int count;
            while ((count = input.read(buffer)) > 0) {
                output.write(buffer, 0, count);
                copied += count;
                progress.run();
            }
        }
        if (copied != entry.size) {
            part.delete();
            throw new IOException(entry.path + " came to " + copied + " bytes, not " + entry.size);
        }
        if (!part.renameTo(target)) {
            part.delete();
            throw new IOException(entry.path + " cannot be put in its place");
        }
    }

    private static void copyAll(Activity activity, File root, Runnable done) {
        LinearLayout layout = new LinearLayout(activity);
        TextView status = new TextView(activity);
        ProgressBar bar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        int padding = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 24,
            activity.getResources().getDisplayMetrics());

        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(padding, padding / 2, padding, 0);
        status.setText("Bringing over your saves and settings...");
        bar.setMax(1000);
        layout.addView(status);
        layout.addView(bar);
        AlertDialog dialog = new AlertDialog.Builder(activity)
            .setTitle("Moving from your old Halo app")
            .setView(layout)
            .setCancelable(false)
            .show();

        new Thread(() -> {
            List<String> leftOut = new ArrayList<>();

            try {
                mark(root, BEGUN);
                List<Entry> entries = chosen(listOld(activity), root.getUsableSpace() - SPARE_BYTES, leftOut);
                long total = 0;

                for (Entry entry : entries)
                    total += entry.size;
                long sizes = Math.max(1, total);
                long[] finished = { 0 };
                byte[] buffer = new byte[1 << 20];
                for (Entry entry : entries) {
                    copyOne(activity, root, entry, buffer, () -> { });
                    finished[0] += entry.size;
                    activity.runOnUiThread(() -> bar.setProgress((int) (finished[0] * 1000 / sizes)));
                }
                mark(root, DONE);
                new File(root, BEGUN).delete();
                android.util.Log.i("halo", "migration: " + entries.size() + " files, " + (total >> 20) + " MB");
                activity.runOnUiThread(() -> {
                    dialog.dismiss();
                    finished(activity, leftOut, done);
                });
            } catch (IOException | RuntimeException e) {
                android.util.Log.i("halo", "migration: failed: " + e);
                activity.runOnUiThread(() -> {
                    dialog.dismiss();
                    new AlertDialog.Builder(activity)
                        .setTitle("Moving from your old Halo app")
                        .setMessage("Your data could not be brought over. Nothing was lost: it is still in the old "
                            + "app.\n\n" + e.getMessage())
                        .setCancelable(false)
                        .setPositiveButton("Try again", (again, which) -> copyAll(activity, root, done))
                        .setNegativeButton("Not now", (later, which) -> done.run())
                        .show();
                });
            }
        }, "migration").start();
    }

    private static void finished(Activity activity, List<String> leftOut, Runnable done) {
        String left = leftOut.isEmpty() ? "" : "\n\nThere was no room for " + String.join(" or ", leftOut) + ".";

        new AlertDialog.Builder(activity)
            .setTitle("Moved")
            .setMessage("Your saves and settings are here now." + left + "\n\nRemove your old Halo app? This app replaces "
                + "it, and it takes up room.")
            .setCancelable(false)
            .setPositiveButton("Remove it", (dialog, which) -> activity.startActivityForResult(
                new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + BuildConfig.HALO_OLD_PACKAGE)), UNINSTALL_OLD))
            .setNegativeButton("Later", (dialog, which) -> done.run())
            .show();
    }
}
