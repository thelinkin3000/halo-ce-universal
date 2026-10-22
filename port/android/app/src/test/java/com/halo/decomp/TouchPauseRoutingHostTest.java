package com.halo.decomp;

import android.app.Application;
import android.os.Looper;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.widget.FrameLayout;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.LooperMode;
import org.robolectric.util.ReflectionHelpers;
import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;
import java.time.Duration;

/** Pause navigation must work even when a demo has no game pause-menu tags. */
@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35},
    qualifiers = "w960dp-h540dp-land-mdpi", instrumentedPackages = {"com.halo.decomp", "org.libsdl.app"},
    shadows = {TouchControlsMultitouchHostTest.NativeSink.class, PauseMenuHostTest.EngineBoundary.class})
@LooperMode(LooperMode.Mode.PAUSED)
public class TouchPauseRoutingHostTest {
    public static class RecordingActivity extends HaloActivity {
        int opened;
        @Override void showTouchPauseMenu() { opened++; }
    }
    private RecordingActivity activity;
    private TouchControls controls;
    private TouchLayout layout;
    private ActivityController<RecordingActivity> controller;

    @Before public void setUp() {
        controller = Robolectric.buildActivity(RecordingActivity.class);
        activity = controller.get();
        activity.getSharedPreferences("touch-layout-v1", 0).edit().clear().commit();
        controls = new TouchControls(activity);
        controls.layout(0, 0, 960, 540);
        ReflectionHelpers.setField(controls, "showing", true);
        layout = ReflectionHelpers.getField(controls, "layout");
        controls.reset();
    }

    @After public void releaseInput() { controls.stopDeviceInput(); }

    @Test public void pauseAndBackOpenNavigationOnReleaseWithoutXboxButtonBits() {
        for (int button : new int[] {10, 11}) {
            send(MotionEvent.ACTION_DOWN, layout.x(button), layout.y(button));
            assertEquals(0, TouchControlsMultitouchHostTest.NativeSink.state[6]);
            int before = activity.opened;
            send(MotionEvent.ACTION_UP, layout.x(button), layout.y(button));
            assertEquals(before + 1, activity.opened);
            assertEquals(0, TouchControlsMultitouchHostTest.NativeSink.state[6]);
        }
    }

    @Test public void cancelledPausePressDoesNotOpenNavigation() {
        send(MotionEvent.ACTION_DOWN, layout.x(10), layout.y(10));
        send(MotionEvent.ACTION_CANCEL, layout.x(10), layout.y(10));
        assertEquals(0, activity.opened);
    }

    @Test public void SwipeBeginningOnPauseDoesNotOpenNavigation() {
        float x = layout.x(10), y = layout.y(10);
        send(MotionEvent.ACTION_DOWN, x, y);
        send(MotionEvent.ACTION_MOVE, x + 60, y + 60);
        send(MotionEvent.ACTION_UP, x + 60, y + 60);
        assertEquals(0, activity.opened);
    }

    @Test public void originalMenuSendsOnlyStartForEightyMilliseconds() {
        attachControls();
        ReflectionHelpers.setField(controls, "deviceInputActive", true);
        ReflectionHelpers.setField(controls, "scene", 5);
        controls.openGameMenu();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(1 << 6, TouchControlsMultitouchHostTest.NativeSink.state[6]);
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(79));
        assertEquals(1 << 6, TouchControlsMultitouchHostTest.NativeSink.state[6]);
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(1));
        assertEquals(0, TouchControlsMultitouchHostTest.NativeSink.state[6]);
    }

    @Test public void resetAndStoppingInputCancelTheMenuPulse() {
        attachControls();
        for (boolean stop : new boolean[]{false, true}) {
            ReflectionHelpers.setField(controls, "deviceInputActive", true);
            ReflectionHelpers.setField(controls, "scene", 5);
            controls.openGameMenu();
            shadowOf(Looper.getMainLooper()).idle();
            assertEquals(1 << 6, TouchControlsMultitouchHostTest.NativeSink.state[6]);
            if (stop) controls.stopDeviceInput(); else controls.reset();
            assertEquals(0, TouchControlsMultitouchHostTest.NativeSink.state[6]);
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(80));
            assertEquals(0, TouchControlsMultitouchHostTest.NativeSink.state[6]);
        }
        controls.openGameMenu();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals("Stopped input cannot reopen the menu", 0, TouchControlsMultitouchHostTest.NativeSink.state[6]);
    }

    private void attachControls() {
        FrameLayout root = new FrameLayout(activity);
        activity.setContentView(root);
        root.addView(controls, new FrameLayout.LayoutParams(-1, -1));
        controller.visible();
        shadowOf(Looper.getMainLooper()).idle();
        controls.layout(0, 0, 960, 540);
        controls.reset();
    }

    private boolean send(int action, float x, float y) {
        long now = SystemClock.uptimeMillis();
        MotionEvent event = MotionEvent.obtain(now, now, action, x, y, 0);
        try { return controls.onTouchEvent(event); }
        finally { event.recycle(); }
    }
}
