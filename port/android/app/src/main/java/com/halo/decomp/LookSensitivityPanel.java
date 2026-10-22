package com.halo.decomp;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import java.util.Locale;

/** A sensitivity draft and a local camera preview; neither sends input to the game. */
final class LookSensitivityPanel extends LinearLayout {
    private static final float STEP = 0.025f;
    private static final int ACCENT = 0xff9ee3f4;
    private final TextView valueLabel;
    private final SeekBar slider;
    private final CameraPreview preview;
    private final TextView horizontalLabel;
    private final Button[] horizontalPresets = new Button[3];
    private final float[] horizontalValues = {1f, TouchLayout.DEFAULT_HORIZONTAL_SENSITIVITY, 4f};
    private final Button[] presets = new Button[3];
    private final float[] presetValues = {0.75f, 1f, 2f};
    private final Switch invertHorizontal, invertVertical;
    private float sensitivity;
    private float horizontalSensitivity = 1f;

    LookSensitivityPanel(Context context, float initial, float logicalUnitsPerPixel) {
        this(context, initial, 1f, logicalUnitsPerPixel);
    }

    LookSensitivityPanel(Context context, float initial, float horizontalInitial, float logicalUnitsPerPixel) {
        super(context);
        setOrientation(HORIZONTAL);
        setPadding(dp(12), dp(8), dp(12), dp(8));
        setBackgroundColor(0xff101b27);
        setGravity(Gravity.CENTER_VERTICAL);

        ScrollView scroll = new ScrollView(context);
        addView(scroll, new LayoutParams(0, dp(252), 1f));
        LinearLayout controls = new LinearLayout(context);
        controls.setOrientation(VERTICAL);
        controls.setPadding(0, 0, dp(12), 0);
        scroll.addView(controls);
        valueLabel = label(20);
        controls.addView(valueLabel);
        TextView mode = label(13);
        mode.setText("Sin zoom · Cámara derecha");
        controls.addView(mode);

        slider = new SeekBar(context);
        slider.setMax(150);
        slider.setContentDescription("Sensibilidad de cámara");
        slider.setProgressTintList(ColorStateList.valueOf(ACCENT));
        slider.setThumbTintList(ColorStateList.valueOf(ACCENT));
        controls.addView(slider, new LayoutParams(LayoutParams.MATCH_PARENT, dp(44)));

        LinearLayout presetRow = new LinearLayout(context);
        String[] names = {"Preciso", "Normal", "Rápido"};
        for (int i = 0; i < presets.length; i++) {
            final float value = presetValues[i];
            presets[i] = button(names[i]);
            presets[i].setOnClickListener(view -> setValue(value));
            presetRow.addView(presets[i], new LayoutParams(0, dp(44), 1f));
        }
        controls.addView(presetRow);
        LinearLayout steps = new LinearLayout(context);
        Button less = button("−");
        less.setContentDescription("Reducir sensibilidad");
        less.setOnClickListener(view -> setValue(value() - STEP));
        Button more = button("+");
        more.setContentDescription("Aumentar sensibilidad");
        more.setOnClickListener(view -> setValue(value() + STEP));
        steps.addView(less, new LayoutParams(0, dp(44), 1f));
        steps.addView(more, new LayoutParams(0, dp(44), 1f));
        controls.addView(steps);
        horizontalLabel = label(13);
        controls.addView(horizontalLabel);
        LinearLayout horizontalRow = new LinearLayout(context);
        String[] horizontalNames = {"Clásico", "Ágil", "Amplio"};
        for (int i = 0; i < horizontalPresets.length; i++) {
            final float gain = horizontalValues[i];
            horizontalPresets[i] = button(horizontalNames[i]);
            horizontalPresets[i].setOnClickListener(view -> setHorizontalValue(gain));
            horizontalRow.addView(horizontalPresets[i], new LayoutParams(0, dp(40), 1f));
        }
        controls.addView(horizontalRow);
        TextView help = label(12);
        help.setText("Ágil: menos deslizamientos para girar. Prueba a la derecha.");
        controls.addView(help);
        invertHorizontal = inversionSwitch("Invertir deslizamiento horizontal");
        invertVertical = inversionSwitch("Invertir deslizamiento vertical");
        controls.addView(invertHorizontal, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));
        controls.addView(invertVertical, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));

        preview = new CameraPreview(context, logicalUnitsPerPixel);
        addView(preview, new LayoutParams(0, dp(252), 1f));
        invertHorizontal.setOnCheckedChangeListener((button, checked) -> preview.invalidate());
        invertVertical.setOnCheckedChangeListener((button, checked) -> preview.invalidate());
        slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int progress, boolean user) {
                setValue(TouchLayout.MIN_SENSITIVITY + progress * STEP);
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        setValue(initial);
        setHorizontalValue(horizontalInitial);
    }

    float value() {
        return sensitivity;
    }

    float horizontalValue() {
        return horizontalSensitivity;
    }

    boolean invertedHorizontal() { return invertHorizontal.isChecked(); }

    boolean invertedVertical() { return invertVertical.isChecked(); }

    void setInvertedAxes(boolean x, boolean y) {
        invertHorizontal.setChecked(x);
        invertVertical.setChecked(y);
        preview.invalidate();
    }

    void setHorizontalValue(float value) {
        horizontalSensitivity = TouchLayout.validHorizontalSensitivity(value)
            ? value : TouchLayout.DEFAULT_HORIZONTAL_SENSITIVITY;
        horizontalLabel.setText(String.format(Locale.getDefault(), "Giro horizontal  %.2f×", horizontalSensitivity));
        for (int i = 0; i < horizontalPresets.length; i++)
            horizontalPresets[i].setTextColor(Math.abs(horizontalSensitivity - horizontalValues[i]) < 0.001f
                ? ACCENT : 0xffeeeeee);
        preview.invalidate();
    }

    void setValue(float value) {
        if (!Float.isFinite(value))
            value = 1f;
        value = Math.max(TouchLayout.MIN_SENSITIVITY, Math.min(TouchLayout.MAX_SENSITIVITY, value));
        int progress = Math.round((value - TouchLayout.MIN_SENSITIVITY) / STEP);
        sensitivity = TouchLayout.MIN_SENSITIVITY + progress * STEP;
        if (slider.getProgress() != progress)
            slider.setProgress(progress);
        valueLabel.setText(String.format(Locale.getDefault(), "General  %.3f×", sensitivity));
        for (int i = 0; i < presets.length; i++)
            presets[i].setTextColor(Math.abs(sensitivity - presetValues[i]) < STEP / 2 ? ACCENT : 0xffeeeeee);
        preview.invalidate();
    }

    private TextView label(int size) {
        TextView label = new TextView(getContext());
        label.setTextColor(0xffeeeeee);
        label.setTextSize(size);
        return label;
    }

    private Switch inversionSwitch(String text) {
        Switch toggle = new Switch(getContext());
        toggle.setText(text);
        toggle.setTextColor(0xffeeeeee);
        toggle.setTextSize(13);
        toggle.setSingleLine(false);
        toggle.setMinimumHeight(dp(48));
        toggle.setSwitchPadding(dp(8));
        toggle.setThumbTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}},
            new int[] {ACCENT, 0xffa4b9c8}));
        toggle.setTrackTintList(new ColorStateList(new int[][] {{android.R.attr.state_checked}, {}},
            new int[] {0xff416d80, 0xff405365}));
        return toggle;
    }

    private Button button(String text) {
        Button button = new Button(getContext());
        button.setText(text);
        button.setTextSize(12);
        button.setAllCaps(false);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setPadding(dp(3), 0, dp(3), 0);
        button.setBackgroundTintList(ColorStateList.valueOf(0xff182936));
        button.setTextColor(0xffeeeeee);
        return button;
    }

    private int dp(float value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private final class CameraPreview extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final float unitsPerPixel;
        private float yaw, pitch, lastX, lastY;
        private int pointer = -1;

        CameraPreview(Context context, float logicalUnitsPerPixel) {
            super(context);
            unitsPerPixel = Float.isFinite(logicalUnitsPerPixel) && logicalUnitsPerPixel > 0
                ? logicalUnitsPerPixel : 1f;
            setContentDescription("Zona de prueba de cámara");
            setClickable(true);
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float width = getWidth(), height = getHeight();
            canvas.drawColor(0xff182a32);
            float horizon = height / 2 - pitch * height / (float) Math.PI;
            paint.setColor(0xff223e3b);
            canvas.drawRect(0, horizon, width, height, paint);
            float spacing = Math.max(1, width / 4);
            float offset = -yaw * width / (float) Math.PI % spacing;
            paint.setColor(0xff456066);
            paint.setStrokeWidth(dp(1));
            for (float x = offset - spacing; x <= width + spacing; x += spacing)
                canvas.drawLine(x, 0, x, height, paint);
            for (int i = -3; i <= 3; i++)
                canvas.drawLine(0, horizon + i * height / 5, width, horizon + i * height / 5, paint);
            paint.setColor(0xff90a6aa);
            canvas.drawLine(0, horizon, width, horizon, paint);
            float targetX = width / 2 - yaw * width / (float) Math.PI;
            float targetY = horizon;
            paint.setColor(ACCENT);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(2));
            canvas.drawCircle(targetX, targetY, dp(15), paint);
            paint.setColor(0xffeeeeee);
            canvas.drawCircle(width / 2, height / 2, dp(6), paint);
            canvas.drawLine(width / 2 - dp(14), height / 2, width / 2 + dp(14), height / 2, paint);
            canvas.drawLine(width / 2, height / 2 - dp(14), width / 2, height / 2 + dp(14), paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(0xdd10171b);
            canvas.drawRect(0, 0, width, dp(44), paint);
            paint.setColor(0xffeeeeee);
            paint.setTextSize(dp(12));
            canvas.drawText("Desliza para probar", dp(8), dp(17), paint);
            paint.setTextSize(dp(10));
            canvas.drawText("Vista de prueba · Sin zoom", dp(8), dp(34), paint);
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                pointer = event.getPointerId(0);
                lastX = event.getX(0);
                lastY = event.getY(0);
                getParent().requestDisallowInterceptTouchEvent(true);
                return true;
            }
            if (action == MotionEvent.ACTION_MOVE) {
                int index = event.findPointerIndex(pointer);
                if (index >= 0)
                    consume(event, index);
            } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL
                || (action == MotionEvent.ACTION_POINTER_UP && event.getPointerId(event.getActionIndex()) == pointer)) {
                int index = event.findPointerIndex(pointer);
                if (action != MotionEvent.ACTION_CANCEL && index >= 0)
                    consume(event, index);
                pointer = -1;
                getParent().requestDisallowInterceptTouchEvent(false);
                if (action == MotionEvent.ACTION_UP)
                    performClick();
            }
            return true;
        }

        private void consume(MotionEvent event, int index) {
            for (int i = 0; i < event.getHistorySize(); i++)
                move(event.getHistoricalX(index, i), event.getHistoricalY(index, i));
            move(event.getX(index), event.getY(index));
        }

        private void move(float x, float y) {
            float dx = (x - lastX) * unitsPerPixel * value() * horizontalValue()
                * TouchControls.LOOK_RADIANS_PER_UNIT;
            float dy = (y - lastY) * unitsPerPixel * value() * TouchControls.LOOK_RADIANS_PER_UNIT;
            if (invertedHorizontal()) dx = -dx;
            if (invertedVertical()) dy = -dy;
            if (Float.isFinite(dx) && Float.isFinite(dy)) {
                yaw = (float) Math.IEEEremainder(yaw + dx, Math.PI * 2);
                pitch = Math.max(-(float) Math.PI / 2, Math.min((float) Math.PI / 2, pitch + dy));
                if (dx != 0 || dy != 0)
                    invalidate();
            }
            lastX = x;
            lastY = y;
        }

        @Override public boolean performClick() {
            super.performClick();
            return true;
        }
    }
}
