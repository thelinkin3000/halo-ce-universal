package com.halo.decomp;

import android.app.AlertDialog;
import android.annotation.TargetApi;
import android.content.Context;
import android.content.Intent;
import android.graphics.Insets;
import android.graphics.Color;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Toast;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.EditText;
import android.widget.ScrollView;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

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
    private static final int EXPORT_LAYOUT = 401, IMPORT_LAYOUT = 402;
    private String pendingLayoutExport;
    private AlertDialog touchPauseDialog;
    private boolean activityResumed;
    private boolean originalMenuRequested;
    static final String CHECKPOINT_BOOT_ID = "checkpoint-boot-id";
    static final String DEMO_BOOT_ID = "demo-boot-id";
    static final String GAME_MENU_BOOT_ID = "game-menu-boot-id";
    private final Handler checkpointHandler = new Handler(Looper.getMainLooper());
    private CheckpointStore checkpointStore;
    private CheckpointSlots checkpointSlots;
    private volatile String checkpointRequestId;
    private boolean checkpointRestoring, checkpointExiting;
    private View checkpointProgress;
    private AlertDialog checkpointErrorDialog;
    private AlertDialog checkpointSaveDialog;
    private Thread checkpointWorker;
    private Runnable checkpointDeadline;
    private Thread demoWorker;

    void restartForGraphicsProfile() {
        Intent restart = new Intent(this, ProfileRestartActivity.class);
        restart.putExtra("game-pid", android.os.Process.myPid());
        restart.putExtra("launch-demo", getIntent().hasExtra(DEMO_BOOT_ID));
        startActivity(restart);
        finishAndRemoveTask();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (checkpointBusy())
            return true;
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && touchControls != null) {
            if (touchControls.isEditing()) {
                if (event.getAction() == KeyEvent.ACTION_UP)
                    touchControls.cancelEditor();
                return true;
            }
            if (touchControls.isGameplayVisible() && !event.isFromSource(InputDevice.SOURCE_GAMEPAD)
                    && !event.isFromSource(InputDevice.SOURCE_JOYSTICK)) {
                if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled())
                    showTouchPauseMenu();
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    void showTouchPauseMenu() {
        if (touchPauseDialog != null || checkpointBusy() || touchControls == null || isFinishing() || isDestroyed())
            return;
        Button save = new Button(this);
        save.setText("Guardar y salir");
        save.setOnClickListener(v -> startCheckpointSave());
        GameMenuStyle.content(save);
        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(24 * getResources().getDisplayMetrics().density);
        actions.setPadding(padding, 0, padding, padding / 3);
        actions.addView(save, new LinearLayout.LayoutParams(-1, -2));
        Button gameSettings = new Button(this);
        gameSettings.setText("Configuración interna de Halo");
        gameSettings.setOnClickListener(v -> openOriginalGameMenu());
        GameMenuStyle.content(gameSettings);
        actions.addView(gameSettings, new LinearLayout.LayoutParams(-1, -2));
        Button exit = new Button(this);
        exit.setText("Salir sin guardar");
        exit.setOnClickListener(v -> confirmExitWithoutSaving());
        GameMenuStyle.content(exit);
        actions.addView(exit, new LinearLayout.LayoutParams(-1, -2));
        ScrollView scroll = new ScrollView(this);
        scroll.addView(actions, new ScrollView.LayoutParams(-1, -2));
        FrameLayout content = new FrameLayout(this);
        float density = getResources().getDisplayMetrics().density;
        int height = Math.round(Math.max(48, Math.min(164,
            getResources().getDisplayMetrics().heightPixels / density - 220)) * density);
        content.addView(scroll, new FrameLayout.LayoutParams(-1, height));
        AlertDialog dialog = GameMenuStyle.builder(this)
            .setTitle("Pausa")
            .setMessage("Guarda el último punto de control, no la posición exacta al salir.")
            .setView(content)
            .setPositiveButton("Reanudar", null)
            .setNeutralButton("Ajustes de controles", (d, which) -> touchControls.showOptions())
            .setNegativeButton("Atrás", null)
            .create();
        touchPauseDialog = dialog;
        touchControls.stopDeviceInput();
        pauseNativeThread();
        dialog.setCanceledOnTouchOutside(false);
        dialog.setOnDismissListener(d -> {
            touchPauseDialog = null;
            resumeTouchGameIfReady();
        });
        dialog.show();
        GameMenuStyle.style(dialog);
        GameMenuStyle.primary(dialog.getButton(AlertDialog.BUTTON_POSITIVE));
    }

    private boolean touchOverlayPaused() {
        return touchPauseDialog != null || checkpointErrorDialog != null || checkpointSaveDialog != null || checkpointExiting
            || (touchControls != null && touchControls.isEditing());
    }

    private void confirmExitWithoutSaving() {
        if (checkpointBusy())
            return;
        AlertDialog confirm = GameMenuStyle.builder(this)
            .setTitle("¿Salir sin guardar?")
            .setMessage("Perderás el progreso desde el último punto guardado. Tus partidas guardadas se conservan.")
            .setNegativeButton("Cancelar", null)
            .setPositiveButton("Salir sin guardar", (d, which) -> {
                if (!checkpointBusy()) {
                    checkpointExiting = true;
                    exitCheckpointGame();
                }
            }).create();
        confirm.show();
        GameMenuStyle.style(confirm);
    }

    void openOriginalGameMenu() {
        if (checkpointBusy() || touchControls == null || isFinishing() || isDestroyed())
            return;
        originalMenuRequested = true;
        if (touchPauseDialog != null)
            touchPauseDialog.dismiss();
        else
            resumeTouchGameIfReady();
    }

    private void resumeTouchGameIfReady() {
        if (!touchOverlayPaused() && activityResumed && !isFinishing() && !isDestroyed()
                && getWindow().getDecorView().hasWindowFocus()) {
            resumeNativeThread();
            if (touchControls != null && !checkpointBusy())
                touchControls.startDeviceInput();
            if (originalMenuRequested && touchControls != null) {
                originalMenuRequested = false;
                touchControls.openGameMenu();
            }
        }
    }

    @Override
    protected void resumeNativeThread() {
        if (touchOverlayPaused())
            super.pauseNativeThread();
        else
            super.resumeNativeThread();
    }

    void setHudEditorPaused(boolean paused) {
        if (paused || touchPauseDialog != null)
            pauseNativeThread();
        else
            resumeTouchGameIfReady();
    }

    private boolean checkpointBusy() { return checkpointRequestId != null || checkpointExiting; }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        return checkpointBusy() || super.dispatchGenericMotionEvent(event);
    }

    private CheckpointStore checkpointStore() throws IOException {
        if (checkpointStore == null)
            checkpointStore = new CheckpointStore(getExternalFilesDir(null), name -> getAssets().open(name));
        return checkpointStore;
    }

    private CheckpointSlots checkpointSlots() throws IOException {
        if (checkpointSlots == null)
            checkpointSlots = new CheckpointSlots(getExternalFilesDir(null), checkpointStore());
        return checkpointSlots;
    }

    void startCheckpointSave() {
        if (checkpointBusy() || checkpointSaveDialog != null || isFinishing() || isDestroyed())
            return;
        AlertDialog loading = GameMenuStyle.builder(this).setTitle("Guardar partida")
            .setMessage("Leyendo las partidas guardadas…").setNegativeButton("Cancelar", null).create();
        showCheckpointSaveDialog(loading);
        checkpointWorker = new Thread(() -> {
            try {
                CheckpointSlots slots = checkpointSlots();
                slots.preserveLegacy();
                List<CheckpointSlots.Entry> entries = slots.list();
                checkpointHandler.post(() -> {
                    if (checkpointSaveDialog == loading && !isFinishing() && !isDestroyed())
                        showCheckpointSaveChoices(entries);
                });
            } catch (IOException e) {
                checkpointHandler.post(() -> {
                    if (checkpointSaveDialog == loading && !isFinishing() && !isDestroyed())
                        showCheckpointSaveDialog(GameMenuStyle.builder(this).setTitle("No se pudieron leer las partidas")
                            .setMessage("Tus guardados se conservan. Comprueba el almacenamiento e inténtalo de nuevo.")
                            .setPositiveButton("Volver a pausa", null).create());
                });
            }
        }, "checkpoint choices");
        checkpointWorker.start();
    }

    private void showCheckpointSaveChoices(List<CheckpointSlots.Entry> entries) {
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        Button create = new Button(this);
        create.setText("Nueva partida guardada");
        create.setEnabled(entries.size() < CheckpointSlots.MAX_SLOTS);
        create.setOnClickListener(v -> showCheckpointName());
        content.addView(create);
        for (CheckpointSlots.Entry entry : entries) {
            Button replace = new Button(this);
            replace.setText(SaveLabels.label(entry));
            replace.setOnClickListener(v -> showCheckpointSaveDialog(GameMenuStyle.builder(this)
                .setTitle("Reemplazar partida")
                .setMessage("¿Reemplazar «" + entry.name + "» con el último punto de control de esta partida?")
                .setPositiveButton("Reemplazar", (dialog, which) -> beginCheckpointSave(entry.id, entry.name))
                .setNegativeButton("Cancelar", null).create()));
            content.addView(replace);
        }
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        GameMenuStyle.content(scroll);
        showCheckpointSaveDialog(GameMenuStyle.builder(this).setTitle("Guardar partida")
            .setMessage(entries.size() >= CheckpointSlots.MAX_SLOTS
                ? "Hay 10 partidas. Elige una para reemplazarla."
                : "Crea otra partida o elige cuál reemplazar. Se guarda el último punto de control.")
            .setView(scroll).setNegativeButton("Cancelar", null).create());
    }

    private void showCheckpointName() {
        EditText name = new EditText(this);
        name.setSingleLine(true);
        name.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI
            | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        name.setHint("Nombre de la partida");
        GameMenuStyle.content(name);
        AlertDialog dialog = GameMenuStyle.builder(this).setTitle("Nueva partida guardada")
            .setView(name).setPositiveButton("Guardar y salir", null).setNegativeButton("Cancelar", null).create();
        showCheckpointSaveDialog(dialog);
        Runnable save = () -> {
            String raw = name.getText().toString();
            String text = raw.trim();
            boolean invalid = text.isEmpty() || text.getBytes(StandardCharsets.UTF_8).length > 160;
            for (int index = 0; index < raw.length(); index++)
                invalid |= Character.isISOControl(raw.charAt(index)) || Character.isSurrogate(raw.charAt(index));
            if (invalid)
                name.setError("Escribe un nombre corto sin saltos de línea.");
            else {
                InputMethodManager keyboard = (InputMethodManager) getSystemService(Context.INPUT_METHOD_SERVICE);
                if (keyboard != null)
                    keyboard.hideSoftInputFromWindow(name.getWindowToken(), 0);
                beginCheckpointSave(null, text);
            }
        };
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> save.run());
        name.setOnEditorActionListener((view, action, event) -> {
            if (action != EditorInfo.IME_ACTION_DONE)
                return false;
            save.run();
            return true;
        });
    }

    private void showCheckpointSaveDialog(AlertDialog dialog) {
        AlertDialog previous = checkpointSaveDialog;
        checkpointSaveDialog = dialog;
        if (touchControls != null)
            touchControls.stopDeviceInput();
        pauseNativeThread();
        dialog.setCanceledOnTouchOutside(false);
        dialog.setOnDismissListener(d -> {
            if (checkpointSaveDialog == dialog) {
                checkpointSaveDialog = null;
                if (!checkpointBusy() && !isFinishing() && !isDestroyed()) {
                    if (checkpointWorker != null)
                        checkpointWorker.interrupt();
                    showTouchPauseMenu();
                }
                resumeTouchGameIfReady();
            }
        });
        dialog.show();
        GameMenuStyle.style(dialog);
        if (previous != null)
            previous.dismiss();
        if (touchPauseDialog != null)
            touchPauseDialog.dismiss();
    }

    void beginCheckpointSave(String target, String name) {
        if (checkpointBusy() || isFinishing() || isDestroyed())
            return;
        String id = CheckpointStore.newId();
        beginCheckpointOperation(id, false);
        if (checkpointSaveDialog != null)
            checkpointSaveDialog.dismiss();
        if (touchPauseDialog != null)
            touchPauseDialog.dismiss();
        watchCheckpoint(id, false, target, name);
    }

    private void beginCheckpointOperation(String id, boolean restoring) {
        checkpointRequestId = id;
        checkpointRestoring = restoring;
        if (touchControls != null)
            touchControls.stopDeviceInput();
        LinearLayout progress = new LinearLayout(this);
        progress.setOrientation(LinearLayout.VERTICAL);
        progress.setGravity(Gravity.CENTER);
        progress.setBackgroundColor(Color.argb(235, 15, 23, 31));
        progress.setClickable(true);
        progress.setFocusable(true);
        progress.addView(new ProgressBar(this));
        TextView text = new TextView(this);
        text.setTextColor(Color.WHITE);
        text.setGravity(Gravity.CENTER);
        text.setPadding(24, 24, 24, 24);
        text.setText(restoring ? (getIntent().hasExtra(GAME_MENU_BOOT_ID) ? "Abriendo el menú de Halo…"
            : getIntent().hasExtra(DEMO_BOOT_ID) ? "Iniciando la demo…"
            : "Continuando desde el último punto de control…")
            : "Guardando el último punto de control…\nLa aplicación se cerrará cuando el juego lo confirme.");
        progress.addView(text);
        checkpointProgress = progress;
        // Keep the SDL window focused: a dialog would suspend the engine before it can acknowledge.
        addContentView(progress, new FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        checkpointDeadline = () -> completeCheckpoint(id, restoring ? "restore_timeout" : "save_timeout");
        checkpointHandler.postDelayed(checkpointDeadline, restoring ? 120000 : 15000);
    }

    private void watchCheckpoint(String id, boolean restoring) {
        watchCheckpoint(id, restoring, null, null);
    }

    private void watchCheckpoint(String id, boolean restoring, String target, String name) {
        checkpointWorker = new Thread(() -> {
            boolean saved = false;
            try {
                CheckpointStore store = checkpointStore();
                if (!restoring) {
                    checkpointSlots().preserveLegacy();
                    store.writeRequest(id);
                }
                while (!Thread.currentThread().isInterrupted()) {
                    String result = store.result(id, restoring);
                    if (result != null) {
                        if (!restoring && result.equals("saved")) {
                            saved = true;
                            if (!id.equals(checkpointRequestId) || Thread.currentThread().isInterrupted())
                                return;
                            checkpointHandler.post(() -> {
                                if (id.equals(checkpointRequestId)) {
                                    if (checkpointDeadline != null)
                                        checkpointHandler.removeCallbacks(checkpointDeadline);
                                    checkpointDeadline = () -> completeCheckpoint(id, "archive_error");
                                    checkpointHandler.postDelayed(checkpointDeadline, 30000);
                                }
                            });
                            if (target == null)
                                checkpointSlots().saveNew(name);
                            else
                                checkpointSlots().replace(target, name);
                        }
                        checkpointHandler.post(() -> completeCheckpoint(id, result));
                        return;
                    }
                    Thread.sleep(200);
                }
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            } catch (IOException e) {
                if (!Thread.currentThread().isInterrupted()) {
                    String result = saved ? "archive_error" : "io_error";
                    checkpointHandler.post(() -> completeCheckpoint(id, result));
                }
            }
        }, restoring ? "checkpoint restore" : "checkpoint save");
        checkpointWorker.start();
    }

    void completeCheckpoint(String id, String result) {
        if (!id.equals(checkpointRequestId) || isFinishing() || isDestroyed())
            return;
        boolean restoring = checkpointRestoring;
        stopCheckpointWatch();
        checkpointRequestId = null;
        if ((!restoring && result.equals("saved")) || (restoring && result.equals("resumed"))) {
            if (restoring) {
                removeCheckpointProgress();
                resumeTouchGameIfReady();
            } else {
                checkpointExiting = true;
                exitCheckpointGame();
            }
            return;
        }
        // A timeout or stale response must never close the game later.
        CheckpointStore store = checkpointStore;
        if (!restoring && store != null)
            new Thread(() -> {
                try { store.cancelRequest(id); }
                catch (IOException ignored) { }
            }, "cancel checkpoint request").start();
        String message;
        switch (result) {
            case "no_checkpoint":
                message = "Todavía no hay un punto de control. Avanza hasta que el juego guarde uno y vuelve a intentarlo.";
                break;
            case "unsupported":
                message = "Esta partida no admite el guardado. Solo se guarda la campaña individual con un punto de control.";
                break;
            case "corrupt":
                message = "El guardado está dañado o no pasó la comprobación. El archivo se conserva.";
                break;
            case "incompatible":
                message = "El guardado corresponde a otra versión o motor gráfico. El archivo se conserva; usa la misma APK y configuración gráfica.";
                break;
            case "save_timeout":
                message = "El juego no confirmó el guardado a tiempo. No se cerró la partida.";
                break;
            case "archive_error":
                message = "El juego escribió el punto de control, pero no se pudo añadir a tus partidas guardadas. "
                    + "La partida sigue abierta; se conservan la copia de recuperación y los guardados anteriores. Comprueba el espacio disponible.";
                break;
            case "restore_timeout":
                message = "El juego no confirmó la continuación a tiempo. Tu archivo guardado se conserva.";
                break;
            default:
                message = restoring ? "No se pudo leer el punto de control. Tu archivo guardado se conserva."
                    : "No se pudo escribir el punto de control. La partida sigue abierta.";
                break;
        }
        AlertDialog error = GameMenuStyle.builder(this)
            .setTitle(restoring ? "No se pudo continuar" : "No se pudo guardar")
            .setMessage(message)
            .setPositiveButton(restoring ? "Salir" : "Volver a pausa", null).create();
        checkpointErrorDialog = error;
        pauseNativeThread();
        removeCheckpointProgress();
        error.setCanceledOnTouchOutside(false);
        error.setOnDismissListener(d -> {
            checkpointErrorDialog = null;
            if (!isFinishing() && !isDestroyed()) {
                if (restoring) {
                    checkpointExiting = true;
                    exitCheckpointGame();
                } else {
                    showTouchPauseMenu();
                }
            }
        });
        error.show();
        GameMenuStyle.style(error);
    }

    private void removeCheckpointProgress() {
        if (checkpointProgress != null && checkpointProgress.getParent() instanceof ViewGroup)
            ((ViewGroup) checkpointProgress.getParent()).removeView(checkpointProgress);
        checkpointProgress = null;
    }

    private void stopCheckpointWatch() {
        if (checkpointDeadline != null)
            checkpointHandler.removeCallbacks(checkpointDeadline);
        checkpointDeadline = null;
        if (checkpointWorker != null)
            checkpointWorker.interrupt();
        checkpointWorker = null;
        if (demoWorker != null)
            demoWorker.interrupt();
        demoWorker = null;
    }

    void exitCheckpointGame() {
        Intent exit = new Intent(this, ProfileRestartActivity.class);
        exit.putExtra("game-pid", android.os.Process.myPid());
        exit.putExtra("exit-only", true);
        startActivity(exit);
        finishAndRemoveTask();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState != null)
            pendingLayoutExport = savedInstanceState.getString("pending-layout-export");
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
        String bootId = getIntent().getStringExtra(CHECKPOINT_BOOT_ID);
        if (bootId != null && bootId.matches("[0-9a-f]{32}")) {
            beginCheckpointOperation(bootId, true);
            watchCheckpoint(bootId, true);
        }
        String demoId = getIntent().getStringExtra(DEMO_BOOT_ID);
        if (demoId == null)
            demoId = getIntent().getStringExtra(GAME_MENU_BOOT_ID);
        if (demoId != null && demoId.matches("[0-9a-f]{32}"))
            watchDemoLaunch(demoId);
        // a new version looked for while the game starts
        Updater.start(this);
    }

    private void watchDemoLaunch(String id) {
        beginCheckpointOperation(id, true);
        demoWorker = new Thread(() -> {
            try {
                DemoLaunch demo = new DemoLaunch(getExternalFilesDir(null), getFilesDir());
                while (!Thread.currentThread().isInterrupted()) {
                    if (demo.restoreIfAcknowledged()) {
                        checkpointHandler.post(() -> completeCheckpoint(id, "resumed"));
                        return;
                    }
                    Thread.sleep(200);
                }
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            } catch (IOException e) {
                // The cold launcher recovers the original script after an interrupted launch.
                checkpointHandler.post(() -> completeCheckpoint(id, "io_error"));
            }
        }, "demo startup");
        demoWorker.start();
    }

    /** the system's file picker, for the touch layout; no storage permission */
    public void chooseLayoutFile(boolean export, String configuration) {
        String message = export
            ? "Choose the folder and the name of the touch layout file."
            : "Choose a touch layout file. It replaces your buttons, the look sensitivity and the General settings.";
        AlertDialog prompt = GameMenuStyle.builder(this)
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
            .create();
        prompt.show();
        GameMenuStyle.style(prompt);
    }

    @Override
    protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        state.putString("pending-layout-export", pendingLayoutExport);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
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
        AlertDialog dialog = GameMenuStyle.builder(this)
            .setTitle("Touch layout file")
            .setMessage("The operation did not complete: " + error.getMessage())
            .setPositiveButton("OK", null)
            .create();
        dialog.show();
        GameMenuStyle.style(dialog);
    }

    @Override
    protected void onResume() {
        activityResumed = true;
        super.onResume();
        if (touchOverlayPaused())
            pauseNativeThread();
        if (!touchOverlayPaused() && !checkpointBusy() && touchControls != null
                && getWindow().getDecorView().hasWindowFocus())
            touchControls.startDeviceInput();
    }

    @Override
    protected void onPause() {
        activityResumed = false;
        if (touchControls != null)
            touchControls.stopDeviceInput();
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (touchControls != null) {
            if (hasFocus && !touchOverlayPaused() && !checkpointBusy())
                touchControls.startDeviceInput();
            else
                touchControls.stopDeviceInput();
        }
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            if (touchOverlayPaused())
                pauseNativeThread();
            else
                resumeTouchGameIfReady();
        }
        // the system bars come back after a swipe or after the launcher was over them
        if (hasFocus)
            hideSystemBars();
    }

    @Override
    protected void onDestroy() {
        if (demoWorker != null)
            demoWorker.interrupt();
        checkpointRequestId = null;
        stopCheckpointWatch();
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
