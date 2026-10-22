package com.halo.decomp;

import android.app.AlertDialog;
import android.app.Application;
import android.content.SharedPreferences;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.LooperMode;
import org.robolectric.shadows.ShadowAlertDialog;
import org.robolectric.util.ReflectionHelpers;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

/** Calibrates production camera routing at its JNI boundary, without frame-rate assumptions. */
@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35},
    qualifiers = "w960dp-h540dp-land-mdpi", instrumentedPackages = {"com.halo.decomp", "org.libsdl.app"},
    shadows = {TouchControlsMultitouchHostTest.NativeSink.class, PauseMenuHostTest.EngineBoundary.class})
@LooperMode(LooperMode.Mode.PAUSED)
public class TouchCameraTurnHostTest {
    private TouchControls controls;
    private HaloActivity activity;
    private long downTime;

    @Before public void createControls() {
        TouchControlsMultitouchHostTest.NativeSink.bindingSerial = 0;
        TouchControlsMultitouchHostTest.NativeSink.suppliedBindings = null;
        activity = Robolectric.buildActivity(HaloActivity.class).get();
        activity.getSharedPreferences("touch-layout-v1", 0).edit().clear().commit();
        controls = new TouchControls(activity);
        controls.layout(0, 0, 960, 540);
        ReflectionHelpers.setField(controls, "showing", true);
        controls.reset();
        downTime = SystemClock.uptimeMillis();
    }

    @After public void closeControls() {
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        if (dialog != null && dialog.isShowing()) dialog.dismiss();
        controls.stopDeviceInput();
    }

    @Test public void defaultHorizontalGainImprovesTurnWithoutChangingVerticalAim() {
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        send(MotionEvent.ACTION_MOVE, 100, 910, 220);
        assertEquals(765f, x(), .001f);
        assertEquals(-20f, y(), .001f);
        assertEquals(96.430f, (float) Math.toDegrees(x() * TouchControls.LOOK_RADIANS_PER_UNIT), .01f);
    }

    @Test public void classicModeRetainsOriginalHorizontalTravel() {
        ReflectionHelpers.setField(controls, "horizontalSensitivity", 1f);
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        send(MotionEvent.ACTION_MOVE, 100, 910, 220);
        assertEquals(340f, x(), .001f);
        assertEquals(-20f, y(), .001f);
    }

    @Test public void sensitivityScalesBothAxesAndHorizontalFactorOnlyScalesYaw() {
        ReflectionHelpers.setField(controls, "sensitivity", .5f);
        ReflectionHelpers.setField(controls, "horizontalSensitivity", 4f);
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        send(MotionEvent.ACTION_MOVE, 100, 590, 250);
        assertEquals(40f, x(), .001f);
        assertEquals(5f, y(), .001f);
    }

    @Test public void quarterPixelMotionIsNeverRoundedAway() {
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        for (int i = 1; i <= 8; i++)
            send(MotionEvent.ACTION_MOVE, i * 10, 570 + i * .25f, 240 - i * .25f);
        assertEquals(4.5f, x(), .0001f);
        assertEquals(-2f, y(), .0001f);
    }

    @Test public void finalLiftContributesItsLastCameraDisplacementExactlyOnce() {
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        send(MotionEvent.ACTION_MOVE, 10, 580, 238);
        send(MotionEvent.ACTION_UP, 20, 590, 235);
        assertEquals(45f, x(), .001f);
        assertEquals(-5f, y(), .001f);
        assertEquals(2, TouchControlsMultitouchHostTest.NativeSink.lookCalls);
    }

    @Test public void unchangedLiftDoesNotSubmitDuplicateCameraInput() {
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        send(MotionEvent.ACTION_MOVE, 10, 590, 235);
        send(MotionEvent.ACTION_UP, 20, 590, 235);
        assertEquals(45f, x(), .001f);
        assertEquals(1, TouchControlsMultitouchHostTest.NativeSink.lookCalls);
    }

    @Test public void batchedMotionProcessesEveryHistoricalSampleInOrder() {
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        MotionEvent move = motion(MotionEvent.ACTION_MOVE, 10, 620, 240);
        move.addBatch(downTime + 20, 640, 235, 1, 1, 0);
        move.addBatch(downTime + 30, 660, 230, 1, 1, 0);
        try { assertTrue(controls.onTouchEvent(move)); }
        finally { move.recycle(); }
        assertEquals(202.5f, x(), .001f);
        assertEquals(-10f, y(), .001f);
        assertEquals(3, TouchControlsMultitouchHostTest.NativeSink.lookCalls);
    }

    @Test public void batchedReversalCancelsWithoutLosingItsIntermediateInput() {
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        MotionEvent move = motion(MotionEvent.ACTION_MOVE, 10, 590, 240);
        move.addBatch(downTime + 20, 550, 240, 1, 1, 0);
        move.addBatch(downTime + 30, 570, 240, 1, 1, 0);
        try { assertTrue(controls.onTouchEvent(move)); }
        finally { move.recycle(); }
        assertEquals(0f, x(), .001f);
        assertEquals(3, TouchControlsMultitouchHostTest.NativeSink.lookCalls);
    }

    @Test public void speedDoesNotChangeTheTravelOfTheSameSwipe() {
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        send(MotionEvent.ACTION_MOVE, 40, 670, 240);
        float fast = x();
        controls.reset();
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        send(MotionEvent.ACTION_MOVE, 1500, 670, 240);
        assertEquals(fast, x(), .001f);
    }

    @Test public void oldLayoutWithoutHorizontalPropertyGetsNewDefault() {
        String old = new TouchLayout().exportConfiguration(1f)
            .replaceAll("(?m)^horizontal-sensitivity=.*(?:\\r?\\n|$)", "");
        assertEquals(2.25f, TouchLayout.importConfiguration(old).horizontalSensitivity, 0);
    }

    @Test public void configurationAndControlImportPreserveHorizontalGain() {
        String text = new TouchLayout().exportConfiguration(.75f, 3.5f);
        controls.importLayout(text);
        TouchLayout.Configuration saved = TouchLayout.importConfiguration(controls.exportLayout());
        assertEquals(.75f, saved.sensitivity, 0);
        assertEquals(3.5f, saved.horizontalSensitivity, 0);
    }

    @Test public void invalidHorizontalGainDoesNotReplaceTheActiveConfiguration() {
        String original = controls.exportLayout();
        for (String invalid : new String[] {"NaN", "Infinity", "0.99", "4.01", "wrong"}) {
            String malformed = original.replaceAll("(?m)^horizontal-sensitivity=.*$",
                "horizontal-sensitivity=" + invalid);
            try {
                controls.importLayout(malformed);
                fail("Accepted invalid horizontal gain: " + invalid);
            } catch (IllegalArgumentException expected) {
                assertEquals(2.25f, TouchLayout.importConfiguration(controls.exportLayout()).horizontalSensitivity, 0);
            }
        }
    }

    @Test public void swipeInversionReversesOnlyTheSelectedNativeLookAxis() {
        assertFalse((boolean) ReflectionHelpers.getField(controls, "invertSwipeX"));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "invertSwipeY"));
        for (boolean[] axes : new boolean[][] {{false, false}, {true, false}, {false, true}, {true, true}}) {
            controls.reset();
            ReflectionHelpers.setField(controls, "invertSwipeX", axes[0]);
            ReflectionHelpers.setField(controls, "invertSwipeY", axes[1]);
            send(MotionEvent.ACTION_DOWN, 0, 570, 240);
            send(MotionEvent.ACTION_MOVE, 10, 590, 250);
            send(MotionEvent.ACTION_UP, 20, 590, 250);
            assertEquals(axes[0] ? -45f : 45f, x(), .001f);
            assertEquals(axes[1] ? -10f : 10f, y(), .001f);
            assertEquals(1, TouchControlsMultitouchHostTest.NativeSink.lookCalls);
        }
    }

    @Test public void cancelledSensitivityDraftKeepsDirectionAndCameraConfiguration() {
        String before = controls.exportLayout().replaceAll("(?m)^#.*(?:\\r?\\n|$)", "");
        AlertDialog dialog = sensitivityDialog();
        draft(dialog.getWindow().getDecorView()).setInvertedAxes(true, true);
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertFalse(preferences().getBoolean("invert-swipe-x", false));
        assertFalse(preferences().getBoolean("invert-swipe-y", false));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "invertSwipeX"));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "invertSwipeY"));
        assertEquals(before, controls.exportLayout().replaceAll("(?m)^#.*(?:\\r?\\n|$)", ""));
    }

    @Test public void savedSensitivityDirectionPersistsAndControlsANewInstance() {
        AlertDialog dialog = sensitivityDialog();
        LookSensitivityPanel panel = draft(dialog.getWindow().getDecorView());
        panel.setInvertedAxes(true, false);
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertFalse(dialog.isShowing());
        assertTrue(preferences().getBoolean("invert-swipe-x", false));
        assertFalse(preferences().getBoolean("invert-swipe-y", true));
        controls.stopDeviceInput();
        controls = new TouchControls(activity);
        controls.layout(0, 0, 960, 540);
        ReflectionHelpers.setField(controls, "showing", true);
        controls.reset();
        send(MotionEvent.ACTION_DOWN, 0, 570, 240);
        send(MotionEvent.ACTION_UP, 20, 590, 250);
        assertEquals(-45f, x(), .001f);
        assertEquals(10f, y(), .001f);
    }

    @Test public void restoringDirectionIsOnlyADraftUntilSave() {
        preferences().edit().putBoolean("invert-swipe-x", true).putBoolean("invert-swipe-y", true).commit();
        ReflectionHelpers.setField(controls, "invertSwipeX", true);
        ReflectionHelpers.setField(controls, "invertSwipeY", true);
        AlertDialog dialog = sensitivityDialog();
        LookSensitivityPanel panel = draft(dialog.getWindow().getDecorView());
        assertTrue(panel.invertedHorizontal());
        assertTrue(panel.invertedVertical());
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).performClick();
        assertFalse(panel.invertedHorizontal());
        assertFalse(panel.invertedVertical());
        assertTrue(preferences().getBoolean("invert-swipe-x", false));
        assertTrue((boolean) ReflectionHelpers.getField(controls, "invertSwipeX"));
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertFalse(preferences().getBoolean("invert-swipe-x", true));
        assertFalse(preferences().getBoolean("invert-swipe-y", true));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "invertSwipeX"));
        assertFalse((boolean) ReflectionHelpers.getField(controls, "invertSwipeY"));
    }

    private SharedPreferences preferences() { return activity.getSharedPreferences("touch-layout-v1", 0); }

    private AlertDialog sensitivityDialog() {
        ReflectionHelpers.callInstanceMethod(controls, "showSensitivity");
        shadowOf(Looper.getMainLooper()).idle();
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertNotNull(draft(dialog.getWindow().getDecorView()));
        return dialog;
    }

    private LookSensitivityPanel draft(View view) {
        if (view instanceof LookSensitivityPanel) return (LookSensitivityPanel) view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                LookSensitivityPanel found = draft(group.getChildAt(i));
                if (found != null) return found;
            }
        }
        return null;
    }

    private float x() { return TouchControlsMultitouchHostTest.NativeSink.lookX; }
    private float y() { return TouchControlsMultitouchHostTest.NativeSink.lookY; }

    private MotionEvent motion(int action, long elapsed, float x, float y) {
        MotionEvent event = MotionEvent.obtain(downTime, downTime + elapsed, action, x, y, 0);
        event.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        return event;
    }

    private void send(int action, long elapsed, float x, float y) {
        MotionEvent event = motion(action, elapsed, x, y);
        try { assertTrue(controls.onTouchEvent(event)); }
        finally { event.recycle(); }
    }
}
