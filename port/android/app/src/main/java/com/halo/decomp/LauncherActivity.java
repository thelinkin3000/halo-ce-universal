package com.halo.decomp;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.nio.channels.FileChannel;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Starts the game once its data is in place.
 *
 * The game reads the Xbox game data (the folder holding maps/) from the
 * app's external files directory, /sdcard/Android/data/com.halo.decomp/files.
 * If it is missing, this screen lets the player pick an Xbox disc image of
 * the game (.xiso or .iso, any version) with the system file picker, and
 * copies its maps folder there (XisoExtractor), as the desktop games do; or
 * they can push the maps folder with adb.
 *
 * Before the game starts, on an Adreno whose data folder lacks it, the
 * open-source Vulkan driver for its series is downloaded
 * (Updater.driverDownload), its progress on screen, so that the game finds
 * it when the player chooses it (Video Setup's VULKAN DRIVER lists it by its name). A failed download does not stop the game,
 * and "Skip" starts it at once.
 *
 * With debug.profile_run in config.toml, each start makes the run's profile
 * folder, Download/halo_profiles/DDMMYYYY_HHMMSS (on Android 9 and 10 once
 * storage is allowed, which is asked the first time), with the build and the
 * phone in run.txt and the settings in config.toml, and names it in
 * profile_folder.txt for the game (port/android/host/host_main.c), which
 * writes its log and profiles there.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_IMAGE = 1;
    private static final int PROFILE_STORAGE = 2;

    private File dataRoot;
    private TextView status;
    private ProgressBar progress;
    private Button pick;
    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        dataRoot = getExternalFilesDir(null);
        // created by the app, so that files pushed into it with adb stay
        // readable (a directory adb creates there belongs to the shell user)
        if (dataRoot != null)
            new File(dataRoot, "maps").mkdirs();
        passOnHardwareId();
        passOnInvite(getIntent());
        Updater.moveToVulkanDefault(dataRoot);
        if (haveData()) {
            startGameAfterDriver();
            return;
        }
        buildInterface();
    }

    /**
     * An internet play invite link the app was opened with: the game
     * (port/linux/src/p2p.c) picks it up from join_link.txt, whether it is
     * starting now or already running.
     */
    private void passOnInvite(Intent intent) {
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction()) || intent.getData() == null
            || dataRoot == null)
            return;
        // written whole under another name, then renamed: the game never
        // reads it half written
        File partial = new File(dataRoot, "join_link.txt.tmp");
        try (OutputStream out = new FileOutputStream(partial)) {
            out.write(intent.getData().toString().getBytes("UTF-8"));
        } catch (java.io.IOException e) {
            // the link is lost; the player can copy it instead
            partial.delete();
            return;
        }
        if (!partial.renameTo(new File(dataRoot, "join_link.txt")))
            partial.delete();
    }

    /**
     * This device's ANDROID_ID (the app's own: one per app signing key and
     * user, until a factory reset), which native code cannot read: the game
     * (port/linux/src/p2p.c) hashes it from hardware_id.txt into the
     * hardware id a host it joins is told.
     */
    private void passOnHardwareId() {
        String id;

        if (dataRoot == null)
            return;
        try {
            id = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
        } catch (RuntimeException e) {
            return;
        }
        if (id == null || id.isEmpty())
            return;
        File partial = new File(dataRoot, "hardware_id.txt.tmp");
        try (OutputStream out = new FileOutputStream(partial)) {
            out.write(id.getBytes("UTF-8"));
        } catch (java.io.IOException e) {
            partial.delete();
            return;
        }
        if (!partial.renameTo(new File(dataRoot, "hardware_id.txt")))
            partial.delete();
    }

    private boolean haveData() {
        return dataRoot != null && new File(dataRoot, "maps/ui.map").isFile();
    }

    private boolean gameStarted;
    private boolean profileStorageAsked;

    private void startGame() {
        if (gameStarted)
            return;
        if (!profileFolderReady())
            return;
        gameStarted = true;
        startActivity(new Intent(this, HaloActivity.class));
        finish();
    }

    /* ---------- a profiled run's folder (debug.profile_run) */

    /**
     * The run's profile folder made and named for the game, when the settings ask for one; false while storage is
     * being asked for (Android 9 and 10: the answer starts the game, whatever it is).
     */
    private boolean profileFolderReady() {
        File config = dataRoot != null ? new File(dataRoot, "config.toml") : null;
        File named = dataRoot != null ? new File(dataRoot, "profile_folder.txt") : null;
        String value = config != null && config.isFile() ? Updater.readSetting(config, "debug", "profile_run") : null;

        if (named == null)
            return true;
        named.delete();
        if (value == null || !value.startsWith("true"))
            return true;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R
            && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED
            && !profileStorageAsked) {
            profileStorageAsked = true;
            requestPermissions(new String[] { Manifest.permission.WRITE_EXTERNAL_STORAGE }, PROFILE_STORAGE);
            return false;
        }
        String stamp = new SimpleDateFormat("ddMMyyyy_HHmmss", Locale.US).format(new Date());
        File folder = new File(new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
            "halo_profiles"), stamp);
        // (not made: the game makes one in the data folder, and its log says so)
        if (!folder.mkdirs() && !folder.isDirectory())
            return true;
        String text = "Halo for Android, build " + BuildConfig.HALO_BUILD_NUMBER + " (" + BuildConfig.VERSION_NAME + ")\n"
            + "started " + stamp + "\n"
            + "phone: " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ")\n"
            + "android: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")\n"
            + (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? "soc: " + Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL + "\n" : "")
            + "abis: " + String.join(", ", Build.SUPPORTED_ABIS) + "\n";
        try (OutputStream out = new FileOutputStream(new File(folder, "run.txt"))) {
            out.write(text.getBytes("UTF-8"));
        } catch (java.io.IOException e) {
            return true;
        }
        copyFile(config, new File(folder, "config.toml"));
        try (OutputStream out = new FileOutputStream(named)) {
            out.write(folder.getAbsolutePath().getBytes("UTF-8"));
        } catch (java.io.IOException e) {
            named.delete();
        }
        return true;
    }

    private static void copyFile(File from, File to) {
        try (FileInputStream in = new FileInputStream(from); FileOutputStream out = new FileOutputStream(to)) {
            byte[] buffer = new byte[65536];
            int got;

            while ((got = in.read(buffer)) > 0)
                out.write(buffer, 0, got);
        } catch (java.io.IOException e) {
            // (the settings are in the game's log too)
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == PROFILE_STORAGE)
            startGame();
    }

    /* ---------- the Vulkan driver, before the game */

    private boolean driverStarted;

    /** the game started once this phone's driver is downloaded, when it has one and lacks it */
    private void startGameAfterDriver() {
        if (driverStarted)
            return;
        driverStarted = true;
        if (Updater.driverReady(dataRoot)) {
            startGame();
            return;
        }
        new Thread(() -> {
            Updater.Choice choice = Updater.driverChoose(dataRoot);

            if (choice == null || choice.driver == null || Updater.driverPresent(dataRoot, choice.driver)) {
                handler.post(this::startGame);
                return;
            }
            handler.post(() -> buildDriverInterface(choice));
            try {
                Updater.driverDownload(dataRoot, choice.driver, choice.gpu, (received, total) -> report(
                    "Downloading the driver... (" + (received >> 10) + " of " + (total >> 10) + " KB)",
                    total > 0 ? (int) (received * 1000 / total) : 0));
                handler.post(this::startGame);
            } catch (Exception exception) {
                report("The driver could not be downloaded: " + exception.getMessage()
                    + "\nStarting the game. The download is tried again at the next start.", -1);
                handler.postDelayed(this::startGame, 3000);
            }
        }, "driver download").start();
    }

    private void buildDriverInterface(Updater.Choice choice) {
        if (gameStarted)
            return;
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(dp(48), dp(24), dp(48), dp(24));
        layout.setBackgroundColor(Color.rgb(12, 16, 20));

        TextView title = new TextView(this);
        title.setText("Downloading the Vulkan driver");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        TextView message = new TextView(this);
        message.setText("This phone's GPU (" + choice.gpu + ") can run the game's Vulkan renderer on Turnip, the "
            + "open-source Vulkan driver for Adreno GPUs. It is downloaded once into the freedreno_drivers folder of "
            + "the game's data folder (about " + ((choice.driver.size + (1 << 19)) >> 20) + " MB), and is not used "
            + "until you choose it: in the game's Video Setup, VULKAN DRIVER lists it as "
            + choice.driver.name.replaceFirst("\\.zip$", "") + ". Other driver archives put in that folder, or "
            + "added with IMPORT DRIVER, are listed there too.");
        message.setTextColor(Color.rgb(200, 205, 210));
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, dp(16), 0, dp(16));
        layout.addView(message);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        LinearLayout.LayoutParams progressLayout = new LinearLayout.LayoutParams(dp(480),
            LinearLayout.LayoutParams.WRAP_CONTENT);
        progressLayout.topMargin = dp(16);
        layout.addView(progress, progressLayout);

        status = new TextView(this);
        status.setText("Connecting...");
        status.setTextColor(Color.rgb(160, 200, 160));
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(8), 0, dp(16));
        layout.addView(status);

        // (the download goes on; the game starting first uses the phone's driver for this run)
        Button skip = new Button(this);
        skip.setText("Skip");
        skip.setOnClickListener(v -> startGame());
        layout.addView(skip, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT));

        setContentView(layout);
        skip.requestFocus();
    }

    private int dp(float value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
            getResources().getDisplayMetrics());
    }

    private void buildInterface() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(dp(48), dp(24), dp(48), dp(24));
        layout.setBackgroundColor(Color.rgb(12, 16, 20));

        TextView title = new TextView(this);
        title.setText("Halo needs its game data");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);

        TextView message = new TextView(this);
        message.setText("Choose an Xbox disc image of Halo: Combat Evolved (an .iso or .xiso file, any "
            + "version) on this device. Its maps folder is copied into the app's storage (about 1.8 GB), "
            + "and you can delete the image afterwards.\n\n"
            + "You can also copy a maps folder from a computer:\n"
            + "adb push <folder with maps>/. " + (dataRoot != null ? dataRoot.getAbsolutePath() : "") + "/");
        message.setTextColor(Color.rgb(200, 205, 210));
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, dp(16), 0, dp(16));
        layout.addView(message);

        pick = new Button(this);
        pick.setText("Choose disc image");
        pick.setOnClickListener(v -> {
            // (disc images have no MIME type of their own: any file, checked
            // when it is read)
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, PICK_IMAGE);
        });
        layout.addView(pick, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
            LinearLayout.LayoutParams.WRAP_CONTENT));

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams progressLayout = new LinearLayout.LayoutParams(dp(480),
            LinearLayout.LayoutParams.WRAP_CONTENT);
        progressLayout.topMargin = dp(16);
        layout.addView(progress, progressLayout);

        status = new TextView(this);
        status.setTextColor(Color.rgb(160, 200, 160));
        status.setGravity(Gravity.CENTER);
        status.setPadding(0, dp(8), 0, 0);
        layout.addView(status);

        setContentView(layout);
        pick.requestFocus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // data pushed with adb while this screen was open
        if (pick != null && pick.isEnabled() && haveData())
            startGameAfterDriver();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_IMAGE || resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        Uri image = data.getData();
        pick.setEnabled(false);
        progress.setVisibility(View.VISIBLE);
        status.setText("Reading the disc image...");
        new Thread(() -> importImage(image)).start();
    }

    private void report(String text, int permille) {
        handler.post(() -> {
            if (status != null)
                status.setText(text);
            if (permille >= 0 && progress != null)
                progress.setProgress(permille);
        });
    }

    private void fail(String text) {
        handler.post(() -> {
            status.setText(text);
            progress.setVisibility(View.GONE);
            pick.setEnabled(true);
            pick.requestFocus();
        });
    }

    private void importImage(Uri image) {
        try (ParcelFileDescriptor descriptor = getContentResolver().openFileDescriptor(image, "r")) {
            if (descriptor == null)
                throw new java.io.IOException("the file could not be opened");
            try (FileInputStream in = new FileInputStream(descriptor.getFileDescriptor())) {
                FileChannel channel = in.getChannel();

                XisoExtractor.extractMaps(channel, dataRoot, (file, done, total) ->
                    report("Extracting maps/" + file + " (" + (done >> 20) + " of " + (total >> 20) + " MB)",
                        total > 0 ? (int) (done * 1000 / total) : 0));
            }
            handler.post(() -> {
                if (haveData()) {
                    startGameAfterDriver();
                } else {
                    fail("The extraction finished but maps/ui.map is missing.");
                }
            });
        } catch (XisoExtractor.ExtractException exception) {
            fail(exception.getMessage());
        } catch (Exception exception) {
            fail("Extracting failed: " + exception.getMessage());
        }
    }
}
