package com.halo.decomp;

import android.app.AlertDialog;
import android.app.Application;
import android.content.Intent;
import android.net.Uri;
import android.os.Looper;

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

/** The launcher must request an explicit cold-start choice without rewriting init or an old save. */
@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w960dp-h540dp-land-mdpi")
@LooperMode(LooperMode.Mode.PAUSED)
public class CheckpointLauncherHostTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    private TestLauncher activity;
    private File root, directory;

    private static void write(java.nio.file.Path path, String text) throws java.io.IOException {
        Files.write(path, text.getBytes(StandardCharsets.UTF_8));
    }

    private static String read(java.nio.file.Path path) throws java.io.IOException {
        return new String(Files.readAllBytes(path), StandardCharsets.UTF_8);
    }

    public static class TestLauncher extends LauncherActivity {
        boolean running;
        @Override boolean hasRunningGame() { return running; }
    }

    @Before public void prepare() throws Exception {
        activity = Robolectric.buildActivity(TestLauncher.class).get();
        activity.setIntent(new Intent());
        root = temporary.newFolder();
        directory = new File(root, "save/z");
        assertTrue(directory.mkdirs());
        ReflectionHelpers.setField(activity, "dataRoot", root);
        ReflectionHelpers.setField(activity, "checkpointStore", new CheckpointStore(root,
            name -> new ByteArrayInputStream(name.getBytes(StandardCharsets.US_ASCII))));
        write(new File(root, "init.txt").toPath(), "render_shadows false\nmap_name levels\\b30\\b30\n");
        // Its bytes are an existing save, which these UI actions must leave to the engine.
        write(new File(directory, "mobile-checkpoint.bin").toPath(), "old checkpoint bytes");
    }

    private void joinWorker() throws Exception {
        Thread worker = ReflectionHelpers.getField(activity, "checkpointWorker");
        if (worker != null) {
            worker.join(2000);
            assertFalse("File worker must finish", worker.isAlive());
        }
        shadowOf(Looper.getMainLooper()).idle();
    }

    @After public void close() throws Exception {
        Thread worker = ReflectionHelpers.getField(activity, "checkpointWorker");
        if (worker != null) {
            worker.interrupt();
            worker.join(2000);
        }
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        if (dialog != null && dialog.isShowing())
            dialog.dismiss();
        shadowOf(Looper.getMainLooper()).idle();
    }

    private void choice(CheckpointStore.Kind kind) {
        activity.showCheckpointChoice(new CheckpointStore.Candidate(kind, "levels\\b30\\b30", 1));
    }

    private void assertFilesUntouched() throws Exception {
        assertEquals("old checkpoint bytes", read(new File(directory, "mobile-checkpoint.bin").toPath()));
        assertEquals("render_shadows false\nmap_name levels\\b30\\b30\n",
            read(new File(root, "init.txt").toPath()));
    }

    @Test public void continueWritesSeparateBootRequestAndPassesAcknowledgementId() throws Exception {
        choice(CheckpointStore.Kind.READY);
        ShadowAlertDialog.getLatestAlertDialog().getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        joinWorker();
        Intent game = shadowOf(activity).getNextStartedActivity();
        assertNotNull(game);
        assertEquals(HaloActivity.class.getName(), game.getComponent().getClassName());
        String id = game.getStringExtra(HaloActivity.CHECKPOINT_BOOT_ID);
        assertTrue(id.matches("[0-9a-f]{32}"));
        assertTrue(read(new File(directory, "mobile-checkpoint.boot").toPath()).startsWith(id + " continue "));
        assertFilesUntouched();
    }

    @Test public void newGamePreservesOldCheckpointAndInit() throws Exception {
        choice(CheckpointStore.Kind.READY);
        ShadowAlertDialog.getLatestAlertDialog().getButton(AlertDialog.BUTTON_NEUTRAL).performClick();
        joinWorker();
        Intent game = shadowOf(activity).getNextStartedActivity();
        assertNotNull(game);
        assertNull(game.getStringExtra(HaloActivity.CHECKPOINT_BOOT_ID));
        assertTrue(read(new File(directory, "mobile-checkpoint.boot").toPath()).contains(" new "));
        assertFilesUntouched();
    }

    @Test public void cancelDoesNotLaunchOrWriteBootRequest() throws Exception {
        choice(CheckpointStore.Kind.READY);
        ShadowAlertDialog.getLatestAlertDialog().getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertTrue(activity.isFinishing());
        assertNull(shadowOf(activity).getNextStartedActivity());
        assertFalse(new File(directory, "mobile-checkpoint.boot").exists());
        assertFilesUntouched();
    }

    @Test public void malformedAndIncompatibleCandidatesCannotContinue() throws Exception {
        for (CheckpointStore.Kind kind : new CheckpointStore.Kind[] {
                CheckpointStore.Kind.CORRUPT, CheckpointStore.Kind.INCOMPATIBLE }) {
            choice(kind);
            AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
            assertFalse(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
            assertTrue(dialog.getButton(AlertDialog.BUTTON_NEUTRAL).isEnabled());
            dialog.dismiss();
        }
        assertFilesUntouched();
    }

    @Test public void warmGameBypassesPromptAndDoesNotOverwriteBoot() throws Exception {
        activity.running = true;
        File boot = new File(directory, "mobile-checkpoint.boot");
        write(boot.toPath(), "existing request");
        File request = new File(directory, "mobile-checkpoint.request");
        write(request.toPath(), "pending save request");
        activity.startGame();
        activity.startGame();
        Intent game = shadowOf(activity).getNextStartedActivity();
        assertNotNull(game);
        assertNull(game.getStringExtra(HaloActivity.CHECKPOINT_BOOT_ID));
        assertNull(shadowOf(activity).getNextStartedActivity());
        assertEquals("existing request", read(boot.toPath()));
        assertEquals("pending save request", read(request.toPath()));
        assertFilesUntouched();
    }

    @Test public void coldInviteBypassesCampaignChoiceAndClearsAbandonedBoot() throws Exception {
        activity.setIntent(new Intent(Intent.ACTION_VIEW, Uri.parse("halo://join/test")));
        File boot = new File(directory, "mobile-checkpoint.boot");
        write(boot.toPath(), "abandoned request");
        File request = new File(directory, "mobile-checkpoint.request");
        write(request.toPath(), "abandoned save request");
        activity.startGame();
        joinWorker();
        Intent game = shadowOf(activity).getNextStartedActivity();
        assertNotNull(game);
        assertNull(game.getStringExtra(HaloActivity.CHECKPOINT_BOOT_ID));
        assertFalse(boot.exists());
        assertFalse(request.exists());
        assertFilesUntouched();
    }

    @Test public void exitOnlyControllerDoesNotLaunchGameAgain() {
        Intent exit = new Intent().putExtra("exit-only", true);
        ProfileRestartActivity controller = Robolectric.buildActivity(ProfileRestartActivity.class, exit)
            .create().get();
        assertNull(shadowOf(controller).getNextStartedActivity());
        assertTrue(controller.isFinishing());
    }

    @Test public void graphicsRestartStillUsesLauncher() {
        ProfileRestartActivity controller = Robolectric.buildActivity(ProfileRestartActivity.class).create().get();
        Intent next = shadowOf(controller).getNextStartedActivity();
        assertNotNull(next);
        assertEquals(LauncherActivity.class.getName(), next.getComponent().getClassName());
    }
}
