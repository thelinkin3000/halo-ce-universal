package com.halo.decomp;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Paint;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.media.AudioAttributes;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.SparseArray;
import android.util.SparseIntArray;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.util.Arrays;
import java.util.Locale;

/**
 * The on-screen touch controls: a multitouch overlay over SDL's surface,
 * with a stick, the controller's buttons, swipe (and optionally gyroscope)
 * aiming and an editor for the layout (TouchLayout). Its state goes to the
 * game through port/android/host/host_touch.c, as port 0's controller.
 *
 * It shows only in a game, never at the main menu, over a menu (which
 * takes taps itself, port/linux/src/touch_input.c) or during a cinematic,
 * only on a touchscreen, and with input.touch_controls = "auto" only while
 * no controller is connected. While it is not shown it lets every finger
 * through to the game.
 */
public final class TouchControls extends View implements SensorEventListener, InputManager.InputDeviceListener {
    /** what a finger holds besides a control: the view swipe */
    private static final int LOOK = -5;
    private static final int NOTHING = Integer.MIN_VALUE;
    /** the toolbar's items (at the top of the screen) */
    private static final int TOOLBAR_TOGGLE = -3;
    private static final int TOOLBAR_OPTIONS = -4;
    private static final float TOOLBAR_RADIUS = 28;
    private static final float OPTIONS_RADIUS = 34;

    /** the logical grid the controls are laid out on, scaled to the display's height */
    private static final float GRID_WIDTH = 960;
    private static final float GRID_HEIGHT = 540;
    /** a control is never smaller than a finger: 24 dp of radius, 48 across */
    private static final float MINIMUM_RADIUS_DP = 24;
    /** a finger moves this far (logical units) before a press becomes a swipe */
    private static final float SLOP = 10;
    /** the move stick's dead zone and its touch area beyond its circle */
    private static final float STICK_DEAD_ZONE = 0.12f;
    private static final float STICK_REACH = 1.28f;
    private static final float STICK_KNOB_RADIUS = 24;
    private static final long AUTO_WALK_HOLD_MS = 600;
    /** a floating stick goes where a thumb lands in this part of the display (from the lower left) */
    private static final float FLOATING_ZONE_WIDTH = 0.45f;
    private static final float FLOATING_ZONE_TOP = 0.35f;
    /** radians per logical unit of swipe (the mouse's rate: xinput_sdl.c) */
    static final float LOOK_RADIANS_PER_UNIT = 0.0022f;
    /** a label is at most this many radii wide */
    private static final float LABEL_WIDTH = 1.7f;
    private static final long POLL_MS = 16;
    /** Long enough for the game's 30 Hz input to read a released grenade tap. */
    private static final long GRENADE_PULSE_MS = 80;
    /** the game is in play this long before the controls show (no flash at a level's first frame) */
    private static final long SHOW_DELAY_MS = 200;
    private static final long VIBRATION_MS = 110;
    private static final long VIBRATION_RENEW_MS = 70;

    /** the game's HALO_TOUCH_SCENE bits (port/linux/src/touch_input.c) */
    private static final int SCENE_KNOWN = 1;
    private static final int SCENE_MENUS = 2;
    private static final int SCENE_ON = 4;
    private static final int SCENE_OFF = 8;

    private static final String PREFERENCES = "touch-layout-v1";
    private static final String KEY_CONFIGURATION = "configuration";
    private static final String KEY_HIDDEN = "hidden";
    private static final String KEY_AUTO_WALK = "auto-walk";
    private static final String KEY_INVERT_SWIPE_X = "invert-swipe-x";
    private static final String KEY_INVERT_SWIPE_Y = "invert-swipe-y";
    private static final String KEY_JUMP_MELEE_SWAPPED = "jump-melee-swapped-v1";

    /**
     * a kind of control: the SDL button bit or trigger axis it sends (-1 for
     * none), and the game's controller button it is (port/linux/game/
     * touch_game.c numbers them: input.h's _gamepad_*), which names it
     */
    private static final class Kind {
        final int bit;
        final int trigger;
        final int gamepadButton;
        final String buttonName;
        final boolean faceButton;

        Kind(int bit, int trigger, int gamepadButton, String buttonName, boolean faceButton) {
            this.bit = bit;
            this.trigger = trigger;
            this.gamepadButton = gamepadButton;
            this.buttonName = buttonName;
            this.faceButton = faceButton;
        }
    }

    /** by TouchLayout's kinds; bits follow SDL_GamepadButton, triggers are SDL axes 4 and 5 */
    private static final Kind[] KINDS = {
        new Kind(0, -1, 0, "A", true),
        new Kind(1, -1, 1, "B", true),
        new Kind(2, -1, 2, "X", true),
        new Kind(3, -1, 3, "Y", true),
        new Kind(-1, 5, 7, "RT", false),
        new Kind(-1, 4, 6, "LT", false),
        new Kind(7, -1, 14, "LS", false),
        new Kind(8, -1, 15, "RS", false),
        new Kind(9, -1, 5, "White", false),
        new Kind(10, -1, 4, "Black", false),
        new Kind(6, -1, 12, "Start", false),
        new Kind(4, -1, 13, "Back", false),
        new Kind(11, -1, 8, "Up", false),
        new Kind(12, -1, 9, "Down", false),
        new Kind(13, -1, 10, "Left", false),
        new Kind(14, -1, 11, "Right", false),
        new Kind(-1, -1, -1, "Move", false),
        new Kind(-1, 5, 7, "RT", false),
    };

    /** the game's controls (input_abstraction.c's _game_control_*), as the buttons name them */
    private static final String[] GAME_CONTROLS = {
        "Jump", "Gren. type", "Usar / entrar / recargar", "Cambiar arma", "Melee", "Light",
        "Grenade", "Fire", "Pause", "Back", "Crouch", "Zoom",
    };
    /** the default profile's mapping, until the game sends the player's own */
    private static final int[] DEFAULT_BINDINGS = {
        0, 4, 2, 3, 1, 5, 6, 7, -1, -1, -1, -1, 8, 9, 10, 11,
    };

    private static native void nativeState(int lx, int ly, int rx, int ry, int lt, int rt, int buttons);
    private static native void nativeLook(float dx, float dy);
    private static native void nativeGyro(float dx, float dy);
    private static native void nativeLookReset();
    private static native int nativeGyroMode();
    private static native int nativeRumble();
    private static native int nativeScene();
    private static native int nativeBindings(int[] controls);

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final TouchIcons icons = new TouchIcons(getResources());
    private final SharedPreferences preferences;
    private final SensorManager sensors;
    private final Sensor gyroscope;
    private final Vibrator vibrator;
    private final InputManager inputManager;
    private final boolean touchscreenFeature;
    private final GyroscopeAim gyroAim = new GyroscopeAim();
    private final float[] gyroDelta = new float[2];
    private final AudioAttributes rumbleAttributes = new AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_GAME)
        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
        .build();

    private TouchLayout layout = new TouchLayout();
    private float sensitivity = 1f;
    private float horizontalSensitivity = TouchLayout.DEFAULT_HORIZONTAL_SENSITIVITY;
    private boolean invertSwipeX, invertSwipeY;
    private boolean gameMenuPending;
    private final Runnable releaseMenuButton = () -> nativeState(0, 0, 0, 0, 0, 0, 0);
    /** the controls removed by the toolbar's Hide, until its Touch */
    private boolean hidden;
    private boolean editing;
    private HudEditorPanel editorPanel;
    private int selectedControl = 4;
    private int activeHud;
    private int hudBeforeEditing;
    private String layoutBeforeEditing;
    private boolean hiddenBeforeEditing;
    private final String[] hudDrafts = new String[3];
    /** the option dialogs open (one opens before the last one's dismissal is told) */
    private int openDialogs;

    /** what each finger holds: a control, LOOK, or (in toolbarFingers) a toolbar item */
    private final SparseIntArray owners = new SparseIntArray();
    private final Handler releaseHandler = new Handler(Looper.getMainLooper());
    private int grenadePulseType = NOTHING;
    private final Runnable clearGrenadePulse = () -> {
        grenadePulseType = NOTHING;
        publish();
    };
    /** button swipe origins and inactive aiming fingers' latest positions, in pixels */
    private final SparseArray<float[]> buttonTouches = new SparseArray<>();
    /** each finger on a toolbar item: {x, y, item}; the item acts when it lifts there */
    private final SparseArray<float[]> toolbarFingers = new SparseArray<>();
    private final int[] axes = new int[6];
    private int lookPointer = -1;
    private float lookX;
    private float lookY;
    /** a floating stick's centre while a thumb holds it (logical units) */
    private boolean stickFloating;
    private float stickX;
    private float stickY;
    private boolean autoWalkEnabled;
    private boolean autoWalkLocked;
    private long autoWalkSince = -1;
    private final Runnable lockAutoWalk = this::latchAutoWalk;
    private int dragPointer = -1;
    private int dragControl = -1;
    private float dragOffsetX;
    private float dragOffsetY;

    /** pixels per logical unit, and the display in logical units */
    private float scale = 1;
    private float logicalWidth = GRID_WIDTH;
    private float logicalHeight = GRID_HEIGHT;
    private int insetLeft, insetTop, insetRight, insetBottom;

    private int scene;
    private boolean controllerConnected;
    private boolean touchscreen;
    private boolean deviceInputActive;
    /** the controls are on screen: shown() for SHOW_DELAY_MS */
    private boolean showing;
    private long shownSince = -1;
    private boolean gyroRegistered;
    /** the game's Gyro Aim, polled: 0 off, 1 on, 2 only on zoom */
    private int gyroMode;
    private int lastAmplitude;
    private long lastVibration;
    private final int[] bindings = DEFAULT_BINDINGS.clone();
    private final int[] bindingsRead = new int[bindings.length];
    private int bindingsSerial;

    /** reads what the game says every frame: when to show, the buttons' names, and the rumble */
    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (!deviceInputActive)
                return;
            scene = nativeScene();
            tryOpenGameMenu();
            int mode = nativeGyroMode();
            if (mode != gyroMode) {
                gyroMode = mode;
                updateSensors();
            }
            updateShowing();
            readBindings();
            updateRumble();
            postDelayed(this, POLL_MS);
        }
    };

    public TouchControls(Context context) {
        super(context);
        sensors = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        gyroscope = sensors == null ? null : sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        if (Build.VERSION.SDK_INT >= 31) {
            VibratorManager manager = (VibratorManager) context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            vibrator = manager == null ? null : manager.getDefaultVibrator();
        } else {
            vibrator = (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
        }
        inputManager = (InputManager) context.getSystemService(Context.INPUT_SERVICE);
        touchscreenFeature = context.getPackageManager().hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN);
        preferences = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE);
        migrateJumpMeleeLayout(context);
        autoWalkEnabled = preferences.getBoolean(KEY_AUTO_WALK, true);
        invertSwipeX = preferences.getBoolean(KEY_INVERT_SWIPE_X, false);
        invertSwipeY = preferences.getBoolean(KEY_INVERT_SWIPE_Y, false);
        activeHud = Math.max(0, Math.min(2, preferences.getInt("hud-slot", 0)));
        loadLayout(context);
        hidden = preferences.getBoolean(KEY_HIDDEN, false);
        refreshInputDevices();
        setFocusable(false);
        setContentDescription("Halo touch controller");
        setOnApplyWindowInsetsListener((view, insets) -> {
            // Stable margins keep controls out of system gestures without moving when bars fade.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                Insets bars = insets.getInsetsIgnoringVisibility(WindowInsets.Type.systemBars()
                    | WindowInsets.Type.displayCutout());
                insetLeft = bars.left;
                insetTop = bars.top;
                insetRight = bars.right;
                insetBottom = bars.bottom;
            } else {
                insetLeft = insets.getStableInsetLeft();
                insetTop = insets.getStableInsetTop();
                insetRight = insets.getStableInsetRight();
                insetBottom = insets.getStableInsetBottom();
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                Insets gestures = insets.getSystemGestureInsets();
                insetLeft = Math.max(insetLeft, gestures.left);
                insetTop = Math.max(insetTop, gestures.top);
                insetRight = Math.max(insetRight, gestures.right);
                insetBottom = Math.max(insetBottom, gestures.bottom);
            }
            if (insets.getDisplayCutout() != null) {
                insetLeft = Math.max(insetLeft, insets.getDisplayCutout().getSafeInsetLeft());
                insetRight = Math.max(insetRight, insets.getDisplayCutout().getSafeInsetRight());
                insetTop = Math.max(insetTop, insets.getDisplayCutout().getSafeInsetTop());
                insetBottom = Math.max(insetBottom, insets.getDisplayCutout().getSafeInsetBottom());
            }
            updateSafeBounds();
            invalidate();
            return insets;
        });
    }

    private void migrateJumpMeleeLayout(Context context) {
        if (preferences.getBoolean(KEY_JUMP_MELEE_SWAPPED, false))
            return;
        SharedPreferences.Editor changes = preferences.edit();
        for (String key : new String[] {KEY_CONFIGURATION, "hud-layout-0", "hud-layout-1", "hud-layout-2"}) {
            String configuration = preferences.getString(key, null);
            if (configuration == null)
                continue;
            try {
                TouchLayout.Configuration saved = TouchLayout.importConfiguration(configuration);
                saved.layout.swapJumpMeleePositions();
                changes.putString(key, saved.layout.exportConfiguration(saved.sensitivity, saved.horizontalSensitivity));
            } catch (IllegalArgumentException e) {
                // Leave an unreadable layout for the existing fallback and retain the user's file.
            }
        }
        if (!changes.putBoolean(KEY_JUMP_MELEE_SWAPPED, true).commit())
            Toast.makeText(context, "No se pudo guardar el cambio de Salto y Puño. Inténtalo de nuevo.",
                Toast.LENGTH_LONG).show();
    }

    private void loadLayout(Context context) {
        String configuration = preferences.getString(KEY_CONFIGURATION, null);
        if (configuration == null)
            configuration = TouchPresets.twoThumbs();
        try {
            TouchLayout.Configuration saved = TouchLayout.importConfiguration(TouchPresets.upgradeMobileLayout(configuration));
            layout = saved.layout;
            sensitivity = saved.sensitivity;
            horizontalSensitivity = saved.horizontalSensitivity;
        } catch (IllegalArgumentException e) {
            Toast.makeText(context, "The saved touch layout could not be read. Using the default one.",
                Toast.LENGTH_LONG).show();
        }
    }

    // ---------- the activity's lifecycle (HaloActivity)

    public void startDeviceInput() {
        if (deviceInputActive)
            return;
        deviceInputActive = true;
        if (inputManager != null)
            inputManager.registerInputDeviceListener(this, null);
        refreshInputDevices();
        gyroAim.reset();
        updateSensors();
        post(poll);
    }

    public void stopDeviceInput() {
        gameMenuPending = false;
        deviceInputActive = false;
        removeCallbacks(poll);
        if (inputManager != null)
            inputManager.unregisterInputDeviceListener(this);
        updateSensors();
        cancelRumble();
        reset();
    }

    // ---------- the display

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        layoutControls();
        requestApplyInsets();
    }

    /** scales the grid to the display (its height, keeping the circles round) */
    private void layoutControls() {
        if (getWidth() <= 0 || getHeight() <= 0)
            return;
        reset();
        float width = getWidth();
        float height = getHeight();
        scale = Math.max(0.01f, Math.min(width / GRID_WIDTH, height / GRID_HEIGHT));
        logicalWidth = width / scale;
        logicalHeight = height / scale;
        layout.bounds(logicalWidth, logicalHeight);
        layout.setMinimumRadius(minimumRadius());
        updateSafeBounds();
        invalidate();
    }

    private void updateSafeBounds() {
        float toolbarBottom = toolbarY() + toolbarRadius(OPTIONS_RADIUS) + 8;
        layout.safeInsets(insetLeft / scale, toolbarBottom, insetRight / scale, insetBottom / scale);
    }

    /** a finger's width, in logical units (at most TouchLayout.MAX_MINIMUM_RADIUS) */
    private float minimumRadius() {
        float radius = MINIMUM_RADIUS_DP * getResources().getDisplayMetrics().density / scale;
        return Math.min(radius, TouchLayout.MAX_MINIMUM_RADIUS);
    }

    private float logical(float pixels) {
        return pixels / scale;
    }

    private float toolbarY() {
        return Math.max(38, insetTop / scale + 38);
    }

    private float optionsX() {
        return logicalWidth - Math.max(42, insetRight / scale + 42);
    }

    private float toggleX() {
        return (insetLeft / scale + logicalWidth - insetRight / scale) / 2;
    }

    private float toolbarRadius(float radius) {
        return Math.max(radius, minimumRadius());
    }

    // ---------- when the controls show

    boolean isGameplayVisible() {
        return showing && !editing && openDialogs == 0;
    }

    /**
     * whether the controls belong on screen: in a game (no menu or
     * cinematic), never without a touchscreen, and with
     * input.touch_controls = "auto" only while no controller is connected
     */
    private boolean shown() {
        if ((scene & SCENE_KNOWN) == 0 || (scene & (SCENE_MENUS | SCENE_OFF)) != 0 || !touchscreen)
            return false;
        return (scene & SCENE_ON) != 0 || !controllerConnected;
    }

    /** shows the controls once shown() has held for SHOW_DELAY_MS; hides them at once */
    private void updateShowing() {
        boolean show = false;
        if (!shown()) {
            shownSince = -1;
        } else {
            long now = SystemClock.uptimeMillis();
            if (shownSince < 0)
                shownSince = now;
            show = now - shownSince >= SHOW_DELAY_MS;
        }
        if (show != showing) {
            showing = show;
            reset();
            updateSensors();
        }
    }

    /** a game controller: a stick or a hat, not a phone's few gamepad keys */
    private static boolean isController(InputDevice device) {
        if (device == null || device.isVirtual())
            return false;
        if ((device.getSources() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK)
            return false;
        return device.getMotionRange(MotionEvent.AXIS_X, InputDevice.SOURCE_JOYSTICK) != null
            || device.getMotionRange(MotionEvent.AXIS_HAT_X, InputDevice.SOURCE_JOYSTICK) != null;
    }

    private void refreshInputDevices() {
        boolean controller = false;
        boolean touch = touchscreenFeature;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null)
                continue;
            if (isController(device))
                controller = true;
            boolean touchSource = (device.getSources() & InputDevice.SOURCE_TOUCHSCREEN) == InputDevice.SOURCE_TOUCHSCREEN;
            if (!device.isVirtual() && touchSource)
                touch = true;
        }
        controllerConnected = controller;
        touchscreen = touch;
        updateShowing();
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        refreshInputDevices();
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        refreshInputDevices();
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        refreshInputDevices();
    }

    private void setHidden(boolean hidden) {
        reset();
        this.hidden = hidden;
        preferences.edit().putBoolean(KEY_HIDDEN, hidden).apply();
        updateSensors();
    }

    // ---------- the controller state the game reads

    /** lets go of every finger and sends a released controller */
    public void reset() {
        releaseHandler.removeCallbacks(releaseMenuButton);
        cancelGrenadePulse();
        cancelAutoWalk();
        dragPointer = -1;
        dragControl = -1;
        owners.clear();
        buttonTouches.clear();
        toolbarFingers.clear();
        lookPointer = -1;
        stickFloating = false;
        gyroAim.reset();
        nativeLookReset();
        Arrays.fill(axes, 0);
        publish();
        invalidate();
    }

    private void publish() {
        if (editing || openDialogs > 0) {
            nativeState(0, 0, 0, 0, 0, 0, 0);
            return;
        }
        int bits = 0;
        axes[4] = 0;
        axes[5] = 0;
        for (int i = 0; i < owners.size(); i++) {
            int control = owners.valueAt(i);
            if (control < 0 || control >= layout.size())
                continue;
            Kind kind = KINDS[layout.type(control)];
            if (isGrenade(kind))
                continue;
            if (kind.bit >= 0)
                bits |= 1 << kind.bit;
            if (kind.trigger >= 0)
                axes[kind.trigger] = 32767;
        }
        if (grenadePulseType >= 0) {
            Kind kind = KINDS[grenadePulseType];
            if (kind.bit >= 0)
                bits |= 1 << kind.bit;
            if (kind.trigger >= 0)
                axes[kind.trigger] = 32767;
        }
        nativeState(axes[0], axes[1], axes[2], axes[3], axes[4], axes[5], bits);
    }

    private boolean isGrenade(Kind kind) {
        return kind.gamepadButton >= 0 && bindings[kind.gamepadButton] == 6;
    }

    private void cancelGrenadePulse() {
        releaseHandler.removeCallbacks(clearGrenadePulse);
        grenadePulseType = NOTHING;
    }

    private boolean held(int control) {
        return owners.indexOfValue(control) >= 0;
    }

    private float stickCenterX() {
        return stickFloating ? stickX : layout.x(TouchLayout.LEFT);
    }

    private float stickCenterY() {
        return stickFloating ? stickY : layout.y(TouchLayout.LEFT);
    }

    /** the floating stick, centred where a thumb landed (at rest there, even near an edge) */
    private void floatStick(float x, float y) {
        stickFloating = true;
        stickX = x;
        stickY = y;
    }

    private boolean inFloatingZone(float x, float y) {
        return x < logicalWidth * FLOATING_ZONE_WIDTH && y > logicalHeight * FLOATING_ZONE_TOP;
    }

    private void moveStick(int control, float x, float y) {
        float dx = (x - stickCenterX()) / layout.radius(control);
        float dy = (y - stickCenterY()) / layout.radius(control);
        float length = (float) Math.sqrt(dx * dx + dy * dy);
        if (length < STICK_DEAD_ZONE) {
            dx = 0;
            dy = 0;
        } else if (length > 1) {
            dx /= length;
            dy /= length;
        }
        axes[0] = Math.round(dx * 32767);
        axes[1] = Math.round(dy * 32767);
        if (!autoWalkEnabled || !forwardForAutoWalk()) {
            cancelAutoWalk();
        } else if (autoWalkLocked) {
            axes[0] = 0;
            axes[1] = -32767;
        } else if (autoWalkSince < 0) {
            autoWalkSince = SystemClock.uptimeMillis();
            releaseHandler.postDelayed(lockAutoWalk, AUTO_WALK_HOLD_MS);
        }
    }

    private boolean forwardForAutoWalk() {
        return axes[1] <= -0.9f * 32767 && Math.abs(axes[0]) <= 0.25f * 32767;
    }

    private void cancelAutoWalk() {
        releaseHandler.removeCallbacks(lockAutoWalk);
        autoWalkSince = -1;
        autoWalkLocked = false;
    }

    private void latchAutoWalk() {
        autoWalkSince = -1;
        if (!autoWalkEnabled || !held(TouchLayout.LEFT) || !forwardForAutoWalk()
                || !showing || hidden || editing || openDialogs > 0)
            return;
        autoWalkLocked = true;
        axes[0] = 0;
        axes[1] = -32767;
        publish();
        invalidate();
    }

    // ---------- fingers

    private void pulseMenuButton(int bit) {
        reset();
        nativeState(0, 0, 0, 0, 0, 0, 1 << bit);
        releaseHandler.postDelayed(releaseMenuButton, GRENADE_PULSE_MS);
    }

    void openGameMenu() {
        gameMenuPending = true;
        post(this::tryOpenGameMenu);
    }

    private void tryOpenGameMenu() {
        if (!gameMenuPending || editing || openDialogs > 0 || !deviceInputActive
                || (scene & SCENE_KNOWN) == 0)
            return;
        gameMenuPending = false;
        if ((scene & SCENE_MENUS) == 0)
            pulseMenuButton(6); // SDL Start, independent of the profile's gameplay binding.
    }

    private static boolean inside(float x, float y, float cx, float cy, float radius) {
        return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
    }

    /** what is under a point (logical units): a toolbar item, a control, or NOTHING */
    private int hit(float x, float y) {
        if (!editing && inside(x, y, optionsX(), toolbarY(), toolbarRadius(OPTIONS_RADIUS)))
            return TOOLBAR_OPTIONS;
        if (!editing && inside(x, y, toggleX(), toolbarY(), toolbarRadius(TOOLBAR_RADIUS)))
            return TOOLBAR_TOGGLE;
        if (hidden && !editing)
            return NOTHING;
        int nearest = NOTHING;
        float distance = Float.POSITIVE_INFINITY;
        for (int i = 0; i < layout.size(); i++) {
            if ((editing || layout.shown(i)) && layout.type(i) != TouchLayout.LEFT) {
                float dx = x - layout.x(i), dy = y - layout.y(i), radius = layout.radius(i);
                float relative = (dx * dx + dy * dy) / (radius * radius);
                if (relative <= 1 && relative < distance) {
                    nearest = i;
                    distance = relative;
                }
            }
        }
        if (nearest != NOTHING)
            return nearest;
        int stick = TouchLayout.LEFT;
        float reach = editing ? 1 : STICK_REACH;
        if ((editing || layout.shown(stick)) && !held(stick)
                && inside(x, y, layout.x(stick), layout.y(stick), layout.radius(stick) * reach))
            return stick;
        return NOTHING;
    }

    /**
     * whether a finger went down where Android keeps its edge gestures; in
     * sticky full screen the first swipe in from an edge only shows the
     * system bars, and Android also hands it over as an ordinary finger
     */
    private boolean inGestureZone(float x, float y, boolean sidesOnly) {
        int[] insets = ((HaloActivity) getContext()).getSystemGestureInsetsPixels();
        if (x < insets[0] || x >= getWidth() - insets[2])
            return true;
        return !sidesOnly && (y < insets[1] || y >= getHeight() - insets[3]);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (!showing && !editing && openDialogs == 0) {
            // Keep settings reachable while the game loads or plays a cinematic.
            if (action == MotionEvent.ACTION_DOWN) {
                if (hit(logical(event.getX()), logical(event.getY())) == TOOLBAR_OPTIONS) {
                    toolbarFingers.put(event.getPointerId(0), new float[] {event.getX(), event.getY(), TOOLBAR_OPTIONS});
                    return true;
                }
                return false;
            }
            if (toolbarFingers.size() > 0) {
                if (action == MotionEvent.ACTION_UP) {
                    toolbarFingers.clear();
                    if (hit(logical(event.getX()), logical(event.getY())) == TOOLBAR_OPTIONS)
                        showOptions();
                } else if (action == MotionEvent.ACTION_CANCEL) {
                    toolbarFingers.clear();
                }
                return true;
            }
            if (owners.size() > 0 || lookPointer >= 0)
                reset();
            return true;
        }
        int previousStickX = axes[0];
        int previousStickY = axes[1];
        if (editing)
            editTouch(event);
        else
            gameTouch(event);
        publish();
        // Aiming changes the game, not this overlay; keep input unthrottled.
        if (editing || action != MotionEvent.ACTION_MOVE
                || axes[0] != previousStickX || axes[1] != previousStickY)
            invalidate();
        return true;
    }

    @Override
    public boolean performClick() {
        super.performClick();
        return true;
    }

    private void gameTouch(MotionEvent event) {
        int index = event.getActionIndex();
        int id = event.getPointerId(index);
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                fingerDown(id, event.getX(index), event.getY(index));
                break;
            case MotionEvent.ACTION_MOVE:
                for (int h = 0; h < event.getHistorySize(); h++)
                    for (int i = 0; i < event.getPointerCount(); i++)
                        fingerMoved(event.getPointerId(i), event.getHistoricalX(i, h), event.getHistoricalY(i, h));
                for (int i = 0; i < event.getPointerCount(); i++)
                    fingerMoved(event.getPointerId(i), event.getX(i), event.getY(i));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                // The lift can contain the last displacement even without a preceding MOVE.
                if (id == lookPointer)
                    fingerMoved(id, event.getX(index), event.getY(index));
                fingerUp(id, event.getX(index), event.getY(index));
                break;
            case MotionEvent.ACTION_CANCEL:
                reset();
                break;
            default:
                break;
        }
    }

    /** a finger down in a game, at a point in pixels */
    private void fingerDown(int id, float px, float py) {
        if (inGestureZone(px, py, true))
            return;
        float x = logical(px);
        float y = logical(py);
        int target = hit(x, y);
        int stick = TouchLayout.LEFT;
        if (target == NOTHING && layout.floatingStick && !hidden && layout.shown(stick) && !held(stick)
                && inFloatingZone(x, y) && !inGestureZone(px, py, false)) {
            target = stick;
        }
        if (target == TOOLBAR_OPTIONS || target == TOOLBAR_TOGGLE
                || (target >= 0 && (layout.type(target) == 10 || layout.type(target) == 11))) {
            toolbarFingers.put(id, new float[] {px, py, target});
        } else if (target != NOTHING) {
            owners.put(id, target);
            if (layout.type(target) == TouchLayout.LEFT) {
                cancelAutoWalk();
                // Every floating-stick press starts neutral, including inside its resting circle.
                if (layout.floatingStick)
                    floatStick(x, y);
                moveStick(target, x, y);
            } else
                buttonTouches.put(id, new float[] {px, py});
        } else if (!hidden && !inGestureZone(px, py, false)) {
            // A dedicated aiming finger takes over from dragging a held Fire button.
            buttonTouches.put(id, new float[] {px, py});
            startLook(id, px, py);
        }
    }

    private void startLook(int id, float px, float py) {
        if (lookPointer >= 0 && lookPointer != id) {
            float[] previous = buttonTouches.get(lookPointer);
            if (previous != null) {
                previous[0] = lookX;
                previous[1] = lookY;
            }
        }
        lookPointer = id;
        lookX = px;
        lookY = py;
        if (owners.get(id, NOTHING) == NOTHING)
            owners.put(id, LOOK);
    }

    private boolean movedPastSlop(float[] origin, float px, float py) {
        return Math.hypot(px - origin[0], py - origin[1]) > SLOP * scale;
    }

    private void fingerMoved(int id, float px, float py) {
        int control = owners.get(id, NOTHING);
        if (control >= 0 && layout.type(control) == TouchLayout.LEFT)
            moveStick(control, logical(px), logical(py));
        float[] toolbar = toolbarFingers.get(id);
        if (toolbar != null && movedPastSlop(toolbar, px, py)) {
            // a swipe that began on the toolbar aims; the item does not act
            toolbarFingers.remove(id);
            if (!hidden && lookPointer < 0)
                startLook(id, toolbar[0], toolbar[1]);
        }
        float[] origin = buttonTouches.get(id);
        if (origin != null && lookPointer >= 0 && lookPointer != id) {
            // A waiting finger takes over from its latest position, without a camera jump.
            origin[0] = px;
            origin[1] = py;
        }
        if (lookPointer < 0 && origin != null && movedPastSlop(origin, px, py)) {
            // a held button (either Fire too) also aims
            lookPointer = id;
            lookX = origin[0];
            lookY = origin[1];
        }
        if (id == lookPointer) {
            float dx = logical(px - lookX) * sensitivity * horizontalSensitivity;
            float dy = logical(py - lookY) * sensitivity;
            if (invertSwipeX) dx = -dx;
            if (invertSwipeY) dy = -dy;
            if (dx != 0 || dy != 0)
                nativeLook(dx, dy);
            lookX = px;
            lookY = py;
        }
    }

    private void fingerUp(int id, float px, float py) {
        float[] toolbar = toolbarFingers.get(id);
        if (toolbar != null) {
            toolbarFingers.remove(id);
            int item = (int) toolbar[2];
            if (hit(logical(px), logical(py)) == item)
                toolbarAction(item);
            return;
        }
        int control = owners.get(id, NOTHING);
        int releasedType = control >= 0 ? layout.type(control) : NOTHING;
        if (control >= 0 && layout.type(control) == TouchLayout.LEFT) {
            moveStick(control, logical(px), logical(py));
            releaseHandler.removeCallbacks(lockAutoWalk);
            autoWalkSince = -1;
            if (!autoWalkLocked) {
                axes[0] = 0;
                axes[1] = 0;
                stickFloating = false;
            }
        }
        boolean wasLooking = id == lookPointer;
        owners.delete(id);
        buttonTouches.remove(id);
        if (releasedType >= 0 && isGrenade(KINDS[releasedType])) {
            cancelGrenadePulse();
            grenadePulseType = releasedType;
            releaseHandler.postDelayed(clearGrenadePulse, GRENADE_PULSE_MS);
        }
        if (wasLooking) {
            lookPointer = -1;
            for (int i = owners.size() - 1; i >= 0; i--) {
                if (owners.valueAt(i) == LOOK) {
                    int next = owners.keyAt(i);
                    float[] position = buttonTouches.get(next);
                    if (position != null) {
                        startLook(next, position[0], position[1]);
                        break;
                    }
                }
            }
        }
    }

    private void toolbarAction(int item) {
        if (item == TOOLBAR_OPTIONS)
            showOptions();
        else if (item == TOOLBAR_TOGGLE)
            setHidden(!hidden);
        else if (item >= 0 && item < layout.size()
                && (layout.type(item) == 10 || layout.type(item) == 11)) {
            reset();
            ((HaloActivity) getContext()).showTouchPauseMenu();
        }
        performClick();
    }

    /** a finger in the layout editor: drags controls; Save, Export and Import act at once */
    private void editTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        int id = event.getPointerId(index);
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            if (dragPointer >= 0)
                return;
            float x = logical(event.getX(index));
            float y = logical(event.getY(index));
            int target = hit(x, y);
            if (target >= 0) {
                selectedControl = target;
                refreshEditor(false);
                dragPointer = id;
                dragControl = target;
                dragOffsetX = x - layout.x(target);
                dragOffsetY = y - layout.y(target);
            }
        } else if (action == MotionEvent.ACTION_MOVE && dragPointer >= 0) {
            int pointer = event.findPointerIndex(dragPointer);
            if (pointer >= 0) {
                layout.move(dragControl, logical(event.getX(pointer)) - dragOffsetX,
                    logical(event.getY(pointer)) - dragOffsetY);
                editorPanel.setDirty(true);
            }
        } else if (action == MotionEvent.ACTION_CANCEL
                || ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) && id == dragPointer)) {
            dragPointer = -1;
            dragControl = -1;
        }
    }

    // ---------- the buttons' names, from the player's profile

    private void readBindings() {
        int serial = nativeBindings(bindingsRead);
        if (serial == 0 || serial == bindingsSerial)
            return;
        bindingsSerial = serial;
        if (!Arrays.equals(bindingsRead, bindings)) {
            System.arraycopy(bindingsRead, 0, bindings, 0, bindings.length);
            if (grenadePulseType >= 0) {
                cancelGrenadePulse();
                publish();
            }
            invalidate();
        }
    }

    /** a control's name: its button and what the profile has it do ("A / Jump", "Fire", "RT") */
    private String label(int type) {
        Kind kind = KINDS[type];
        if (kind.gamepadButton < 0)
            return kind.buttonName;
        int control = bindings[kind.gamepadButton];
        if (control < 0 || control >= GAME_CONTROLS.length)
            return kind.buttonName;
        return kind.faceButton ? kind.buttonName + " / " + GAME_CONTROLS[control] : GAME_CONTROLS[control];
    }

    private String controlName(int type) {
        return type == TouchLayout.LEFT ? "Move stick" : label(type);
    }

    // ---------- the layout, saved and in files

    private boolean saveLayout() {
        String configuration = exportLayout();
        boolean saved = preferences.edit().putString(KEY_CONFIGURATION, configuration)
            .putString("hud-layout-" + activeHud, configuration)
            .putBoolean(KEY_INVERT_SWIPE_X, invertSwipeX).putBoolean(KEY_INVERT_SWIPE_Y, invertSwipeY).commit();
        if (!saved)
            Toast.makeText(getContext(), "The touch layout could not be saved. Try again.", Toast.LENGTH_LONG).show();
        return saved;
    }

    public String exportLayout() {
        return layout.exportConfiguration(sensitivity, horizontalSensitivity);
    }

    /** replaces the layout with a layout file's (saved first: it throws if the file is not valid) */
    public void importLayout(String text) {
        TouchLayout.Configuration imported = TouchLayout.importConfiguration(text);
        imported.layout.bounds(logicalWidth, logicalHeight);
        imported.layout.setMinimumRadius(minimumRadius());
        String normalized = imported.layout.exportConfiguration(imported.sensitivity, imported.horizontalSensitivity);
        if (!preferences.edit().putString(KEY_CONFIGURATION, normalized)
                .putString("hud-layout-" + activeHud, normalized).commit())
            throw new IllegalArgumentException("Could not save the imported layout");
        reset();
        layout = imported.layout;
        updateSafeBounds();
        sensitivity = imported.sensitivity;
        horizontalSensitivity = imported.horizontalSensitivity;
        setHidden(false);
        cancelRumble();
        invalidate();
    }

    // ---------- the options

    /** opens an option dialog; the controls are released while any is open, and saved when the last closes */
    private void openDialog(AlertDialog dialog) {
        dialog.setOnDismissListener(d -> {
            openDialogs--;
            if (openDialogs > 0)
                return;
            openDialogs = 0;
            if (!editing)
                saveLayout();
            reset();
            updateSensors();
        });
        openDialogs++;
        reset();
        dialog.show();
        GameMenuStyle.style(dialog);
    }

    void showOptions() {
        String[] items = {"Controles predefinidos", "Perfiles gráficos", "Pantalla y resolución", "General",
            "HUD personalizado", "Sensibilidad al mirar", "Exportar HUD", "Importar HUD",
            "Descargar paquete de datos", "Actualizar APK", "Configuración interna de Halo"};
        String[] details = {"Dos pulgares, tres dedos u original", "Rendimiento y calidad de imagen",
            "Formato de pantalla y nitidez", "Vibración, movimiento y botones",
            "Tamaño, opacidad y visibilidad", "Velocidad y precisión de cámara",
            "Guardar tu distribución en un archivo", "Cargar una distribución guardada",
            "Instalar datos desde un enlace", "Comprobar si hay una versión nueva",
            "Menú del juego: Settings, perfil y cámara"};
        AlertDialog dialog = GameMenuStyle.builder(getContext())
            .setTitle("Halo · Controles")
            .setAdapter(new SettingsMenuAdapter(getContext(), items, details), (d, which) -> {
                if (which == 0) {
                    post(this::showPresets);
                } else if (which == 1) {
                    post(() -> GraphicsProfiles.show((HaloActivity) getContext(),
                        () -> ((HaloActivity) getContext()).restartForGraphicsProfile(), this::openDialog));
                } else if (which == 2) {
                    post(() -> ScreenSettings.show((HaloActivity) getContext(),
                        () -> ((HaloActivity) getContext()).restartForGraphicsProfile(), this::openDialog));
                } else if (which == 3) {
                    post(this::showGeneral);
                } else if (which == 4) {
                    post(this::beginEditor);
                } else if (which == 5) {
                    post(this::showSensitivity);
                } else if (which == 6 || which == 7) {
                    ((HaloActivity) getContext()).chooseLayoutFile(which == 6, exportLayout());
                } else if (which == 8) {
                    post(() -> DataPackages.show((HaloActivity) getContext(), () -> {}, this::openDialog));
                } else if (which == 9) {
                    post(() -> ControlsUpdater.show((HaloActivity) getContext(), this::openDialog));
                } else if (which == 10) {
                    ((HaloActivity) getContext()).openOriginalGameMenu();
                }
            })
            .setNegativeButton("Cerrar", null)
            .create();
        openDialog(dialog);
    }

    private void beginEditor() {
        layoutBeforeEditing = exportLayout();
        hudBeforeEditing = activeHud;
        String[] defaults = {TouchPresets.twoThumbs(), TouchPresets.threeFingers(), TouchPresets.original()};
        for (int i = 0; i < hudDrafts.length; i++)
            hudDrafts[i] = preferences.getString("hud-layout-" + i, defaults[i]);
        hudDrafts[activeHud] = layoutBeforeEditing;
        editing = true;
        hiddenBeforeEditing = hidden;
        hidden = false;
        reset();
        selectedControl = 4;
        editorPanel = new HudEditorPanel(getContext(), new HudEditorPanel.Listener() {
            @Override public void onExit() { cancelEditor(); }
            @Override public void onSave() { saveEditor(); }
            @Override public void onReset() {
                applyEditorLayout(defaults[activeHud]);
                refreshEditor(true);
            }
            @Override public void onSelectHud(int slot) {
                hudDrafts[activeHud] = exportLayout();
                activeHud = slot;
                applyEditorLayout(hudDrafts[activeHud]);
                refreshEditor(true);
            }
            @Override public void onSize(float size) {
                layout.setSize(selectedControl, size);
                refreshEditor(true);
            }
            @Override public void onOpacity(float opacity) {
                layout.setOpacity(selectedControl, opacity);
                refreshEditor(true);
            }
            @Override public void onVisibility(boolean visible) {
                layout.setShown(selectedControl, visible);
                refreshEditor(true);
            }
            @Override public void onNudge(float x, float y) {
                layout.move(selectedControl, layout.x(selectedControl) + x, layout.y(selectedControl) + y);
                refreshEditor(true);
            }
        });
        ((ViewGroup) getParent()).addView(editorPanel, new ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        editorPanel.setVisibility(View.VISIBLE);
        ((HaloActivity) getContext()).setHudEditorPaused(true);
        refreshEditor(false);
    }

    private void applyEditorLayout(String configuration) {
        TouchLayout.Configuration saved = TouchLayout.importConfiguration(TouchPresets.upgradeMobileLayout(configuration));
        reset();
        layout = saved.layout;
        sensitivity = saved.sensitivity;
        horizontalSensitivity = saved.horizontalSensitivity;
        layout.bounds(logicalWidth, logicalHeight);
        layout.setMinimumRadius(minimumRadius());
        updateSafeBounds();
        selectedControl = Math.min(selectedControl, layout.size() - 1);
    }

    private void refreshEditor(boolean dirty) {
        String[] actions = {"Saltar", "Tipo de granada", "Usar / entrar; mantener: recoger", "Cambiar arma equipada",
            "Golpear", "Linterna", "Granada", "Disparar", "Pausa", "Atrás", "Agacharse", "Mira"};
        int type = layout.type(selectedControl);
        int action = KINDS[type].gamepadButton >= 0 ? bindings[KINDS[type].gamepadButton] : -1;
        String name = type == TouchLayout.LEFT ? "Movimiento"
            : action >= 0 && action < actions.length ? actions[action] : label(type);
        if (action == 2 || action == 3)
            name = KINDS[type].buttonName + " · " + name;
        editorPanel.bind(activeHud, name, layout.sizeScale(selectedControl), layout.opacity(selectedControl),
            layout.shown(selectedControl));
        if (dirty)
            editorPanel.setDirty(true);
        invalidate();
    }

    private void saveEditor() {
        hudDrafts[activeHud] = exportLayout();
        SharedPreferences.Editor changes = preferences.edit().putInt("hud-slot", activeHud)
            .putString(KEY_CONFIGURATION, hudDrafts[activeHud]).putBoolean(KEY_HIDDEN, false);
        for (int i = 0; i < hudDrafts.length; i++)
            changes.putString("hud-layout-" + i, hudDrafts[i]);
        if (!changes.commit()) {
            toast("No se pudo guardar el HUD. Inténtalo de nuevo.");
            return;
        }
        closeEditor();
        toast("HUD " + (activeHud + 1) + " guardado");
    }

    public boolean cancelEditor() {
        if (!editing)
            return false;
        activeHud = hudBeforeEditing;
        applyEditorLayout(layoutBeforeEditing);
        hidden = hiddenBeforeEditing;
        closeEditor();
        return true;
    }

    public boolean isEditing() {
        return editing;
    }

    private void closeEditor() {
        ((ViewGroup) getParent()).removeView(editorPanel);
        editorPanel = null;
        editing = false;
        reset();
        ((HaloActivity) getContext()).setHudEditorPaused(false);
        updateSensors();
    }

    private void showPresets() {
        String previous = preferences.getString("previous-layout", null);
        String[] names = previous == null
            ? new String[] {"Dos pulgares · estilo Free Fire", "Tres dedos · disparo izquierdo", "Original Ports 106"}
            : new String[] {"Dos pulgares · estilo Free Fire", "Tres dedos · disparo izquierdo", "Original Ports 106",
                "Restaurar diseño anterior"};
        openDialog(GameMenuStyle.builder(getContext())
            .setTitle("Controles predefinidos")
            .setItems(names, (dialog, which) -> {
                String next = which == 0 ? TouchPresets.twoThumbs()
                    : which == 1 ? TouchPresets.threeFingers()
                    : which == 2 ? TouchPresets.original() : previous;
                String current = exportLayout();
                try {
                    importLayout(next);
                    preferences.edit().putString("previous-layout", current).apply();
                    Toast.makeText(getContext(), "Diseño guardado. Arrastra el disparo para apuntar.",
                        Toast.LENGTH_LONG).show();
                } catch (IllegalArgumentException e) {
                    Toast.makeText(getContext(), e.getMessage(), Toast.LENGTH_LONG).show();
                }
            })
            .setNegativeButton("Cancelar", null).create());
    }

    private TextView text(String value) {
        TextView view = new TextView(getContext());
        view.setText(value);
        return view;
    }

    private Button button(String label, View.OnClickListener listener) {
        Button button = new Button(getContext());
        button.setText(label);
        button.setOnClickListener(listener);
        return button;
    }

    private void showGeneral() {
        cancelRumble();
        LinearLayout panel = new LinearLayout(getContext());
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(24, 16, 24, 16);

        boolean hasRumble = vibrator != null && vibrator.hasVibrator();
        Switch rumble = new Switch(getContext());
        rumble.setText(hasRumble ? "Vibration" : "Vibration (not on this phone)");
        rumble.setChecked(layout.rumbleEnabled);
        rumble.setEnabled(hasRumble);
        rumble.setOnCheckedChangeListener((view, enabled) -> {
            layout.rumbleEnabled = enabled;
            cancelRumble();
            saveLayout();
        });
        panel.addView(rumble);

        // (gyroscope aiming is the game's: a profile's Gamepads, Gyro Aim)
        panel.addView(text(gyroscope != null ?
            "Gyroscope aiming: in the game's settings, a profile's Gamepads, Gyro Aim" :
            "Gyroscope aiming: not on this phone"));

        Switch floating = new Switch(getContext());
        floating.setText("Floating move stick: it goes where your thumb lands in the lower left");
        floating.setChecked(layout.floatingStick);
        floating.setOnCheckedChangeListener((view, enabled) -> {
            layout.floatingStick = enabled;
            saveLayout();
        });
        panel.addView(floating);

        Switch leftFire = new Switch(getContext());
        leftFire.setText("Disparo izquierdo");
        leftFire.setChecked(layout.shown(TouchLayout.FIRE_LEFT));
        leftFire.setOnCheckedChangeListener((view, enabled) -> {
            reset();
            layout.setShown(TouchLayout.FIRE_LEFT, enabled);
            saveLayout();
            invalidate();
        });
        panel.addView(leftFire);

        Switch autoWalk = new Switch(getContext());
        autoWalk.setText("Avance automático");
        autoWalk.setChecked(autoWalkEnabled);
        autoWalk.setOnCheckedChangeListener((view, enabled) -> {
            autoWalkEnabled = enabled;
            reset();
            preferences.edit().putBoolean(KEY_AUTO_WALK, enabled).commit();
        });
        panel.addView(autoWalk);
        panel.addView(text("Mantén el joystick arriba para fijar el avance. Tócalo de nuevo para detenerte."));

        TextView opacityLabel = text("");
        SeekBar opacity = new SeekBar(getContext());
        int steps = Math.round((TouchLayout.MAX_OPACITY - TouchLayout.MIN_OPACITY) * 20);
        opacity.setMax(steps);
        opacity.setProgress(Math.round((layout.opacity - TouchLayout.MIN_OPACITY) * 20));
        Runnable showOpacity = () -> opacityLabel.setText(String.format(Locale.US, "Opacity: %d%%",
            Math.round(layout.opacity * 100)));
        showOpacity.run();
        opacity.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int progress, boolean user) {
                float value = Math.min(TouchLayout.MAX_OPACITY, TouchLayout.MIN_OPACITY + progress / 20f);
                layout.setOpacity(value);
                for (int i = 0; i < layout.size(); i++)
                    layout.setOpacity(i, value);
                showOpacity.run();
                invalidate();
            }

            @Override
            public void onStartTrackingTouch(SeekBar bar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar bar) {
                saveLayout();
            }
        });
        panel.addView(opacityLabel);
        panel.addView(opacity);

        ScrollView scroll = new ScrollView(getContext());
        scroll.addView(panel);
        AlertDialog dialog = GameMenuStyle.builder(getContext())
            .setTitle("General")
            .setView(scroll)
            .setPositiveButton("Done", null)
            .create();
        panel.addView(button("Hide or add buttons", view -> {
            dialog.dismiss();
            post(this::showButtonManager);
        }));
        panel.addView(button("Edit buttons size", view -> {
            dialog.dismiss();
            post(this::showButtonSizes);
        }));
        openDialog(dialog);
    }

    private String rowName(int control, boolean withHidden) {
        String name = controlName(layout.type(control));
        if (control >= TouchLayout.BASE_COUNT)
            name += " (copy)";
        if (withHidden && !layout.shown(control))
            name += " (hidden)";
        return name;
    }

    private LinearLayout row(int control, boolean withHidden) {
        LinearLayout row = new LinearLayout(getContext());
        row.setPadding(16, 4, 16, 4);
        TextView name = text(rowName(control, withHidden));
        name.setTextColor(Color.WHITE);
        row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        return row;
    }

    private void showButtonSizes() {
        LinearLayout list = new LinearLayout(getContext());
        list.setOrientation(LinearLayout.VERTICAL);
        for (int i = 0; i < layout.size(); i++) {
            final int control = i;
            LinearLayout row = row(control, true);
            TextView value = text("");
            value.setTextColor(Color.WHITE);
            Button smaller = new Button(getContext());
            Button larger = new Button(getContext());
            Runnable refresh = () -> {
                int percent = Math.round(layout.sizeScale(control) * 100);
                value.setText(percent + "%");
                smaller.setEnabled(percent > Math.round(TouchLayout.MIN_SIZE * 100));
                larger.setEnabled(percent < Math.round(TouchLayout.MAX_SIZE * 100));
            };
            smaller.setText("-");
            smaller.setOnClickListener(view -> resize(control, -10, refresh));
            larger.setText("+");
            larger.setOnClickListener(view -> resize(control, 10, refresh));
            row.addView(smaller);
            row.addView(value);
            row.addView(larger);
            list.addView(row);
            refresh.run();
        }
        ScrollView scroll = new ScrollView(getContext());
        scroll.addView(list);
        openDialog(GameMenuStyle.builder(getContext())
            .setTitle("Edit buttons size")
            .setView(scroll)
            .setPositiveButton("Done", null)
            .create());
    }

    private void resize(int control, int percent, Runnable refresh) {
        int size = Math.round(layout.sizeScale(control) * 100) + percent;
        size = Math.max(Math.round(TouchLayout.MIN_SIZE * 100), Math.min(Math.round(TouchLayout.MAX_SIZE * 100), size));
        layout.setSize(control, size / 100f);
        saveLayout();
        refresh.run();
        invalidate();
    }

    private void showButtonManager() {
        LinearLayout list = new LinearLayout(getContext());
        list.setOrientation(LinearLayout.VERTICAL);
        Runnable[] refresh = new Runnable[1];
        refresh[0] = () -> {
            list.removeAllViews();
            for (int i = 0; i < layout.size(); i++) {
                final int control = i;
                LinearLayout row = row(control, false);
                row.addView(button(layout.shown(control) ? "Hide" : "Show", view -> {
                    reset();
                    layout.setShown(control, !layout.shown(control));
                    saveLayout();
                    refresh[0].run();
                    invalidate();
                }));
                if (layout.type(control) != TouchLayout.LEFT) {
                    row.addView(button("Duplicate", view -> {
                        reset();
                        if (layout.duplicate(control) < 0)
                            toast("At most " + TouchLayout.MAX_CONTROLS + " controls. Reset removes the copies.");
                        else
                            saveLayout();
                        refresh[0].run();
                        invalidate();
                    }));
                }
                list.addView(row);
            }
        };
        refresh[0].run();
        ScrollView scroll = new ScrollView(getContext());
        scroll.addView(list);
        AlertDialog dialog = GameMenuStyle.builder(getContext())
            .setTitle("Hide or add buttons")
            .setView(scroll)
            .setPositiveButton("Done", null)
            .setNegativeButton("Reset", null)
            .setNeutralButton("Add button", null)
            .create();
        openDialog(dialog);
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(view -> confirmReset(refresh[0]));
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(view -> chooseButtonToAdd(refresh[0]));
    }

    private void confirmReset(Runnable refresh) {
        GameMenuStyle.builder(getContext())
            .setTitle("Reset the buttons?")
            .setMessage("This puts every button back in its place and size, shows them all and removes the copies.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Reset", (d, which) -> {
                reset();
                layout.resetDefaults();
                setHidden(false);
                saveLayout();
                refresh.run();
                invalidate();
            })
            .show();
    }

    private void chooseButtonToAdd(Runnable refresh) {
        String[] names = new String[TouchLayout.BASE_COUNT];
        for (int i = 0; i < names.length; i++)
            names[i] = controlName(i);
        GameMenuStyle.builder(getContext())
            .setTitle("Add button")
            .setItems(names, (d, type) -> {
                reset();
                if (layout.add(type) < 0)
                    toast(type == TouchLayout.LEFT ? "The move stick already shows." : "At most " + TouchLayout.MAX_CONTROLS + " controls.");
                else
                    saveLayout();
                refresh.run();
                invalidate();
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void showSensitivity() {
        LookSensitivityPanel panel = new LookSensitivityPanel(getContext(), sensitivity, horizontalSensitivity, logical(1));
        panel.setInvertedAxes(invertSwipeX, invertSwipeY);
        AlertDialog dialog = GameMenuStyle.builder(getContext())
            .setTitle("Sensibilidad al mirar")
            .setView(panel)
            .setNegativeButton("Cancelar", null)
            .setNeutralButton("Restablecer", null)
            .setPositiveButton("Guardar", null)
            .create();
        dialog.setOnShowListener(d -> {
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(view -> {
                panel.setValue(1f);
                panel.setHorizontalValue(TouchLayout.DEFAULT_HORIZONTAL_SENSITIVITY);
                panel.setInvertedAxes(false, false);
            });
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
                float previous = sensitivity;
                float previousHorizontal = horizontalSensitivity;
                boolean previousX = invertSwipeX, previousY = invertSwipeY;
                sensitivity = panel.value();
                horizontalSensitivity = panel.horizontalValue();
                invertSwipeX = panel.invertedHorizontal();
                invertSwipeY = panel.invertedVertical();
                if (saveLayout()) {
                    dialog.dismiss();
                } else {
                    invertSwipeX = previousX;
                    invertSwipeY = previousY;
                    sensitivity = previous;
                    horizontalSensitivity = previousHorizontal;
                }
            });
        });
        openDialog(dialog);
    }

    private void toast(String message) {
        Toast.makeText(getContext(), message, Toast.LENGTH_LONG).show();
    }

    // ---------- the gyroscope and the vibration

    /** the game's Gyro Aim (input.gyro_aim): on, or only on zoom, which the game itself applies */
    private boolean gyroscopeWanted() {
        return gyroMode != 0;
    }

    private void updateSensors() {
        boolean needed = deviceInputActive && showing && !hidden && gyroscopeWanted() && gyroscope != null;
        if (needed && !gyroRegistered) {
            gyroAim.reset();
            gyroRegistered = sensors.registerListener(this, gyroscope, SensorManager.SENSOR_DELAY_GAME);
        } else if (!needed && gyroRegistered) {
            sensors.unregisterListener(this);
            gyroRegistered = false;
            gyroAim.reset();
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (!deviceInputActive || !showing || hidden || !gyroscopeWanted() || editing || openDialogs > 0) {
            gyroAim.reset();
            return;
        }
        int rotation = ((WindowManager) getContext().getSystemService(Context.WINDOW_SERVICE))
            .getDefaultDisplay().getRotation();
        if (gyroAim.sample(event.timestamp, event.values[0], event.values[1], rotation, gyroDelta)) {
            // as a swipe of this many logical units, the phone's own turn (the game applies
            // Gyro Aim's sensitivity and invert; the profile's invert leaves it alone)
            nativeGyro(-gyroDelta[0] / LOOK_RADIANS_PER_UNIT, -gyroDelta[1] / LOOK_RADIANS_PER_UNIT);
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }

    /** the phone vibrates as port 0's controller would, while the controls show */
    private void updateRumble() {
        if (vibrator == null || !vibrator.hasVibrator())
            return;
        boolean active = showing && !hidden && layout.rumbleEnabled && !editing && openDialogs == 0;
        int amplitude = active ? nativeRumble() : 0;
        if (amplitude == 0) {
            cancelRumble();
            return;
        }
        long now = SystemClock.uptimeMillis();
        if (amplitude == lastAmplitude && now - lastVibration < VIBRATION_RENEW_MS)
            return;
        try {
            int strength = vibrator.hasAmplitudeControl() ? amplitude : VibrationEffect.DEFAULT_AMPLITUDE;
            vibrator.vibrate(VibrationEffect.createOneShot(VIBRATION_MS, strength), rumbleAttributes);
            lastAmplitude = amplitude;
            lastVibration = now;
        } catch (RuntimeException e) {
            cancelRumble();
        }
    }

    private void cancelRumble() {
        if (lastAmplitude != 0 && vibrator != null) {
            try {
                vibrator.cancel();
            } catch (RuntimeException ignored) {
                // the vibrator may already be gone with the activity
            }
        }
        lastAmplitude = 0;
    }

    // ---------- drawing

    private static int alpha(int color, float opacity) {
        int alpha = Math.round(Color.alpha(color) * opacity);
        return (color & 0x00ffffff) | (Math.max(0, Math.min(255, alpha)) << 24);
    }

    private void circle(Canvas canvas, float x, float y, float radius, String label, boolean active,
            float textSize, float opacity) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(alpha(active ? 0x9983d9ff : 0x55304050, opacity));
        canvas.drawCircle(x, y, radius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(2);
        paint.setColor(alpha(active ? 0xffaee7ff : 0xffffffff, opacity));
        canvas.drawCircle(x, y, radius, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(alpha(Color.WHITE, opacity));
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(textSize);
        // (a long name, "Y / Weapon", is made smaller to fit inside the circle)
        float width = paint.measureText(label);
        if (width > radius * LABEL_WIDTH) {
            textSize *= radius * LABEL_WIDTH / width;
            paint.setTextSize(textSize);
        }
        canvas.drawText(label, x, y + textSize * 0.36f, paint);
    }

    /** a toolbar item: never fainter than half, so that it can always be found */
    private void toolbarItem(Canvas canvas, float x, float radius, String label) {
        circle(canvas, x, toolbarY(), toolbarRadius(radius), label, false, 11, Math.max(layout.opacity, 0.5f));
    }

    private void drawToolbar(Canvas canvas) {
        if (editing)
            return;
        toolbarItem(canvas, toggleX(), TOOLBAR_RADIUS, hidden ? "Mostrar" : "Ocultar");
        toolbarItem(canvas, optionsX(), OPTIONS_RADIUS, "Ajustes");
    }

    private void drawStick(Canvas canvas) {
        int stick = TouchLayout.LEFT;
        float radius = layout.radius(stick);
        float size = layout.sizeScale(stick);
        // (a floating stick near an edge is drawn whole, nearer the middle than its centre)
        float x = layout.constrainX(stickCenterX(), radius);
        float y = layout.constrainY(stickCenterY(), radius);
        float opacity = editing && !layout.shown(stick) ? 0.15f : layout.opacity(stick);
        circle(canvas, x, y, radius, "", false, 11 * size, opacity);
        circle(canvas, x + axes[0] / 32767f * radius, y + axes[1] / 32767f * radius,
            Math.max(STICK_KNOB_RADIUS * size, radius * 0.375f), "", held(stick) || autoWalkLocked, 11, opacity);
        if (!editing && (autoWalkLocked || autoWalkSince >= 0)) {
            float badgeY = layout.constrainY(y - radius - 40, 20);
            circle(canvas, x, badgeY, 18, "", autoWalkLocked, 11, Math.max(opacity, .8f));
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2);
            if (autoWalkLocked) {
                canvas.drawRoundRect(x - 7, badgeY - 3, x + 7, badgeY + 8, 2, 2, paint);
                canvas.drawArc(x - 5, badgeY - 11, x + 5, badgeY + 1, 180, 180, false, paint);
            } else {
                float progress = Math.min(1, (SystemClock.uptimeMillis() - autoWalkSince) / (float) AUTO_WALK_HOLD_MS);
                paint.setColor(0xffaee7ff);
                canvas.drawArc(x - 20, badgeY - 20, x + 20, badgeY + 20, -90, progress * 360, false, paint);
                postInvalidateDelayed(33);
            }
            paint.setStyle(Paint.Style.FILL);
            paint.setTextSize(11);
            canvas.drawText(autoWalkLocked ? "AVANCE FIJO" : "MANTÉN ↑", x, badgeY + 33, paint);
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        canvas.save();
        canvas.scale(scale, scale);
        if (!showing && !editing) {
            toolbarItem(canvas, optionsX(), OPTIONS_RADIUS, "Ajustes");
            canvas.restore();
            return;
        }
        if (editing)
            canvas.drawColor(0x66000000);
        if (!hidden || editing) {
            if (editing || layout.shown(TouchLayout.LEFT))
                drawStick(canvas);
            for (int i = 0; i < layout.size(); i++) {
                if ((!editing && !layout.shown(i)) || layout.type(i) == TouchLayout.LEFT)
                    continue;
                float textSize = 11 * Math.max(layout.sizeScale(i), layout.radius(i) / 36f);
                int type = layout.type(i);
                int action = KINDS[type].gamepadButton >= 0 ? bindings[KINDS[type].gamepadButton] : -1;
                float opacity = editing && !layout.shown(i) ? 0.15f : layout.opacity(i);
                circle(canvas, layout.x(i), layout.y(i), layout.radius(i), action < 0 ? label(type) : "",
                    held(i) || dragControl == i, Math.min(textSize, 16), opacity);
                if (action == 0 || action == 2 || action == 3 || action == 4)
                    icons.drawCaption(canvas, paint, layout.x(i), layout.y(i), layout.radius(i),
                        KINDS[type].buttonName, action == 0 ? "SALTO" : action == 4 ? "GOLPE"
                            : action == 2 ? "USAR" : "ARMA");
                else if (action >= 0) {
                    icons.draw(canvas, paint, layout.x(i), layout.y(i), layout.radius(i), action);
                    icons.drawLabel(canvas, paint, layout.x(i), layout.y(i), layout.radius(i), action);
                }
            }
            if (editing) {
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(2);
                paint.setColor(0xffffc400);
                float radius = layout.radius(selectedControl) + 6;
                canvas.drawRect(layout.x(selectedControl) - radius, layout.y(selectedControl) - radius,
                    layout.x(selectedControl) + radius, layout.y(selectedControl) + radius, paint);
                paint.setStyle(Paint.Style.FILL);
            }
        }
        drawToolbar(canvas);
        canvas.restore();
    }
}
