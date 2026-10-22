package com.halo.decomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.content.Intent;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ScrollView;
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
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicBoolean;
import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w800dp-h360dp-land-mdpi")
@LooperMode(LooperMode.Mode.PAUSED)
public class DataPackagesHostTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    public static class TestActivity extends Activity {
        File root;
        @Override public File getExternalFilesDir(String type) { return root; }
    }
    public static class TestLauncherActivity extends LauncherActivity {
        File root;
        @Override public File getExternalFilesDir(String type) { return root; }
        @Override boolean hasRunningGame() { return false; }
        @Override void startGame() { fail("Missing data must not start the game"); }
    }
    private TestActivity activity;
    private TestLauncherActivity launcher;

    @Before public void prepare() throws Exception {
        activity = Robolectric.buildActivity(TestActivity.class).setup().get();
        activity.root = temporary.newFolder();
    }

    @After public void close() {
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        if (dialog != null)
            dialog.dismiss();
        if (launcher != null)
            launcher.finish();
        activity.finish();
    }

    @Test public void onlyAcceptsHttpsSourcesWithoutCredentialsOrFragments() {
        assertTrue(DataPackages.validAddress("https://example.com/data.zip?token=abc%2Fdef"));
        for (String address : new String[] {"", "http://example.com/a.iso", "file:///a.zip",
                "https://user:pass@example.com/a.zip", "https://example.com/a.zip#part",
                "https://example.com:70000/a", "https://bad host/a", "https://example.com/\na"})
            assertFalse(address, DataPackages.validAddress(address));
    }

    @Test public void invalidSourceStaysInFormWithoutStartingAnyDownload() {
        DataPackages.show(activity, () -> fail("must not launch game"), dialog -> dialog.show());
        AlertDialog form = ShadowAlertDialog.getLatestAlertDialog();
        EditText field = find(form.getWindow().getDecorView(), EditText.class, null);
        assertNotNull(field);
        assertEquals("", field.getText().toString());
        field.setText("http://example.com/data.zip");
        form.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        assertTrue(form.isShowing());
        assertNotNull(field.getError());
        assertFalse(DataPackages.isBusy());
        assertFalse(new File(activity.getCacheDir(), "game-data.partial").exists());
    }

    @Test public void installedMapsAreNotReplacedAndExistingFilesStayUntouched() throws Exception {
        File maps = new File(activity.root, "maps");
        assertTrue(maps.mkdir());
        File ui = new File(maps, "ui.map");
        Files.write(ui.toPath(), "existing map".getBytes(StandardCharsets.US_ASCII));
        DataPackages.show(activity, () -> fail("must not restart game"), dialog -> dialog.show());
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNull(find(dialog.getWindow().getDecorView(), EditText.class, null));
        assertEquals("existing map", new String(Files.readAllBytes(ui.toPath()), StandardCharsets.US_ASCII));
        assertFalse(DataPackages.isBusy());
    }

    @Test public void launcherOffersBothDownloadActionsInScrollableContent() throws Exception {
        LauncherActivity launcher = Robolectric.buildActivity(LauncherActivity.class).get();
        ReflectionHelpers.setField(launcher, "dataRoot", activity.root);
        ReflectionHelpers.callInstanceMethod(launcher, "buildInterface");
        View view = launcher.getWindow().getDecorView();
        assertNotNull(find(view, ScrollView.class, null));
        Button data = find(view, Button.class, "Descargar paquete de datos");
        assertNotNull(data);
        assertNotNull(find(view, Button.class, "Buscar actualización de la APK"));
        data.performClick();
        assertNotNull(find(ShadowAlertDialog.getLatestAlertDialog().getWindow().getDecorView(), EditText.class, null));
        launcher.finish();
    }

    @Test public void playWithoutDataOpensMediaFireHttpsFormBeforeAnyGameStartup() throws Exception {
        Button play = missingDataPlayButton();
        assertTrue(play.isEnabled());
        play.performClick();
        AlertDialog form = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(form);
        assertTrue(form.isShowing());
        assertEquals("Descargar paquete de datos", shadowOf(form).getTitle());
        EditText address = find(form.getWindow().getDecorView(), EditText.class, null);
        assertNotNull(address);
        assertEquals("", address.getText().toString());
        assertTrue(address.getHint().toString().contains("MediaFire"));
        assertTrue(address.getHint().toString().contains("HTTPS"));
        assertEquals("Descargar", form.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
        assertMissingDataFlowUntouched();
    }

    @Test public void cancellingMissingDataDownloadReturnsToTheSameLauncher() throws Exception {
        Button play = missingDataPlayButton();
        play.performClick();
        AlertDialog form = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(form);
        form.getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertFalse(form.isShowing());
        assertFalse(launcher.isFinishing());
        assertSame(play, find(launcher.getWindow().getDecorView(), Button.class, "Jugar / Continuar"));
        assertTrue(play.isEnabled());
        assertMissingDataFlowUntouched();
    }

    @Test public void playWithoutDataDoesNotOpenAnotherFlowWhileDownloadIsBusy() throws Exception {
        Button play = missingDataPlayButton();
        AlertDialog previous = ShadowAlertDialog.getLatestAlertDialog();
        AtomicBoolean busy = ReflectionHelpers.getStaticField(DataPackages.class, "busy");
        busy.set(true);
        try {
            play.performClick();
            shadowOf(Looper.getMainLooper()).idle();
            assertSame(previous, ShadowAlertDialog.getLatestAlertDialog());
            assertTrue(DataPackages.isBusy());
            assertNoGameStartupOrFileChanges();
        } finally {
            busy.set(false);
        }
    }

    private Button missingDataPlayButton() throws Exception {
        Files.write(new File(activity.root, "init.txt").toPath(), "startup fixture".getBytes(StandardCharsets.UTF_8));
        assertTrue(new File(activity.root, "save/z").mkdirs());
        Files.write(new File(activity.root, "save/z/mobile-checkpoint.bin").toPath(),
            "checkpoint fixture".getBytes(StandardCharsets.UTF_8));
        launcher = Robolectric.buildActivity(TestLauncherActivity.class).get();
        launcher.root = activity.root;
        ReflectionHelpers.setField(launcher, "dataRoot", activity.root);
        ReflectionHelpers.callInstanceMethod(launcher, "buildInterface");
        Button play = find(launcher.getWindow().getDecorView(), Button.class, "Jugar / Continuar");
        assertNotNull(play);
        assertFalse(new File(activity.root, "maps/ui.map").exists());
        return play;
    }

    private void assertMissingDataFlowUntouched() throws Exception {
        assertFalse(DataPackages.isBusy());
        assertNoGameStartupOrFileChanges();
    }

    private void assertNoGameStartupOrFileChanges() throws Exception {
        assertNull(shadowOf(launcher).getNextStartedActivity());
        assertFalse((Boolean) ReflectionHelpers.getField(launcher, "driverStarted"));
        assertFalse((Boolean) ReflectionHelpers.getField(launcher, "checkpointChoiceStarted"));
        assertNull(ReflectionHelpers.getField(launcher, "checkpointWorker"));
        assertEquals("startup fixture", new String(Files.readAllBytes(new File(activity.root, "init.txt").toPath()),
            StandardCharsets.UTF_8));
        assertEquals("checkpoint fixture", new String(Files.readAllBytes(
            new File(activity.root, "save/z/mobile-checkpoint.bin").toPath()), StandardCharsets.UTF_8));
        for (String suffix : new String[] { ".boot", ".request", ".status", ".tmp" })
            assertFalse(new File(activity.root, "save/z/mobile-checkpoint" + suffix).exists());
        assertFalse(new File(activity.root, "maps/ui.map").exists());
        assertFalse(new File(launcher.getCacheDir(), "game-data.partial").exists());
    }

    @Test public void blockedMediaFirePageCanOpenInUserBrowserWithoutDownloadingPayload() {
        String page = "https://www.mediafire.com/file/example123/package.iso/file";
        DataPackages.downloadFailure(activity, AlertDialog::show, "HTTP 403", page);
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("Abrir MediaFire", dialog.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
        assertTrue(shadowOf(dialog).getMessage().toString().contains("Elegir imagen de disco"));
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        Intent browser = shadowOf(activity).getNextStartedActivity();
        assertNotNull(browser);
        assertEquals(Intent.ACTION_VIEW, browser.getAction());
        assertEquals(page, browser.getDataString());
        assertFalse(new File(activity.getCacheDir(), "game-data.partial").exists());
    }

    @Test public void archiveErrorsDoNotOpenABrowserOrExposeAnUntrustedDestination() {
        for (String page : new String[] {null, "https://www.mediafire.com.evil.example/file/key/archive.iso"}) {
            DataPackages.downloadFailure(activity, AlertDialog::show, "El mapa no es compatible", page);
            AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
            assertEquals("Aceptar", dialog.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
            assertNull(shadowOf(activity).getNextStartedActivity());
            dialog.dismiss();
        }
    }

    @Test public void installedDemoPlaysImmediatelyWithoutNetworkOrChangingFiles() throws Exception {
        File maps = new File(activity.root, "maps");
        assertTrue(maps.mkdir());
        byte[] map = new byte[0x800];
        ByteBuffer.wrap(map).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(0, 0x68656164).putInt(4, 5).putInt(8, 0x800)
            .putInt(0x10, 0x800).putInt(0x14, 0).putInt(0x7fc, 0x666f6f74);
        Files.write(new File(maps, "ui.map").toPath(), map);
        Files.write(new File(maps, "demob30.map").toPath(), map);
        AtomicBoolean ready = new AtomicBoolean();
        DataPackages.playDemo(activity, () -> ready.set(true), dialog -> fail("No dialog or network needed"));
        assertTrue(ready.get());
        assertFalse(DataPackages.isBusy());
        assertArrayEquals(map, Files.readAllBytes(new File(maps, "ui.map").toPath()));
        assertArrayEquals(map, Files.readAllBytes(new File(maps, "demob30.map").toPath()));
        assertFalse(new File(activity.getCacheDir(), "game-data.partial").exists());
    }

    @Test public void demoCannotDownloadWithMissingStorageOrWhileAnotherTransferIsBusy() {
        activity.root = null;
        DataPackages.playDemo(activity, () -> fail("No storage"), AlertDialog::show);
        assertTrue(shadowOf(ShadowAlertDialog.getLatestAlertDialog()).getMessage().toString()
            .contains("almacenamiento"));
        AtomicBoolean busy = ReflectionHelpers.getStaticField(DataPackages.class, "busy");
        busy.set(true);
        try {
            activity.root = temporary.getRoot();
            DataPackages.playDemo(activity, () -> fail("Do not start a second transfer"), AlertDialog::show);
            assertTrue(shadowOf(ShadowAlertDialog.getLatestAlertDialog()).getMessage().toString()
                .contains("en curso"));
            assertFalse(new File(activity.getCacheDir(), "game-data.partial").exists());
        } finally {
            busy.set(false);
        }
    }

    private static <T extends View> T find(View view, Class<T> type, String text) {
        if (type.isInstance(view) && (text == null || view instanceof Button
                && text.contentEquals(((Button) view).getText())))
            return type.cast(view);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                T found = find(group.getChildAt(i), type, text);
                if (found != null)
                    return found;
            }
        }
        return null;
    }
}
