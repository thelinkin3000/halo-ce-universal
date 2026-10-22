package com.halo.decomp;

import android.app.Application;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Rect;
import android.view.ContextThemeWrapper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ScrollView;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.annotation.LooperMode;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w960dp-h540dp-land-mdpi")
@LooperMode(LooperMode.Mode.PAUSED)
public class LauncherScreenHostTest {
    private LauncherScreen screen(boolean data, boolean demo, float fontScale, int[] clicks) {
        Context app = RuntimeEnvironment.getApplication();
        Configuration configuration = new Configuration(app.getResources().getConfiguration());
        configuration.fontScale = fontScale;
        Context context = new ContextThemeWrapper(app.createConfigurationContext(configuration),
            android.R.style.Theme_DeviceDefault_NoActionBar_Fullscreen);
        return new LauncherScreen(context, data, demo, () -> clicks[0]++, () -> clicks[1]++,
            () -> clicks[2]++, () -> clicks[3]++, () -> clicks[4]++);
    }

    private LauncherScreen screen(boolean data, boolean demo, float fontScale) {
        return screen(data, demo, fontScale, new int[5]);
    }

    private int dp(View view, int value) {
        return Math.round(value * view.getResources().getDisplayMetrics().density);
    }

    private void size(LauncherScreen screen, int width, int height) {
        int widthSpec = View.MeasureSpec.makeMeasureSpec(dp(screen, width), View.MeasureSpec.EXACTLY);
        int heightSpec = View.MeasureSpec.makeMeasureSpec(dp(screen, height), View.MeasureSpec.EXACTLY);
        for (int pass = 0; pass < 3; pass++) {
            screen.measure(widthSpec, heightSpec);
            screen.layout(0, 0, screen.getMeasuredWidth(), screen.getMeasuredHeight());
        }
    }

    private Button[] actions(LauncherScreen screen) {
        return new Button[] {screen.play, screen.demo, screen.pick, screen.download, screen.update};
    }

    private ScrollView scroll(View view) {
        if (view instanceof ScrollView)
            return (ScrollView) view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                ScrollView found = scroll(group.getChildAt(i));
                if (found != null)
                    return found;
            }
        }
        return null;
    }

    private Rect bounds(ViewGroup parent, View child) {
        Rect rect = new Rect();
        child.getDrawingRect(rect);
        parent.offsetDescendantRectToMyCoords(child, rect);
        return rect;
    }

    private void usableActions(LauncherScreen screen) {
        ScrollView scroll = scroll(screen);
        assertNotNull("Actions remain reachable by scrolling", scroll);
        assertEquals(1, scroll.getChildCount());
        ViewGroup content = (ViewGroup) scroll.getChildAt(0);
        int viewport = scroll.getHeight() - scroll.getPaddingTop() - scroll.getPaddingBottom();
        assertTrue("Scroll viewport has height", viewport > 0);
        Button[] buttons = actions(screen);
        Rect[] rects = new Rect[buttons.length];
        for (int i = 0; i < buttons.length; i++) {
            Button button = buttons[i];
            String label = button.getText().toString();
            assertEquals(label + " remains visible", View.VISIBLE, button.getVisibility());
            assertTrue(label + " keeps a 48 dp touch target", button.getHeight() >= dp(screen, 48));
            rects[i] = bounds(content, button);
            assertTrue(label + " stays inside content width", rects[i].left >= 0
                && rects[i].right <= content.getWidth());
            assertTrue(label + " stays inside screen width", bounds(screen, button).left >= 0
                && bounds(screen, button).right <= screen.getWidth());
            assertTrue(label + " stays inside scroll content", rects[i].top >= 0
                && rects[i].bottom <= content.getHeight());
            int lastScroll = Math.max(0, content.getHeight() - viewport);
            int target = Math.min(lastScroll, Math.max(0, rects[i].top));
            assertTrue(label + " can fit in the scroll viewport", rects[i].top >= target
                && rects[i].bottom <= target + viewport);
            for (int previous = 0; previous < i; previous++)
                assertFalse(label + " does not overlap " + buttons[previous].getText(),
                    Rect.intersects(rects[i], rects[previous]));
        }
    }

    @Test public void installedDataKeepsActionsAndEnabledStates() {
        LauncherScreen screen = screen(true, true, 1f);
        assertEquals("Jugar / Continuar", screen.play.getText().toString());
        assertEquals("Jugar demo", screen.demo.getText().toString());
        assertEquals("Elegir imagen de disco", screen.pick.getText().toString());
        assertEquals("Descargar paquete de datos", screen.download.getText().toString());
        assertEquals("Buscar actualización de la APK", screen.update.getText().toString());
        assertTrue(screen.play.isEnabled());
        assertTrue(screen.demo.isEnabled());
        assertFalse(screen.pick.isEnabled());
        assertEquals(1000, screen.progress.getMax());
        assertEquals(View.GONE, screen.progress.getVisibility());
    }

    @Test public void missingDataOffersDemoDownloadAndImport() {
        LauncherScreen screen = screen(false, false, 1f);
        assertEquals("Descargar y jugar demo", screen.demo.getText().toString());
        assertTrue(screen.play.isEnabled());
        assertTrue(screen.demo.isEnabled());
        assertTrue(screen.pick.isEnabled());
    }

    @Test public void eachActionInvokesOnlyItsCallback() {
        int[] clicks = new int[5];
        LauncherScreen installed = screen(true, true, 1f, clicks);
        installed.play.performClick();
        assertArrayEquals(new int[] {1, 0, 0, 0, 0}, clicks);
        installed.demo.performClick();
        assertArrayEquals(new int[] {1, 1, 0, 0, 0}, clicks);
        LauncherScreen empty = screen(false, false, 1f, clicks);
        empty.pick.performClick();
        assertArrayEquals(new int[] {1, 1, 1, 0, 0}, clicks);
        empty.download.performClick();
        assertArrayEquals(new int[] {1, 1, 1, 1, 0}, clicks);
        empty.update.performClick();
        assertArrayEquals(new int[] {1, 1, 1, 1, 1}, clicks);
    }

    @Test public void wideScreenHasUsableActions() {
        LauncherScreen screen = screen(true, true, 1f);
        size(screen, 960, 540);
        usableActions(screen);
    }

    @Test public void shortScreenKeepsAllActionsReachable() {
        LauncherScreen screen = screen(false, false, 1f);
        size(screen, 720, 320);
        usableActions(screen);
    }

    @Test public void portraitScreenKeepsAllActionsReachable() {
        LauncherScreen screen = screen(false, false, 1f);
        size(screen, 360, 640);
        usableActions(screen);
    }

    @Test public void resizingKeepsActionsStateAndListeners() {
        int[] clicks = new int[5];
        LauncherScreen screen = screen(false, false, 1f, clicks);
        Button[] original = actions(screen);
        for (int[] dimensions : new int[][] {{960, 540}, {360, 640}, {720, 320}}) {
            size(screen, dimensions[0], dimensions[1]);
            usableActions(screen);
            Button[] current = actions(screen);
            for (int i = 0; i < original.length; i++)
                assertSame(original[i], current[i]);
            assertTrue(screen.play.isEnabled());
            assertTrue(screen.pick.isEnabled());
        }
        screen.demo.performClick();
        assertArrayEquals(new int[] {0, 1, 0, 0, 0}, clicks);
    }

    @Test public void largerFontsKeepActionsUsable() {
        for (float scale : new float[] {1.3f, 2f}) {
            LauncherScreen screen = screen(false, false, scale);
            assertEquals(scale, screen.getResources().getConfiguration().fontScale, .001f);
            for (int[] dimensions : new int[][] {{960, 540}, {720, 320}, {360, 640}}) {
                size(screen, dimensions[0], dimensions[1]);
                usableActions(screen);
            }
        }
    }

    // Legacy graphics measures each character as one pixel, so wrapping needs real font metrics.
    @Test @GraphicsMode(GraphicsMode.Mode.NATIVE)
    public void longStatusWrapsWithoutClipping() {
        LauncherScreen screen = screen(false, false, 2f);
        screen.status.setText("Preparando los archivos de la demo. La descarga continúa y tus partidas "
            + "guardadas se conservan. Puedes consultar el progreso aquí mientras se completa la instalación.");
        screen.status.setVisibility(View.VISIBLE);
        screen.progress.setVisibility(View.VISIBLE);
        size(screen, 360, 640);
        assertNotNull(screen.status.getLayout());
        assertTrue("Status wraps onto several lines: viewWidth=" + screen.status.getWidth()
            + ", layoutWidth=" + screen.status.getLayout().getWidth()
            + ", measuredText=" + screen.status.getPaint().measureText(screen.status.getText().toString())
            + ", textSize=" + screen.status.getTextSize() + ", maxLines=" + screen.status.getMaxLines()
            + ", lineCount=" + screen.status.getLineCount(), screen.status.getLineCount() > 1);
        assertNull("Status text is not ellipsized", screen.status.getEllipsize());
        assertTrue("All status lines fit", screen.status.getHeight() - screen.status.getPaddingTop()
            - screen.status.getPaddingBottom() >= screen.status.getLayout().getHeight());
        usableActions(screen);
    }
}
