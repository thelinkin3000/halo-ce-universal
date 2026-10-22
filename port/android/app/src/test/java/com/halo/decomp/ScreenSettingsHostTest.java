package com.halo.decomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Spinner;
import android.widget.TextView;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.rules.TemporaryFolder;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.LooperMode;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

/** Host tests exercise production dialogs and file updates; no native renderer is loaded. */
@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w800dp-h360dp-land-mdpi")
@LooperMode(LooperMode.Mode.PAUSED)
public class ScreenSettingsHostTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    private ActivityController<Activity> controller;
    private Activity activity;
    private File game;
    private final List<AlertDialog> dialogs = new ArrayList<>();
    private final AtomicInteger restarts = new AtomicInteger();

    @Before public void createActivity() throws IOException {
        controller = Robolectric.buildActivity(Activity.class).setup();
        activity = controller.get();
        game = activity.getExternalFilesDir(null);
        assertNotNull(game);
        Files.createDirectories(game.toPath());
        Files.deleteIfExists(new File(game, "config.toml").toPath());
        Files.deleteIfExists(new File(game, "config.toml.before-resolution").toPath());
    }

    @After public void closeActivity() {
        for (AlertDialog dialog : dialogs)
            dialog.dismiss();
        controller.pause().stop().destroy();
    }

    @Test public void absentSettingsUseOriginalAutomaticResolution() {
        ScreenSettings.Selection value = ScreenSettings.readSelection("[audio]\nvolume = 0.3\n");
        assertEquals(0, value.width);
        assertEquals(1f, value.scale, 0f);
    }

    @Test public void readsQuotedKeysAndInlineComments() {
        ScreenSettings.Selection value = ScreenSettings.readSelection(
            "[display] # active\n'screen_width' = 1200 # format\n\"render_scale\" = 0.75 # pixels\n");
        assertEquals(1200, value.width);
        assertEquals(0.75f, value.scale, 0f);
    }

    @Test public void missingResolutionLimitKeepsLegacy480Semantics() {
        ScreenSettings.Selection value = ScreenSettings.readSelection(
            "[display]\nscreen_width = 0\nrender_scale = 0.75\n");
        assertNull(value.resolutionLimit);
        assertEquals(0.75f, ScreenSettings.effectiveScale(value, 1612, 720), 0f);
    }

    @Test public void readsNativeAndNumericResolutionLimitWithQuotedKeysAndComments() {
        ScreenSettings.Selection nativeValue = ScreenSettings.readSelection(
            "[display]\n'resolution_limit' = 'native' # phone pixels\nrender_scale = 1.0\n");
        assertEquals("native", nativeValue.resolutionLimit);
        assertEquals(1.5f, ScreenSettings.effectiveScale(nativeValue, 1612, 720), 0f);
        ScreenSettings.Selection limited = ScreenSettings.readSelection(
            "[display]\n\"resolution_limit\" = \"720\" # cap\nrender_scale = 0.5\n");
        assertEquals("720", limited.resolutionLimit);
        assertEquals(0.75f, ScreenSettings.effectiveScale(limited, 1920, 1080), 0f);
    }

    @Test public void resolutionLimitCapsAtPhoneHeightAndKeepsLowDisplayFallback() {
        ScreenSettings.Selection limited = ScreenSettings.readSelection(
            "[display]\nresolution_limit = \"2160\"\nrender_scale = 0.75\n");
        assertEquals(1.125f, ScreenSettings.effectiveScale(limited, 1612, 720), 0f);
        assertEquals(0.75f, ScreenSettings.effectiveScale(limited, 800, 360), 0f);
        ScreenSettings.Selection nativeValue = ScreenSettings.readSelection(
            "[display]\nresolution_limit = \"native\"\nrender_scale = 0.75\n");
        assertEquals(1.125f, ScreenSettings.effectiveScale(nativeValue, 720, 1612), 0f);
    }

    @Test public void effectiveScaleMultipliesNativeResolutionAfterClampingRenderScale() {
        ScreenSettings.Selection clipped = ScreenSettings.readSelection(
            "[display]\nresolution_limit = \"native\"\nrender_scale = 0.1\n");
        assertEquals(0.75f, ScreenSettings.effectiveScale(clipped, 1612, 720), 0f);
        ScreenSettings.Selection original = ScreenSettings.readSelection(
            "[display]\nresolution_limit = \"native\"\nrender_scale = NaN\n");
        assertEquals(1.5f, ScreenSettings.effectiveScale(original, 1612, 720), 0f);
    }

    @Test public void ignoresOtherSectionsAndMultilineFakeSettings() {
        String config = "[notes]\ntext = \"\"\"\n[display]\nscreen_width = 640\nrender_scale = 0.5\n\"\"\"\n"
            + "[display]\nscreen_width = 1200\nrender_scale = 0.9\n"
            + "[audio]\nscreen_width = 854\nrender_scale = 0.5\n";
        ScreenSettings.Selection value = ScreenSettings.readSelection(config);
        assertEquals(1200, value.width);
        assertEquals(0.9f, value.scale, 0.0001f);
    }

    @Test public void automaticPixelWidthMatchesNativeLandscapeAndPortraitMath() {
        assertEquals(1074, ScreenSettings.pixelWidth(0, 1f, 1612, 720));
        assertEquals(1074, ScreenSettings.pixelWidth(0, 1f, 720, 1612));
        assertEquals(852, ScreenSettings.pixelWidth(0, 1f, 1920, 1080));
    }

    @Test public void scaledPixelWidthRoundsHalfPixelsLikeNative() {
        assertEquals(806, ScreenSettings.pixelWidth(1074, 0.75f, 1612, 720));
        assertEquals(537, ScreenSettings.pixelWidth(1074, 0.5f, 1612, 720));
        assertEquals(480, ScreenSettings.pixelWidth(640, 0.75f, 1612, 720));
    }

    @Test public void automaticLogicalWidthUsesNativeBounds() {
        assertEquals(640, ScreenSettings.pixelWidth(0, 1f, 720, 720));
        assertEquals(1600, ScreenSettings.pixelWidth(0, 1f, 10000, 720));
    }

    @Test public void openingAndCancellingDoNotWriteOrRestart() throws IOException {
        String before = "[display]\nscreen_width = 640\nrender_scale = 0.5\n";
        write(game, "config.toml", before);
        AlertDialog dialog = open();
        assertTrue(hasText(dialog, "Pantalla y resolución"));
        assertTrue(hasText(dialog, "Revisar"));
        click(dialog, AlertDialog.BUTTON_NEGATIVE);
        assertEquals(before, read(game, "config.toml"));
        assertFalse(new File(game, "config.toml.before-resolution").exists());
        assertEquals(0, restarts.get());
    }

    @Test public void defaultDialogSelectsFullScreenAndOriginalDetail() {
        AlertDialog dialog = open();
        assertEquals("Pantalla completa (recomendado)", selected(format(dialog)));
        assertEquals("Original · 480p", selected(detail(dialog)));
        assertFalse(hasChoice(detail(dialog), "Nativa · pantalla del teléfono"));
        click(dialog, AlertDialog.BUTTON_NEGATIVE);
        assertFalse(new File(game, "config.toml").exists());
    }

    @Test public void nativeCapableDialogSelectsNativeAndPreviewsPhysicalPhonePixels() throws IOException {
        write(game, "config.toml", "[display]\nresolution_limit = \"native\"\nrender_scale = 1.0\n");
        activity.getWindow().getDecorView().layout(0, 0, 1612, 720);
        AlertDialog dialog = open();
        assertEquals("Nativa · pantalla del teléfono", selected(detail(dialog)));
        assertTrue(hasText(dialog, "Resolución interna: 1611 × 720"));
        assertFalse(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        click(dialog, AlertDialog.BUTTON_NEGATIVE);
    }

    @Test public void nativeResolutionResetChangesOnlyDraftAndRetainsCancelSafety() throws IOException {
        String before = "[display]\nresolution_limit = \"480\"\nscreen_width = 640\nrender_scale = 0.5\n";
        write(game, "config.toml", before);
        AlertDialog dialog = open();
        assertEquals("Mínima · 240p", selected(detail(dialog)));
        assertTrue(hasChoice(detail(dialog), "Nativa · pantalla del teléfono"));
        click(dialog, AlertDialog.BUTTON_NEUTRAL);
        assertEquals("Pantalla completa (recomendado)", selected(format(dialog)));
        assertEquals("Nativa · pantalla del teléfono", selected(detail(dialog)));
        assertTrue(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        assertEquals(before, read(game, "config.toml"));
        assertEquals(0, restarts.get());
        click(dialog, AlertDialog.BUTTON_NEGATIVE);
        assertEquals(before, read(game, "config.toml"));
        assertFalse(new File(game, "config.toml.before-resolution").exists());
    }

    @Test public void unchangedResolutionCannotTriggerReviewOrRestart() {
        AlertDialog dialog = open();
        assertFalse(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        select(detail(dialog), "Mínima · 240p");
        assertTrue(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        select(detail(dialog), "Original · 480p");
        assertFalse(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        assertFalse(new File(game, "config.toml").exists());
        assertEquals(0, restarts.get());
    }

    @Test public void currentCustomValuesAreShownWithoutSilentlyReplacingThem() throws IOException {
        write(game, "config.toml", "[display]\nscreen_width = 1200\nrender_scale = 0.9\n");
        AlertDialog dialog = open();
        assertTrue(selected(format(dialog)).contains("Actual"));
        assertTrue(selected(detail(dialog)).contains("Actual"));
        click(dialog, AlertDialog.BUTTON_NEGATIVE);
        ScreenSettings.Selection value = ScreenSettings.readSelection(read(game, "config.toml"));
        assertEquals(1200, value.width);
        assertEquals(0.9f, value.scale, 0.0001f);
    }

    @Test public void changingFormatAndDetailKeepsSelectionsIndependent() {
        AlertDialog dialog = open();
        select(format(dialog), "Clásica 4:3");
        assertEquals("Original · 480p", selected(detail(dialog)));
        select(detail(dialog), "Ligera · 360p");
        assertEquals("Clásica 4:3", selected(format(dialog)));
        select(format(dialog), "Panorámica 16:9");
        assertEquals("Ligera · 360p", selected(detail(dialog)));
        select(detail(dialog), "Mínima · 240p");
        assertEquals("Panorámica 16:9", selected(format(dialog)));
        assertFalse(new File(game, "config.toml").exists());
        assertEquals(0, restarts.get());
    }

    @Test public void restoreDefaultsOnlyChangesDraftUntilConfirmed() throws IOException {
        String before = "[display]\nscreen_width = 640\nrender_scale = 0.5\nmax_fps = 30\n";
        write(game, "config.toml", before);
        AlertDialog dialog = open();
        click(dialog, AlertDialog.BUTTON_NEUTRAL);
        assertTrue("Reset keeps editor open", dialog.isShowing());
        assertEquals("Pantalla completa (recomendado)", selected(format(dialog)));
        assertEquals("Original · 480p", selected(detail(dialog)));
        assertEquals(before, read(game, "config.toml"));
        assertEquals(0, restarts.get());
        click(dialog, AlertDialog.BUTTON_NEGATIVE);
        assertEquals(before, read(game, "config.toml"));
    }

    @Test public void reviewThenCancelDoesNotWriteOrRestart() throws IOException {
        String before = "[display]\nrenderer = \"vulkan\"\nscreen_width = 0\nrender_scale = 1.0\n";
        write(game, "config.toml", before);
        AlertDialog dialog = open();
        select(format(dialog), "Clásica 4:3");
        select(detail(dialog), "Mínima · 240p");
        click(dialog, AlertDialog.BUTTON_POSITIVE);
        AlertDialog confirm = lastDialog();
        assertNotSame(dialog, confirm);
        assertEquals("Aplicar y reiniciar", confirm.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
        click(confirm, AlertDialog.BUTTON_NEGATIVE);
        assertEquals(before, read(game, "config.toml"));
        assertFalse(new File(game, "config.toml.before-resolution").exists());
        assertEquals(0, restarts.get());
    }

    @Test public void confirmationWritesSelectionAndRestartsExactlyOnce() throws Exception {
        write(game, "config.toml", "[display]\nrenderer = \"vulkan\"\nmax_fps = 30\n");
        AlertDialog dialog = open();
        select(format(dialog), "Clásica 4:3");
        select(detail(dialog), "Ligera · 360p");
        click(dialog, AlertDialog.BUTTON_POSITIVE);
        click(lastDialog(), AlertDialog.BUTTON_POSITIVE);
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (restarts.get() == 0 && System.nanoTime() < deadline) {
            shadowOf(Looper.getMainLooper()).idle();
            Thread.sleep(10);
        }
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(1, restarts.get());
        ScreenSettings.Selection value = ScreenSettings.readSelection(read(game, "config.toml"));
        assertEquals(640, value.width);
        assertEquals(0.75f, value.scale, 0f);
        assertTrue(read(game, "config.toml").contains("max_fps = 30"));
    }

    @Test public void reviewingOriginal480FromNativeSavesTheSelectedLimit() throws Exception {
        String before = "[display]\nresolution_limit = \"native\"\nrenderer = \"gl\"\n"
            + "screen_width = 0\nrender_scale = 1.0\nmax_fps = 30\n";
        write(game, "config.toml", before);
        AlertDialog dialog = open();
        select(detail(dialog), "Original · 480p");
        assertTrue(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        click(dialog, AlertDialog.BUTTON_POSITIVE);
        click(lastDialog(), AlertDialog.BUTTON_POSITIVE);
        awaitRestart();
        assertEquals(before.replace("resolution_limit = \"native\"", "resolution_limit = \"480\""),
            read(game, "config.toml"));
        assertEquals(1, restarts.get());
    }

    @Test public void confirmingResetRestoresNativeOnCapableBuilds() throws Exception {
        write(game, "config.toml", "[display]\nresolution_limit = \"480\"\nrenderer = \"gl\"\n"
            + "screen_width = 640\nrender_scale = 0.5\nmax_fps = 30\n");
        AlertDialog dialog = open();
        click(dialog, AlertDialog.BUTTON_NEUTRAL);
        click(dialog, AlertDialog.BUTTON_POSITIVE);
        click(lastDialog(), AlertDialog.BUTTON_POSITIVE);
        awaitRestart();
        ScreenSettings.Selection value = ScreenSettings.readSelection(read(game, "config.toml"));
        assertEquals("native", value.resolutionLimit);
        assertEquals(0, value.width);
        assertEquals(1f, value.scale, 0f);
        assertTrue(read(game, "config.toml").contains("max_fps = 30"));
        assertEquals(1, restarts.get());
    }

    @Test public void appliesOnlyResolutionAndRequiredGlesKeepingCommentsAndCrlf() throws IOException {
        File root = temporary.newFolder();
        String before = "# user's config\r\n[display] # main\r\n"
            + "renderer = \"vulkan\" # GPU\r\nscreen_width = 0 # fit\r\nrender_scale = 1.0 # detail\r\n"
            + "max_fps = 30\r\nvsync = false\r\nshadow_limit = 4\r\nanti_aliasing = \"fxaa\"\r\n"
            + "interpolation = true\r\nper_pixel_lighting = false\r\n"
            + "[audio]\r\nvolume = 0.6 # keep\r\n[notes]\r\ntext = \"\"\"\r\n"
            + "[display]\r\nrenderer = \"vulkan\"\r\nscreen_width = 999\r\nrender_scale = 0.9\r\n\"\"\"\r\n";
        write(root, "config.toml", before);
        String init = "render_shadows false\r\nmap_name b30\r\n";
        write(root, "init.txt", init);
        ScreenSettings.applyFiles(root, 640, 0.75f);
        String expected = before.replace("renderer = \"vulkan\" # GPU", "renderer = \"gl\" # GPU")
            .replace("screen_width = 0 # fit", "screen_width = 640 # fit")
            .replace("render_scale = 1.0 # detail", "render_scale = 0.75 # detail");
        assertEquals(expected, read(root, "config.toml"));
        assertEquals(init, read(root, "init.txt"));
    }

    @Test public void originalDetailPreservesExistingVulkanRendererAndOtherOptions() throws IOException {
        File root = temporary.newFolder();
        String before = "[display]\nrenderer = \"vulkan\" # keep GPU\nscreen_width = 640\n"
            + "render_scale = 0.5\nmax_fps = 60\nhigh_res_hud = true\n[debug]\ngpu_stats = false\n";
        write(root, "config.toml", before);
        ScreenSettings.applyFiles(root, 0, 1f);
        assertEquals(before.replace("screen_width = 640", "screen_width = 0")
            .replace("render_scale = 0.5", "render_scale = 1.0"), read(root, "config.toml"));
    }

    @Test public void originalDetailDoesNotInsertARendererOverride() throws IOException {
        File root = temporary.newFolder();
        write(root, "config.toml", "[display]\nmax_fps = 30\n");
        ScreenSettings.applyFiles(root, 0, 1f);
        String after = read(root, "config.toml");
        assertFalse(after.contains("renderer"));
        assertTrue(after.contains("max_fps = 30"));
        assertEquals(0, ScreenSettings.readSelection(after).width);
        assertEquals(1f, ScreenSettings.readSelection(after).scale, 0f);
    }

    @Test public void explicit480DetailResetsNativeLimitWithoutChangingOtherSettings() throws IOException {
        File root = temporary.newFolder();
        String before = "[display]\nresolution_limit = \"native\" # phone\nrenderer = \"vulkan\"\n"
            + "screen_width = 0\nrender_scale = 1.0\nmax_fps = 30\n[audio]\nvolume = 0.6\n";
        write(root, "config.toml", before);
        ScreenSettings.applyFiles(root, 640, 0.75f);
        String expected = before.replace("resolution_limit = \"native\"", "resolution_limit = \"480\"")
            .replace("renderer = \"vulkan\"", "renderer = \"gl\"")
            .replace("screen_width = 0", "screen_width = 640")
            .replace("render_scale = 1.0", "render_scale = 0.75");
        assertEquals(expected, read(root, "config.toml"));
        assertEquals(before, read(root, "config.toml.before-resolution"));
        ScreenSettings.Selection value = ScreenSettings.readSelection(expected);
        assertEquals(0.75f, ScreenSettings.effectiveScale(value, 1612, 720), 0f);
    }

    @Test public void explicitNativeCanRestorePhoneResolutionWithoutForcingRenderer() throws IOException {
        File root = temporary.newFolder();
        String before = "[display]\nresolution_limit = \"480\"\nrenderer = \"vulkan\" # keep\n"
            + "screen_width = 640\nrender_scale = 0.5\nmax_fps = 60\n";
        write(root, "config.toml", before);
        ScreenSettings.applyFiles(root, 0, 1f, "native");
        assertEquals(before.replace("resolution_limit = \"480\"", "resolution_limit = \"native\"")
            .replace("screen_width = 640", "screen_width = 0")
            .replace("render_scale = 0.5", "render_scale = 1.0"), read(root, "config.toml"));
        assertEquals(1.5f, ScreenSettings.effectiveScale(
            ScreenSettings.readSelection(read(root, "config.toml")), 1612, 720), 0f);
    }

    @Test public void explicitNumericCapRoundTripsWhileRetainingOriginalBackup() throws IOException {
        File root = temporary.newFolder();
        String before = "[display]\nresolution_limit = \"native\"\nmax_fps = 30\n";
        write(root, "config.toml", before);
        ScreenSettings.applyFiles(root, 0, 0.75f, "720");
        ScreenSettings.Selection value = ScreenSettings.readSelection(read(root, "config.toml"));
        assertEquals("720", value.resolutionLimit);
        assertEquals(1.125f, ScreenSettings.effectiveScale(value, 1920, 1080), 0f);
        ScreenSettings.applyFiles(root, 0, 1f, "native");
        assertEquals(before, read(root, "config.toml.before-resolution"));
    }

    @Test public void rejectsInvalidExplicitResolutionLimitsBeforeBackupOrWrite() throws IOException {
        File root = temporary.newFolder();
        String before = "[display]\nresolution_limit = \"native\"\nrender_scale = 1.0\n";
        write(root, "config.toml", before);
        for (String limit : new String[] {"479", "4321", "720p", "", "720\n[debug]", "NaN"})
            assertThrows(limit, IOException.class, () -> ScreenSettings.applyFiles(root, 0, 1f, limit));
        assertEquals(before, read(root, "config.toml"));
        assertFalse(new File(root, "config.toml.before-resolution").exists());
    }

    @Test public void resolutionChangeDoesNotCreateOrBackUpInitFile() throws IOException {
        File root = temporary.newFolder();
        ScreenSettings.applyFiles(root, 640, 0.5f);
        assertFalse(new File(root, "init.txt").exists());
        assertFalse(new File(root, "init.txt.before-resolution").exists());
        ScreenSettings.Selection value = ScreenSettings.readSelection(read(root, "config.toml"));
        assertEquals(640, value.width);
        assertEquals(0.5f, value.scale, 0f);
    }

    @Test public void firstConfigBackupIsNotOverwrittenOnLaterChanges() throws IOException {
        File root = temporary.newFolder();
        String before = "[display]\nscreen_width = 0\nrender_scale = 1.0\nmax_fps = 30\n";
        write(root, "config.toml", before);
        ScreenSettings.applyFiles(root, 640, 0.5f);
        assertEquals(before, read(root, "config.toml.before-resolution"));
        ScreenSettings.applyFiles(root, 1200, 0.75f);
        assertEquals(before, read(root, "config.toml.before-resolution"));
        ScreenSettings.Selection value = ScreenSettings.readSelection(read(root, "config.toml"));
        assertEquals(1200, value.width);
        assertEquals(0.75f, value.scale, 0f);
    }

    @Test public void addsMissingDisplaySectionWithoutChangingExistingSections() throws IOException {
        File root = temporary.newFolder();
        String before = "[audio]\nvolume = 0.25\n[debug]\nprofile_hz = 0\n";
        write(root, "config.toml", before);
        ScreenSettings.applyFiles(root, 640, 0.5f);
        String after = read(root, "config.toml");
        assertTrue(after.startsWith(before));
        assertTrue(after.contains("[display]\n"));
        assertEquals(640, ScreenSettings.readSelection(after).width);
        assertEquals(0.5f, ScreenSettings.readSelection(after).scale, 0f);
    }

    @Test public void fileFailureIsReportedAsIOException() throws IOException {
        File root = temporary.newFile();
        assertThrows(IOException.class, () -> ScreenSettings.applyFiles(root, 640, 0.5f));
    }

    @Test public void malformedOrOutOfRangeValuesMatchNativeSafeDefaultsAndBounds() {
        ScreenSettings.Selection malformed = ScreenSettings.readSelection(
            "[display]\nscreen_width = garbage\nrender_scale = NaN\n");
        assertEquals(0, malformed.width);
        assertEquals(1f, malformed.scale, 0f);
        ScreenSettings.Selection clipped = ScreenSettings.readSelection(
            "[display]\nscreen_width = 1_799\nrender_scale = 0.1\n");
        assertEquals(1600, clipped.width);
        assertEquals(0.5f, clipped.scale, 0f);
        ScreenSettings.Selection odd = ScreenSettings.readSelection(
            "[display]\nscreen_width = 1_075\nrender_scale = 2.0\n");
        assertEquals(1074, odd.width);
        assertEquals(1f, odd.scale, 0f);
    }

    @Test public void rejectsInvalidRequestedResolutionBeforeChangingFiles() throws IOException {
        File root = temporary.newFolder();
        String before = "[display]\nscreen_width = 0\nrender_scale = 1.0\n";
        write(root, "config.toml", before);
        assertThrows(IOException.class, () -> ScreenSettings.applyFiles(root, 639, 1f));
        assertThrows(IOException.class, () -> ScreenSettings.applyFiles(root, 1075, 1f));
        assertThrows(IOException.class, () -> ScreenSettings.applyFiles(root, 1602, 1f));
        assertThrows(IOException.class, () -> ScreenSettings.applyFiles(root, 0, Float.NaN));
        assertThrows(IOException.class, () -> ScreenSettings.applyFiles(root, 0, Float.POSITIVE_INFINITY));
        assertThrows(IOException.class, () -> ScreenSettings.applyFiles(root, 0, 0.4f));
        assertEquals(before, read(root, "config.toml"));
        assertFalse(new File(root, "config.toml.before-resolution").exists());
    }

    @Test public void unreadableConfigDirectoryIsNotReplaced() throws IOException {
        File root = temporary.newFolder();
        File config = new File(root, "config.toml");
        assertTrue(config.mkdir());
        write(config, "keep.txt", "keep");
        assertThrows(IOException.class, () -> ScreenSettings.applyFiles(root, 640, 0.5f));
        assertTrue(config.isDirectory());
        assertEquals("keep", read(config, "keep.txt"));
    }

    @Test public void reopenedDialogLoadsSavedSelection() throws IOException {
        write(game, "config.toml", "[display]\nscreen_width = 640\nrender_scale = 0.75\n");
        AlertDialog first = open();
        assertEquals("Clásica 4:3", selected(format(first)));
        assertEquals("Ligera · 360p", selected(detail(first)));
        click(first, AlertDialog.BUTTON_NEGATIVE);
        ScreenSettings.applyFiles(game, 0, 0.5f);
        AlertDialog second = open();
        assertEquals("Pantalla completa (recomendado)", selected(format(second)));
        assertEquals("Mínima · 240p", selected(detail(second)));
        assertEquals(0, restarts.get());
    }

    private AlertDialog open() {
        ScreenSettings.show(activity, restarts::incrementAndGet, dialog -> {
            dialogs.add(dialog);
            dialog.show();
        });
        shadowOf(Looper.getMainLooper()).idle();
        return lastDialog();
    }

    private AlertDialog lastDialog() {
        assertFalse("A dialog was opened", dialogs.isEmpty());
        return dialogs.get(dialogs.size() - 1);
    }

    private void awaitRestart() throws InterruptedException {
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (restarts.get() == 0 && System.nanoTime() < deadline) {
            shadowOf(Looper.getMainLooper()).idle();
            Thread.sleep(10);
        }
        shadowOf(Looper.getMainLooper()).idle();
    }

    private static Spinner format(AlertDialog dialog) {
        return spinner(dialog, "Formato de pantalla");
    }

    private static Spinner detail(AlertDialog dialog) {
        return spinner(dialog, "Resolución interna");
    }

    private static Spinner spinner(AlertDialog dialog, String description) {
        List<View> views = new ArrayList<>();
        collect(dialog.getWindow().getDecorView(), views);
        for (View view : views)
            if (view instanceof Spinner && description.contentEquals(view.getContentDescription()))
                return (Spinner) view;
        throw new AssertionError("Missing spinner: " + description);
    }

    private static String selected(Spinner spinner) {
        return spinner.getSelectedItem().toString();
    }

    private static boolean hasChoice(Spinner spinner, String label) {
        for (int i = 0; i < spinner.getCount(); i++)
            if (label.equals(spinner.getItemAtPosition(i).toString())) return true;
        return false;
    }

    private static void select(Spinner spinner, String label) {
        for (int i = 0; i < spinner.getCount(); i++) {
            if (label.equals(spinner.getItemAtPosition(i).toString())) {
                spinner.setSelection(i);
                shadowOf(Looper.getMainLooper()).idle();
                return;
            }
        }
        throw new AssertionError("Missing selection: " + label);
    }

    private static void click(AlertDialog dialog, int button) {
        assertNotNull(dialog.getButton(button));
        assertTrue(dialog.getButton(button).performClick());
        shadowOf(Looper.getMainLooper()).idle();
    }

    private static boolean hasText(AlertDialog dialog, String text) {
        List<View> views = new ArrayList<>();
        collect(dialog.getWindow().getDecorView(), views);
        for (View view : views)
            if (view instanceof TextView && text.contentEquals(((TextView) view).getText()))
                return true;
        return false;
    }

    private static void collect(View view, List<View> views) {
        views.add(view);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++)
                collect(group.getChildAt(i), views);
        }
    }

    private static void write(File root, String name, String text) throws IOException {
        Files.write(new File(root, name).toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    private static String read(File root, String name) throws IOException {
        return new String(Files.readAllBytes(new File(root, name).toPath()), StandardCharsets.UTF_8);
    }
}
