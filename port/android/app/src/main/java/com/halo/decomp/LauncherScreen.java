package com.halo.decomp;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Build;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

/** Native, responsive launcher. Game and download state remain in LauncherActivity. */
final class LauncherScreen extends FrameLayout {
    private static final int INK = 0xff080e16, WHITE = 0xffedf5fb, MUTED = 0xffa4b9c8, ICE = 0xffa0e1f5;
    final Button play, demo, pick, download, update;
    final ProgressBar progress;
    final TextView status;
    private final LinearLayout body, brand, actions, footer;
    private final ScrollView scroll;
    private Boolean wide;

    LauncherScreen(Context context, boolean haveData, boolean haveDemo, Runnable onPlay,
            Runnable onDemo, Runnable onPick, Runnable onDownload, Runnable onUpdate) {
        super(context);
        setBackgroundColor(INK);
        addView(new LauncherBackdrop(context), new FrameLayout.LayoutParams(-1, -1));
        scroll = new ScrollView(context);
        scroll.setFillViewport(true);
        scroll.setClipToPadding(false);
        LinearLayout shell = new LinearLayout(context);
        shell.setOrientation(LinearLayout.VERTICAL);
        shell.setPadding(dp(28), dp(24), dp(28), dp(12));
        scroll.addView(shell, new ScrollView.LayoutParams(-1, -2));
        addView(scroll, new FrameLayout.LayoutParams(-1, -1));

        body = new LinearLayout(context);
        body.setGravity(Gravity.CENTER_VERTICAL);
        shell.addView(body, new LinearLayout.LayoutParams(-1, -2, 1));
        brand = new LinearLayout(context);
        brand.setOrientation(LinearLayout.VERTICAL);
        body.addView(brand);
        TextView edition = text("OPENCE  /  ANDROID", 11, ICE);
        edition.setLetterSpacing(.12f);
        brand.addView(edition);
        TextView title = text("HALO", 54, WHITE);
        title.setTypeface(Typeface.create("sans-serif-light", Typeface.NORMAL));
        title.setLetterSpacing(.14f);
        title.setAccessibilityHeading(true);
        LinearLayout.LayoutParams titleLayout = new LinearLayout.LayoutParams(-1, -2);
        titleLayout.topMargin = dp(6);
        brand.addView(title, titleLayout);
        TextView subtitle = text("COMBAT EVOLVED", 12, WHITE);
        subtitle.setLetterSpacing(.22f);
        brand.addView(subtitle);
        TextView availability = text(haveData ? "Datos instalados" : "Descarga la demo para empezar", 14, MUTED);
        LinearLayout.LayoutParams availabilityLayout = new LinearLayout.LayoutParams(-1, -2);
        availabilityLayout.topMargin = dp(24);
        brand.addView(availability, availabilityLayout);

        actions = new LinearLayout(context);
        actions.setOrientation(LinearLayout.VERTICAL);
        body.addView(actions);
        play = button("Jugar / Continuar", haveData ? 0 : 1, 16, onPlay);
        actions.addView(play, new LinearLayout.LayoutParams(-1, -2));
        demo = button(haveDemo ? "Jugar demo" : "Descargar y jugar demo", haveData ? 1 : 0, 16, onDemo);
        LinearLayout.LayoutParams demoLayout = new LinearLayout.LayoutParams(-1, -2);
        demoLayout.topMargin = dp(10);
        actions.addView(demo, demoLayout);
        TextView demoHint = text(haveDemo ? "Una nueva partida de la demo" : "Descarga inicial · 380 MB", 12, MUTED);
        demoHint.setPadding(dp(2), dp(8), 0, dp(18));
        actions.addView(demoHint);
        TextView dataTitle = text("DATOS DEL JUEGO", 10, MUTED);
        dataTitle.setLetterSpacing(.1f);
        actions.addView(dataTitle);
        LinearLayout tools = new LinearLayout(context);
        tools.setGravity(Gravity.CENTER_VERTICAL);
        actions.addView(tools);
        pick = button("Elegir imagen de disco", 2, 13, onPick);
        pick.setEnabled(!haveData);
        download = button("Descargar paquete de datos", 2, 13, onDownload);
        tools.addView(pick, new LinearLayout.LayoutParams(0, -2, 1));
        LinearLayout.LayoutParams downloadLayout = new LinearLayout.LayoutParams(0, -2, 1);
        downloadLayout.leftMargin = dp(6);
        tools.addView(download, downloadLayout);
        progress = new ProgressBar(context, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setProgressTintList(ColorStateList.valueOf(ICE));
        progress.setVisibility(View.GONE);
        actions.addView(progress, new LinearLayout.LayoutParams(-1, dp(8)));
        status = text("", 13, ICE);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        actions.addView(status, new LinearLayout.LayoutParams(-1, -2));

        footer = new LinearLayout(context);
        footer.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams footerLayout = new LinearLayout.LayoutParams(-1, -2);
        footerLayout.topMargin = dp(16);
        shell.addView(footer, footerLayout);
        TextView caption = text("OpenCE · Combat Evolved", 11, MUTED);
        footer.addView(caption, new LinearLayout.LayoutParams(0, -2, 1));
        update = button("Buscar actualización de la APK", 2, 13, onUpdate);
        footer.addView(update);
        setOnApplyWindowInsetsListener((view, insets) -> {
            int left = insets.getSystemWindowInsetLeft(), top = insets.getSystemWindowInsetTop();
            int right = insets.getSystemWindowInsetRight(), bottom = insets.getSystemWindowInsetBottom();
            if (Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
                left = Math.max(left, insets.getDisplayCutout().getSafeInsetLeft());
                top = Math.max(top, insets.getDisplayCutout().getSafeInsetTop());
                right = Math.max(right, insets.getDisplayCutout().getSafeInsetRight());
                bottom = Math.max(bottom, insets.getDisplayCutout().getSafeInsetBottom());
            }
            scroll.setPadding(left, top, right, bottom);
            return insets;
        });
    }

    void addGameMenuAction(boolean available, Runnable onGameMenu) {
        Button internal = button("Configuración interna de Halo", 2, 13, onGameMenu);
        internal.setEnabled(available);
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(-1, -2);
        layout.bottomMargin = dp(18);
        actions.addView(internal, 3, layout);
    }

    @Override protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        boolean nextWide = MeasureSpec.getSize(widthMeasureSpec) / getResources().getDisplayMetrics().density >= 680
            && getResources().getConfiguration().fontScale <= 1.5f;
        if (wide == null || wide != nextWide) {
            wide = nextWide;
            body.setOrientation(nextWide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
            body.setMinimumHeight(nextWide ? dp(232) : 0);
            brand.setLayoutParams(nextWide ? new LinearLayout.LayoutParams(0, -2, 1)
                : new LinearLayout.LayoutParams(-1, -2));
            LinearLayout.LayoutParams menu = new LinearLayout.LayoutParams(nextWide ? dp(330) : -1, -2);
            menu.leftMargin = nextWide ? dp(28) : 0;
            menu.topMargin = nextWide ? 0 : dp(28);
            actions.setLayoutParams(menu);
            footer.setOrientation(nextWide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
            footer.getChildAt(0).setLayoutParams(nextWide ? new LinearLayout.LayoutParams(0, -2, 1)
                : new LinearLayout.LayoutParams(-1, -2));
            update.setLayoutParams(new LinearLayout.LayoutParams(nextWide ? -2 : -1, -2));
        }
        super.onMeasure(widthMeasureSpec, heightMeasureSpec);
    }

    private TextView text(String value, int size, int color) {
        TextView view = new TextView(getContext());
        view.setText(value);
        view.setTextColor(color);
        view.setTextSize(size);
        view.setFontFeatureSettings("kern");
        view.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
        return view;
    }

    private Button button(String label, int style, int size, Runnable action) {
        Button button = new Button(getContext());
        button.setText(label);
        button.setTextSize(size);
        button.setAllCaps(false);
        button.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        button.setGravity(Gravity.CENTER);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setMinHeight(dp(style == 2 ? 48 : 54));
        button.setMinimumHeight(dp(style == 2 ? 48 : 54));
        button.setPadding(dp(style == 2 ? 8 : 18), dp(12), dp(style == 2 ? 8 : 18), dp(12));
        button.setTextColor(new ColorStateList(new int[][] { {-android.R.attr.state_enabled}, {} },
            new int[] { 0xff627b8c, style == 0 ? INK : WHITE }));
        GradientDrawable surface = new GradientDrawable();
        surface.setCornerRadius(dp(10));
        surface.setColor(new ColorStateList(new int[][] { {-android.R.attr.state_enabled}, {} },
            new int[] { style == 2 ? Color.TRANSPARENT : 0xff14202a,
                style == 0 ? ICE : style == 1 ? 0xff152633 : Color.TRANSPARENT }));
        if (style == 1)
            surface.setStroke(dp(1), 0xff365264);
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(0x3368cbe8), surface, null));
        button.setOnClickListener(v -> action.run());
        return button;
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
