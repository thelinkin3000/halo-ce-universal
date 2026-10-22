package com.halo.decomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.util.DisplayMetrics;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;

/** Changes image format independently of internal rendering detail. */
public final class ScreenSettings {
    static final class Selection {
        final int width;
        final float scale;
        final String resolutionLimit;
        Selection(int width, float scale) { this(width, scale, null); }
        Selection(int width, float scale, String resolutionLimit) {
            this.width = width;
            this.scale = scale;
            this.resolutionLimit = resolutionLimit;
        }
    }

    private ScreenSettings() {}

    public static void show(Activity activity, Runnable restart, Consumer<AlertDialog> open) {
        File root = activity.getExternalFilesDir(null);
        final Selection current;
        final boolean needsGles;
        try {
            if (root == null)
                throw new IOException("El almacenamiento del juego no está disponible.");
            String config = GraphicsProfiles.read(new File(root, "config.toml"));
            current = readSelection(config);
            needsGles = !GraphicsProfiles.displaySettings(config).getOrDefault("renderer", "vulkan")
                .replace("\"", "").replace("'", "").equals("gl");
        } catch (IOException e) {
            error(activity, open, e);
            return;
        }
        List<Integer> widths = new ArrayList<>(Arrays.asList(0, 854, 640));
        List<String> formats = new ArrayList<>(Arrays.asList(
            "Pantalla completa (recomendado)", "Panorámica 16:9", "Clásica 4:3"));
        if (!widths.contains(current.width)) {
            widths.add(current.width);
            formats.add("Actual · " + current.width + " × 480");
        }
        List<Float> scales = new ArrayList<>(Arrays.asList(1.0f, 0.75f, 0.5f));
        List<String> limits = new ArrayList<>(Arrays.asList(current.resolutionLimit == null ? null : "480",
            current.resolutionLimit == null ? null : "480", current.resolutionLimit == null ? null : "480"));
        List<String> qualities = new ArrayList<>(Arrays.asList(
            "Original · 480p", "Ligera · 360p", "Mínima · 240p"));
        if (current.resolutionLimit != null) {
            scales.add(0, 1.0f);
            limits.add(0, "native");
            qualities.add(0, "Nativa · pantalla del teléfono");
        }
        int currentQuality = -1;
        for (int i = 0; i < scales.size(); i++) {
            if (scales.get(i) == current.scale && java.util.Objects.equals(limits.get(i), current.resolutionLimit))
                currentQuality = i;
        }
        if (currentQuality < 0) {
            currentQuality = scales.size();
            scales.add(current.scale);
            limits.add(current.resolutionLimit);
            qualities.add("Actual · " + (current.resolutionLimit == null
                ? Math.round(current.scale * 480) + "p" : current.resolutionLimit + " × " + current.scale));
        }
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        content.setPadding(padding, 0, padding, padding / 2);
        Spinner format = spinner(activity, content, "Formato de pantalla", formats);
        Spinner quality = spinner(activity, content, "Resolución interna", qualities);
        format.setSelection(widths.indexOf(current.width));
        quality.setSelection(currentQuality);
        TextView preview = new TextView(activity);
        preview.setPadding(0, padding / 2, 0, 0);
        content.addView(preview);
        TextView help = new TextView(activity);
        help.setText("Pantalla completa aprovecha el ancho del teléfono. Bajar la resolución reduce la nitidez, "
            + "sin hacer más pequeña la imagen, y puede mejorar el rendimiento.\n"
            + "Los perfiles gráficos pueden cambiar estos ajustes.");
        help.setPadding(0, padding / 2, 0, 0);
        content.addView(help);
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(content);
        DisplayMetrics metrics = activity.getResources().getDisplayMetrics();
        View decor = activity.getWindow().getDecorView();
        int displayWidth = decor.getWidth() > 0 ? decor.getWidth() : metrics.widthPixels;
        int displayHeight = decor.getHeight() > 0 ? decor.getHeight() : metrics.heightPixels;
        AlertDialog dialog = GameMenuStyle.builder(activity)
            .setTitle("Pantalla y resolución")
            .setView(scroll)
            .setNegativeButton("Cancelar", null)
            .setNeutralButton("Restablecer", null)
            .setPositiveButton("Revisar", (d, which) -> {
                int width = widths.get(format.getSelectedItemPosition());
                float scale = scales.get(quality.getSelectedItemPosition());
                confirm(activity, root, width, scale, limits.get(quality.getSelectedItemPosition()),
                    preview.getText().toString(), restart, open);
            }).create();
        Runnable update = () -> {
            int width = widths.get(format.getSelectedItemPosition());
            float scale = scales.get(quality.getSelectedItemPosition());
            String limit = limits.get(quality.getSelectedItemPosition());
            float actualScale = effectiveScale(new Selection(width, scale, limit), displayWidth, displayHeight);
            preview.setText("Resolución interna: " + pixelWidth(width, actualScale, displayWidth, displayHeight)
                + " × " + Math.round(480 * actualScale));
            if (dialog.isShowing())
                dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(width != current.width
                    || scale != current.scale || !java.util.Objects.equals(limit, current.resolutionLimit)
                    || (scale < 1.0f && needsGles));
        };
        AdapterView.OnItemSelectedListener listener = new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View view, int position, long id) { update.run(); }
            @Override public void onNothingSelected(AdapterView<?> parent) { }
        };
        format.setOnItemSelectedListener(listener);
        quality.setOnItemSelectedListener(listener);
        dialog.setOnShowListener(d -> {
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(view -> {
                format.setSelection(0);
                quality.setSelection(0);
                update.run();
            });
            update.run();
        });
        open.accept(dialog);
    }

    private static Spinner spinner(Activity activity, LinearLayout content, String label, List<String> choices) {
        TextView title = new TextView(activity);
        title.setText(label);
        content.addView(title);
        Context themed = GameMenuStyle.builder(activity).getContext();
        Spinner spinner = new Spinner(themed);
        spinner.setId(View.generateViewId());
        spinner.setContentDescription(label);
        title.setLabelFor(spinner.getId());
        ArrayAdapter<String> adapter = new ArrayAdapter<>(themed, android.R.layout.simple_spinner_item, choices);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        content.addView(spinner, new LinearLayout.LayoutParams(-1, -2));
        return spinner;
    }

    private static void confirm(Activity activity, File root, int width, float scale, String limit, String preview,
            Runnable restart, Consumer<AlertDialog> open) {
        open.accept(GameMenuStyle.builder(activity).setTitle("Aplicar pantalla y resolución")
            .setMessage(preview + ".\n\nRequiere reiniciar el juego. Se perderá el avance desde el último punto de control.")
            .setNegativeButton("Cancelar", null)
            .setPositiveButton("Aplicar y reiniciar", (dialog, which) -> save(activity, root, width, scale, limit, restart, open))
            .create());
    }

    private static void save(Activity activity, File root, int width, float scale, String limit,
            Runnable restart, Consumer<AlertDialog> open) {
        AlertDialog saving = GameMenuStyle.builder(activity).setMessage("Guardando la resolución…")
            .setCancelable(false).create();
        open.accept(saving);
        new Thread(() -> {
            try {
                applyFiles(root, width, scale, limit);
                activity.runOnUiThread(() -> {
                    if (!activity.isFinishing() && !activity.isDestroyed()) restart.run();
                    saving.dismiss();
                });
            } catch (IOException e) {
                activity.runOnUiThread(() -> {
                    if (!activity.isFinishing() && !activity.isDestroyed()) error(activity, open, e);
                    saving.dismiss();
                });
            }
        }, "halo-screen-settings").start();
    }

    private static void error(Activity activity, Consumer<AlertDialog> open, IOException error) {
        open.accept(GameMenuStyle.builder(activity).setTitle("Pantalla y resolución")
            .setMessage("No se pudo guardar o leer la resolución: " + error.getMessage())
            .setPositiveButton("Cerrar", null).create());
    }

    static Selection readSelection(String config) {
        Map<String, String> values = GraphicsProfiles.displaySettings(config);
        int width = 0;
        float scale = 1.0f;
        try { width = Integer.parseInt(values.getOrDefault("screen_width", "0").replace("_", "")); }
        catch (NumberFormatException ignored) { }
        try { scale = Float.parseFloat(values.getOrDefault("render_scale", "1.0").replace("_", "")); }
        catch (NumberFormatException ignored) { }
        width = width <= 0 ? 0 : Math.max(640, Math.min(1600, width)) & ~1;
        scale = Float.isFinite(scale) ? Math.max(0.5f, Math.min(1.0f, scale)) : 1.0f;
        String limit = values.get("resolution_limit");
        if (limit != null)
            limit = limit.replace("\"", "").replace("'", "");
        return new Selection(width, scale, limit);
    }

    static float effectiveScale(Selection selection, int displayWidth, int displayHeight) {
        float scale = Float.isFinite(selection.scale)
            ? Math.max(0.5f, Math.min(1.0f, selection.scale)) : 1.0f;
        int height = Math.min(displayWidth, displayHeight);
        if (selection.resolutionLimit == null || height < 480)
            return scale;
        int lines = height;
        if (!selection.resolutionLimit.equals("native")) {
            try { lines = Integer.parseInt(selection.resolutionLimit); }
            catch (NumberFormatException ignored) { lines = 480; }
            if (lines < 480 || lines > 4320)
                lines = 480;
        }
        return Math.min(lines, height) / 480.0f * scale;
    }

    static int pixelWidth(int width, float scale, int displayWidth, int displayHeight) {
        if (width <= 0) {
            int shortest = Math.min(displayWidth, displayHeight);
            width = shortest > 0 ? (int)Math.min(1600L, 480L * Math.max(displayWidth, displayHeight) / shortest) : 640;
        }
        width = Math.max(640, Math.min(1600, width)) & ~1;
        return Math.round(width * scale);
    }

    static synchronized void applyFiles(File root, int width, float scale) throws IOException {
        String old = GraphicsProfiles.read(new File(root, "config.toml"));
        applyFiles(root, width, scale, GraphicsProfiles.displaySettings(old).containsKey("resolution_limit")
            ? "480" : null);
    }

    static synchronized void applyFiles(File root, int width, float scale, String limit) throws IOException {
        if ((width != 0 && (width < 640 || width > 1600 || (width & 1) != 0))
                || !Float.isFinite(scale) || scale < 0.5f || scale > 1.0f)
            throw new IOException("Resolución no válida.");
        if (limit != null && !limit.equals("native")) {
            try {
                int lines = Integer.parseInt(limit);
                if (lines < 480 || lines > 4320)
                    throw new NumberFormatException();
            } catch (NumberFormatException error) {
                throw new IOException("Límite de resolución no válido.", error);
            }
        }
        File file = new File(root, "config.toml");
        String old = GraphicsProfiles.read(file);
        File backup = new File(root, "config.toml.before-resolution");
        if (file.exists() && !backup.exists())
            GraphicsProfiles.writeAtomic(backup, old);
        Map<String, String> settings = new LinkedHashMap<>();
        settings.put("screen_width", Integer.toString(width));
        settings.put("render_scale", Float.toString(scale));
        if (limit != null)
            settings.put("resolution_limit", "\"" + limit + "\"");
        if (scale < 1.0f)
            settings.put("renderer", "\"gl\"");
        GraphicsProfiles.writeAtomic(file, GraphicsProfiles.updateConfig(old, settings));
    }
}
