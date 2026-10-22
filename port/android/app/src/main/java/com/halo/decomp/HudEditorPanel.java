package com.halo.decomp;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Insets;
import android.graphics.Typeface;
import android.graphics.drawable.ClipDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Build;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.Locale;

/** The HUD editor's floating controls; the rest of this frame passes touches through. */
public final class HudEditorPanel extends FrameLayout {
    public interface Listener {
        void onExit();
        void onReset();
        void onSave();
        void onSize(float size);
        void onOpacity(float opacity);
        void onNudge(float dx, float dy);
        void onSelectHud(int slot);
        void onVisibility(boolean visible);
    }

    private static final int YELLOW = 0xff9ee3f4;
    private static final int PANEL = 0xff101b27;
    private static final int BORDER = 0xff263848;

    private final Listener listener;
    private final ScrollView viewport;
    private final LinearLayout panel;
    private final LinearLayout tabs;
    private final LinearLayout details;
    private final Button[] hudButtons = new Button[3];
    private final Button[] nudgeButtons = new Button[4];
    private final Button saveButton;
    private final Button collapseButton;
    private final TextView selectedLabel;
    private final TextView sizeLabel;
    private final TextView opacityLabel;
    private final SeekBar sizeSlider;
    private final SeekBar opacitySlider;
    private final CheckBox visibleButton;
    private int selectedSlot;
    private boolean binding;
    private boolean collapsed;
    private int safeLeft, safeTop, safeRight, safeBottom;
    private int dragPointer = -1;
    private float dragDownX, dragDownY, dragStartX, dragStartY;
    private boolean draggingPanel;
    private final int dragSlop;

    public HudEditorPanel(Context context, Listener listener) {
        super(context);
        if (listener == null)
            throw new IllegalArgumentException("HUD editor listener is required");
        this.listener = listener;
        dragSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        setClickable(false);
        setFocusable(false);
        setVisibility(GONE);

        viewport = new ScrollView(context);
        viewport.setVerticalScrollBarEnabled(false);
        addView(viewport, new LayoutParams(dp(270), LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL));
        viewport.addOnLayoutChangeListener((view, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> clampPanel());
        panel = new LinearLayout(context);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(8), dp(5), dp(8), 0);
        panel.setBackgroundColor(PANEL);
        panel.setClickable(true);
        panel.setElevation(dp(6));
        viewport.addView(panel, new ScrollView.LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));

        tabs = horizontal();
        for (int i = 0; i < hudButtons.length; i++) {
            final int slot = i;
            Button button = action("HUD " + (i + 1), false);
            button.setContentDescription("Seleccionar HUD " + (i + 1));
            button.setOnClickListener(view -> {
                if (slot != selectedSlot)
                    listener.onSelectHud(slot);
            });
            hudButtons[i] = button;
            tabs.addView(button, weighted(25));
        }
        panel.addView(tabs);

        LinearLayout actions = horizontal();
        Button exit = action("Salir", true);
        exit.setOnClickListener(view -> listener.onExit());
        Button reset = action("Restablecer", true);
        reset.setOnClickListener(view -> listener.onReset());
        saveButton = action("Guardar", true);
        saveButton.setOnClickListener(view -> listener.onSave());
        actions.addView(exit, weighted(28));
        actions.addView(reset, weighted(28));
        actions.addView(saveButton, weighted(28));
        LinearLayout.LayoutParams actionsParams = new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, dp(28));
        actionsParams.topMargin = dp(4);
        panel.addView(actions, actionsParams);

        details = new LinearLayout(context);
        details.setOrientation(LinearLayout.VERTICAL);
        selectedLabel = label(11);
        selectedLabel.setSingleLine(true);
        selectedLabel.setGravity(Gravity.CENTER_VERTICAL);
        details.addView(selectedLabel, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, dp(19)));

        LinearLayout controls = horizontal();
        LinearLayout sliders = new LinearLayout(context);
        sliders.setOrientation(LinearLayout.VERTICAL);
        opacityLabel = label(12);
        sizeLabel = label(12);
        opacitySlider = slider(90, "Opacidad del botón");
        sizeSlider = slider(150, "Tamaño del botón");
        sliders.addView(opacityLabel, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, dp(16)));
        sliders.addView(opacitySlider, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, dp(23)));
        sliders.addView(sizeLabel, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, dp(16)));
        sliders.addView(sizeSlider, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, dp(23)));
        LinearLayout.LayoutParams slidersParams = new LinearLayout.LayoutParams(0, dp(78), 1);
        slidersParams.rightMargin = dp(8);
        controls.addView(sliders, slidersParams);

        FrameLayout arrows = new FrameLayout(context);
        addArrow(arrows, 0, "▲", "Mover arriba", 26, 0, 0, -1);
        addArrow(arrows, 1, "◀", "Mover a la izquierda", 0, 26, -1, 0);
        addArrow(arrows, 2, "▶", "Mover a la derecha", 52, 26, 1, 0);
        addArrow(arrows, 3, "▼", "Mover abajo", 26, 52, 0, 1);
        controls.addView(arrows, new LinearLayout.LayoutParams(dp(78), dp(78)));
        details.addView(controls);

        visibleButton = new CheckBox(context);
        visibleButton.setText("Mostrar botón");
        visibleButton.setTextSize(11);
        visibleButton.setTextColor(Color.WHITE);
        visibleButton.setPadding(0, 0, 0, 0);
        visibleButton.setMinimumHeight(0);
        visibleButton.setButtonTintList(new ColorStateList(
            new int[][] {new int[] {android.R.attr.state_checked}, new int[] {}},
            new int[] {YELLOW, Color.LTGRAY}));
        visibleButton.setOnCheckedChangeListener((view, checked) -> {
            if (!binding)
                listener.onVisibility(checked);
        });
        details.addView(visibleButton, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, dp(26)));
        panel.addView(details);

        collapseButton = action("⌃", false);
        collapseButton.setTextSize(18);
        collapseButton.setOnClickListener(view -> {
            collapsed = !collapsed;
            tabs.setVisibility(collapsed ? GONE : VISIBLE);
            details.setVisibility(collapsed ? GONE : VISIBLE);
            collapseButton.setText(collapsed ? "⌄" : "⌃");
            collapseButton.setContentDescription(collapsed ? "Ampliar editor; arrastra para mover el panel"
                : "Contraer editor; arrastra para mover el panel");
        });
        collapseButton.setContentDescription("Contraer editor; arrastra para mover el panel");
        collapseButton.setOnTouchListener(this::dragPanel);
        panel.addView(collapseButton, new LinearLayout.LayoutParams(LayoutParams.MATCH_PARENT, dp(17)));

        listen(sizeSlider, true);
        listen(opacitySlider, false);
        bind(0, null, 1f, 1f, true);
        setDirty(false);
    }

    /** Slot indices are zero-based. Binding only refreshes widgets; it never changes the model or dirty state. */
    public void bind(int slot, String controlName, float size, float opacity, boolean visible) {
        if (slot < 0 || slot >= hudButtons.length)
            throw new IllegalArgumentException("HUD slot must be between 0 and 2");
        binding = true;
        try {
            selectedSlot = slot;
            for (int i = 0; i < hudButtons.length; i++) {
                hudButtons[i].setTextColor(i == slot ? Color.BLACK : Color.WHITE);
                hudButtons[i].setBackground(rectangle(i == slot ? YELLOW : 0xff142027, BORDER));
            }
            boolean selected = controlName != null && !controlName.isEmpty();
            selectedLabel.setText(selected ? controlName : "Toca un botón para editar");
            int sizePercent = Math.round(finiteClamp(size, 0.5f, 2f) * 100);
            int opacityPercent = Math.round(finiteClamp(opacity, 0.1f, 1f) * 100);
            sizeSlider.setProgress(sizePercent - 50);
            opacitySlider.setProgress(opacityPercent - 10);
            percentage(sizeLabel, "Tamaño", sizePercent);
            percentage(opacityLabel, "Opacidad", opacityPercent);
            sizeSlider.setEnabled(selected);
            opacitySlider.setEnabled(selected);
            visibleButton.setEnabled(selected);
            visibleButton.setChecked(visible);
            for (Button button : nudgeButtons)
                button.setEnabled(selected);
            selectedLabel.setAlpha(selected ? 1f : 0.7f);
        } finally {
            binding = false;
        }
    }

    public void setDirty(boolean dirty) {
        saveButton.setEnabled(dirty);
    }

    private boolean dragPanel(View handle, MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            dragPointer = event.getPointerId(0);
            dragDownX = event.getRawX();
            dragDownY = event.getRawY();
            dragStartX = viewport.getTranslationX();
            dragStartY = viewport.getTranslationY();
            draggingPanel = false;
            handle.setPressed(true);
            handle.getParent().requestDisallowInterceptTouchEvent(true);
        } else if (action == MotionEvent.ACTION_MOVE && dragPointer >= 0) {
            int pointer = event.findPointerIndex(dragPointer);
            if (pointer >= 0) {
                float dx = event.getRawX() + event.getX(pointer) - event.getX(0) - dragDownX;
                float dy = event.getRawY() + event.getY(pointer) - event.getY(0) - dragDownY;
                if (!draggingPanel && dx * dx + dy * dy > dragSlop * dragSlop) {
                    draggingPanel = true;
                    handle.setPressed(false);
                }
                if (draggingPanel) {
                    viewport.setTranslationX(dragStartX + dx);
                    viewport.setTranslationY(dragStartY + dy);
                    clampPanel();
                }
            }
        } else if (action == MotionEvent.ACTION_CANCEL || action == MotionEvent.ACTION_UP
                || (action == MotionEvent.ACTION_POINTER_UP
                    && event.getPointerId(event.getActionIndex()) == dragPointer)) {
            boolean click = action == MotionEvent.ACTION_UP && dragPointer >= 0 && !draggingPanel;
            dragPointer = -1;
            draggingPanel = false;
            handle.setPressed(false);
            handle.getParent().requestDisallowInterceptTouchEvent(false);
            if (click)
                handle.performClick();
        }
        return true;
    }

    private void clampPanel() {
        if (viewport.getWidth() <= 0 || viewport.getHeight() <= 0)
            return;
        float minimumX = safeLeft + dp(4) - viewport.getLeft();
        float maximumX = Math.max(minimumX, getWidth() - safeRight - dp(4) - viewport.getWidth() - viewport.getLeft());
        float minimumY = safeTop + dp(1) - viewport.getTop();
        float maximumY = Math.max(minimumY, getHeight() - safeBottom - dp(1) - viewport.getHeight() - viewport.getTop());
        viewport.setTranslationX(Math.max(minimumX, Math.min(maximumX, viewport.getTranslationX())));
        viewport.setTranslationY(Math.max(minimumY, Math.min(maximumY, viewport.getTranslationY())));
    }

    private void listen(SeekBar slider, boolean size) {
        slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                if (binding || !fromUser)
                    return;
                int percent = progress + (size ? 50 : 10);
                percentage(size ? sizeLabel : opacityLabel, size ? "Tamaño" : "Opacidad", percent);
                if (size)
                    listener.onSize(percent / 100f);
                else
                    listener.onOpacity(percent / 100f);
            }

            @Override public void onStartTrackingTouch(SeekBar bar) { }
            @Override public void onStopTrackingTouch(SeekBar bar) { }
        });
    }

    private void addArrow(FrameLayout parent, int index, String symbol, String description,
            int x, int y, float dx, float dy) {
        Button button = action(symbol, false);
        button.setTextSize(16);
        button.setContentDescription(description);
        button.setOnClickListener(view -> listener.onNudge(dx, dy));
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(dp(25), dp(25));
        params.leftMargin = dp(x);
        params.topMargin = dp(y);
        parent.addView(button, params);
        nudgeButtons[index] = button;
    }

    private LinearLayout horizontal() {
        LinearLayout view = new LinearLayout(getContext());
        view.setOrientation(LinearLayout.HORIZONTAL);
        return view;
    }

    private LinearLayout.LayoutParams weighted(int height) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0, dp(height), 1);
        params.leftMargin = dp(2);
        params.rightMargin = dp(2);
        return params;
    }

    private TextView label(int sp) {
        TextView view = new TextView(getContext());
        view.setTextColor(Color.WHITE);
        view.setTextSize(sp);
        view.setTypeface(Typeface.create("sans-serif-condensed", Typeface.NORMAL));
        view.setIncludeFontPadding(false);
        return view;
    }

    private Button action(String text, boolean white) {
        Button button = new Button(getContext());
        button.setText(text);
        button.setTextSize(11);
        button.setAllCaps(false);
        button.setTypeface(Typeface.create("sans-serif-condensed", Typeface.BOLD));
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setPadding(dp(1), 0, dp(1), 0);
        button.setGravity(Gravity.CENTER);
        button.setIncludeFontPadding(false);
        button.setTextColor(new ColorStateList(
            new int[][] {new int[] {-android.R.attr.state_enabled}, new int[] {}},
            new int[] {0xff868a8d, white ? Color.BLACK : Color.WHITE}));
        StateListDrawable background = new StateListDrawable();
        background.addState(new int[] {-android.R.attr.state_enabled}, rectangle(0xff414447, BORDER));
        background.addState(new int[] {android.R.attr.state_pressed}, rectangle(YELLOW, BORDER));
        background.addState(new int[] {}, rectangle(white ? Color.WHITE : 0xff0a1014, BORDER));
        button.setBackground(background);
        button.setBackgroundTintList(null);
        return button;
    }

    private SeekBar slider(int maximum, String description) {
        SeekBar view = new SeekBar(getContext());
        view.setMax(maximum);
        view.setContentDescription(description);
        view.setPadding(dp(3), 0, dp(3), 0);
        LayerDrawable track = new LayerDrawable(new android.graphics.drawable.Drawable[] {
            rectangle(Color.BLACK, Color.TRANSPARENT),
            new ClipDrawable(rectangle(YELLOW, Color.TRANSPARENT), Gravity.LEFT, ClipDrawable.HORIZONTAL)
        });
        track.setId(0, android.R.id.background);
        track.setId(1, android.R.id.progress);
        view.setProgressDrawable(track);
        view.setProgressTintList(ColorStateList.valueOf(YELLOW));
        view.setProgressBackgroundTintList(ColorStateList.valueOf(Color.BLACK));
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            view.setMinHeight(dp(4));
            view.setMaxHeight(dp(4));
        }
        GradientDrawable thumb = rectangle(Color.WHITE, 0xff73797d);
        thumb.setSize(dp(5), dp(12));
        view.setThumb(thumb);
        view.setThumbTintList(null);
        view.setSplitTrack(false);
        return view;
    }

    private GradientDrawable rectangle(int color, int border) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setStroke(dp(1), border);
        return drawable;
    }

    private static void percentage(TextView label, String name, int percent) {
        label.setText(String.format(Locale.ROOT, "%s    %d%%", name, percent));
    }

    private static float finiteClamp(float value, float minimum, float maximum) {
        return Float.isFinite(value) ? Math.max(minimum, Math.min(maximum, value)) : 1f;
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        updateBounds();
    }

    @Override
    protected void onVisibilityChanged(View changedView, int visibility) {
        super.onVisibilityChanged(changedView, visibility);
        if (visibility == VISIBLE)
            requestApplyInsets();
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            safeLeft = bars.left;
            safeTop = bars.top;
            safeRight = bars.right;
            safeBottom = bars.bottom;
        } else {
            safeLeft = insets.getSystemWindowInsetLeft();
            safeTop = insets.getSystemWindowInsetTop();
            safeRight = insets.getSystemWindowInsetRight();
            safeBottom = insets.getSystemWindowInsetBottom();
        }
        updateBounds();
        return super.onApplyWindowInsets(insets);
    }

    private void updateBounds() {
        if (viewport == null || getWidth() <= 0)
            return;
        int available = Math.max(1, getWidth() - safeLeft - safeRight - dp(8));
        LayoutParams params = (LayoutParams) viewport.getLayoutParams();
        params.width = Math.min(dp(270), Math.round(available * 0.45f));
        params.topMargin = safeTop + dp(1);
        params.leftMargin = 0;
        params.rightMargin = 0;
        viewport.setLayoutParams(params);
        setPadding(0, 0, 0, safeBottom);
    }
}
