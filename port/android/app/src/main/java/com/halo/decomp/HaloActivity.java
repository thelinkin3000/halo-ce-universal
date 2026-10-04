package com.halo.decomp;

import android.annotation.TargetApi;
import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL3's activity, running libmain.so (port/android/host), which
 * loads the game image from the APK's assets.
 */
public class HaloActivity extends SDLActivity {
    /** lets system link's broadcasts in over Wi-Fi while the game runs */
    private WifiManager.MulticastLock multicastLock;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars();
        preferHighestRefreshRate();
        acquireMulticastLock();
        // a new version looked for while the game starts
        Updater.start(this);
    }

    @Override
    protected void onDestroy() {
        if (multicastLock != null && multicastLock.isHeld())
            multicastLock.release();
        multicastLock = null;
        super.onDestroy();
    }

    /**
     * Many phones drop the Wi-Fi's broadcast and multicast datagrams to
     * save power unless an app holds this: without it they would not see
     * system link games on the local network, nor be seen hosting one.
     */
    private void acquireMulticastLock() {
        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifi == null)
                return;
            multicastLock = wifi.createMulticastLock("halo-system-link");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
        } catch (RuntimeException e) {
            // (no Wi-Fi, or not allowed: the local network may miss games)
            multicastLock = null;
        }
    }

    /**
     * Keeps the status and navigation bars out of the way. The manifest's
     * fullscreen theme does this on older releases, but Android 15 and later
     * draw apps edge to edge regardless, so the bars have to be asked away.
     * Swiping brings them back transiently and they leave again on their own.
     *
     * The two ways of asking are split, because {@code WindowInsetsController}
     * arrived in Android 11: on 9 and 10 the call is a hidden one, which the
     * system refuses (and logs), and it is not the older call that hides the
     * bars there.
     */
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            hideSystemBarsWithInsets();
        else
            hideSystemBarsLegacy();
    }

    /** Android 11 and later */
    @TargetApi(Build.VERSION_CODES.R)
    private void hideSystemBarsWithInsets() {
        WindowInsetsController controller = getWindow().getInsetsController();

        if (controller == null) {
            hideSystemBarsLegacy();
            return;
        }
        controller.hide(WindowInsets.Type.systemBars());
        controller.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    /** Android 9 and 10, and anywhere the above has no controller */
    private void hideSystemBarsLegacy() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        // they come back after a swipe or after the launcher was over them
        if (hasFocus)
            hideSystemBars();
    }

    /**
     * The game draws a frame at every display refresh, between its 30 Hz
     * ticks (port/linux/game/render_interpolation.c); Android otherwise
     * often keeps an app at 60 Hz on a faster display.
     */
    private void preferHighestRefreshRate() {
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode();
        Display.Mode best = current;

        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth() &&
                mode.getPhysicalHeight() == current.getPhysicalHeight() &&
                mode.getRefreshRate() > best.getRefreshRate()) {
                best = mode;
            }
        }
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(attributes);
    }
}
