package com.halo.decomp;

import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Insets;
import android.graphics.Rect;
import android.os.Build;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.WindowInsets;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.annotation.LooperMode;
import org.robolectric.shadows.ShadowCanvas;
import org.robolectric.util.ReflectionHelpers;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

/** Exercises real overlay geometry and touch routing with stable system margins. */
@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35},
    qualifiers = "w1612dp-h720dp-land-mdpi", instrumentedPackages = "com.halo.decomp",
    shadows = TouchControlsMultitouchHostTest.NativeSink.class)
@LooperMode(LooperMode.Mode.PAUSED)
@GraphicsMode(GraphicsMode.Mode.LEGACY)
public class TouchSafeAreaHostTest {
    private static final float EPSILON = .002f;
    private HaloActivity activity;
    private TouchControls controls;
    private TouchLayout layout;

    @Before public void createControls() {
        activity = Robolectric.buildActivity(HaloActivity.class).get();
        activity.getSharedPreferences("touch-layout-v1", 0).edit().clear().commit();
        controls = new TouchControls(activity);
        controls.layout(0, 0, 1612, 720);
        ReflectionHelpers.setField(controls, "showing", true);
        layout = ReflectionHelpers.getField(controls, "layout");
        controls.reset();
    }

    @Test public void visibleCirclesStayOutsideRightTopAndBottomSystemAreas() {
        applyInsets(0, 77, 129, 56, false);
        assertCirclesInside(0, 77, 129, 56);
        for (int i = 0; i < layout.size(); i++)
            if (layout.shown(i)) layout.setSize(i, 2);
        assertCirclesInside(0, 77, 129, 56);
    }

    @Test public void leftCutoutMarginAlsoKeepsVisibleControlsInside() {
        applyInsets(140, 77, 129, 56, false);
        layout.move(TouchLayout.LEFT, -1000, 400);
        assertCirclesInside(140, 77, 129, 56);
    }

    @Test public void insetsNeverRewriteAuthoredPositions() {
        float[] saved = savedPositions();
        applyInsets(0, 77, 129, 56, false);
        assertArrayEquals(saved, savedPositions(), EPSILON);
        applyInsets(140, 40, 83, 24, true);
        assertArrayEquals(saved, savedPositions(), EPSILON);
        applyInsets(0, 0, 0, 0, false);
        assertArrayEquals(saved, savedPositions(), EPSILON);
    }

    @Test public void authoredGridSurvivesResizeWithSafeMarginsActive() {
        float[] saved = savedPositions();
        applyInsets(0, 77, 129, 56, false);
        controls.layout(0, 0, 1280, 720);
        assertArrayEquals(saved, savedPositions(), EPSILON);
        assertCirclesInside(0, 77, 129, 56);
        controls.layout(0, 0, 1612, 720);
        assertArrayEquals(saved, savedPositions(), EPSILON);
    }

    @Test public void transientBarVisibilityDoesNotMoveButtonsOrToolbar() {
        applyInsets(0, 77, 129, 56, false);
        float[] positions = displayedPositions();
        float toolbarY = value("toolbarY");
        applyInsets(0, 77, 129, 56, true);
        assertArrayEquals(positions, displayedPositions(), EPSILON);
        assertEquals(toolbarY, value("toolbarY"), EPSILON);
    }

    @Test public void rightEdgeButtonUsesTheVisibleSafePositionForTouches() {
        applyInsets(0, 77, 129, 56, false);
        layout.move(0, 10000, 320);
        float x = layout.x(0) * scale(), y = layout.y(0) * scale();
        assertTrue(x + layout.radius(0) * scale() <= 1612 - 129 + EPSILON);
        touch(MotionEvent.ACTION_DOWN, x, y);
        assertEquals("The visible A button reaches the controller", 1,
            TouchControlsMultitouchHostTest.NativeSink.state[6]);
        touch(MotionEvent.ACTION_UP, x, y);
        assertArrayEquals(new int[7], TouchControlsMultitouchHostTest.NativeSink.state);
    }

    @Test public void applyingInsetsKeepsAnActiveFingerAndItsHeldTrigger() {
        applyInsets(0, 77, 129, 56, false);
        float x = layout.x(4) * scale(), y = layout.y(4) * scale();
        touch(MotionEvent.ACTION_DOWN, x, y);
        assertEquals(32767, TouchControlsMultitouchHostTest.NativeSink.state[5]);
        applyInsets(25, 77, 159, 56, true);
        assertEquals("Insets must not release an active finger", 32767,
            TouchControlsMultitouchHostTest.NativeSink.state[5]);
        touch(MotionEvent.ACTION_UP, x, y);
        assertArrayEquals(new int[7], TouchControlsMultitouchHostTest.NativeSink.state);
    }

    @Test public void toolbarDrawingAndHitTargetsAgreeAndDoNotCoverTopControls() {
        applyInsets(140, 77, 129, 56, false);
        float y = value("toolbarY");
        float toggle = value("toggleX"), options = value("optionsX");
        int toggleHit = ReflectionHelpers.callInstanceMethod(controls, "hit",
            ReflectionHelpers.ClassParameter.from(float.class, toggle),
            ReflectionHelpers.ClassParameter.from(float.class, y));
        int optionsHit = ReflectionHelpers.callInstanceMethod(controls, "hit",
            ReflectionHelpers.ClassParameter.from(float.class, options),
            ReflectionHelpers.ClassParameter.from(float.class, y));
        assertEquals(-3, toggleHit);
        assertEquals(-4, optionsHit);
        Canvas canvas = new Canvas(Bitmap.createBitmap(1612, 720, Bitmap.Config.ARGB_8888));
        controls.draw(canvas);
        assertDrawnCircle(shadowOf(canvas), toggle, y);
        assertDrawnCircle(shadowOf(canvas), options, y);
        for (int control : new int[] {10, 11}) {
            float distance = (float) Math.hypot(layout.x(control) - toggle, layout.y(control) - y);
            assertTrue("Pause/back must not intersect Hide/Show", distance > layout.radius(control) + 34);
        }
    }

    private void applyInsets(int left, int top, int right, int bottom, boolean visible) {
        WindowInsets insets;
        if (Build.VERSION.SDK_INT >= 30) {
            Insets stable = Insets.of(left, top, right, bottom);
            insets = new WindowInsets.Builder()
                .setInsetsIgnoringVisibility(WindowInsets.Type.systemBars(), stable)
                .setInsets(WindowInsets.Type.systemBars(), visible ? stable : Insets.NONE)
                .setInsets(WindowInsets.Type.systemGestures(), stable)
                .setVisible(WindowInsets.Type.systemBars(), visible).build();
        } else {
            insets = ReflectionHelpers.callConstructor(WindowInsets.class,
                ReflectionHelpers.ClassParameter.from(Rect.class,
                    visible ? new Rect(left, top, right, bottom) : new Rect()));
            ReflectionHelpers.setField(insets, "mStableInsets", new Rect(left, top, right, bottom));
        }
        ReflectionHelpers.setField(activity, "gestureInsets", new int[] {left, top, right, bottom});
        controls.dispatchApplyWindowInsets(insets);
    }

    private void assertCirclesInside(int left, int top, int right, int bottom) {
        float scale = scale();
        for (int i = 0; i < layout.size(); i++) {
            if (!layout.shown(i)) continue;
            float x = layout.x(i) * scale, y = layout.y(i) * scale;
            float radius = layout.radius(i) * scale;
            assertTrue("Left edge of control " + i, x - radius >= left - EPSILON);
            assertTrue("Top edge of control " + i, y - radius >= top - EPSILON);
            assertTrue("Right edge of control " + i, x + radius <= controls.getWidth() - right + EPSILON);
            assertTrue("Bottom edge of control " + i, y + radius <= controls.getHeight() - bottom + EPSILON);
        }
    }

    private float[] savedPositions() {
        float[] values = new float[layout.size() * 2];
        for (int i = 0; i < layout.size(); i++) {
            values[i * 2] = layout.savedX(i);
            values[i * 2 + 1] = layout.savedY(i);
        }
        return values;
    }

    private float[] displayedPositions() {
        float[] values = new float[layout.size() * 2];
        for (int i = 0; i < layout.size(); i++) {
            values[i * 2] = layout.x(i);
            values[i * 2 + 1] = layout.y(i);
        }
        return values;
    }

    private float scale() { return ReflectionHelpers.getField(controls, "scale"); }
    private float value(String method) { return ReflectionHelpers.callInstanceMethod(controls, method); }

    private void touch(int action, float x, float y) {
        long now = SystemClock.uptimeMillis();
        MotionEvent event = MotionEvent.obtain(now, now, action, x, y, 0);
        try { assertTrue(controls.onTouchEvent(event)); }
        finally { event.recycle(); }
    }

    private void assertDrawnCircle(ShadowCanvas canvas, float x, float y) {
        for (int i = 0; i < canvas.getCirclePaintHistoryCount(); i++) {
            ShadowCanvas.CirclePaintHistoryEvent circle = canvas.getDrawnCircle(i);
            if (Math.abs(circle.centerX - x) < EPSILON && Math.abs(circle.centerY - y) < EPSILON)
                return;
        }
        fail("Toolbar hit target has no matching drawn circle");
    }
}
