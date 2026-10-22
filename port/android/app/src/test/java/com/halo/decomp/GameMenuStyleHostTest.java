package com.halo.decomp;

import android.app.AlertDialog;
import android.app.Application;
import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Rect;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.android.controller.ActivityController;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.annotation.LooperMode;
import org.robolectric.shadows.ShadowAlertDialog;
import org.robolectric.util.ReflectionHelpers;
import java.io.IOException;
import java.io.StringReader;
import java.util.Properties;
import java.util.concurrent.atomic.AtomicInteger;
import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35}, qualifiers = "w720dp-h320dp-land-mdpi",
    instrumentedPackages = {"com.halo.decomp", "org.libsdl.app"},
    shadows = {PauseMenuHostTest.EngineBoundary.class, TouchControlsMultitouchHostTest.NativeSink.class})
@LooperMode(LooperMode.Mode.PAUSED)
public class GameMenuStyleHostTest {
    private HaloActivity activity;
    private TouchControls controls;

    @Before public void createControls() {
        PauseMenuHostTest.EngineBoundary.pauses = PauseMenuHostTest.EngineBoundary.resumes = 0;
        ActivityController<HaloActivity> controller = Robolectric.buildActivity(HaloActivity.class);
        activity = controller.get();
        activity.getSharedPreferences("touch-layout-v1", 0).edit().clear().commit();
        FrameLayout root = new FrameLayout(activity);
        activity.setContentView(root);
        controls = new TouchControls(activity);
        root.addView(controls);
        controls.layout(0, 0, 720, 320);
        ReflectionHelpers.setField(controls, "showing", true);
        ReflectionHelpers.setField(activity, "touchControls", controls);
        ReflectionHelpers.setField(activity, "activityResumed", true);
        controller.visible();
        shadowOf(Looper.getMainLooper()).idle();
    }

    @After public void closeDialogs() {
        ReflectionHelpers.setField(activity, "activityResumed", false);
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        if (dialog != null && dialog.isShowing())
            dialog.dismiss();
        controls.stopDeviceInput();
        shadowOf(Looper.getMainLooper()).idle();
    }

    private <T extends View> T find(View view, Class<T> type, String label) {
        if (type.isInstance(view) && (label == null || view instanceof TextView
                && label.contentEquals(((TextView) view).getText())))
            return type.cast(view);
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                T match = find(group.getChildAt(i), type, label);
                if (match != null) return match;
            }
        }
        return null;
    }

    private void clickChoice(AlertDialog dialog, int index) {
        View row = dialog.getListView().getAdapter().getView(index, null, dialog.getListView());
        dialog.getListView().performItemClick(row, index, index);
        shadowOf(Looper.getMainLooper()).idle();
    }

    private Properties hudSnapshot() {
        Properties values = new Properties();
        try {
            // Properties.store adds a timestamp comment; compare every actual setting instead.
            values.load(new StringReader(controls.exportLayout()));
        } catch (IOException impossible) {
            throw new AssertionError(impossible);
        }
        return values;
    }

    @Test public void optionsKeepEveryActionAndClosingKeepsExactHud() {
        Properties before = hudSnapshot();
        controls.showOptions();
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        String[] expected = {"Controles predefinidos", "Perfiles gráficos", "Pantalla y resolución", "General",
            "HUD personalizado", "Sensibilidad al mirar", "Exportar HUD", "Importar HUD",
            "Descargar paquete de datos", "Actualizar APK", "Configuración interna de Halo"};
        assertEquals(expected.length, dialog.getListView().getAdapter().getCount());
        for (int index = 0; index < expected.length; index++)
            assertEquals(expected[index], dialog.getListView().getAdapter().getItem(index));
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(before, hudSnapshot());
        assertEquals(0, (int) ReflectionHelpers.getField(controls, "openDialogs"));
    }

    @Test public void optionsStillOpenSensitivityAndCancelKeepsExactHud() {
        Properties before = hudSnapshot();
        controls.showOptions();
        clickChoice(ShadowAlertDialog.getLatestAlertDialog(), 5);
        AlertDialog sensitivity = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("Sensibilidad al mirar", shadowOf(sensitivity).getTitle());
        sensitivity.getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(before, hudSnapshot());
    }

    @Test public void pauseKeepsResumeSettingsAndSaveActionsWithoutMovingHud() {
        Properties before = hudSnapshot();
        activity.showTouchPauseMenu();
        AlertDialog pause = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("Pausa", shadowOf(pause).getTitle());
        assertEquals("Reanudar", pause.getButton(AlertDialog.BUTTON_POSITIVE).getText().toString());
        assertEquals("Ajustes de controles", pause.getButton(AlertDialog.BUTTON_NEUTRAL).getText().toString());
        assertNotNull(find(pause.getWindow().getDecorView(), Button.class, "Guardar y salir"));
        assertEquals(1, PauseMenuHostTest.EngineBoundary.pauses);
        pause.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertFalse(pause.isShowing());
        assertNull(ReflectionHelpers.getField(activity, "touchPauseDialog"));
        assertEquals(before, hudSnapshot());
    }

    @Test public void pauseSettingsCallbackStillOpensControls() {
        Properties before = hudSnapshot();
        activity.showTouchPauseMenu();
        ShadowAlertDialog.getLatestAlertDialog().getButton(AlertDialog.BUTTON_NEUTRAL).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        AlertDialog options = ShadowAlertDialog.getLatestAlertDialog();
        assertEquals("Halo · Controles", shadowOf(options).getTitle());
        options.getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(before, hudSnapshot());
    }

    @Test public void stylingKeepsCheckedSliderAndSpinnerValuesAndCallbacks() {
        Context context = GameMenuStyle.context(activity);
        LinearLayout panel = new LinearLayout(context);
        panel.setOrientation(LinearLayout.VERTICAL);
        AtomicInteger changes = new AtomicInteger();
        Switch toggle = new Switch(context);
        toggle.setText("Disparo izquierdo");
        toggle.setChecked(true);
        toggle.setOnCheckedChangeListener((button, checked) -> changes.incrementAndGet());
        CheckBox check = new CheckBox(context);
        check.setText("Avance automático");
        check.setChecked(true);
        SeekBar slider = new SeekBar(context);
        slider.setMax(200);
        slider.setProgress(73);
        Spinner spinner = new Spinner(context);
        spinner.setAdapter(new ArrayAdapter<>(context, android.R.layout.simple_spinner_item,
            new String[] {"Automática", "Original", "Ligera"}));
        spinner.setSelection(1);
        panel.addView(toggle);
        panel.addView(check);
        panel.addView(slider);
        panel.addView(spinner);
        GameMenuStyle.content(panel);
        assertTrue(toggle.isChecked());
        assertTrue(check.isChecked());
        assertEquals(73, slider.getProgress());
        assertEquals(200, slider.getMax());
        assertEquals(1, spinner.getSelectedItemPosition());
        assertEquals(0, changes.get());
        assertNotNull(slider.getProgressTintList());
        assertNotNull(slider.getThumbTintList());
        assertNotNull(toggle.getThumbTintList());
        assertNotNull(check.getButtonTintList());
        ColorStateList tint = check.getButtonTintList();
        assertNotEquals(tint.getColorForState(new int[] {android.R.attr.state_enabled,
            android.R.attr.state_checked}, Color.TRANSPARENT), tint.getColorForState(
                new int[] {android.R.attr.state_enabled, -android.R.attr.state_checked}, Color.TRANSPARENT));
        assertTrue(contrast(toggle.getCurrentTextColor(), 0xff152633) >= 4.5);
        toggle.setChecked(false);
        assertEquals(1, changes.get());
    }

    @Test public void stylingKeepsTextInputAndPrimaryButtonCallbackReadable() {
        Context context = GameMenuStyle.context(activity);
        LinearLayout panel = new LinearLayout(context);
        EditText input = new EditText(context);
        input.setText("Mi partida");
        input.setHint("Nombre de la partida");
        Button button = new Button(context);
        button.setText("Guardar y salir");
        AtomicInteger clicks = new AtomicInteger();
        button.setOnClickListener(view -> clicks.incrementAndGet());
        panel.addView(input);
        panel.addView(button);
        GameMenuStyle.content(panel);
        GameMenuStyle.primary(button);
        assertEquals("Mi partida", input.getText().toString());
        assertEquals("Nombre de la partida", input.getHint().toString());
        assertTrue(contrast(input.getCurrentTextColor(), 0xff152633) >= 4.5);
        assertTrue(contrast(button.getCurrentTextColor(), 0xffa0e1f5) >= 4.5);
        assertTrue(button.getMinimumHeight() >= Math.round(48 * context.getResources().getDisplayMetrics().density));
        button.performClick();
        assertEquals(1, clicks.get());
    }

    @Test public void dialogStylingPreservesChoiceAndDismissListeners() {
        AtomicInteger selected = new AtomicInteger(-1), shows = new AtomicInteger(), dismisses = new AtomicInteger();
        AlertDialog dialog = GameMenuStyle.builder(activity).setTitle("Perfiles gráficos")
            .setSingleChoiceItems(new String[] {"Ligero", "Equilibrado", "Alta calidad"}, 1,
                (choice, index) -> selected.set(index))
            .setNegativeButton("Cancelar", null).create();
        dialog.setOnShowListener(value -> shows.incrementAndGet());
        dialog.setOnDismissListener(value -> dismisses.incrementAndGet());
        dialog.show();
        GameMenuStyle.style(dialog);
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(1, shows.get());
        assertEquals(1, dialog.getListView().getCheckedItemPosition());
        assertEquals(-1, selected.get());
        clickChoice(dialog, 2);
        assertEquals(2, selected.get());
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        shadowOf(Looper.getMainLooper()).idle();
        assertEquals(1, dismisses.get());
    }

    // Native metrics are needed because legacy graphics measures each character as one pixel.
    @Test @GraphicsMode(GraphicsMode.Mode.NATIVE)
    public void scaledTextInShortViewportWrapsAndKeepsTouchTargets() {
        Configuration configuration = new Configuration(activity.getResources().getConfiguration());
        configuration.fontScale = 1.3f;
        Context context = GameMenuStyle.context(activity.createConfigurationContext(configuration));
        LinearLayout panel = new LinearLayout(context);
        panel.setOrientation(LinearLayout.VERTICAL);
        TextView description = new TextView(context);
        description.setText("Guarda el último punto de control; puedes continuar después desde tus partidas guardadas.");
        description.setTextSize(16);
        Button action = new Button(context);
        action.setText("Guardar y salir");
        panel.addView(description, new LinearLayout.LayoutParams(-1, -2));
        panel.addView(action, new LinearLayout.LayoutParams(-1, -2));
        GameMenuStyle.content(panel);
        panel.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(320, View.MeasureSpec.AT_MOST));
        panel.layout(0, 0, panel.getMeasuredWidth(), panel.getMeasuredHeight());
        assertTrue(description.getLineCount() > 1);
        assertTrue(description.getHeight() - description.getPaddingTop() - description.getPaddingBottom()
            >= description.getLayout().getHeight());
        assertTrue(action.getHeight() >= 48);
        Rect label = new Rect(), button = new Rect();
        description.getHitRect(label);
        action.getHitRect(button);
        assertFalse(Rect.intersects(label, button));
        assertTrue(button.bottom <= 320);
    }

    private double contrast(int first, int second) {
        double one = luminance(first), two = luminance(second);
        return (Math.max(one, two) + .05) / (Math.min(one, two) + .05);
    }

    private double luminance(int color) {
        double[] channels = {Color.red(color) / 255d, Color.green(color) / 255d, Color.blue(color) / 255d};
        for (int i = 0; i < channels.length; i++)
            channels[i] = channels[i] <= .04045 ? channels[i] / 12.92 : Math.pow((channels[i] + .055) / 1.055, 2.4);
        return .2126 * channels[0] + .7152 * channels[1] + .0722 * channels[2];
    }
}
