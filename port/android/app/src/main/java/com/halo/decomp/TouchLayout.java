package com.halo.decomp;

import java.io.IOException;
import java.io.StringReader;
import java.io.StringWriter;
import java.util.ArrayList;
import java.util.Properties;

/**
 * The touch controls' layout (TouchControls): each control's kind, place,
 * size and whether it shows, with the settings that go with them, in the
 * overlay's logical units. Places are kept for the display the layout is
 * on and saved on a 960 x 540 grid, so a layout moves between displays of
 * other shapes. A layout file is Java properties text (version 4; 1, 2 and 3
 * are still read).
 */
final class TouchLayout {
    /** the kinds of control: a button of TouchControls's list, by index */
    static final int LEFT = 16;
    static final int FIRE_LEFT = 17;
    static final int BASE_COUNT = 18;
    static final int MAX_CONTROLS = 64;
    static final float MIN_SIZE = 0.5f;
    static final float MAX_SIZE = 2f;
    static final float MIN_OPACITY = 0.2f;
    static final float MIN_CONTROL_OPACITY = 0.1f;
    static final float MAX_OPACITY = 1f;
    static final float MIN_SENSITIVITY = 0.25f;
    static final float MAX_SENSITIVITY = 4f;
    static final float DEFAULT_HORIZONTAL_SENSITIVITY = 2.25f;
    static final float MIN_HORIZONTAL_SENSITIVITY = 1f;
    static final float MAX_HORIZONTAL_SENSITIVITY = 4f;
    /**
     * the largest minimum radius: a finger's width is capped here on a small,
     * dense display, where the D-pad's buttons (69 apart) would overlap
     */
    static final float MAX_MINIMUM_RADIUS = 34;

    /** the saved grid */
    private static final float GRID_WIDTH = 960;
    private static final float GRID_HEIGHT = 540;
    private static final int VERSION = 4;
    private static final int MAX_FILE_LENGTH = 65536;

    /** each kind's default place on the grid, and its radius */
    private static final float[][] DEFAULTS = {
        {913, 377}, {856, 447}, {794, 377}, {850, 312}, {915, 239}, {802, 239},
        {236, 449}, {691, 449}, {360, 490}, {438, 490}, {570, 36}, {390, 36},
        {100, 237}, {100, 335}, {51, 286}, {149, 286}, {115, 440}, {255, 239}
    };
    private static final float[] RADII = {
        36, 32, 34, 32, 39, 35, 32, 32, 27, 29, 28, 28, 25, 25, 25, 25, 64, 39
    };

    private static final class Control {
        final int type;
        float x;
        float y;
        boolean shown = true;
        float size = 1f;
        float opacity = Float.NaN;

        Control(int type, float x, float y) {
            this.type = type;
            this.x = x;
            this.y = y;
        }
    }

    /** a layout and the sensitivity saved with it */
    static final class Configuration {
        final TouchLayout layout;
        final float sensitivity;
        final float horizontalSensitivity;

        Configuration(TouchLayout layout, float sensitivity) {
            this(layout, sensitivity, DEFAULT_HORIZONTAL_SENSITIVITY);
        }

        Configuration(TouchLayout layout, float sensitivity, float horizontalSensitivity) {
            this.layout = layout;
            this.sensitivity = sensitivity;
            this.horizontalSensitivity = horizontalSensitivity;
        }
    }

    private final ArrayList<Control> controls = new ArrayList<>();
    private float width = GRID_WIDTH;
    private float height = GRID_HEIGHT;
    /** the smallest radius a control is drawn and touched at, in logical units */
    private float minimumRadius;
    /** Display-only margins; saved positions keep the author's full-screen grid. */
    private float safeLeft, safeTop, safeRight, safeBottom;

    boolean rumbleEnabled = true;
    /**
     * the overlay's gyroscope aiming as it was before the game's Gyro Aim
     * (Gamepads, input.gyro_aim) took its place: kept only so that layout
     * files of either kind still read and write
     */
    boolean gyroscopeEnabled = false;
    /** how solid the controls are drawn, MIN_OPACITY..MAX_OPACITY */
    float opacity = 1f;
    /** the move stick goes where the thumb lands in the lower left, not at its place */
    boolean floatingStick = false;

    TouchLayout() {
        resetDefaults();
    }

    /** places the layout on a display of this size, in logical units */
    void bounds(float width, float height) {
        if (!Float.isFinite(width) || !Float.isFinite(height) || width <= 0 || height <= 0)
            throw new IllegalArgumentException("Invalid display dimensions");
        for (Control control : controls) {
            control.x *= width / this.width;
            control.y *= height / this.height;
        }
        this.width = width;
        this.height = height;
        keepAllInside();
    }

    /**
     * the smallest radius, in logical units, at which a control is drawn and
     * touched (a finger's width, whatever the display's density), at most
     * MAX_MINIMUM_RADIUS
     */
    void setMinimumRadius(float radius) {
        minimumRadius = Float.isFinite(radius) && radius > 0 ? Math.min(radius, MAX_MINIMUM_RADIUS) : 0;
        keepAllInside();
    }

    int size() {
        return controls.size();
    }

    int type(int control) {
        return controls.get(control).type;
    }

    float x(int control) {
        float usable = Math.max(0, width - safeLeft - safeRight);
        return constrainX(safeLeft + controls.get(control).x * usable / width, radius(control));
    }

    float y(int control) {
        return constrainY(controls.get(control).y, radius(control));
    }

    float savedX(int control) {
        return controls.get(control).x * GRID_WIDTH / width;
    }

    float savedY(int control) {
        return controls.get(control).y * GRID_HEIGHT / height;
    }

    /** Exchange the base jump and melee positions without changing either control's settings. */
    void swapJumpMeleePositions() {
        Control jump = controls.get(0), melee = controls.get(1);
        float x = jump.x, y = jump.y;
        jump.x = melee.x;
        jump.y = melee.y;
        melee.x = x;
        melee.y = y;
    }

    void safeInsets(float left, float top, float right, float bottom) {
        safeLeft = Math.max(0, left);
        safeTop = Math.max(0, top);
        safeRight = Math.max(0, right);
        safeBottom = Math.max(0, bottom);
    }

    float constrainX(float x, float radius) {
        return constrain(x, safeLeft + radius, width - safeRight - radius);
    }

    float constrainY(float y, float radius) {
        return constrain(y, safeTop + radius, height - safeBottom - radius);
    }

    private static float constrain(float value, float low, float high) {
        return low <= high ? Math.max(low, Math.min(high, value)) : (low + high) / 2;
    }

    float radius(int control) {
        return Math.max(RADII[type(control)] * sizeScale(control), minimumRadius);
    }

    float sizeScale(int control) {
        return controls.get(control).size;
    }

    void setSize(int control, float size) {
        if (!Float.isFinite(size) || size < MIN_SIZE || size > MAX_SIZE)
            throw new IllegalArgumentException("Button size must be between 50% and 200%");
        controls.get(control).size = size;
        move(control, x(control), y(control));
    }

    boolean shown(int control) {
        return controls.get(control).shown;
    }

    void setShown(int control, boolean shown) {
        controls.get(control).shown = shown;
    }

    void setOpacity(float opacity) {
        if (!validOpacity(opacity))
            throw new IllegalArgumentException("Opacity must be between 20% and 100%");
        this.opacity = opacity;
    }

    float opacity(int control) {
        float value = controls.get(control).opacity;
        return Float.isNaN(value) ? opacity : value;
    }

    void setOpacity(int control, float opacity) {
        if (!validControlOpacity(opacity))
            throw new IllegalArgumentException("Button opacity must be between 10% and 100%");
        controls.get(control).opacity = opacity;
    }

    /** the default controls, at their default places, sizes and visibility */
    void resetDefaults() {
        controls.clear();
        for (int i = 0; i < DEFAULTS.length; i++)
            controls.add(new Control(i, DEFAULTS[i][0] * width / GRID_WIDTH, DEFAULTS[i][1] * height / GRID_HEIGHT));
        keepAllInside();
    }

    /** adds a copy of a control beside it; -1 if there may be no more (or it is the stick) */
    int duplicate(int source) {
        if (size() >= MAX_CONTROLS || type(source) == LEFT)
            return -1;
        float offset = radius(source) * 2 + 12;
        Control added = new Control(type(source), x(source) + offset, y(source));
        added.size = sizeScale(source);
        added.opacity = controls.get(source).opacity;
        controls.add(added);
        int index = size() - 1;
        // where there is no room to the right, the copy goes to the left
        if (added.x > width - radius(index))
            added.x = x(source) - offset;
        move(index, added.x, added.y);
        return index;
    }

    /** shows a hidden control of this kind, or else adds a copy; -1 if neither can be */
    int add(int type) {
        if (type < 0 || type >= BASE_COUNT)
            return -1;
        for (int i = 0; i < size(); i++) {
            if (type(i) == type && !shown(i)) {
                setShown(i, true);
                return i;
            }
        }
        return duplicate(type);
    }

    /** moves a control, keeping all of it on the display */
    void move(int control, float x, float y) {
        if (!Float.isFinite(x) || !Float.isFinite(y))
            return;
        float radius = radius(control);
        float usable = width - safeLeft - safeRight;
        controls.get(control).x = usable > 0
            ? (constrainX(x, radius) - safeLeft) * width / usable : width / 2;
        controls.get(control).y = constrainY(y, radius);
    }

    /** puts a control at a saved place on the grid, if it is a valid one */
    void restore(int control, float x, float y) {
        if (Float.isFinite(x) && Float.isFinite(y) && x >= 0 && x <= GRID_WIDTH && y >= 0 && y <= GRID_HEIGHT) {
            controls.get(control).x = x;
            controls.get(control).y = y;
        }
    }

    private void keepAllInside() {
        for (int i = 0; i < size(); i++) {
            Control control = controls.get(i);
            float radius = radius(i);
            control.x = Math.max(radius, Math.min(width - radius, control.x));
            control.y = Math.max(radius, Math.min(height - radius, control.y));
        }
    }

    /** the layout as a layout file's text, with the look sensitivity */
    String exportConfiguration(float sensitivity) {
        return exportConfiguration(sensitivity, DEFAULT_HORIZONTAL_SENSITIVITY);
    }

    String exportConfiguration(float sensitivity, float horizontalSensitivity) {
        if (!validSensitivity(sensitivity) || !validHorizontalSensitivity(horizontalSensitivity))
            throw new IllegalArgumentException("Invalid sensitivity");
        Properties values = new Properties();
        values.setProperty("format", "halo-touch-layout");
        values.setProperty("version", Integer.toString(VERSION));
        values.setProperty("rumble", Boolean.toString(rumbleEnabled));
        values.setProperty("gyroscope", Boolean.toString(gyroscopeEnabled));
        values.setProperty("opacity", Float.toString(opacity));
        values.setProperty("floating-stick", Boolean.toString(floatingStick));
        values.setProperty("count", Integer.toString(size()));
        values.setProperty("sensitivity", Float.toString(sensitivity));
        values.setProperty("horizontal-sensitivity", Float.toString(horizontalSensitivity));
        for (int i = 0; i < size(); i++) {
            String key = "control." + i + ".";
            values.setProperty(key + "type", Integer.toString(type(i)));
            values.setProperty(key + "x", Float.toString(savedX(i)));
            values.setProperty(key + "y", Float.toString(savedY(i)));
            values.setProperty(key + "visible", Boolean.toString(shown(i)));
            values.setProperty(key + "size", Float.toString(sizeScale(i)));
            values.setProperty(key + "opacity", Float.toString(opacity(i)));
        }
        StringWriter text = new StringWriter();
        try {
            values.store(text, "Halo Android touch layout");
        } catch (IOException e) {
            throw new IllegalStateException(e);
        }
        return text.toString();
    }

    /** reads a layout file's text; throws IllegalArgumentException for anything not valid */
    static Configuration importConfiguration(String text) {
        if (text == null || text.length() > MAX_FILE_LENGTH)
            throw new IllegalArgumentException("Invalid layout file");
        Properties values = new Properties();
        try {
            values.load(new StringReader(text));
            int version = readVersion(values);
            int count = Integer.parseInt(values.getProperty("count"));
            float sensitivity = Float.parseFloat(values.getProperty("sensitivity"));
            float horizontalSensitivity = Float.parseFloat(values.getProperty("horizontal-sensitivity",
                Float.toString(DEFAULT_HORIZONTAL_SENSITIVITY)));
            if (count < BASE_COUNT || count > MAX_CONTROLS || !validSensitivity(sensitivity)
                    || !validHorizontalSensitivity(horizontalSensitivity))
                throw new IllegalArgumentException("Invalid layout settings");
            TouchLayout layout = new TouchLayout();
            layout.controls.clear();
            if (version >= 2) {
                layout.rumbleEnabled = readBoolean(values, "rumble");
                layout.gyroscopeEnabled = readBoolean(values, "gyroscope");
            }
            if (version >= 3) {
                float opacity = Float.parseFloat(values.getProperty("opacity"));
                if (!validOpacity(opacity))
                    throw new IllegalArgumentException("Invalid opacity");
                layout.opacity = opacity;
                layout.floatingStick = readBoolean(values, "floating-stick");
            }
            for (int i = 0; i < count; i++)
                layout.controls.add(readControl(values, i, version));
            return new Configuration(layout, sensitivity, horizontalSensitivity);
        } catch (IOException | NullPointerException e) {
            throw new IllegalArgumentException("Incomplete layout file", e);
        }
    }

    private static int readVersion(Properties values) {
        if (!"halo-touch-layout".equals(values.getProperty("format")))
            throw new IllegalArgumentException("Unsupported layout file");
        String version = values.getProperty("version");
        for (int known = 1; known <= VERSION; known++) {
            if (Integer.toString(known).equals(version))
                return known;
        }
        throw new IllegalArgumentException("Unsupported layout file");
    }

    private static Control readControl(Properties values, int index, int version) {
        String key = "control." + index + ".";
        int type = Integer.parseInt(values.getProperty(key + "type"));
        float x = Float.parseFloat(values.getProperty(key + "x"));
        float y = Float.parseFloat(values.getProperty(key + "y"));
        String shown = values.getProperty(key + "visible");
        float size = version >= 2 ? Float.parseFloat(values.getProperty(key + "size")) : 1f;
        boolean validType = type >= 0 && type < BASE_COUNT && (index >= BASE_COUNT ? type != LEFT : type == index);
        boolean validPlace = Float.isFinite(x) && Float.isFinite(y) && x >= 0 && x <= GRID_WIDTH
            && y >= 0 && y <= GRID_HEIGHT;
        boolean validSize = Float.isFinite(size) && size >= MIN_SIZE && size <= MAX_SIZE;
        if (!validType || !validPlace || !validSize || !("true".equals(shown) || "false".equals(shown)))
            throw new IllegalArgumentException("Invalid control in layout file");
        Control control = new Control(type, x, y);
        control.shown = Boolean.parseBoolean(shown);
        control.size = size;
        if (version >= 4) {
            float opacity = Float.parseFloat(values.getProperty(key + "opacity"));
            if (!validControlOpacity(opacity))
                throw new IllegalArgumentException("Invalid control opacity in layout file");
            control.opacity = opacity;
        }
        return control;
    }

    private static boolean readBoolean(Properties values, String key) {
        String value = values.getProperty(key);
        if (!("true".equals(value) || "false".equals(value)))
            throw new IllegalArgumentException("Invalid " + key + " option");
        return Boolean.parseBoolean(value);
    }

    static boolean validSensitivity(float value) {
        return Float.isFinite(value) && value >= MIN_SENSITIVITY && value <= MAX_SENSITIVITY;
    }

    static boolean validHorizontalSensitivity(float value) {
        return Float.isFinite(value) && value >= MIN_HORIZONTAL_SENSITIVITY && value <= MAX_HORIZONTAL_SENSITIVITY;
    }

    static boolean validOpacity(float value) {
        return Float.isFinite(value) && value >= MIN_OPACITY && value <= MAX_OPACITY;
    }

    static boolean validControlOpacity(float value) {
        return Float.isFinite(value) && value >= MIN_CONTROL_OPACITY && value <= MAX_OPACITY;
    }
}
