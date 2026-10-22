package com.halo.decomp;

import android.app.AlertDialog;
import android.app.Application;
import android.content.Intent;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.EditText;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.LooperMode;
import org.robolectric.shadows.ShadowAlertDialog;
import org.robolectric.util.ReflectionHelpers;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.Duration;
import java.util.zip.CRC32;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

/** Real save/restore UI and lifecycle, with the native engine boundary replaced. */
@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w960dp-h540dp-land-mdpi",
    instrumentedPackages = {"com.halo.decomp", "org.libsdl.app"},
    shadows = {PauseMenuHostTest.EngineBoundary.class, TouchControlsMultitouchHostTest.NativeSink.class})
@LooperMode(LooperMode.Mode.PAUSED)
public class CheckpointUiHostTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    private HaloActivity activity;
    private TouchControls controls;
    private File dataRoot;
    private CheckpointStore store;
    private CheckpointSlots slots;

    @Before public void prepare() throws Exception {
        PauseMenuHostTest.EngineBoundary.pauses = PauseMenuHostTest.EngineBoundary.resumes = 0;
        activity = Robolectric.buildActivity(HaloActivity.class).get();
        FrameLayout root = new FrameLayout(activity);
        activity.setContentView(root);
        controls = new TouchControls(activity);
        root.addView(controls);
        controls.layout(0, 0, 960, 540);
        ReflectionHelpers.setField(controls, "showing", true);
        ReflectionHelpers.setField(activity, "touchControls", controls);
        ReflectionHelpers.setField(activity, "activityResumed", true);
        dataRoot = temporary.newFolder();
        store = new CheckpointStore(dataRoot,
            name -> new ByteArrayInputStream(name.getBytes(StandardCharsets.US_ASCII)));
        slots = new CheckpointSlots(dataRoot, store);
        ReflectionHelpers.setField(activity, "checkpointStore", store);
        ReflectionHelpers.setField(activity, "checkpointSlots", slots);
    }

    @After public void stopWork() throws Exception {
        ReflectionHelpers.setField(activity, "activityResumed", false);
        ReflectionHelpers.setField(activity, "checkpointExiting", true);
        ReflectionHelpers.setField(activity, "checkpointRequestId", null);
        Thread worker = ReflectionHelpers.getField(activity, "checkpointWorker");
        ReflectionHelpers.callInstanceMethod(activity, "stopCheckpointWatch");
        if (worker != null)
            worker.join(2000);
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        if (dialog != null && dialog.isShowing())
            dialog.dismiss();
        controls.stopDeviceInput();
        shadowOf(Looper.getMainLooper()).idle();
    }

    private Button button(View view, String text) {
        if (view instanceof Button && ((Button) view).getText().toString().equals(text))
            return (Button) view;
        if (view instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++) {
                Button result = button(((ViewGroup) view).getChildAt(i), text);
                if (result != null)
                    return result;
            }
        return null;
    }

    private String save() {
        activity.showTouchPauseMenu();
        AlertDialog pause = ShadowAlertDialog.getLatestAlertDialog();
        Button save = button(pause.getWindow().getDecorView(), "Guardar y salir");
        assertNotNull(save);
        save.performClick();
        joinWorker();
        AlertDialog choices = ShadowAlertDialog.getLatestAlertDialog();
        Button create = button(choices.getWindow().getDecorView(), "Nueva partida guardada");
        assertNotNull(create);
        create.performClick();
        AlertDialog naming = ShadowAlertDialog.getLatestAlertDialog();
        findName(naming.getWindow().getDecorView()).setText("Mi partida");
        naming.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertFalse("No dialog may suspend SDL during the save", pause.isShowing());
        assertNotNull(ReflectionHelpers.getField(activity, "checkpointProgress"));
        return ReflectionHelpers.getField(activity, "checkpointRequestId");
    }

    private EditText findName(View view) {
        if (view instanceof EditText)
            return (EditText) view;
        if (view instanceof ViewGroup)
            for (int index = 0; index < ((ViewGroup) view).getChildCount(); index++) {
                EditText result = findName(((ViewGroup) view).getChildAt(index));
                if (result != null)
                    return result;
            }
        return null;
    }

    private void joinWorker() {
        Thread worker = ReflectionHelpers.getField(activity, "checkpointWorker");
        try {
            if (worker != null) {
                worker.join(5000);
                assertFalse("File worker must finish", worker.isAlive());
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new AssertionError(e);
        }
        shadowOf(Looper.getMainLooper()).idle();
    }

    private File checkpoint(String suffix) { return new File(dataRoot, "save/z/" + CheckpointStore.PREFIX + suffix); }

    private void waitForRequest() throws Exception {
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (!checkpoint(".request").isFile() && System.nanoTime() < deadline)
            Thread.sleep(10);
        assertTrue("Native save request must be published", checkpoint(".request").isFile());
    }

    private void nativeCheckpoint(int seed) throws Exception {
        File directory = checkpoint(".bin").getParentFile();
        assertTrue(directory.isDirectory() || directory.mkdirs());
        byte[] payload = new byte[32768];
        java.util.Arrays.fill(payload, (byte) seed);
        CRC32 crc = new CRC32();
        for (int offset = 0; offset < CheckpointStore.IMAGE_SIZE; offset += payload.length)
            crc.update(payload);
        ByteBuffer header = ByteBuffer.allocate(CheckpointStore.HEADER_SIZE).order(ByteOrder.LITTLE_ENDIAN);
        header.put("OCESAVE1".getBytes(StandardCharsets.US_ASCII));
        header.putInt(8, 1).putInt(12, CheckpointStore.HEADER_SIZE).putInt(16, CheckpointStore.IMAGE_SIZE)
            .putInt(20, (int) crc.getValue()).putInt(24, 1).putInt(28, 0);
        header.position(32);
        header.put(store.fingerprints().gl.getBytes(StandardCharsets.US_ASCII));
        header.position(96);
        header.put("levels\\demob30\\demob30".getBytes(StandardCharsets.US_ASCII));
        try (RandomAccessFile output = new RandomAccessFile(checkpoint(".bin"), "rw")) {
            output.setLength(0);
            output.write(header.array());
            for (int offset = 0; offset < CheckpointStore.IMAGE_SIZE; offset += payload.length)
                output.write(payload);
        }
    }

    private void acknowledge(String id) throws Exception {
        Files.write(checkpoint(".status").toPath(), (id + "\nsaved\n").getBytes(StandardCharsets.US_ASCII));
        joinWorker();
    }

    private String restore() {
        String id = CheckpointStore.newId();
        ReflectionHelpers.callInstanceMethod(activity, "beginCheckpointOperation",
            ReflectionHelpers.ClassParameter.from(String.class, id),
            ReflectionHelpers.ClassParameter.from(boolean.class, true));
        return id;
    }

    @Test public void saveKeepsPauseButtonsAndExplainsCheckpointSemantics() {
        activity.showTouchPauseMenu();
        AlertDialog pause = ShadowAlertDialog.getLatestAlertDialog();
        assertTrue(shadowOf(pause).getMessage().toString().contains("no la posición exacta"));
        assertNotNull(button(pause.getWindow().getDecorView(), "Guardar y salir"));
        assertEquals("Reanudar", pause.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
        assertEquals("Ajustes de controles", pause.getButton(AlertDialog.BUTTON_NEUTRAL).getText().toString());
        assertEquals("Atrás", pause.getButton(AlertDialog.BUTTON_NEGATIVE).getText().toString());
    }

    @Test public void busySaveBlocksInputAndCannotStartTwice() {
        String id = save();
        activity.startCheckpointSave();
        activity.onResume();
        activity.onWindowFocusChanged(true);
        assertEquals(id, ReflectionHelpers.getField(activity, "checkpointRequestId"));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
        assertTrue(activity.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK)));
        assertNull(ReflectionHelpers.getField(activity, "touchPauseDialog"));
        assertNull(shadowOf(activity).getNextStartedActivity());
    }

    @Test public void cancellingSaveChooserReturnsToPauseWithoutWritingRequest() {
        activity.showTouchPauseMenu();
        activity.startCheckpointSave();
        joinWorker();
        assertNotNull(ReflectionHelpers.getField(activity, "checkpointSaveDialog"));
        activity.onWindowFocusChanged(true);
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
        ShadowAlertDialog.getLatestAlertDialog().getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals("Pausa", shadowOf(ShadowAlertDialog.getLatestAlertDialog()).getTitle());
        assertFalse(checkpoint(".request").exists());
    }

    @Test public void emptySaveNameDoesNotWriteRequestOrDismissChooser() {
        activity.showTouchPauseMenu();
        activity.startCheckpointSave();
        joinWorker();
        button(ShadowAlertDialog.getLatestAlertDialog().getWindow().getDecorView(), "Nueva partida guardada").performClick();
        AlertDialog naming = ShadowAlertDialog.getLatestAlertDialog();
        naming.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        assertTrue(naming.isShowing());
        assertNotNull(findName(naming.getWindow().getDecorView()).getError());
        assertFalse(checkpoint(".request").exists());
    }

    @Test public void landscapeNameUsesDoneWithoutFullscreenAndStartsOneSave() throws Exception {
        activity.showTouchPauseMenu();
        activity.startCheckpointSave();
        joinWorker();
        button(ShadowAlertDialog.getLatestAlertDialog().getWindow().getDecorView(), "Nueva partida guardada").performClick();
        AlertDialog naming = ShadowAlertDialog.getLatestAlertDialog();
        EditText name = findName(naming.getWindow().getDecorView());
        assertEquals(EditorInfo.IME_ACTION_DONE, name.getImeOptions() & EditorInfo.IME_MASK_ACTION);
        assertNotEquals(0, name.getImeOptions() & EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        assertNotEquals(0, name.getImeOptions() & EditorInfo.IME_FLAG_NO_FULLSCREEN);
        name.onEditorAction(EditorInfo.IME_ACTION_DONE);
        assertTrue(naming.isShowing());
        assertNotNull(name.getError());
        assertFalse(checkpoint(".request").exists());
        name.setText("Mi partida con teclado");
        name.onEditorAction(EditorInfo.IME_ACTION_DONE);
        String id = ReflectionHelpers.getField(activity, "checkpointRequestId");
        assertNotNull(id);
        waitForRequest();
        name.onEditorAction(EditorInfo.IME_ACTION_DONE);
        assertEquals(id, ReflectionHelpers.getField(activity, "checkpointRequestId"));
        assertTrue(new String(Files.readAllBytes(checkpoint(".request").toPath()), StandardCharsets.US_ASCII)
            .startsWith(id + " save "));
        assertFalse(naming.isShowing());
    }

    @Test public void savedAcknowledgementAddsAnotherNamedSlotBeforeExit() throws Exception {
        nativeCheckpoint(1);
        CheckpointSlots.Entry first = slots.saveNew("Primera");
        String id = save();
        waitForRequest();
        assertEquals(1, slots.list().size());
        nativeCheckpoint(2);
        acknowledge(id);
        assertEquals(2, slots.list().size());
        assertTrue(slots.list().stream().anyMatch(entry -> entry.id.equals(first.id) && entry.name.equals("Primera")));
        assertTrue(slots.list().stream().anyMatch(entry -> entry.name.equals("Mi partida")));
        assertNotNull(shadowOf(activity).getNextStartedActivity());
    }

    @Test public void failedArchiveKeepsNewWorkingCheckpointAndOtherSlotsWithoutExit() throws Exception {
        nativeCheckpoint(1);
        CheckpointSlots.Entry first = slots.saveNew("Primera");
        activity.beginCheckpointSave("0123456789abcdef0123456789abcdef", "Destino no disponible");
        String id = ReflectionHelpers.getField(activity, "checkpointRequestId");
        waitForRequest();
        nativeCheckpoint(2);
        acknowledge(id);
        assertEquals(1, slots.list().size());
        assertEquals(first.id, slots.list().get(0).id);
        assertEquals(CheckpointStore.Kind.READY, store.inspect().kind);
        assertNull(shadowOf(activity).getNextStartedActivity());
        assertTrue(shadowOf(ShadowAlertDialog.getLatestAlertDialog()).getMessage().toString()
            .contains("copia de recuperación"));
    }

    @Test public void replacingNamedSlotRequiresConfirmationAndPreservesItsIdentity() throws Exception {
        nativeCheckpoint(1);
        CheckpointSlots.Entry first = slots.saveNew("Primera");
        activity.showTouchPauseMenu();
        activity.startCheckpointSave();
        joinWorker();
        button(ShadowAlertDialog.getLatestAlertDialog().getWindow().getDecorView(), SaveLabels.label(first)).performClick();
        AlertDialog confirm = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("Reemplazar partida", shadowOf(confirm).getTitle());
        assertFalse(checkpoint(".request").exists());
        assertEquals(first.id, slots.list().get(0).id);
        confirm.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        String id = ReflectionHelpers.getField(activity, "checkpointRequestId");
        waitForRequest();
        nativeCheckpoint(2);
        acknowledge(id);
        assertEquals(1, slots.list().size());
        assertEquals(first.id, slots.list().get(0).id);
        assertEquals("Primera", slots.list().get(0).name);
        assertNotNull(shadowOf(activity).getNextStartedActivity());
    }

    @Test public void fullChooserDisablesNewAndKeepsExplicitReplaceAvailable() {
        java.util.List<CheckpointSlots.Entry> entries = new java.util.ArrayList<>();
        for (int index = 0; index < CheckpointSlots.MAX_SLOTS; index++)
            entries.add(new CheckpointSlots.Entry(CheckpointStore.newId(), "Partida " + index,
                System.currentTimeMillis(), new CheckpointStore.Candidate(CheckpointStore.Kind.READY,
                    "levels\\demob30\\demob30", 1), new byte[32]));
        ReflectionHelpers.callInstanceMethod(activity, "showCheckpointSaveChoices",
            ReflectionHelpers.ClassParameter.from(java.util.List.class, entries));
        AlertDialog chooser = ShadowAlertDialog.getLatestAlertDialog();
        assertFalse(button(chooser.getWindow().getDecorView(), "Nueva partida guardada").isEnabled());
        assertTrue(button(chooser.getWindow().getDecorView(), SaveLabels.label(entries.get(0))).isEnabled());
    }

    @Test public void onlyMatchingSavedAcknowledgementExitsWithoutRestart() {
        String id = save();
        activity.completeCheckpoint(CheckpointStore.newId(), "saved");
        assertNull(shadowOf(activity).getNextStartedActivity());
        activity.completeCheckpoint(id, "saved");
        Intent exit = shadowOf(activity).getNextStartedActivity();
        assertNotNull(exit);
        assertEquals(ProfileRestartActivity.class.getName(), exit.getComponent().getClassName());
        assertTrue(exit.getBooleanExtra("exit-only", false));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
    }

    @Test public void missingCheckpointKeepsGamePausedAndOffersReturn() {
        String id = save();
        activity.completeCheckpoint(id, "no_checkpoint");
        AlertDialog error = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("No se pudo guardar", shadowOf(error).getTitle());
        assertTrue(shadowOf(error).getMessage().toString().contains("Todavía no hay un punto de control"));
        assertNull(shadowOf(activity).getNextStartedActivity());
        error.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals("Pausa", shadowOf(ShadowAlertDialog.getLatestAlertDialog()).getTitle());
    }

    @Test public void failedSaveNeverReportsSuccessOrExits() {
        String id = save();
        activity.completeCheckpoint(id, "io_error");
        assertEquals("No se pudo guardar", shadowOf(ShadowAlertDialog.getLatestAlertDialog()).getTitle());
        assertNull(shadowOf(activity).getNextStartedActivity());
        assertNull(ReflectionHelpers.getField(activity, "checkpointRequestId"));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
    }

    @Test public void timeoutRejectsLateSaveAcknowledgement() {
        String id = save();
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(15));
        AlertDialog error = ShadowAlertDialog.getLatestAlertDialog();
        assertTrue(shadowOf(error).getMessage().toString().contains("No se cerró la partida"));
        activity.completeCheckpoint(id, "saved");
        assertNull(shadowOf(activity).getNextStartedActivity());
        assertSame(error, ShadowAlertDialog.getLatestAlertDialog());
    }

    @Test public void restoreAcknowledgementClearsProgressWithoutExiting() {
        String id = restore();
        activity.completeCheckpoint(id, "resumed");
        assertNull(ReflectionHelpers.getField(activity, "checkpointProgress"));
        assertNull(ReflectionHelpers.getField(activity, "checkpointRequestId"));
        assertNull(shadowOf(activity).getNextStartedActivity());
    }

    @Test public void backgroundRestoreDoesNotActivateInput() {
        String id = restore();
        activity.onPause();
        activity.completeCheckpoint(id, "resumed");
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
        assertEquals(0, PauseMenuHostTest.EngineBoundary.resumes);
    }

    @Test public void incompatibleRestoreExplainsErrorAndCannotSilentlyResumeNewGame() {
        String id = restore();
        activity.completeCheckpoint(id, "incompatible");
        AlertDialog error = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("No se pudo continuar", shadowOf(error).getTitle());
        assertTrue(shadowOf(error).getMessage().toString().contains("El archivo se conserva"));
        activity.onWindowFocusChanged(true);
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
        error.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        Intent exit = shadowOf(activity).getNextStartedActivity();
        assertNotNull(exit);
        assertTrue(exit.getBooleanExtra("exit-only", false));
    }

    @Test public void restoreTimeoutPreservesSlotAndRejectsLateAcknowledgement() {
        String id = restore();
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofSeconds(120));
        AlertDialog error = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("No se pudo continuar", shadowOf(error).getTitle());
        activity.completeCheckpoint(id, "resumed");
        assertSame(error, ShadowAlertDialog.getLatestAlertDialog());
        assertNotNull(ReflectionHelpers.getField(activity, "checkpointErrorDialog"));
        assertNull(shadowOf(activity).getNextStartedActivity());
    }
}
