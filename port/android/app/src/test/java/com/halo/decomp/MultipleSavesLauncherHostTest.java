package com.halo.decomp;

import android.app.AlertDialog;
import android.app.Application;
import android.content.Intent;
import android.os.Looper;
import android.view.View;
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
import java.util.List;
import java.util.zip.CRC32;
import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w960dp-h540dp-land-mdpi")
@LooperMode(LooperMode.Mode.PAUSED)
public class MultipleSavesLauncherHostTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    private CheckpointLauncherHostTest.TestLauncher activity;
    private File root;
    private CheckpointStore store;
    private CheckpointSlots slots;
    private CheckpointSlots.Entry first;

    @Before public void prepare() throws Exception {
        root = temporary.newFolder();
        assertTrue(new File(root, "save/z").mkdirs());
        Files.write(new File(root, "init.txt").toPath(), "render_scale 1\n".getBytes(StandardCharsets.US_ASCII));
        store = new CheckpointStore(root, name -> new ByteArrayInputStream(name.getBytes(StandardCharsets.US_ASCII)));
        slots = new CheckpointSlots(root, store);
        checkpoint("levels\\a10\\a10", 1);
        first = slots.saveNew("Campaña A");
        checkpoint("levels\\demob30\\demob30", 2);
        slots.saveNew("Demo B");
        activity = Robolectric.buildActivity(CheckpointLauncherHostTest.TestLauncher.class).get();
        activity.setIntent(new Intent());
        ReflectionHelpers.setField(activity, "dataRoot", root);
        ReflectionHelpers.setField(activity, "checkpointStore", store);
        ReflectionHelpers.setField(activity, "checkpointSlots", slots);
    }

    private void checkpoint(String map, int marker) throws Exception {
        byte[] buffer = new byte[65536];
        CRC32 crc = new CRC32();
        buffer[0] = (byte) marker;
        crc.update(buffer);
        buffer[0] = 0;
        for (int count = buffer.length; count < CheckpointStore.IMAGE_SIZE; count += buffer.length)
            crc.update(buffer);
        byte[] header = new byte[CheckpointStore.HEADER_SIZE];
        ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN).put("OCESAVE1".getBytes(StandardCharsets.US_ASCII))
            .putInt(8, 1).putInt(12, 512).putInt(16, CheckpointStore.IMAGE_SIZE)
            .putInt(20, (int) crc.getValue()).putInt(24, 1).putInt(28, 0);
        System.arraycopy(store.fingerprints().gl.getBytes(StandardCharsets.US_ASCII), 0, header, 32, 64);
        System.arraycopy(map.getBytes(StandardCharsets.US_ASCII), 0, header, 96, map.length());
        try (RandomAccessFile file = new RandomAccessFile(store.checkpointFile(), "rw")) {
            file.setLength(512L + CheckpointStore.IMAGE_SIZE);
            file.write(header);
            file.write(marker);
        }
    }

    private void joinWorker() throws Exception {
        Thread worker = ReflectionHelpers.getField(activity, "checkpointWorker");
        if (worker != null) {
            worker.join(5000);
            assertFalse(worker.isAlive());
        }
        shadowOf(Looper.getMainLooper()).idle();
    }

    @After public void close() throws Exception {
        joinWorker();
        activity.finish();
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        if (dialog != null && dialog.isShowing()) dialog.dismiss();
    }

    private AlertDialog openPicker() throws Exception {
        activity.startGame();
        joinWorker();
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("Partidas guardadas", shadowOf(dialog).getTitle());
        assertEquals(2, dialog.getListView().getCount());
        return dialog;
    }

    private int index(String id) throws Exception {
        List<CheckpointSlots.Entry> entries = slots.list();
        for (int i = 0; i < entries.size(); i++) if (entries.get(i).id.equals(id)) return i;
        throw new AssertionError("Save missing");
    }

    @Test public void selectingOlderSaveActivatesItAndPassesMatchingRestoreId() throws Exception {
        AlertDialog dialog = openPicker();
        int row = index(first.id);
        dialog.getListView().performItemClick(new View(activity), row, row);
        joinWorker();
        Intent game = shadowOf(activity).getNextStartedActivity();
        assertNotNull(game);
        String id = game.getStringExtra(HaloActivity.CHECKPOINT_BOOT_ID);
        assertTrue(id.matches("[0-9a-f]{32}"));
        assertTrue(CheckpointStore.readBounded(new File(root, "save/z/mobile-checkpoint.boot"), 256)
            .startsWith(id + " continue "));
        assertEquals("levels\\a10\\a10", store.inspect().map);
        assertEquals(2, slots.list().size());
        assertEquals("render_scale 1\n", CheckpointStore.readBounded(new File(root, "init.txt"), 64));
    }

    @Test public void archivesRemainSelectableWhenWorkingSlotIsAbsent() throws Exception {
        Files.delete(store.checkpointFile().toPath());
        openPicker();
        assertNull(shadowOf(activity).getNextStartedActivity());
        assertEquals(2, slots.list().size());
    }

    @Test public void cancelReturnsToDashboardAndAllowsOpeningAgain() throws Exception {
        openPicker().getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertFalse(activity.isFinishing());
        assertFalse((boolean) ReflectionHelpers.getField(activity, "checkpointChoiceStarted"));
        assertFalse((boolean) ReflectionHelpers.getField(activity, "driverStarted"));
        openPicker();
        assertEquals("levels\\demob30\\demob30", store.inspect().map);
        assertNull(shadowOf(activity).getNextStartedActivity());
    }

    @Test public void damagedArchiveIsVisibleButCannotBeActivated() throws Exception {
        try (RandomAccessFile file = new RandomAccessFile(new File(root,
                "save/z/mobile-checkpoint-slots/" + first.id + ".slot"), "rw")) {
            file.write(0);
        }
        AlertDialog dialog = openPicker();
        int row = index(first.id);
        dialog.getListView().performItemClick(new View(activity), row, row);
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals("Partida no disponible", shadowOf(ShadowAlertDialog.getLatestAlertDialog()).getTitle());
        assertNull(shadowOf(activity).getNextStartedActivity());
        assertEquals("levels\\demob30\\demob30", store.inspect().map);
    }

    @Test public void newGamePreservesEveryArchive() throws Exception {
        openPicker().getButton(AlertDialog.BUTTON_NEUTRAL).performClick();
        joinWorker();
        assertNotNull(shadowOf(activity).getNextStartedActivity());
        assertEquals(2, slots.list().size());
        assertEquals("levels\\demob30\\demob30", store.inspect().map);
    }
}
