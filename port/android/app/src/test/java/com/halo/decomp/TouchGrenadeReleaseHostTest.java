package com.halo.decomp;

import android.app.Application;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.MotionEvent;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.LooperMode;
import org.robolectric.util.ReflectionHelpers;

import java.time.Duration;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

/** Real touch routing with a JNI sink: grenade aim holds and native release pulses stay independent. */
@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35},
    qualifiers = "w960dp-h540dp-land-mdpi", instrumentedPackages = "com.halo.decomp",
    shadows = TouchControlsMultitouchHostTest.NativeSink.class)
@LooperMode(LooperMode.Mode.PAUSED)
public class TouchGrenadeReleaseHostTest {
    private TouchControls controls;
    private TouchLayout layout;
    private long downTime;

    @Before public void createControls() {
        TouchControlsMultitouchHostTest.NativeSink.bindingSerial = 0;
        TouchControlsMultitouchHostTest.NativeSink.suppliedBindings = null;
        HaloActivity activity = Robolectric.buildActivity(HaloActivity.class).get();
        activity.getSharedPreferences("touch-layout-v1", 0).edit().clear().commit();
        controls = new TouchControls(activity);
        ReflectionHelpers.setField(controls, "horizontalSensitivity", 1f);
        controls.layout(0, 0, 960, 540);
        ReflectionHelpers.setField(controls, "showing", true);
        layout = ReflectionHelpers.getField(controls, "layout");
        controls.reset();
        downTime = SystemClock.uptimeMillis();
    }

    @After public void closeControls() {
        controls.stopDeviceInput();
    }

    @Test public void longGrenadeHoldDoesNotThrowUntilReleaseAndPulseLasts80Milliseconds() {
        event(MotionEvent.ACTION_DOWN, control(11, 5));
        assertReleased();
        advance(1200);
        assertReleased();
        event(MotionEvent.ACTION_UP, control(11, 5));
        assertEquals(32767, state(4));
        assertEquals(0, state(5));
        advance(79);
        assertEquals("The native trigger stays down for the complete pulse", 32767, state(4));
        advance(1);
        assertReleased();
        advance(1000);
        assertReleased();
    }

    @Test public void holdingAndDraggingGrenadeAimsWithoutThrowingEarly() {
        float x = layout.x(5), y = layout.y(5);
        event(MotionEvent.ACTION_DOWN, point(7, x, y));
        event(MotionEvent.ACTION_MOVE, point(7, x + 30, y - 15));
        assertEquals(30f, TouchControlsMultitouchHostTest.NativeSink.lookX, .001f);
        assertEquals(-15f, TouchControlsMultitouchHostTest.NativeSink.lookY, .001f);
        assertEquals(0, state(4));
        advance(1200);
        event(MotionEvent.ACTION_MOVE, point(7, x + 50, y - 20));
        assertEquals(50f, TouchControlsMultitouchHostTest.NativeSink.lookX, .001f);
        assertEquals(0, state(4));
        event(MotionEvent.ACTION_UP, point(7, x + 50, y - 20));
        assertEquals(32767, state(4));
        advance(80);
        assertReleased();
    }

    @Test public void cancellationBeforeReleaseNeverThrowsGrenade() {
        event(MotionEvent.ACTION_DOWN, control(4, 5));
        advance(1200);
        event(MotionEvent.ACTION_CANCEL, control(4, 5));
        assertReleased();
        advance(1200);
        assertReleased();
    }

    @Test public void cancellationAfterReleaseClearsActivePulseAndItsCallback() {
        tapGrenade();
        advance(30);
        assertEquals(32767, state(4));
        event(MotionEvent.ACTION_CANCEL, control(1, 5));
        assertReleased();
        event(MotionEvent.ACTION_DOWN, control(8, 4));
        advance(100);
        assertEquals("Cancelled grenade cannot clear another held action", 32767, state(5));
        assertEquals(0, state(4));
        event(MotionEvent.ACTION_UP, control(8, 4));
        assertReleased();
    }

    @Test public void resetClearsPulseBeforeAnotherInteraction() {
        tapGrenade();
        assertEquals(32767, state(4));
        controls.reset();
        assertReleased();
        advance(40);
        tapGrenade();
        advance(40);
        assertEquals("The first pulse's callback cannot release the second pulse", 32767, state(4));
        advance(39);
        assertEquals(32767, state(4));
        advance(1);
        assertReleased();
    }

    @Test public void stoppingInputClearsPulseAndDoesNotLeaveDelayedControllerWrites() {
        tapGrenade();
        assertEquals(32767, state(4));
        controls.stopDeviceInput();
        assertReleased();
        advance(40);
        tapGrenade();
        advance(40);
        assertEquals("Stop removes the old release callback", 32767, state(4));
        advance(40);
        assertReleased();
    }

    @Test public void threeFingersMoveFireAndReleaseGrenadeWithoutLosingOtherOwners() {
        float sx = layout.x(TouchLayout.LEFT), sy = layout.y(TouchLayout.LEFT);
        float fx = layout.x(4), fy = layout.y(4);
        float gx = layout.x(5), gy = layout.y(5);
        event(MotionEvent.ACTION_DOWN, point(11, sx, sy));
        event(pointerDown(1), point(11, sx, sy), point(22, fx, fy));
        event(pointerDown(2), point(11, sx, sy), point(22, fx, fy), point(33, gx, gy));
        event(MotionEvent.ACTION_MOVE, point(33, gx, gy), point(11, sx + 30, sy), point(22, fx, fy));
        assertTrue(state(0) > 0);
        assertEquals(32767, state(5));
        assertEquals(0, state(4));
        event(pointerUp(0), point(33, gx, gy), point(11, sx + 30, sy), point(22, fx, fy));
        assertEquals(32767, state(4));
        assertEquals(32767, state(5));
        assertTrue(state(0) > 0);
        advance(80);
        assertEquals(0, state(4));
        assertEquals(32767, state(5));
        assertTrue(state(0) > 0);
        event(pointerUp(1), point(11, sx + 30, sy), point(22, fx, fy));
        assertEquals(0, state(5));
        assertTrue(state(0) > 0);
        event(MotionEvent.ACTION_UP, point(11, sx + 30, sy));
        assertReleased();
    }

    @Test public void grenadeReboundToXboxXUsesReleasePulseInsteadOfHoldingX() {
        int[] bindings = ReflectionHelpers.getField(controls, "bindings");
        bindings[2] = 6;
        bindings[6] = 2;
        event(MotionEvent.ACTION_DOWN, control(10, 2));
        assertReleased();
        advance(1200);
        assertReleased();
        event(MotionEvent.ACTION_UP, control(10, 2));
        assertEquals(1 << 2, state(6));
        assertEquals(0, state(4));
        advance(79);
        assertEquals(1 << 2, state(6));
        advance(1);
        assertReleased();
    }

    @Test public void grenadeReboundToRightTriggerUsesRightTriggerPulse() {
        int[] bindings = ReflectionHelpers.getField(controls, "bindings");
        bindings[7] = 6;
        bindings[6] = 7;
        event(MotionEvent.ACTION_DOWN, control(12, 4));
        assertReleased();
        event(MotionEvent.ACTION_UP, control(12, 4));
        assertEquals(0, state(4));
        assertEquals(32767, state(5));
        advance(80);
        assertReleased();
    }

    @Test public void leftTriggerBoundToAnotherActionRemainsAnImmediateHeldControl() {
        int[] bindings = ReflectionHelpers.getField(controls, "bindings");
        bindings[6] = 7;
        event(MotionEvent.ACTION_DOWN, control(2, 5));
        assertEquals(32767, state(4));
        advance(1200);
        assertEquals(32767, state(4));
        event(MotionEvent.ACTION_UP, control(2, 5));
        assertReleased();
        advance(80);
        assertReleased();
    }

    private void tapGrenade() {
        event(MotionEvent.ACTION_DOWN, control(1, 5));
        event(MotionEvent.ACTION_UP, control(1, 5));
    }

    private void advance(long milliseconds) {
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(milliseconds));
    }

    private static int state(int index) { return TouchControlsMultitouchHostTest.NativeSink.state[index]; }

    private void assertReleased() { assertArrayEquals(new int[7], TouchControlsMultitouchHostTest.NativeSink.state); }

    private float[] control(int id, int kind) { return point(id, layout.x(kind), layout.y(kind)); }

    private static float[] point(int id, float x, float y) { return new float[] {id, x, y}; }

    private static int pointerDown(int index) {
        return MotionEvent.ACTION_POINTER_DOWN | index << MotionEvent.ACTION_POINTER_INDEX_SHIFT;
    }

    private static int pointerUp(int index) {
        return MotionEvent.ACTION_POINTER_UP | index << MotionEvent.ACTION_POINTER_INDEX_SHIFT;
    }

    private void event(int action, float[]... points) {
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[points.length];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[points.length];
        for (int i = 0; i < points.length; i++) {
            properties[i] = new MotionEvent.PointerProperties();
            properties[i].id = (int) points[i][0];
            properties[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].x = points[i][1];
            coords[i].y = points[i][2];
            coords[i].pressure = coords[i].size = 1;
        }
        MotionEvent event = MotionEvent.obtain(downTime, SystemClock.uptimeMillis(), action,
            points.length, properties, coords, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        try { assertTrue(controls.onTouchEvent(event)); }
        finally { event.recycle(); }
    }
}
