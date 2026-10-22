package com.halo.decomp;

import android.app.AlertDialog;
import android.app.Application;
import android.content.Intent;
import android.os.Looper;
import android.view.KeyEvent;
import android.widget.FrameLayout;

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

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

/** Demo input remains gated until the native startup acknowledgement restores the player's script. */
@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w960dp-h540dp-land-mdpi",
    instrumentedPackages = {"com.halo.decomp", "org.libsdl.app"},
    shadows = {PauseMenuHostTest.EngineBoundary.class, TouchControlsMultitouchHostTest.NativeSink.class})
@LooperMode(LooperMode.Mode.PAUSED)
public class DemoActivityHostTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    private TestGame activity;
    private TouchControls controls;
    private File root, privateRoot;
    private String id;
    private static final String INIT = "render_shadows false\nmap_name levels\\b30\\b30\n";

    public static class TestGame extends HaloActivity {
        File root, privateRoot;
        @Override public File getExternalFilesDir(String type) { return root; }
        @Override public File getFilesDir() { return privateRoot; }
    }

    @Before public void prepare() throws Exception {
        PauseMenuHostTest.EngineBoundary.pauses = PauseMenuHostTest.EngineBoundary.resumes = 0;
        root = temporary.newFolder();
        privateRoot = temporary.newFolder();
        assertTrue(new File(root, "maps").mkdir());
        assertTrue(new File(root, "save/z").mkdirs());
        write("maps/ui.map", "ui");
        write("maps/demob30.map", "demo");
        write("init.txt", INIT);
        write("save/z/mobile-checkpoint.bin", "existing campaign checkpoint");
        id = CheckpointStore.newId();
        new DemoLaunch(root, privateRoot).prepare(id);
        activity = Robolectric.buildActivity(TestGame.class).get();
        activity.root = root;
        activity.privateRoot = privateRoot;
        activity.setIntent(new Intent().putExtra(HaloActivity.DEMO_BOOT_ID, id));
        FrameLayout content = new FrameLayout(activity);
        activity.setContentView(content);
        controls = new TouchControls(activity);
        content.addView(controls);
        controls.layout(0, 0, 960, 540);
        ReflectionHelpers.setField(controls, "showing", true);
        ReflectionHelpers.setField(activity, "touchControls", controls);
        ReflectionHelpers.setField(activity, "activityResumed", true);
    }

    private void write(String name, String text) throws Exception {
        Files.write(new File(root, name).toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    private String read(String name) throws Exception {
        return new String(Files.readAllBytes(new File(root, name).toPath()), StandardCharsets.UTF_8);
    }

    private void start() {
        ReflectionHelpers.callInstanceMethod(activity, "watchDemoLaunch",
            ReflectionHelpers.ClassParameter.from(String.class, id));
    }

    private void joinWorker() throws Exception {
        Thread worker = ReflectionHelpers.getField(activity, "demoWorker");
        assertNotNull(worker);
        worker.join(3000);
        assertFalse("The demo acknowledgement worker must finish", worker.isAlive());
    }

    @After public void stopWork() throws Exception {
        ReflectionHelpers.setField(activity, "activityResumed", false);
        ReflectionHelpers.setField(activity, "checkpointExiting", true);
        ReflectionHelpers.setField(activity, "checkpointRequestId", null);
        Thread worker = ReflectionHelpers.getField(activity, "demoWorker");
        if (worker != null) {
            worker.interrupt();
            worker.join(3000);
            assertFalse(worker.isAlive());
        }
        ReflectionHelpers.callInstanceMethod(activity, "stopCheckpointWatch");
        activity.finish();
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        if (dialog != null && dialog.isShowing())
            dialog.dismiss();
        controls.stopDeviceInput();
        shadowOf(Looper.getMainLooper()).idle();
    }

    @Test public void pendingDemoBlocksInputPauseAndCheckpointSave() throws Exception {
        start();
        activity.onResume();
        activity.onWindowFocusChanged(true);
        assertTrue(activity.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A)));
        assertTrue(activity.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK)));
        activity.showTouchPauseMenu();
        activity.startCheckpointSave();
        assertEquals(id, ReflectionHelpers.getField(activity, "checkpointRequestId"));
        assertNotNull(ReflectionHelpers.getField(activity, "checkpointProgress"));
        assertNull(ReflectionHelpers.getField(activity, "touchPauseDialog"));
        assertNull(ReflectionHelpers.getField(activity, "checkpointWorker"));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
        assertTrue(new File(privateRoot, "demo-launch.bin").isFile());
        assertFalse(new File(root, "save/z/mobile-checkpoint.request").exists());
        assertEquals("existing campaign checkpoint", read("save/z/mobile-checkpoint.bin"));
    }

    @Test public void acknowledgementRestoresScriptBeforeReleasingInput() throws Exception {
        start();
        write("save/z/mobile-checkpoint.status", id + "\nnew\n");
        joinWorker();
        // The worker restored the script, but its UI completion is still queued.
        assertEquals(INIT, read("init.txt"));
        assertFalse(new File(privateRoot, "demo-launch.bin").exists());
        assertEquals(id, ReflectionHelpers.getField(activity, "checkpointRequestId"));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
        shadowOf(Looper.getMainLooper()).idle();
        assertNull(ReflectionHelpers.getField(activity, "checkpointRequestId"));
        assertNull(ReflectionHelpers.getField(activity, "checkpointProgress"));
        activity.onWindowFocusChanged(true);
        assertTrue((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
        assertEquals("existing campaign checkpoint", read("save/z/mobile-checkpoint.bin"));
        assertNull(shadowOf(activity).getNextStartedActivity());
    }

    @Test public void failedRestoreKeepsRecoveryJournalAndInputBlocked() throws Exception {
        String changed = "render_shadows true\nmap_name levels\\a10\\a10\n";
        write("init.txt", changed);
        write("save/z/mobile-checkpoint.status", id + "\nnew\n");
        start();
        joinWorker();
        shadowOf(Looper.getMainLooper()).idle();
        AlertDialog error = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(error);
        assertTrue(error.isShowing());
        assertSame(error, ReflectionHelpers.getField(activity, "checkpointErrorDialog"));
        assertNull(ReflectionHelpers.getField(activity, "checkpointProgress"));
        assertTrue(new File(privateRoot, "demo-launch.bin").isFile());
        assertEquals(changed, read("init.txt"));
        activity.onResume();
        activity.onWindowFocusChanged(true);
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
        assertEquals("existing campaign checkpoint", read("save/z/mobile-checkpoint.bin"));
        assertNull(shadowOf(activity).getNextStartedActivity());
    }

    @Test public void timeoutStopsPollingAndRetainsColdRecovery() throws Exception {
        start();
        Thread worker = ReflectionHelpers.getField(activity, "demoWorker");
        activity.completeCheckpoint(id, "restore_timeout");
        worker.join(3000);
        assertFalse(worker.isAlive());
        assertNull(ReflectionHelpers.getField(activity, "demoWorker"));
        write("save/z/mobile-checkpoint.status", id + "\nnew\n");
        shadowOf(Looper.getMainLooper()).idle();
        assertTrue(new File(privateRoot, "demo-launch.bin").isFile());
        assertNotNull(ReflectionHelpers.getField(activity, "checkpointErrorDialog"));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
        assertEquals("existing campaign checkpoint", read("save/z/mobile-checkpoint.bin"));
    }
}
