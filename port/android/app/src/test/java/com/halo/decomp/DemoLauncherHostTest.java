package com.halo.decomp;

import android.app.AlertDialog;
import android.app.Application;
import android.content.Intent;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;

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
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w800dp-h360dp-land-mdpi")
@LooperMode(LooperMode.Mode.PAUSED)
public class DemoLauncherHostTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    private TestLauncher activity;
    private File root, privateRoot;
    private static final String INIT = "render_shadows false\nmap_name levels\\b30\\b30\n";

    public static class TestLauncher extends LauncherActivity {
        boolean running;
        File privateRoot;
        File root;
        @Override boolean hasRunningGame() { return running; }
        @Override public File getFilesDir() { return privateRoot; }
        @Override public File getExternalFilesDir(String type) { return root; }
    }

    @Before public void prepare() throws Exception {
        root = temporary.newFolder();
        privateRoot = temporary.newFolder();
        assertTrue(new File(root, "maps").mkdir());
        assertTrue(new File(root, "save/z").mkdirs());
        byte[] map = new byte[0x800];
        java.nio.ByteBuffer.wrap(map).order(java.nio.ByteOrder.LITTLE_ENDIAN)
            .putInt(0, 0x68656164).putInt(4, 5).putInt(8, 0x800)
            .putInt(0x10, 0x800).putInt(0x14, 0).putInt(0x7fc, 0x666f6f74);
        Files.write(new File(root, "maps/ui.map").toPath(), map);
        Files.write(new File(root, "maps/demob30.map").toPath(), map);
        write("vk_driver_auto.txt", "none\n");
        write("init.txt", INIT);
        write("save/z/mobile-checkpoint.bin", "existing checkpoint");
        activity = Robolectric.buildActivity(TestLauncher.class).get();
        activity.privateRoot = privateRoot;
        activity.root = root;
        activity.setIntent(new Intent());
        ReflectionHelpers.setField(activity, "dataRoot", root);
        ReflectionHelpers.setField(activity, "checkpointStore", new CheckpointStore(root,
            name -> new ByteArrayInputStream(name.getBytes(StandardCharsets.US_ASCII))));
    }

    private void write(String name, String text) throws Exception {
        Files.write(new File(root, name).toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    private String read(String name) throws Exception {
        return new String(Files.readAllBytes(new File(root, name).toPath()), StandardCharsets.UTF_8);
    }

    private Button button(View view, String text) {
        if (view instanceof Button && text.contentEquals(((Button) view).getText()))
            return (Button) view;
        if (view instanceof ViewGroup)
            for (int i = 0; i < ((ViewGroup) view).getChildCount(); i++) {
                Button found = button(((ViewGroup) view).getChildAt(i), text);
                if (found != null)
                    return found;
            }
        return null;
    }

    private View screen() {
        ReflectionHelpers.callInstanceMethod(activity, "buildInterface");
        return activity.getWindow().getDecorView();
    }

    private void joinWorker() throws Exception {
        Thread worker = ReflectionHelpers.getField(activity, "checkpointWorker");
        if (worker != null) {
            worker.join(3000);
            assertFalse(worker.isAlive());
        }
        shadowOf(Looper.getMainLooper()).idle();
    }

    @After public void close() throws Exception {
        joinWorker();
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        if (dialog != null && dialog.isShowing())
            dialog.dismiss();
        activity.finish();
    }

    @Test public void installedDemoHasSeparatePlayButtonAndResumeKeepsDashboard() {
        View view = screen();
        assertTrue(button(view, "Jugar demo").isEnabled());
        assertTrue(button(view, "Jugar / Continuar").isEnabled());
        assertFalse(button(view, "Elegir imagen de disco").isEnabled());
        activity.onResume();
        assertNull(shadowOf(activity).getNextStartedActivity());
        assertTrue(button(view, "Jugar demo").isEnabled());
    }

    @Test public void missingDemoOffersDownloadWithoutChangingStartupBeforeClick() throws Exception {
        assertTrue(new File(root, "maps/demob30.map").delete());
        assertTrue(button(screen(), "Descargar y jugar demo").isEnabled());
        assertNull(shadowOf(activity).getNextStartedActivity());
        assertEquals(INIT, read("init.txt"));
        assertFalse(new File(root, "save/z/mobile-checkpoint.boot").exists());
    }

    @Test public void demoButtonStartsFreshDemoOnceAndPreservesCheckpoint() throws Exception {
        button(screen(), "Jugar demo").performClick();
        activity.startDemo();
        joinWorker();
        Intent game = shadowOf(activity).getNextStartedActivity();
        assertNotNull(game);
        assertEquals(HaloActivity.class.getName(), game.getComponent().getClassName());
        String id = game.getStringExtra(HaloActivity.DEMO_BOOT_ID);
        assertTrue(id.matches("[0-9a-f]{32}"));
        assertNull(game.getStringExtra(HaloActivity.CHECKPOINT_BOOT_ID));
        assertTrue(read("save/z/mobile-checkpoint.boot").startsWith(id + " new "));
        assertTrue(read("init.txt").contains("map_name levels\\demob30\\demob30"));
        assertTrue(read("init.txt").contains("render_shadows false"));
        assertEquals("existing checkpoint", read("save/z/mobile-checkpoint.bin"));
        assertNull(shadowOf(activity).getNextStartedActivity());
        write("save/z/mobile-checkpoint.status", id + "\nnew\n");
        assertTrue(new DemoLaunch(root, privateRoot).restoreIfAcknowledged());
        assertEquals(INIT, read("init.txt"));
    }

    @Test public void warmDemoRequestOffersReturnWithoutModifyingGameFiles() throws Exception {
        activity.running = true;
        activity.startDemo();
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("Hay una partida abierta", shadowOf(dialog).getTitle());
        assertEquals(INIT, read("init.txt"));
        assertFalse(new File(root, "save/z/mobile-checkpoint.boot").exists());
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        Intent game = shadowOf(activity).getNextStartedActivity();
        assertNotNull(game);
        assertNull(game.getStringExtra(HaloActivity.DEMO_BOOT_ID));
    }

    @Test public void coldNormalStartRecoversAbandonedDemoBeforeCheckpointChoice() throws Exception {
        new DemoLaunch(root, privateRoot).prepare("0123456789abcdef0123456789abcdef");
        activity.startGame();
        joinWorker();
        assertEquals(INIT, read("init.txt"));
        assertNotNull(ShadowAlertDialog.getLatestAlertDialog());
        assertNull(shadowOf(activity).getNextStartedActivity());
        assertEquals("existing checkpoint", read("save/z/mobile-checkpoint.bin"));
    }

    @Test public void graphicsRestartRetainsDemoRouting() {
        Intent request = new Intent().putExtra("launch-demo", true);
        ProfileRestartActivity controller = Robolectric.buildActivity(ProfileRestartActivity.class, request)
            .create().get();
        Intent next = shadowOf(controller).getNextStartedActivity();
        assertTrue(next.getBooleanExtra("resume-after-graphics", false));
        assertTrue(next.getBooleanExtra("launch-demo", false));
    }
}
