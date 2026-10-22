package com.halo.decomp;

import java.io.IOException;
import java.io.StringWriter;
import java.util.Properties;

/** Touch presets on the layout file's 960 x 540 grid. */
final class TouchPresets {
    static final String TWO_THUMBS_NAME = "Dos pulgares";
    static final String THREE_FINGERS_NAME = "Tres dedos";
    static final String ORIGINAL_NAME = "Original";

    // A, B, X, Y, RT, LT, LS, RS, White, Black, Start, Back, D-pad, stick, second RT.
    private static final float[][] LEGACY_PLACES = {
        {900, 430}, {900, 330}, {812, 146}, {900, 146}, {800, 350}, {700, 450},
        {800, 480}, {900, 236}, {526, 480}, {612, 480}, {576, 68}, {390, 68},
        {100, 237}, {100, 335}, {51, 286}, {149, 286}, {130, 420}, {145, 110}
    };
    private static final float[][] REDISTRIBUTED_PLACES = {
        {900, 445}, {900, 350}, {790, 180}, {900, 180}, {785, 350}, {260, 410},
        {790, 470}, {900, 265}, {105, 205}, {220, 205}, {576, 68}, {390, 68},
        {100, 237}, {100, 335}, {51, 286}, {149, 286}, {130, 420}, {310, 300}
    };
    private static final float[][] PREVIOUS_PLACES = {
        {900, 430}, {900, 330}, {812, 146}, {900, 146}, {780, 350}, {700, 450},
        {800, 480}, {900, 236}, {526, 480}, {612, 480}, {576, 68}, {390, 68},
        {100, 237}, {100, 335}, {51, 286}, {149, 286}, {130, 420}, {145, 110}
    };
    private static final float[][] PLACES = {
        {900, 342}, {900, 432}, {812, 162}, {900, 162}, {780, 342}, {700, 480},
        {800, 480}, {900, 252}, {526, 480}, {612, 480}, {576, 68}, {390, 68},
        {100, 237}, {100, 335}, {51, 286}, {149, 286}, {130, 420}, {200, 230}
    };
    private static final float[] OLD_SIZES = {
        1, 34f / 32, 1, 34f / 32, 1.35f, 1, 34f / 32, 34f / 32,
        34f / 27, 34f / 29, 34f / 28, 34f / 28,
        34f / 25, 34f / 25, 34f / 25, 34f / 25, 1, 1.35f
    };
    private static final float[] SIZES = {
        34f / 36, 34f / 32, 1, 34f / 32, 48f / 39, 34f / 35, 34f / 32, 34f / 32,
        34f / 27, 34f / 29, 34f / 28, 34f / 28,
        34f / 25, 34f / 25, 34f / 25, 34f / 25, 1, 44f / 39
    };

    private TouchPresets() {
    }

    static String twoThumbs() {
        return mobile(false);
    }

    static String threeFingers() {
        return mobile(true);
    }

    static String original() {
        return new TouchLayout().exportConfiguration(1f);
    }

    /** Align preset controls without replacing a hand-positioned HUD or its settings. */
    static String upgradeMobileLayout(String configuration) {
        TouchLayout.Configuration saved = TouchLayout.importConfiguration(configuration);
        if (saved.layout.size() != LEGACY_PLACES.length)
            return configuration;
        boolean legacy = true, redistributed = true, previous = true;
        for (int i = 0; i < LEGACY_PLACES.length; i++) {
            if (saved.layout.type(i) != i)
                return configuration;
            legacy &= Math.abs(saved.layout.savedX(i) - LEGACY_PLACES[i][0]) <= 0.1f
                && Math.abs(saved.layout.savedY(i) - LEGACY_PLACES[i][1]) <= 0.1f;
            redistributed &= Math.abs(saved.layout.savedX(i) - REDISTRIBUTED_PLACES[i][0]) <= 0.1f
                && Math.abs(saved.layout.savedY(i) - REDISTRIBUTED_PLACES[i][1]) <= 0.1f;
            previous &= Math.abs(saved.layout.savedX(i) - PREVIOUS_PLACES[i][0]) <= 0.1f
                && Math.abs(saved.layout.savedY(i) - PREVIOUS_PLACES[i][1]) <= 0.1f;
        }
        if (!legacy && !redistributed && !previous)
            return configuration;
        for (int i = 0; i < PLACES.length; i++) {
            if (Math.abs(saved.layout.savedX(i) - PLACES[i][0]) > 0.1f
                    || Math.abs(saved.layout.savedY(i) - PLACES[i][1]) > 0.1f)
                saved.layout.move(i, PLACES[i][0], PLACES[i][1]);
            if (Float.compare(saved.layout.sizeScale(i), OLD_SIZES[i]) == 0
                    && Float.compare(SIZES[i], OLD_SIZES[i]) != 0)
                saved.layout.setSize(i, SIZES[i]);
        }
        return saved.layout.exportConfiguration(saved.sensitivity, saved.horizontalSensitivity);
    }

    private static String mobile(boolean secondFire) {
        Properties values = new Properties();
        values.setProperty("format", "halo-touch-layout");
        values.setProperty("version", "3");
        values.setProperty("count", Integer.toString(PLACES.length));
        values.setProperty("sensitivity", "1.0");
        values.setProperty("rumble", "false");
        values.setProperty("gyroscope", "false");
        values.setProperty("opacity", "0.6");
        values.setProperty("floating-stick", "true");
        for (int i = 0; i < PLACES.length; i++) {
            String key = "control." + i + ".";
            values.setProperty(key + "type", Integer.toString(i));
            values.setProperty(key + "x", Float.toString(PLACES[i][0]));
            values.setProperty(key + "y", Float.toString(PLACES[i][1]));
            values.setProperty(key + "size", Float.toString(SIZES[i]));
            values.setProperty(key + "visible", Boolean.toString((i < 12 || i >= 16) &&
                (i != TouchLayout.FIRE_LEFT || secondFire)));
        }
        StringWriter text = new StringWriter();
        try {
            values.store(text, "Halo Android touch preset");
        } catch (IOException e) {
            throw new IllegalStateException(e);
        }
        return text.toString();
    }
}
