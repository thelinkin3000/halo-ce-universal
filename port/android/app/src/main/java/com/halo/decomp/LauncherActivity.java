package com.halo.decomp;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
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
 * it when the player turns it on (display.vk_driver = "auto"). A failed download does not stop the game,
 * and "Skip" starts it at once.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_IMAGE = 1;

    private File dataRoot;
    private TextView status;
    private ProgressBar progress;
    private Button pick;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private CheckpointStore checkpointStore;
    private CheckpointSlots checkpointSlots;
    private boolean checkpointChoiceStarted, checkpointPreparing;
    private Thread checkpointWorker;
    private boolean demoRequested;
    private String demoBootId;
    private boolean gameMenuRequested;
    private String gameMenuBootId;

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
        if (haveData() && (hasRunningGame()
                || (Intent.ACTION_VIEW.equals(getIntent().getAction()) && getIntent().getData() != null)
                || getIntent().getBooleanExtra("resume-after-graphics", false))) {
            demoRequested = !hasRunningGame() && getIntent().getBooleanExtra("launch-demo", false);
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

    void startGame() {
        if (gameStarted || checkpointChoiceStarted || isFinishing() || isDestroyed())
            return;
        if (hasRunningGame()) {
            if (gameMenuRequested) {
                gameMenuRequested = false;
                driverStarted = false;
                gameMenuBlocked();
            } else {
                launchGame(null);
            }
            return;
        }
        checkpointChoiceStarted = true;
        boolean invite = Intent.ACTION_VIEW.equals(getIntent().getAction()) && getIntent().getData() != null;
        checkpointWorker = new Thread(() -> {
            try {
                DemoLaunch demo = new DemoLaunch(dataRoot, getFilesDir());
                demo.recover();
                CheckpointStore store = checkpointStore();
                // A request abandoned before the engine started must not be replayed on a later launch.
                store.clearAbandonedRequests();
                if (demoRequested || gameMenuRequested) {
                    String id = CheckpointStore.newId();
                    store.writeBoot(id, false);
                    if (gameMenuRequested)
                        demo.prepareMenu(id);
                    else
                        demo.prepare(id);
                    handler.post(() -> {
                        if (gameMenuRequested)
                            gameMenuBootId = id;
                        else
                            demoBootId = id;
                        launchGame(null);
                    });
                    return;
                }
                CheckpointStore.Candidate candidate = invite ? null : store.inspect();
                java.util.List<CheckpointSlots.Entry> saves = java.util.Collections.emptyList();
                if (!invite) {
                    if (candidate.kind == CheckpointStore.Kind.READY || candidate.kind == CheckpointStore.Kind.INCOMPATIBLE)
                        checkpointSlots().preserveLegacy();
                    saves = checkpointSlots().list();
                }
                java.util.List<CheckpointSlots.Entry> choices = saves;
                handler.post(() -> {
                    if (isFinishing() || isDestroyed())
                        return;
                    if (candidate == null)
                        launchGame(null);
                    else if (!choices.isEmpty())
                        showSavedGames(choices);
                    else if (candidate.kind == CheckpointStore.Kind.ABSENT)
                        launchGame(null);
                    else
                        showCheckpointChoice(candidate);
                });
            } catch (java.io.IOException e) {
                if (demoRequested || gameMenuRequested) {
                    try {
                        checkpointStore().clearBoot();
                        new DemoLaunch(dataRoot, getFilesDir()).recover();
                    } catch (java.io.IOException ignored) { }
                }
                handler.post(() -> {
                    if (isFinishing() || isDestroyed())
                        return;
                    if (demoRequested || gameMenuRequested) {
                        boolean menu = gameMenuRequested;
                        checkpointChoiceStarted = false;
                        driverStarted = false;
                        demoRequested = false;
                        gameMenuRequested = false;
                        new AlertDialog.Builder(this).setTitle(menu ? "Menú no disponible" : "Demo no disponible")
                            .setMessage(menu ? "No se pudo preparar el menú de Halo. Se conservaron tus datos."
                                : "No se pudo preparar la demo. Comprueba que sus datos estén instalados.")
                            .setPositiveButton("Aceptar", null).show();
                    } else {
                        checkpointLaunchError(null);
                    }
                });
            }
        }, "checkpoint startup");
        checkpointWorker.start();
    }

    void startDemo() {
        if (gameStarted || checkpointChoiceStarted || driverStarted || isFinishing() || isDestroyed()
                || DataPackages.isBusy())
            return;
        if (hasRunningGame()) {
            new AlertDialog.Builder(this).setTitle("Hay una partida abierta")
                .setMessage("Guarda y sal de la partida antes de iniciar la demo.")
                .setPositiveButton("Volver a la partida", (d, which) -> launchGame(null))
                .setNegativeButton("Cancelar", null).show();
            return;
        }
        DataPackages.playDemo(this, () -> {
            if (isFinishing() || isDestroyed())
                return;
            gameMenuRequested = false;
            demoRequested = true;
            startGameAfterDriver();
        }, dialog -> dialog.show());
    }

    void startGameMenu() {
        if (gameStarted || checkpointChoiceStarted || driverStarted || isFinishing() || isDestroyed()
                || DataPackages.isBusy() || !haveData())
            return;
        if (hasRunningGame()) {
            gameMenuBlocked();
            return;
        }
        demoRequested = false;
        gameMenuRequested = true;
        startGameAfterDriver();
    }

    private void gameMenuBlocked() {
        new AlertDialog.Builder(this).setTitle("Hay una partida abierta")
            .setMessage("Guarda y sal de la partida antes de abrir la configuración interna de Halo.")
            .setPositiveButton("Aceptar", null).show();
    }

    private boolean haveDemo() {
        return dataRoot != null && GameDataPackage.demoAvailable(dataRoot);
    }

    private CheckpointStore checkpointStore() throws java.io.IOException {
        if (checkpointStore == null)
            checkpointStore = new CheckpointStore(dataRoot, name -> getAssets().open(name));
        return checkpointStore;
    }

    private CheckpointSlots checkpointSlots() throws java.io.IOException {
        if (checkpointSlots == null)
            checkpointSlots = new CheckpointSlots(dataRoot, checkpointStore());
        return checkpointSlots;
    }

    void showSavedGames(java.util.List<CheckpointSlots.Entry> saves) {
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle("Partidas guardadas")
            .setAdapter(new LauncherSaveAdapter(this, saves), (d, which) -> {
                CheckpointSlots.Entry entry = saves.get(which);
                if (entry.candidate.kind == CheckpointStore.Kind.READY) {
                    prepareCheckpointBoot(entry.candidate, true, entry.id);
                } else {
                    AlertDialog unavailable = new AlertDialog.Builder(this).setTitle("Partida no disponible")
                        .setMessage("El guardado se conserva, pero está dañado o pertenece a otro motor gráfico o versión.")
                        .setPositiveButton("Volver", (back, button) -> showSavedGames(saves)).show();
                    unavailable.setOnCancelListener(back -> resetLaunchChoice());
                }
            })
            .setNeutralButton("Nueva partida", (d, which) -> prepareCheckpointBoot(null, false))
            .setNegativeButton("Volver", (d, which) -> resetLaunchChoice()).create();
        dialog.setOnCancelListener(d -> resetLaunchChoice());
        dialog.show();
        LauncherDialogs.style(dialog);
    }

    private void resetLaunchChoice() {
        checkpointChoiceStarted = false;
        driverStarted = false;
    }

    boolean hasRunningGame() {
        ActivityManager manager = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
        if (manager == null)
            return false;
        try {
            boolean running = false;
            java.util.List<ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
            if (processes != null)
                for (ActivityManager.RunningAppProcessInfo process : processes)
                    running |= process.uid == android.os.Process.myUid()
                        && process.processName.equals(getPackageName() + ":game");
            if (running)
                for (ActivityManager.AppTask task : manager.getAppTasks()) {
                    ActivityManager.RecentTaskInfo info = task.getTaskInfo();
                    android.content.ComponentName component = info.baseIntent.getComponent();
                    if (info.numActivities > 0 && component != null
                            && component.getClassName().equals(HaloActivity.class.getName()))
                        return true;
                }
        } catch (SecurityException ignored) { }
        return false;
    }

    void showCheckpointChoice(CheckpointStore.Candidate candidate) {
        if (isFinishing() || isDestroyed())
            return;
        String message = candidate.kind == CheckpointStore.Kind.READY
            ? "Puedes continuar desde el último punto de control. Nueva partida conserva ese guardado hasta que guardes otro."
            : candidate.kind == CheckpointStore.Kind.INCOMPATIBLE
                ? "El guardado pertenece a otra versión del juego. Se conserva, pero esta APK no puede continuarlo."
                : "El archivo de guardado está incompleto o dañado. Se conserva y no se cargará.";
        AlertDialog dialog = new AlertDialog.Builder(this)
            .setTitle("Partida guardada")
            .setMessage(message)
            .setPositiveButton("Continuar", (d, which) -> prepareCheckpointBoot(candidate, true))
            .setNeutralButton("Nueva partida", (d, which) -> prepareCheckpointBoot(candidate, false))
            .setNegativeButton("Cancelar", (d, which) -> finish())
            .create();
        dialog.setCanceledOnTouchOutside(false);
        dialog.setOnCancelListener(d -> finish());
        dialog.show();
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(candidate.kind == CheckpointStore.Kind.READY);
    }

    private void prepareCheckpointBoot(CheckpointStore.Candidate candidate, boolean resume) {
        prepareCheckpointBoot(candidate, resume, null);
    }

    private void prepareCheckpointBoot(CheckpointStore.Candidate candidate, boolean resume, String slotId) {
        if (checkpointPreparing || gameStarted || isFinishing() || isDestroyed())
            return;
        checkpointPreparing = true;
        AlertDialog preparing = new AlertDialog.Builder(this)
            .setMessage("Preparando la partida…").setCancelable(false).create();
        preparing.show();
        String id = CheckpointStore.newId();
        checkpointWorker = new Thread(() -> {
            try {
                if (slotId != null)
                    checkpointSlots().activate(slotId);
                checkpointStore().writeBoot(id, resume);
                handler.post(() -> {
                    if (isFinishing() || isDestroyed())
                        return;
                    preparing.dismiss();
                    launchGame(resume ? id : null);
                });
            } catch (java.io.IOException e) {
                handler.post(() -> {
                    preparing.dismiss();
                    if (isFinishing() || isDestroyed())
                        return;
                    checkpointPreparing = false;
                    if (slotId == null) {
                        checkpointLaunchError(candidate);
                    } else {
                        resetLaunchChoice();
                        new AlertDialog.Builder(this).setTitle("No se pudo abrir la partida")
                            .setMessage("Tus partidas se conservan. Comprueba el espacio disponible y vuelve a intentarlo.")
                            .setPositiveButton("Aceptar", null).show();
                    }
                });
            }
        }, "checkpoint boot request");
        checkpointWorker.start();
    }

    private void checkpointLaunchError(CheckpointStore.Candidate candidate) {
        if (isFinishing() || isDestroyed())
            return;
        AlertDialog.Builder dialog = new AlertDialog.Builder(this).setTitle("Guardado no disponible")
            .setMessage("No se pudo preparar la partida. Tu archivo guardado se conserva.")
            .setNegativeButton("Salir", (d, which) -> finish());
        if (candidate != null)
            dialog.setPositiveButton("Volver", (d, which) -> showCheckpointChoice(candidate));
        dialog.show().setOnCancelListener(d -> finish());
    }

    private void launchGame(String bootId) {
        if (gameStarted || isFinishing() || isDestroyed())
            return;
        gameStarted = true;
        Intent game = new Intent(this, HaloActivity.class);
        if (bootId != null)
            game.putExtra(HaloActivity.CHECKPOINT_BOOT_ID, bootId);
        if (demoBootId != null)
            game.putExtra(HaloActivity.DEMO_BOOT_ID, demoBootId);
        if (gameMenuBootId != null)
            game.putExtra(HaloActivity.GAME_MENU_BOOT_ID, gameMenuBootId);
        startActivity(game);
        finish();
    }

    @Override
    protected void onDestroy() {
        if (checkpointWorker != null)
            checkpointWorker.interrupt();
        super.onDestroy();
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
            + "open-source Vulkan driver for Adreno GPUs. It is downloaded once into the game's data folder (about "
            + ((choice.driver.size + (1 << 19)) >> 20) + " MB), and is not used until you turn it on: set vk_driver = "
            + "\"auto\" in the [display] section of config.toml (the README says how).");
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
        LauncherScreen screen = new LauncherScreen(this, haveData(), haveDemo(), () -> {
            if (DataPackages.isBusy())
                return;
            if (haveData())
                startGameAfterDriver();
            else
                DataPackages.show(this, this::buildInterface, dialog -> dialog.show());
        }, this::startDemo, () -> {
            if (DataPackages.isBusy())
                return;
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, PICK_IMAGE);
        }, () -> {
            if (!DataPackages.isBusy())
                DataPackages.show(this, this::buildInterface, dialog -> dialog.show());
        }, () -> ControlsUpdater.show(this, dialog -> dialog.show()));
        screen.addGameMenuAction(haveData(), this::startGameMenu);
        pick = screen.pick;
        progress = screen.progress;
        status = screen.status;
        setContentView(screen);
        (screen.play.isEnabled() ? screen.play : screen.demo).requestFocus();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // data pushed with adb while this screen was open
        if (pick != null && pick.isEnabled() && !DataPackages.isBusy() && haveData())
            buildInterface();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_IMAGE || resultCode != RESULT_OK || data == null || data.getData() == null)
            return;
        if (DataPackages.isBusy())
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
