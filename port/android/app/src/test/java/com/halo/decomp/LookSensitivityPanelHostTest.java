package com.halo.decomp;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Looper;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.SeekBar;
import android.widget.Switch;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.annotation.LooperMode;
import org.robolectric.util.ReflectionHelpers;

import java.util.HashMap;
import java.util.Map;
import java.util.function.Predicate;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w800dp-h480dp-land-mdpi")
@LooperMode(LooperMode.Mode.PAUSED)
@GraphicsMode(GraphicsMode.Mode.LEGACY)
public class LookSensitivityPanelHostTest {
    private ActivityController<Activity> controller;
    private Activity activity;
    private FrameLayout root;
    private LookSensitivityPanel panel;
    private SharedPreferences preferences;
    private Map<String, ?> savedPreferences;

    @Before public void createPanel() {
        controller = Robolectric.buildActivity(Activity.class).setup();
        activity = controller.get();
        preferences = activity.getSharedPreferences("touch-layout-v1", Context.MODE_PRIVATE);
        assertTrue(preferences.edit().clear().putString("configuration", "saved-layout-sentinel")
            .putInt("hud-slot", 2).putBoolean("hidden", true).commit());
        savedPreferences = new HashMap<>(preferences.getAll());
        root = new FrameLayout(activity);
        activity.setContentView(root);
        install(new LookSensitivityPanel(activity, 1f, 1f));
    }

    @After public void closeActivity() {
        controller.pause().stop().destroy();
    }

    @Test public void valuesClampAndNonFiniteInputRecoversWithSliderInSync() {
        panel.setValue(-5f);
        assertEquals(0.25f, panel.value(), 0.0001f);
        assertEquals(0, slider().getProgress());
        panel.setValue(50f);
        assertEquals(4f, panel.value(), 0.0001f);
        assertEquals(slider().getMax(), slider().getProgress());
        for (float invalid : new float[]{Float.NaN, Float.POSITIVE_INFINITY, Float.NEGATIVE_INFINITY}) {
            panel.setValue(invalid);
            assertEquals(1f, panel.value(), 0.0001f);
            assertEquals(30, slider().getProgress());
        }
    }

    @Test public void actualSeekbarDragChangesTheDraftValue() {
        SeekBar slider = slider();
        assertTrue("Measured slider", slider.getWidth() > 100 && slider.getHeight() > 0);
        gesture(slider, slider.getWidth() * 0.2f, slider.getHeight() / 2f,
            slider.getWidth() * 0.85f, slider.getHeight() / 2f, MotionEvent.ACTION_UP);
        assertTrue("Faster camera selected by touch", panel.value() > 3f && panel.value() <= 4f);
        assertEquals(0.25f + slider.getProgress() * 0.025f, panel.value(), 0.0001f);
        assertEquals(savedPreferences, preferences.getAll());
    }

    @Test public void presetsAndFineAdjustmentWorkByTouchAndRespectLimits() {
        tap(button("Preciso"));
        assertEquals(0.75f, panel.value(), 0.0001f);
        tap(button("Normal"));
        assertEquals(1f, panel.value(), 0.0001f);
        tap(button("Rápido"));
        assertEquals(2f, panel.value(), 0.0001f);
        tap(button("+"));
        assertEquals(2.025f, panel.value(), 0.0001f);
        tap(button("−"));
        assertEquals(2f, panel.value(), 0.0001f);
        panel.setValue(4f);
        tap(button("+"));
        assertEquals(4f, panel.value(), 0.0001f);
        panel.setValue(0.25f);
        tap(button("−"));
        assertEquals(0.25f, panel.value(), 0.0001f);
    }

    @Test public void theSamePreviewSwipeRespondsMoreAtHigherSensitivity() {
        View preview = preview();
        panel.setValue(0.75f);
        float start = yaw(preview);
        gesture(preview, 50, 50, 90, 50, MotionEvent.ACTION_UP);
        float preciseTurn = Math.abs(yaw(preview) - start);
        assertTrue("Swipe turns the practice camera", preciseTurn > 0f);
        assertEquals(0.75f, panel.value(), 0.0001f);
        panel.setValue(2f);
        start = yaw(preview);
        gesture(preview, 50, 50, 90, 50, MotionEvent.ACTION_UP);
        float fastTurn = Math.abs(yaw(preview) - start);
        assertTrue("Higher sensitivity visibly turns further", fastTurn > preciseTurn * 2f);
        assertEquals(2f, panel.value(), 0.0001f);
    }

    @Test public void previewUsesLogicalScreenScaleForConsistentAim() {
        View fullScale = preview();
        float start = yaw(fullScale);
        gesture(fullScale, 50, 50, 90, 50, MotionEvent.ACTION_UP);
        float fullTurn = Math.abs(yaw(fullScale) - start);
        install(new LookSensitivityPanel(activity, 1f, 0.5f));
        View halfScale = preview();
        start = yaw(halfScale);
        gesture(halfScale, 50, 50, 130, 50, MotionEvent.ACTION_UP);
        assertEquals("Twice the physical pixels at half logical scale gives the same turn",
            fullTurn, Math.abs(yaw(halfScale) - start), 0.0001f);
    }

    @Test public void cancelStopsTheGestureAndNextDownStartsWithoutAJump() {
        View preview = preview();
        long time = SystemClock.uptimeMillis();
        dispatch(preview, time, time, MotionEvent.ACTION_DOWN, 50, 50);
        dispatch(preview, time, time + 10, MotionEvent.ACTION_MOVE, 70, 50);
        float firstTurn = yaw(preview);
        assertTrue(firstTurn != 0f);
        dispatch(preview, time, time + 20, MotionEvent.ACTION_CANCEL, 70, 50);
        dispatch(preview, time, time + 30, MotionEvent.ACTION_MOVE, 400, 50);
        assertEquals("Movement after cancel is ignored", firstTurn, yaw(preview), 0f);
        dispatch(preview, time + 40, time + 40, MotionEvent.ACTION_DOWN, 300, 50);
        assertEquals("A new finger location does not rotate the camera", firstTurn, yaw(preview), 0f);
        dispatch(preview, time + 40, time + 50, MotionEvent.ACTION_MOVE, 310, 50);
        assertEquals("Only the fresh finger's ten-pixel delta is used", firstTurn / 2f,
            yaw(preview) - firstTurn, 0.0001f);
        dispatch(preview, time + 40, time + 60, MotionEvent.ACTION_UP, 310, 50);
    }

    @Test public void aSecondFingerCannotStealOrJumpThePracticeCamera() {
        View preview = preview();
        long time = SystemClock.uptimeMillis();
        pointers(preview, time, time, MotionEvent.ACTION_DOWN,
            new int[]{7}, new float[]{50}, new float[]{50});
        pointers(preview, time, time + 10, MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT),
            new int[]{7, 19}, new float[]{50, 250}, new float[]{50, 80});
        assertEquals("Second finger down does not rotate", 0f, yaw(preview), 0f);
        pointers(preview, time, time + 20, MotionEvent.ACTION_MOVE,
            new int[]{19, 7}, new float[]{290, 60}, new float[]{80, 50});
        float ownerTurn = yaw(preview);
        assertTrue("The original pointer still turns", ownerTurn != 0f);
        pointers(preview, time, time + 30, MotionEvent.ACTION_POINTER_UP | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT),
            new int[]{19, 7}, new float[]{290, 60}, new float[]{80, 50});
        pointers(preview, time, time + 40, MotionEvent.ACTION_MOVE,
            new int[]{19}, new float[]{390}, new float[]{90});
        assertEquals("Remaining finger does not take over", ownerTurn, yaw(preview), 0f);
        pointers(preview, time, time + 50, MotionEvent.ACTION_UP,
            new int[]{19}, new float[]{390}, new float[]{90});
    }

    @Test public void practiceAndWidgetChangesNeverOverwriteTheSavedHud() {
        final int[] changes = {0};
        SharedPreferences.OnSharedPreferenceChangeListener listener = (prefs, key) -> changes[0]++;
        preferences.registerOnSharedPreferenceChangeListener(listener);
        try {
            install(new LookSensitivityPanel(activity, 1.5f, 1f));
            tap(button("Rápido"));
            tap(button("+"));
            tap(inversion("Invertir deslizamiento horizontal"));
            tap(inversion("Invertir deslizamiento vertical"));
            gesture(preview(), 50, 50, 90, 70, MotionEvent.ACTION_CANCEL);
            shadowOf(Looper.getMainLooper()).idle();
            assertEquals("No persistence until the containing dialog applies", 0, changes[0]);
            assertEquals(savedPreferences, preferences.getAll());
        } finally {
            preferences.unregisterOnSharedPreferenceChangeListener(listener);
        }
    }

    @Test public void horizontalPresetsChangeOnlyTheDraftByTouch() {
        install(new LookSensitivityPanel(activity, 1.5f, 2.25f, 1f));
        assertEquals(2.25f, panel.horizontalValue(), 0.0001f);
        tap(button("Clásico"));
        assertEquals(1f, panel.horizontalValue(), 0.0001f);
        tap(button("Ágil"));
        assertEquals(2.25f, panel.horizontalValue(), 0.0001f);
        tap(button("Amplio"));
        assertEquals(4f, panel.horizontalValue(), 0.0001f);
        panel.setHorizontalValue(2.25f);
        assertEquals(2.25f, panel.horizontalValue(), 0.0001f);
        assertEquals(1.5f, panel.value(), 0.0001f);
        assertEquals(savedPreferences, preferences.getAll());
    }

    @Test public void agileHorizontalGainTurnsFurtherWithoutChangingVerticalAim() {
        install(new LookSensitivityPanel(activity, 1.5f, 1f, 0.5f));
        View classic = preview();
        gesture(classic, 50, 50, 90, 70, MotionEvent.ACTION_UP);
        float classicYaw = yaw(classic);
        float classicPitch = ReflectionHelpers.getField(classic, "pitch");
        assertEquals(40 * 0.5f * 1.5f * 0.0022f, classicYaw, 0.0001f);
        assertEquals(20 * 0.5f * 1.5f * 0.0022f, classicPitch, 0.0001f);
        install(new LookSensitivityPanel(activity, 1.5f, 2.25f, 0.5f));
        View agile = preview();
        gesture(agile, 50, 50, 90, 70, MotionEvent.ACTION_UP);
        assertEquals(classicYaw * 2.25f, yaw(agile), 0.0001f);
        assertEquals(classicPitch, ReflectionHelpers.<Float>getField(agile, "pitch"), 0.0001f);
        assertEquals(savedPreferences, preferences.getAll());
    }

    @Test public void horizontalPreviewConsumesBatchedMovementAndTheFinalReleaseDelta() {
        install(new LookSensitivityPanel(activity, 1.5f, 2.25f, 0.5f));
        View preview = preview();
        long time = SystemClock.uptimeMillis();
        dispatch(preview, time, time, MotionEvent.ACTION_DOWN, 50, 50);
        MotionEvent movement = MotionEvent.obtain(time, time + 10, MotionEvent.ACTION_MOVE, 70, 55, 0);
        try {
            movement.addBatch(time + 20, 80, 65, 1f, 1f, 0);
            assertEquals(1, movement.getHistorySize());
            preview.dispatchTouchEvent(movement);
        } finally {
            movement.recycle();
        }
        assertEquals(30 * 0.5f * 1.5f * 2.25f * 0.0022f, yaw(preview), 0.0001f);
        dispatch(preview, time, time + 30, MotionEvent.ACTION_UP, 100, 70);
        assertEquals(50 * 0.5f * 1.5f * 2.25f * 0.0022f, yaw(preview), 0.0001f);
        assertEquals(20 * 0.5f * 1.5f * 0.0022f,
            ReflectionHelpers.<Float>getField(preview, "pitch"), 0.0001f);
        assertEquals(savedPreferences, preferences.getAll());
    }

    @Test public void inversionSwitchesAreIndependentDraftsAndResetWithoutChangingSensitivity() {
        assertFalse(panel.invertedHorizontal());
        assertFalse(panel.invertedVertical());
        tap(inversion("Invertir deslizamiento horizontal"));
        assertTrue(panel.invertedHorizontal());
        assertFalse(panel.invertedVertical());
        tap(inversion("Invertir deslizamiento vertical"));
        assertTrue(panel.invertedHorizontal());
        assertTrue(panel.invertedVertical());
        panel.setInvertedAxes(false, false);
        assertFalse(inversion("Invertir deslizamiento horizontal").isChecked());
        assertFalse(inversion("Invertir deslizamiento vertical").isChecked());
        assertEquals(1f, panel.value(), 0f);
        assertEquals(1f, panel.horizontalValue(), 0f);
        assertEquals(savedPreferences, preferences.getAll());
    }

    @Test public void invertedPreviewSwipesReverseOnlyTheSelectedAxis() {
        install(new LookSensitivityPanel(activity, 1.5f, 2.25f, 0.5f));
        gesture(preview(), 50, 50, 90, 70, MotionEvent.ACTION_UP);
        float normalYaw = yaw(preview());
        float normalPitch = ReflectionHelpers.getField(preview(), "pitch");
        assertTrue(normalYaw > 0 && normalPitch > 0);
        for (boolean[] axes : new boolean[][] {{true, false}, {false, true}, {true, true}}) {
            install(new LookSensitivityPanel(activity, 1.5f, 2.25f, 0.5f));
            panel.setInvertedAxes(axes[0], axes[1]);
            gesture(preview(), 50, 50, 90, 70, MotionEvent.ACTION_UP);
            assertEquals("Only horizontal inversion changes the turn direction",
                axes[0] ? -normalYaw : normalYaw, yaw(preview()), .0001f);
            assertEquals("Only vertical inversion changes the pitch direction",
                axes[1] ? -normalPitch : normalPitch,
                ReflectionHelpers.<Float>getField(preview(), "pitch"), .0001f);
        }
        assertEquals(savedPreferences, preferences.getAll());
    }

    private void install(LookSensitivityPanel next) {
        root.removeAllViews();
        panel = next;
        root.addView(panel, new FrameLayout.LayoutParams(-1, -1));
        layout();
    }

    private void layout() {
        root.measure(View.MeasureSpec.makeMeasureSpec(800, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(480, View.MeasureSpec.EXACTLY));
        root.layout(0, 0, 800, 480);
        shadowOf(Looper.getMainLooper()).idle();
    }

    private SeekBar slider() { return (SeekBar) described("Sensibilidad de cámara"); }
    private View preview() { return described("Zona de prueba de cámara"); }
    private float yaw(View preview) { return ReflectionHelpers.getField(preview, "yaw"); }
    private Switch inversion(String text) {
        View result = find(panel, view -> view instanceof Switch && text.contentEquals(((Switch) view).getText()));
        assertNotNull("Inversion option: " + text, result);
        return (Switch) result;
    }
    private Button button(String text) {
        View result = find(panel, view -> view instanceof Button && text.contentEquals(((Button) view).getText()));
        assertNotNull("Button: " + text, result);
        return (Button) result;
    }
    private View described(String description) {
        View result = find(panel, view -> view.getContentDescription() != null
            && description.contentEquals(view.getContentDescription()));
        assertNotNull(description, result);
        return result;
    }
    private View find(View view, Predicate<View> predicate) {
        if (predicate.test(view)) return view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                View result = find(group.getChildAt(i), predicate);
                if (result != null) return result;
            }
        }
        return null;
    }
    private void tap(View view) {
        assertTrue("Measured touch target", view.getWidth() > 0 && view.getHeight() > 0);
        long time = SystemClock.uptimeMillis();
        dispatch(view, time, time, MotionEvent.ACTION_DOWN, view.getWidth() / 2f, view.getHeight() / 2f);
        dispatch(view, time, time + 16, MotionEvent.ACTION_UP, view.getWidth() / 2f, view.getHeight() / 2f);
        shadowOf(Looper.getMainLooper()).idle();
    }
    private void gesture(View view, float x, float y, float endX, float endY, int endAction) {
        long time = SystemClock.uptimeMillis();
        dispatch(view, time, time, MotionEvent.ACTION_DOWN, x, y);
        dispatch(view, time, time + 16, MotionEvent.ACTION_MOVE, endX, endY);
        dispatch(view, time, time + 32, endAction, endX, endY);
        shadowOf(Looper.getMainLooper()).idle();
    }
    private static void dispatch(View view, long down, long time, int action, float x, float y) {
        MotionEvent event = MotionEvent.obtain(down, time, action, x, y, 0);
        try { view.dispatchTouchEvent(event); } finally { event.recycle(); }
    }
    private static void pointers(View view, long down, long time, int action,
            int[] ids, float[] x, float[] y) {
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[ids.length];
        MotionEvent.PointerCoords[] coordinates = new MotionEvent.PointerCoords[ids.length];
        for (int i = 0; i < ids.length; i++) {
            properties[i] = new MotionEvent.PointerProperties();
            properties[i].id = ids[i];
            properties[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coordinates[i] = new MotionEvent.PointerCoords();
            coordinates[i].x = x[i];
            coordinates[i].y = y[i];
            coordinates[i].pressure = 1f;
            coordinates[i].size = 1f;
        }
        MotionEvent event = MotionEvent.obtain(down, time, action, ids.length, properties,
            coordinates, 0, 0, 1f, 1f, 0, 0, android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
        try { view.dispatchTouchEvent(event); } finally { event.recycle(); }
    }
}
