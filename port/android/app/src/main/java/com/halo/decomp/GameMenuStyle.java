package com.halo.decomp;

import android.app.AlertDialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.ContextThemeWrapper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckedTextView;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.ListView;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.ThemedSpinnerAdapter;

import java.util.WeakHashMap;

/** Shared dialog presentation. Widget state, listeners and game rendering stay with their owners. */
final class GameMenuStyle {
    private static final int WHITE = 0xffedf5fb, MUTED = 0xffa4b9c8, ICE = 0xffa0e1f5;
    private static final int INK = 0xff080e16, SURFACE = 0xff152633, DISABLED = 0xff627b8c;
    private static final ColorStateList TEXT = enabled(WHITE, DISABLED);
    private static final ColorStateList ACCENT = enabled(ICE, DISABLED);
    private static final ColorStateList CONTROL = new ColorStateList(new int[][] {
        {-android.R.attr.state_enabled}, {android.R.attr.state_checked}, {}
    }, new int[] {DISABLED, ICE, MUTED});
    private static final WeakHashMap<Button, Boolean> PRIMARY = new WeakHashMap<>();

    private GameMenuStyle() { }

    static Context context(Context context) {
        return new ContextThemeWrapper(context, R.style.GameMenuTheme);
    }

    static AlertDialog.Builder builder(Context context) {
        return new AlertDialog.Builder(context(context)) {
            @Override public AlertDialog show() {
                AlertDialog dialog = super.show();
                style(dialog);
                return dialog;
            }
        };
    }

    static void style(AlertDialog dialog) {
        if (dialog.getWindow() == null) return;
        content(dialog.getWindow().getDecorView());
        LauncherDialogs.style(dialog);
        int[] actions = {AlertDialog.BUTTON_POSITIVE, AlertDialog.BUTTON_NEGATIVE, AlertDialog.BUTTON_NEUTRAL};
        for (int action : actions) {
            Button button = dialog.getButton(action);
            if (button != null && button.getLayoutParams() instanceof ViewGroup.MarginLayoutParams) {
                ViewGroup.MarginLayoutParams params = (ViewGroup.MarginLayoutParams) button.getLayoutParams();
                params.setMargins(dp(button, 4), 0, dp(button, 4), 0);
                button.setLayoutParams(params);
            }
        }
        ListView list = dialog.getListView();
        if (list != null && !ownsRows(list)) {
            list.post(() -> {
                for (int i = 0; i < list.getChildCount(); i++) content(list.getChildAt(i));
            });
        }
    }

    static void content(View view) {
        if (view == null) return;
        // These panels and adapters already own their selection colors and compact layout.
        if (view instanceof LookSensitivityPanel || view instanceof HudEditorPanel
                || view instanceof ListView && ownsRows((ListView) view)) return;
        if (view instanceof TextView) {
            TextView text = (TextView) view;
            text.setTextColor(TEXT);
            if (text instanceof CompoundButton) {
                CompoundButton toggle = (CompoundButton) text;
                toggle.setButtonTintList(CONTROL);
                toggle.setMinimumHeight(dp(view, 48));
                if (toggle instanceof Switch) {
                    Switch control = (Switch) toggle;
                    control.setThumbTintList(CONTROL);
                    control.setTrackTintList(new ColorStateList(new int[][] {
                        {-android.R.attr.state_enabled}, {android.R.attr.state_checked}, {}
                    }, new int[] {0xff283744, 0xff416d80, 0xff405365}));
                }
            } else if (text instanceof Button) {
                button((Button) text, false);
            } else if (text instanceof EditText) {
                text.setHintTextColor(MUTED);
                text.setBackgroundTintList(ACCENT);
            } else if (text instanceof CheckedTextView) {
                ((CheckedTextView) text).setCheckMarkTintList(CONTROL);
            }
        }
        if (view instanceof ProgressBar) {
            ProgressBar bar = (ProgressBar) view;
            bar.setProgressTintList(ACCENT);
            bar.setSecondaryProgressTintList(ColorStateList.valueOf(0xff416d80));
            bar.setProgressBackgroundTintList(ColorStateList.valueOf(0xff304454));
            bar.setIndeterminateTintList(ACCENT);
            if (bar instanceof SeekBar) ((SeekBar) bar).setThumbTintList(ACCENT);
        }
        if (view instanceof Spinner) {
            Spinner spinner = (Spinner) view;
            spinner.setBackgroundTintList(ACCENT);
            GradientDrawable popup = new GradientDrawable();
            popup.setColor(SURFACE);
            popup.setCornerRadius(dp(view, 10));
            spinner.setPopupBackgroundDrawable(popup);
            if (spinner.getAdapter() instanceof ThemedSpinnerAdapter) {
                ((ThemedSpinnerAdapter) spinner.getAdapter()).setDropDownViewTheme(context(view.getContext()).getTheme());
            }
            spinner.post(() -> content(spinner.getSelectedView()));
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) content(group.getChildAt(i));
        }
    }

    static void primary(Button button) {
        if (button != null) {
            PRIMARY.put(button, true);
            button(button, true);
        }
    }

    private static void button(Button button, boolean primary) {
        primary = primary || Boolean.TRUE.equals(PRIMARY.get(button));
        button.setTextColor(primary ? enabled(INK, DISABLED) : TEXT);
        button.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        button.setAllCaps(false);
        button.setSingleLine(false);
        ViewGroup.LayoutParams params = button.getLayoutParams();
        boolean compact = params != null && (params.height > 0 && params.height < dp(button, 48)
            || params.width > 0 && params.width < dp(button, 88));
        if (!compact) {
            button.setMinHeight(dp(button, 48));
            button.setMinimumHeight(dp(button, 48));
        }
        GradientDrawable surface = new GradientDrawable();
        surface.setCornerRadius(dp(button, 10));
        surface.setColor(new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}},
            new int[] {0xff14202a, primary ? ICE : SURFACE}));
        if (!primary) surface.setStroke(dp(button, 1), 0xff365264);
        button.setBackgroundTintList(null);
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(0x3368cbe8), surface, null));
    }

    private static ColorStateList enabled(int color, int disabled) {
        return new ColorStateList(new int[][] {{-android.R.attr.state_enabled}, {}}, new int[] {disabled, color});
    }

    private static boolean ownsRows(ListView list) {
        return list.getAdapter() instanceof SettingsMenuAdapter
            || list.getAdapter() instanceof LauncherSaveAdapter;
    }

    private static int dp(View view, int value) {
        return Math.round(value * view.getResources().getDisplayMetrics().density);
    }
}
