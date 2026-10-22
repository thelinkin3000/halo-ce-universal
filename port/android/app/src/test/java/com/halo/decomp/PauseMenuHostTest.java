package com.halo.decomp;

import android.app.AlertDialog;
import android.app.Application;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.libsdl.app.SDLActivity;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.Implementation;
import org.robolectric.annotation.Implements;
import org.robolectric.annotation.LooperMode;
import org.robolectric.shadows.ShadowActivity;
import org.robolectric.shadows.ShadowAlertDialog;
import org.robolectric.util.ReflectionHelpers;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

/** The real pause dialog, with only the native engine boundary replaced. */
@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w960dp-h540dp-land-mdpi",
    instrumentedPackages = {"com.halo.decomp", "org.libsdl.app"},
    shadows = {PauseMenuHostTest.EngineBoundary.class, TouchControlsMultitouchHostTest.NativeSink.class})
@LooperMode(LooperMode.Mode.PAUSED)
public class PauseMenuHostTest {
    private RecordingActivity activity;
    private TouchControls controls;

    public static class RecordingActivity extends HaloActivity {
        int exits, saveRequests;
        @Override void exitCheckpointGame() { exits++; }
        @Override void startCheckpointSave() { saveRequests++; }
    }

    @Implements(SDLActivity.class)
    public static class EngineBoundary extends ShadowActivity {
        static int pauses, resumes;
        @Implementation protected void pauseNativeThread() { pauses++; }
        @Implementation protected void resumeNativeThread() { resumes++; }
        @Implementation protected void onResume() { }
        @Implementation protected void onPause() { }
        @Implementation public void onWindowFocusChanged(boolean hasFocus) { }
    }

    @Before public void prepare() {
        EngineBoundary.pauses = EngineBoundary.resumes = 0;
        activity = Robolectric.buildActivity(RecordingActivity.class).get();
        FrameLayout root = new FrameLayout(activity);
        activity.setContentView(root);
        controls = new TouchControls(activity);
        root.addView(controls);
        controls.layout(0, 0, 960, 540);
        ReflectionHelpers.setField(controls, "showing", true);
        ReflectionHelpers.setField(activity, "touchControls", controls);
        ReflectionHelpers.setField(activity, "activityResumed", true);
    }

    @After public void closeDialogs() {
        ReflectionHelpers.setField(activity, "activityResumed", false);
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        if (dialog != null && dialog.isShowing())
            dialog.dismiss();
        AlertDialog pause = ReflectionHelpers.getField(activity, "touchPauseDialog");
        if (pause != null && pause.isShowing()) pause.dismiss();
        controls.stopDeviceInput();
        shadowOf(Looper.getMainLooper()).idle();
    }

    @Test public void pauseShowsUsableActionsAndStopsEngine() {
        activity.showTouchPauseMenu();
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("Pausa", shadowOf(dialog).getTitle());
        assertEquals("Reanudar", dialog.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
        assertEquals("Ajustes de controles", dialog.getButton(AlertDialog.BUTTON_NEUTRAL).getText().toString());
        assertEquals(1, EngineBoundary.pauses);
        assertFalse((boolean) ReflectionHelpers.getField(controls, "deviceInputActive"));
    }

    @Test public void repeatedPauseKeepsOneDialog() {
        activity.showTouchPauseMenu();
        AlertDialog original = ShadowAlertDialog.getLatestAlertDialog();
        activity.showTouchPauseMenu();
        assertSame(original, ShadowAlertDialog.getLatestAlertDialog());
        assertEquals(1, EngineBoundary.pauses);
    }

    @Test public void backgroundDismissNeverResumesEngine() {
        activity.showTouchPauseMenu();
        ReflectionHelpers.setField(activity, "activityResumed", false);
        ShadowAlertDialog.getLatestAlertDialog().getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertNull(ReflectionHelpers.getField(activity, "touchPauseDialog"));
        assertEquals(0, EngineBoundary.resumes);
    }

    @Test public void focusAndResumeKeepOpenPausePaused() {
        activity.showTouchPauseMenu();
        activity.onWindowFocusChanged(true);
        activity.onResume();
        assertTrue(EngineBoundary.pauses >= 3);
        assertEquals(0, EngineBoundary.resumes);
        assertTrue(ShadowAlertDialog.getLatestAlertDialog().isShowing());
    }

    @Test public void editorCannotResumeWhilePauseIsOpen() {
        activity.showTouchPauseMenu();
        activity.setHudEditorPaused(false);
        assertEquals(2, EngineBoundary.pauses);
        assertEquals(0, EngineBoundary.resumes);
    }

    @Test public void settingsOpensRealControlsOptions() {
        activity.showTouchPauseMenu();
        AlertDialog pause = ShadowAlertDialog.getLatestAlertDialog();
        pause.getButton(AlertDialog.BUTTON_NEUTRAL).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        AlertDialog options = ShadowAlertDialog.getLatestAlertDialog();
        assertNotSame(pause, options);
        assertEquals("Halo · Controles", shadowOf(options).getTitle());
        assertFalse(pause.isShowing());
        assertEquals(1, (int) ReflectionHelpers.getField(controls, "openDialogs"));
    }

    @Test public void hardwareBackOpensPauseOnlyOnRelease() {
        assertTrue(activity.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK)));
        assertNull(ReflectionHelpers.getField(activity, "touchPauseDialog"));
        assertTrue(activity.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK)));
        assertEquals("Pausa", shadowOf(ShadowAlertDialog.getLatestAlertDialog()).getTitle());
    }

    @Test public void canceledBackDoesNotOpenPause() {
        KeyEvent canceled = new KeyEvent(0, 0, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK,
            0, 0, -1, 0, KeyEvent.FLAG_CANCELED);
        assertTrue(activity.dispatchKeyEvent(canceled));
        assertNull(ReflectionHelpers.getField(activity, "touchPauseDialog"));
    }

    @Test public void originalMenuActionClosesPauseAndDefersWhileActivityIsNotResumed() {
        activity.showTouchPauseMenu();
        AlertDialog pause = ShadowAlertDialog.getLatestAlertDialog();
        Button original = button(pause.getWindow().getDecorView(), "Configuración interna de Halo");
        assertNotNull(original);
        ReflectionHelpers.setField(activity, "activityResumed", false);
        original.performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertFalse(pause.isShowing());
        assertNull(ReflectionHelpers.getField(activity, "touchPauseDialog"));
        assertTrue((boolean) ReflectionHelpers.getField(activity, "originalMenuRequested"));
        assertEquals(0, EngineBoundary.resumes);
        assertEquals(0, TouchControlsMultitouchHostTest.NativeSink.state[6]);
    }

    @Test public void settingsEntryTenRequestsTheOriginalMenuWithoutStartingInactiveGameplay() {
        ReflectionHelpers.setField(activity, "activityResumed", false);
        controls.showOptions();
        AlertDialog settings = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals(11, settings.getListView().getAdapter().getCount());
        View row = settings.getListView().getAdapter().getView(10, null, settings.getListView());
        settings.getListView().performItemClick(row, 10, 10);
        shadowOf(Looper.getMainLooper()).idle();
        assertFalse(settings.isShowing());
        assertTrue((boolean) ReflectionHelpers.getField(activity, "originalMenuRequested"));
        assertEquals(0, EngineBoundary.resumes);
    }

    @Test public void cancelExitWithoutSavingKeepsThePausedGameAndNeverRequestsASave() {
        activity.showTouchPauseMenu();
        AlertDialog pause = ShadowAlertDialog.getLatestAlertDialog();
        Button exit = button(pause.getWindow().getDecorView(), "Salir sin guardar");
        assertNotNull(exit);
        exit.performClick();
        AlertDialog confirm = ShadowAlertDialog.getLatestAlertDialog();
        assertNotSame(pause, confirm);
        assertEquals("¿Salir sin guardar?", shadowOf(confirm).getTitle());
        confirm.getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertTrue(pause.isShowing());
        assertSame(pause, ReflectionHelpers.getField(activity, "touchPauseDialog"));
        assertEquals(0, activity.exits);
        assertEquals(0, activity.saveRequests);
        assertFalse((boolean) ReflectionHelpers.getField(activity, "checkpointExiting"));
        assertEquals(0, EngineBoundary.resumes);
    }

    @Test public void confirmedExitWithoutSavingUsesExitPathWithoutCheckpointWork() {
        activity.showTouchPauseMenu();
        AlertDialog pause = ShadowAlertDialog.getLatestAlertDialog();
        Button exit = button(pause.getWindow().getDecorView(), "Salir sin guardar");
        assertNotNull(exit);
        exit.performClick();
        AlertDialog confirm = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("Salir sin guardar", confirm.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
        confirm.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(1, activity.exits);
        assertEquals(0, activity.saveRequests);
        assertTrue((boolean) ReflectionHelpers.getField(activity, "checkpointExiting"));
        assertNull(ReflectionHelpers.getField(activity, "checkpointWorker"));
        assertNull(ReflectionHelpers.getField(activity, "checkpointRequestId"));
        assertEquals(0, EngineBoundary.resumes);
    }

    private Button button(View view, String text) {
        if (view instanceof Button && text.contentEquals(((Button) view).getText())) return (Button) view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                Button found = button(group.getChildAt(i), text);
                if (found != null) return found;
            }
        }
        return null;
    }
}
