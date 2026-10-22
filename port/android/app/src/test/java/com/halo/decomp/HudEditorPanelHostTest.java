package com.halo.decomp;

import android.app.Activity;
import android.app.Application;
import android.graphics.Insets;
import android.graphics.Rect;
import android.os.Build;
import android.os.Looper;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.FrameLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

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

import java.util.ArrayList;
import java.util.List;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w800dp-h360dp-land-mdpi")
@LooperMode(LooperMode.Mode.PAUSED)
@GraphicsMode(GraphicsMode.Mode.LEGACY)
public class HudEditorPanelHostTest {
    private ActivityController<Activity> controller;
    private FrameLayout root;
    private HudEditorPanel editor;
    private Recorder recorder;

    private static class Recorder implements HudEditorPanel.Listener {
        final List<String> events = new ArrayList<>();
        float size, opacity;
        int slot = -1;
        boolean visible;
        public void onExit() { events.add("exit"); }
        public void onReset() { events.add("reset"); }
        public void onSave() { events.add("save"); }
        public void onSize(float value) { size = value; events.add("size"); }
        public void onOpacity(float value) { opacity = value; events.add("opacity"); }
        public void onNudge(float x, float y) { events.add("nudge:" + x + ":" + y); }
        public void onSelectHud(int value) { slot = value; events.add("slot"); }
        public void onVisibility(boolean value) { visible = value; events.add("visible"); }
    }

    @Before public void createPanel() {
        controller = Robolectric.buildActivity(Activity.class).setup();
        Activity activity = controller.get();
        root = new FrameLayout(activity);
        activity.setContentView(root);
        recorder = new Recorder();
        editor = new HudEditorPanel(activity, recorder);
        root.addView(editor, new FrameLayout.LayoutParams(-1, -1));
        editor.setVisibility(View.VISIBLE);
        layout();
        assertTrue("Measured editor", editor.getWidth() > 0);
        assertTrue("Measured panel", viewport().getWidth() > 0);
        assertTrue("No constructor callbacks", recorder.events.isEmpty());
    }

    @After public void closeActivity() {
        controller.pause().stop().destroy();
    }

    @Test public void bindIsSilentAndKeepsDirtyState() {
        editor.setDirty(true);
        editor.bind(2, "Disparar", 1.35f, 0.1f, false);
        assertTrue(recorder.events.isEmpty());
        assertEquals(85, slider("Tamaño del botón").getProgress());
        assertEquals(0, slider("Opacidad del botón").getProgress());
        assertFalse(checkbox().isChecked());
        assertTrue(button("Guardar").isEnabled());
        editor.bind(0, null, Float.NaN, Float.POSITIVE_INFINITY, true);
        assertFalse(slider("Tamaño del botón").isEnabled());
        assertFalse(slider("Opacidad del botón").isEnabled());
        assertFalse(checkbox().isEnabled());
        assertTrue(recorder.events.isEmpty());
        assertThrows(IllegalArgumentException.class, () -> editor.bind(-1, "x", 1, 1, true));
        assertThrows(IllegalArgumentException.class, () -> editor.bind(3, "x", 1, 1, true));
    }

    @Test public void hudTabsEmitZeroBasedSlotsAndIgnoreCurrentSlot() {
        editor.bind(0, "Disparar", 1, 1, true);
        tap(button("HUD 1"));
        assertTrue(recorder.events.isEmpty());
        tap(button("HUD 2"));
        assertEquals(1, recorder.slot);
        assertEquals(List.of("slot"), recorder.events);
        editor.bind(1, "Disparar", 1, 1, true);
        tap(button("HUD 3"));
        assertEquals(2, recorder.slot);
    }

    @Test public void saveRequiresDirtyState() {
        assertFalse(button("Guardar").isEnabled());
        tap(button("Guardar"));
        assertTrue(recorder.events.isEmpty());
        editor.setDirty(true);
        tap(button("Guardar"));
        assertEquals(List.of("save"), recorder.events);
        editor.setDirty(false);
        assertFalse(button("Guardar").isEnabled());
    }

    @Test public void actualSeekbarTouchesEmitUserSizeAndOpacity() {
        editor.bind(0, "Disparar", 1, 1, true);
        layout();
        dragSlider(slider("Tamaño del botón"), 0.25f, 0.9f);
        assertTrue("User size callback", recorder.events.contains("size"));
        assertTrue("Size follows drag", recorder.size > 1.5f && recorder.size <= 2f);
        dragSlider(slider("Opacidad del botón"), 0.9f, 0.2f);
        assertTrue("User opacity callback", recorder.events.contains("opacity"));
        assertTrue("Opacity follows drag", recorder.opacity >= 0.1f && recorder.opacity < 0.6f);
    }

    @Test public void visibilityNudgesResetAndExitUseRealWidgets() {
        editor.bind(0, "Movimiento", 1, 0.5f, false);
        tap(checkbox());
        assertTrue(recorder.visible);
        tap(described("Mover arriba"));
        tap(described("Mover a la derecha"));
        tap(button("Restablecer"));
        tap(button("Salir"));
        assertEquals(List.of("visible", "nudge:0.0:-1.0", "nudge:1.0:0.0", "reset", "exit"), recorder.events);
    }

    @Test public void caretTapCollapsesAndExpandsWithoutModelCallbacks() {
        editor.bind(0, "Disparar", 1, 1, true);
        View handle = caret();
        int expandedHeight = viewport().getHeight();
        tap(handle);
        layout();
        assertEquals("⌄", ((TextView) handle).getText().toString());
        assertFalse(button("HUD 1").isShown());
        assertTrue(viewport().getHeight() < expandedHeight);
        tap(handle);
        layout();
        assertEquals("⌃", ((TextView) handle).getText().toString());
        assertTrue(button("HUD 1").isShown());
        assertTrue(recorder.events.isEmpty());
    }

    @Test public void caretDragMovesPanelAndNeverClicksCollapse() {
        View handle = caret();
        float startX = viewport().getTranslationX(), startY = viewport().getTranslationY();
        gesture(handle, 10, 5, 70, 45, MotionEvent.ACTION_UP);
        assertTrue(viewport().getTranslationX() > startX);
        assertTrue(viewport().getTranslationY() > startY);
        assertEquals("⌃", ((TextView) handle).getText().toString());
        assertTrue(recorder.events.isEmpty());
        gesture(handle, 10, 5, 5000, 5000, MotionEvent.ACTION_UP);
        assertInside(0, 0, 0, 0);
        gesture(handle, 10, 5, -5000, -5000, MotionEvent.ACTION_UP);
        assertInside(0, 0, 0, 0);
    }

    @Test public void caretCancelDoesNotCollapseAndNextTapWorks() {
        View handle = caret();
        gesture(handle, 10, 5, 60, 35, MotionEvent.ACTION_CANCEL);
        assertEquals("⌃", ((TextView) handle).getText().toString());
        tap(handle);
        assertEquals("⌄", ((TextView) handle).getText().toString());
        assertTrue(recorder.events.isEmpty());
    }

    @Test public void panelStartsCenteredAndStaysInsideNotchAndBars() {
        ScrollView viewport = viewport();
        assertEquals(editor.getWidth() / 2f, viewport.getLeft() + viewport.getWidth() / 2f, 1f);
        assertTrue(viewport.getWidth() <= 270);
        int left = 150, top = 30, right = 25, bottom = 20;
        WindowInsets insets;
        if (Build.VERSION.SDK_INT >= 30) {
            insets = new WindowInsets.Builder()
                .setInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout(), Insets.of(left, top, right, bottom))
                .build();
        } else {
            insets = ReflectionHelpers.callConstructor(WindowInsets.class,
                ReflectionHelpers.ClassParameter.from(Rect.class, new Rect(left, top, right, bottom)));
        }
        editor.onApplyWindowInsets(insets);
        layout();
        assertInside(left, top, right, bottom);
        gesture(caret(), 10, 5, -5000, -5000, MotionEvent.ACTION_UP);
        assertInside(left, top, right, bottom);
        gesture(caret(), 10, 5, 5000, 5000, MotionEvent.ACTION_UP);
        assertInside(left, top, right, bottom);
    }

    private void assertInside(int left, int top, int right, int bottom) {
        ScrollView view = viewport();
        float x = view.getLeft() + view.getTranslationX();
        float y = view.getTop() + view.getTranslationY();
        assertTrue("left " + x, x >= left);
        assertTrue("top " + y, y >= top);
        assertTrue("right " + (x + view.getWidth()), x + view.getWidth() <= editor.getWidth() - right);
        assertTrue("bottom " + (y + view.getHeight()), y + view.getHeight() <= editor.getHeight() - bottom);
    }

    private void layout() {
        root.measure(View.MeasureSpec.makeMeasureSpec(800, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(360, View.MeasureSpec.EXACTLY));
        root.layout(0, 0, 800, 360);
        shadowOf(Looper.getMainLooper()).idle();
        root.measure(View.MeasureSpec.makeMeasureSpec(800, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(360, View.MeasureSpec.EXACTLY));
        root.layout(0, 0, 800, 360);
    }

    private ScrollView viewport() { return (ScrollView) editor.getChildAt(0); }
    private Button button(String text) { return (Button) find(editor, view -> view instanceof Button && text.contentEquals(((Button) view).getText())); }
    private View described(String description) { return find(editor, view -> description.contentEquals(view.getContentDescription() == null ? "" : view.getContentDescription())); }
    private SeekBar slider(String description) { return (SeekBar) described(description); }
    private CheckBox checkbox() { return (CheckBox) find(editor, view -> view instanceof CheckBox); }
    private View caret() { return find(editor, view -> view.getContentDescription() != null && view.getContentDescription().toString().contains("arrastra para mover el panel")); }

    private View find(View view, java.util.function.Predicate<View> predicate) {
        if (predicate.test(view)) return view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                View found = find(group.getChildAt(i), predicate);
                if (found != null) return found;
            }
        }
        return null;
    }

    private void tap(View view) {
        assertNotNull(view);
        long time = SystemClock.uptimeMillis();
        dispatch(view, time, time, MotionEvent.ACTION_DOWN, view.getWidth() / 2f, view.getHeight() / 2f);
        dispatch(view, time, time + 16, MotionEvent.ACTION_UP, view.getWidth() / 2f, view.getHeight() / 2f);
        shadowOf(Looper.getMainLooper()).idle();
    }

    private void dragSlider(SeekBar view, float from, float to) {
        assertTrue("SeekBar measured", view.getWidth() > 30 && view.getHeight() > 0);
        gesture(view, view.getWidth() * from, view.getHeight() / 2f,
            view.getWidth() * to, view.getHeight() / 2f, MotionEvent.ACTION_UP);
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
}
