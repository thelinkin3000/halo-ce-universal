package com.halo.decomp;

import android.app.Application;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35})
public class TouchDistributionHostTest {
    private static final float[][] LEGACY = {
        {900, 430}, {900, 330}, {812, 146}, {900, 146}, {800, 350}, {700, 450},
        {800, 480}, {900, 236}, {526, 480}, {612, 480}, {576, 68}, {390, 68},
        {100, 237}, {100, 335}, {51, 286}, {149, 286}, {130, 420}, {145, 110}
    };
    private static final float[][] REDISTRIBUTED = {
        {900, 445}, {900, 350}, {790, 180}, {900, 180}, {785, 350}, {260, 410},
        {790, 470}, {900, 265}, {105, 205}, {220, 205}, {576, 68}, {390, 68},
        {100, 237}, {100, 335}, {51, 286}, {149, 286}, {130, 420}, {310, 300}
    };
    private static final float[][] PREVIOUS = {
        {900, 430}, {900, 330}, {812, 146}, {900, 146}, {780, 350}, {700, 450},
        {800, 480}, {900, 236}, {526, 480}, {612, 480}, {576, 68}, {390, 68},
        {100, 237}, {100, 335}, {51, 286}, {149, 286}, {130, 420}, {145, 110}
    };
    private static final float[][] ALIGNED = {
        {900, 342}, {900, 432}, {812, 162}, {900, 162}, {780, 342}, {700, 480},
        {800, 480}, {900, 252}, {526, 480}, {612, 480}, {576, 68}, {390, 68},
        {100, 237}, {100, 335}, {51, 286}, {149, 286}, {130, 420}, {200, 230}
    };
    private static final float[] OLD_SIZES = {
        1, 34f / 32, 1, 34f / 32, 1.35f, 1, 34f / 32, 34f / 32,
        34f / 27, 34f / 29, 34f / 28, 34f / 28,
        34f / 25, 34f / 25, 34f / 25, 34f / 25, 1, 1.35f
    };

    @Test public void mobilePresetsAlignRowsAndKeepAdjacentTargetsApartWithPhoneInsets() {
        for (String text : new String[] {TouchPresets.twoThumbs(), TouchPresets.threeFingers()}) {
            TouchLayout layout = TouchLayout.importConfiguration(text).layout;
            assertAlignedPositions(layout);
            assertEquals(layout.savedY(2), layout.savedY(3), 0);
            assertEquals(layout.savedY(0), layout.savedY(4), 0);
            assertEquals(layout.savedY(5), layout.savedY(6), 0);
            assertEquals(90, layout.savedY(7) - layout.savedY(3), 0);
            assertEquals(90, layout.savedY(0) - layout.savedY(7), 0);
            assertEquals(90, layout.savedY(1) - layout.savedY(0), 0);
            // 1612 x 720 display, using TouchControls' 4/3 pixel-to-logical scale.
            layout.bounds(1209, 540);
            layout.setMinimumRadius(34);
            layout.safeInsets(39, 137, 96.75f, 42);
            for (int[] pair : new int[][] {{2, 3}, {3, 7}, {7, 0}, {0, 1}, {4, 0}, {5, 6}}) {
                double distance = Math.hypot(layout.x(pair[0]) - layout.x(pair[1]),
                    layout.y(pair[0]) - layout.y(pair[1]));
                assertTrue("Adjacent targets " + pair[0] + " and " + pair[1] + " need a clear gap",
                    distance >= layout.radius(pair[0]) + layout.radius(pair[1]) + 8);
            }
            for (int i : new int[] {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11})
                assertEquals("Action radius " + i, 34, layout.radius(i), .001f);
            assertEquals(48, layout.radius(4), .001f);
            assertEquals(44, layout.radius(TouchLayout.FIRE_LEFT), .001f);
            assertTrue("Top action targets stay below the system and toolbar area",
                layout.y(2) - layout.radius(2) >= 137 && layout.y(3) - layout.radius(3) >= 137);
        }
    }

    @Test public void leftFireIsOptionalAndStaysAboveTheMovementStick() {
        TouchLayout twoThumbs = TouchLayout.importConfiguration(TouchPresets.twoThumbs()).layout;
        TouchLayout threeFingers = TouchLayout.importConfiguration(TouchPresets.threeFingers()).layout;
        assertFalse(twoThumbs.shown(TouchLayout.FIRE_LEFT));
        assertTrue(threeFingers.shown(TouchLayout.FIRE_LEFT));
        assertTrue(threeFingers.y(TouchLayout.FIRE_LEFT) + threeFingers.radius(TouchLayout.FIRE_LEFT) + 8
            < threeFingers.y(TouchLayout.LEFT) - threeFingers.radius(TouchLayout.LEFT));
    }

    @Test public void draggingUsesDisplayPositionWithoutAJumpAndKeepsAuthoredCoordinates() {
        TouchLayout layout = TouchLayout.importConfiguration(TouchPresets.twoThumbs()).layout;
        float saved = layout.savedX(3);
        layout.bounds(1209, 540);
        layout.safeInsets(39, 137, 96.75f, 42);
        float x = layout.x(3), y = layout.y(3);
        layout.move(3, x, y);
        assertEquals(x, layout.x(3), .001f);
        assertEquals(saved, layout.savedX(3), .001f);
        layout.move(3, x - 20, y + 15);
        assertEquals(x - 20, layout.x(3), .001f);
        assertEquals(y + 15, layout.y(3), .001f);
    }

    @Test public void previousPresetsAlignWithoutChangingSettingsAndOnlyOnce() {
        TouchLayout defaults = TouchLayout.importConfiguration(TouchPresets.threeFingers()).layout;
        for (float[][] positions : new float[][][] {LEGACY, REDISTRIBUTED, PREVIOUS}) {
            TouchLayout layout = layoutAt(positions);
            String migrated = TouchPresets.upgradeMobileLayout(layout.exportConfiguration(1.75f, 3.5f));
            TouchLayout.Configuration upgraded = TouchLayout.importConfiguration(migrated);
            assertAlignedPositions(upgraded.layout);
            assertEquals(1.75f, upgraded.sensitivity, 0);
            assertEquals(3.5f, upgraded.horizontalSensitivity, 0);
            assertEquals(layout.rumbleEnabled, upgraded.layout.rumbleEnabled);
            assertEquals(layout.gyroscopeEnabled, upgraded.layout.gyroscopeEnabled);
            assertEquals(layout.floatingStick, upgraded.layout.floatingStick);
            assertEquals(layout.opacity, upgraded.layout.opacity, 0);
            for (int i = 0; i < layout.size(); i++) {
                assertEquals("Only unmodified default sizes migrate", i == 4 ? 1.4f : defaults.sizeScale(i),
                    upgraded.layout.sizeScale(i), 0);
                assertEquals(layout.opacity(i), upgraded.layout.opacity(i), 0);
                assertEquals(layout.shown(i), upgraded.layout.shown(i));
            }
            assertEquals(migrated, TouchPresets.upgradeMobileLayout(migrated));
        }
    }

    @Test public void migrationPreservesManualSizesAndTheLeftFireVisibilityChoice() {
        TouchLayout layout = layoutAt(PREVIOUS);
        for (int i : new int[] {0, 4, 5, TouchLayout.FIRE_LEFT}) layout.setSize(i, 1.4f);
        layout.setShown(TouchLayout.FIRE_LEFT, false);
        TouchLayout upgraded = TouchLayout.importConfiguration(
            TouchPresets.upgradeMobileLayout(layout.exportConfiguration(1.75f, 4))).layout;
        assertAlignedPositions(upgraded);
        for (int i : new int[] {0, 4, 5, TouchLayout.FIRE_LEFT})
            assertEquals("Manual size of control " + i, 1.4f, upgraded.sizeScale(i), 0);
        assertFalse(upgraded.shown(TouchLayout.FIRE_LEFT));
    }

    @Test public void handPositionedLayoutsRemainExactlyAsAuthored() {
        for (float[][] positions : new float[][][] {LEGACY, REDISTRIBUTED, PREVIOUS}) {
            TouchLayout layout = layoutAt(positions);
            layout.move(2, layout.x(2) - 5, layout.y(2));
            String custom = layout.exportConfiguration(1.75f, 3.5f);
            assertEquals(custom, TouchPresets.upgradeMobileLayout(custom));
        }
    }

    private static TouchLayout layoutAt(float[][] positions) {
        TouchLayout layout = TouchLayout.importConfiguration(TouchPresets.threeFingers()).layout;
        for (int i = 0; i < positions.length; i++) {
            layout.setSize(i, OLD_SIZES[i]);
            layout.move(i, positions[i][0], positions[i][1]);
        }
        layout.rumbleEnabled = true;
        layout.gyroscopeEnabled = true;
        layout.floatingStick = false;
        layout.setOpacity(.7f);
        layout.setOpacity(4, .4f);
        layout.setSize(4, 1.4f);
        layout.setShown(6, false);
        return layout;
    }

    private static void assertAlignedPositions(TouchLayout layout) {
        assertEquals(ALIGNED.length, layout.size());
        for (int i = 0; i < ALIGNED.length; i++) {
            assertEquals("X of control " + i, ALIGNED[i][0], layout.savedX(i), .001f);
            assertEquals("Y of control " + i, ALIGNED[i][1], layout.savedY(i), .001f);
        }
    }
}
