package com.halo.decomp;

import android.app.Activity;
import android.app.AlertDialog;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.function.Consumer;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Android build 106 settings; render scale and the explicit frame cap require the custom GLES guest. */
public final class GraphicsProfiles {
    private static final Pattern SECTION = Pattern.compile("^\\s*\\[(\\[?[^\\]]+)]\\]?\\s*(?:#.*)?$");
    private static final Pattern SETTING = Pattern.compile(
        "^(\\s*)([A-Za-z0-9_-]+|\"[A-Za-z0-9_-]+\"|'[A-Za-z0-9_-]+')(\\s*=\\s*)(.*)$");
    private static final String[] INIT_SETTINGS = {
        "render_shadows", "rasterizer_detail_objects", "rasterizer_environment_reflections",
        "rasterizer_environment_reflection_mirrors", "rasterizer_environment_specular_lights",
        "rasterizer_environment_decals"
    };
    private static final Pattern INIT_SETTING = Pattern.compile(
        "^\\s*(?:((?:" + String.join("|", INIT_SETTINGS) + "))\\s+(true|false|0|1)|\\(\\s*set\\s+((?:"
            + String.join("|", INIT_SETTINGS) + "))\\s+(true|false|0|1)\\s*\\))\\s*$",
        Pattern.CASE_INSENSITIVE);
    private static final Pattern MAP_NAME = Pattern.compile("^\\s*\\(?\\s*map_name\\s+.*", Pattern.CASE_INSENSITIVE);

    private static final class Profile {
        final String name;
        final String description;
        final boolean shadows;
        final boolean reducedEffects;
        final LinkedHashMap<String, String> settings = new LinkedHashMap<>();

        Profile(String name, String description, int width, boolean shadows, int limit, int resolution,
                String aa, boolean interpolation, boolean hd, boolean lighting, boolean reducedEffects,
                float renderScale, int maxFps) {
            this.name = name;
            this.description = description;
            this.shadows = shadows;
            this.reducedEffects = reducedEffects;
            // These scale/cap controls apply only to GLES; quality profiles retain the Vulkan fallback path.
            settings.put("renderer", maxFps > 0 ? "\"gl\"" : "\"vulkan\"");
            settings.put("screen_width", Integer.toString(width));
            settings.put("render_scale", Float.toString(renderScale));
            settings.put("max_fps", Integer.toString(maxFps));
            settings.put("vsync", Boolean.toString(!reducedEffects));
            settings.put("shadow_limit", Integer.toString(limit));
            settings.put("shadow_resolution", Integer.toString(resolution));
            settings.put("anti_aliasing", "\"" + aa + "\"");
            settings.put("interpolation", Boolean.toString(interpolation));
            settings.put("high_res_hud", Boolean.toString(hd));
            settings.put("high_res_text", Boolean.toString(hd));
            settings.put("per_pixel_lighting", Boolean.toString(lighting));
            settings.put("shield_glow", Boolean.toString(lighting));
        }
    }

    private static final Profile[] PROFILES = {
        new Profile("Supremo ligero", "OpenGL ES: 480 × 360 píxeles internos; 640 × 480 lógicos, formato 4:3.\n"
            + "Sin sombras, suavizado ni sincronización vertical; HUD y textos originales.\n"
            + "Sin vegetación de detalle, reflejos, espejos, luces especulares ni marcas en superficies; "
            + "sin iluminación adicional. Interpolación activada con límite de 30 FPS.",
            640, false, 1, 128, "off", true, false, false, true, 0.75f, 30),
        new Profile("Ligero panorámico", "OpenGL ES: 801 × 360 píxeles internos; 1068 × 480 lógicos, panorámico.\n"
            + "Sin sombras ni suavizado; HUD y textos originales; sin iluminación adicional. "
            + "Interpolación y sincronización vertical activadas, con límite de 30 FPS.",
            1068, false, 1, 128, "off", true, false, false, false, 0.75f, 30),
        new Profile("Equilibrado", "1068 × 480, panorámico.\n"
            + "Hasta 4 sombras de 128 píxeles; HUD y textos HD; sin suavizado ni interpolación.",
            1068, true, 4, 128, "off", false, true, false, false, 1.0f, 0),
        new Profile("Alta calidad", "1068 × 480, panorámico.\n"
            + "Hasta 16 sombras de 256 píxeles; FXAA, interpolación, HUD y textos HD, "
            + "iluminación por píxel y resplandor de escudos.",
            1068, true, 16, 256, "fxaa", true, true, true, false, 1.0f, 0),
        new Profile("Fluidez experimental", "OpenGL ES: 320 × 240 píxeles internos; 640 × 480 lógicos, formato 4:3.\n"
            + "Los recortes de Supremo ligero, interpolación activada y sincronización vertical desactivada. "
            + "Límite de 60 FPS, con menor nitidez. "
            + "Frecuencia variable según el dispositivo y la escena.",
            640, false, 1, 128, "off", true, false, false, true, 0.5f, 60)
    };

    private GraphicsProfiles() {}

    public static String activeProfile(Activity activity) {
        File root = activity.getExternalFilesDir(null);
        if (root != null) {
            try {
                int active = detectProfile(read(new File(root, "config.toml")), read(new File(root, "init.txt")));
                if (active >= 0)
                    return PROFILES[active].name;
            } catch (IOException ignored) {
                return "No disponible";
            }
        }
        return "Personalizado";
    }

    public static void show(Activity activity, Runnable restart, Consumer<AlertDialog> open) {
        File root = activity.getExternalFilesDir(null);
        if (root == null) {
            error(activity, open, "El almacenamiento del juego no está disponible.");
            return;
        }
        final int active;
        try {
            active = detectProfile(read(new File(root, "config.toml")), read(new File(root, "init.txt")));
        } catch (IOException e) {
            error(activity, open, "No se pudo leer la configuración: " + e.getMessage());
            return;
        }
        int[] selected = {active >= 0 ? active : 1};
        String[] choices = new String[PROFILES.length];
        for (int i = 0; i < PROFILES.length; i++)
            choices[i] = PROFILES[i].name;
        open.accept(GameMenuStyle.builder(activity)
            .setTitle("Perfiles gráficos · " + (active >= 0 ? PROFILES[active].name : "Personalizado"))
            .setSingleChoiceItems(choices, selected[0], (dialog, which) -> selected[0] = which)
            .setPositiveButton("Revisar", (dialog, which) -> confirm(activity, root, selected[0], restart, open))
            .setNegativeButton("Cancelar", null)
            .create());
    }

    private static void confirm(Activity activity, File root, int index, Runnable restart, Consumer<AlertDialog> open) {
        Profile profile = PROFILES[index];
        open.accept(GameMenuStyle.builder(activity)
            .setTitle("Aplicar «" + profile.name + "»")
            .setMessage(profile.description + "\n\nRequiere reiniciar el juego. "
                + "Se perderá el avance desde el último punto de control.")
            .setPositiveButton("Aplicar y reiniciar", (dialog, which) -> save(activity, root, index, restart, open))
            .setNegativeButton("Cancelar", null)
            .create());
    }

    private static void save(Activity activity, File root, int index, Runnable restart, Consumer<AlertDialog> open) {
        AlertDialog saving = GameMenuStyle.builder(activity)
            .setMessage("Guardando el perfil gráfico…")
            .setCancelable(false)
            .create();
        open.accept(saving);
        new Thread(() -> {
            try {
                applyFiles(root, index);
                activity.runOnUiThread(() -> {
                    if (!activity.isFinishing() && !activity.isDestroyed())
                        restart.run();
                    saving.dismiss();
                });
            } catch (IOException e) {
                activity.runOnUiThread(() -> {
                    if (!activity.isFinishing() && !activity.isDestroyed())
                        error(activity, open, "No se pudo guardar el perfil: " + e.getMessage());
                    saving.dismiss();
                });
            }
        }, "halo-graphics-profile").start();
    }

    private static void error(Activity activity, Consumer<AlertDialog> open, String message) {
        open.accept(GameMenuStyle.builder(activity).setTitle("Perfil gráfico")
            .setMessage(message).setPositiveButton("Cerrar", null).create());
    }

    static synchronized void applyFiles(File root, int index) throws IOException {
        File config = new File(root, "config.toml");
        File init = new File(root, "init.txt");
        boolean hadConfig = config.exists();
        String oldConfig = read(config);
        String oldInit = read(init);
        backup(config, oldConfig);
        backup(init, oldInit);
        writeAtomic(config, updateConfig(oldConfig, index));
        try {
            writeAtomic(init, updateInit(oldInit, index));
        } catch (IOException e) {
            try {
                if (hadConfig)
                    writeAtomic(config, oldConfig);
                else
                    Files.deleteIfExists(config.toPath());
            } catch (IOException rollback) {
                e.addSuppressed(rollback);
            }
            throw e;
        }
    }

    static String updateConfig(String text, int index) {
        Map<String, String> settings = new LinkedHashMap<>(PROFILES[index].settings);
        if (displaySettings(text).containsKey("resolution_limit"))
            settings.put("resolution_limit", "\"480\"");
        return updateConfig(text, settings);
    }

    static String updateConfig(String text, Map<String, String> settings) {
        Map<String, String> missing = new LinkedHashMap<>(settings);
        String newline = text.contains("\r\n") ? "\r\n" : "\n";
        StringBuilder out = new StringBuilder();
        boolean display = false, found = false;
        String multiline = null;
        for (String line : text.split("(?<=\n)", -1)) {
            String body = line.replaceFirst("\\r?\\n$", "");
            String previous = multiline;
            multiline = multilineAfter(body, multiline);
            if (previous != null) {
                out.append(line);
                continue;
            }
            Matcher section = SECTION.matcher(body);
            if (section.matches()) {
                if (display)
                    appendSettings(out, missing, newline);
                display = unquote(section.group(1).trim()).equals("display");
                found |= display;
            } else if (display) {
                Matcher setting = SETTING.matcher(body);
                if (setting.matches()) {
                    String key = unquote(setting.group(2));
                    String value = settings.get(key);
                    if (value != null) {
                        int comment = commentStart(setting.group(4));
                        out.append(setting.group(1)).append(setting.group(2)).append(setting.group(3)).append(value);
                        if (comment >= 0)
                            out.append(' ').append(setting.group(4).substring(comment));
                        out.append(line.substring(body.length()));
                        missing.remove(key);
                        continue;
                    }
                }
            }
            out.append(line);
        }
        if (!found) {
            finishLine(out, newline);
            out.append("[display]").append(newline);
        }
        appendSettings(out, missing, newline);
        return out.toString();
    }

    static String updateInit(String text, int index) {
        String newline = text.contains("\r\n") ? "\r\n" : "\n";
        StringBuilder settings = new StringBuilder();
        for (String key : INIT_SETTINGS)
            settings.append(key).append(' ').append(key.equals("render_shadows")
                ? PROFILES[index].shadows : !PROFILES[index].reducedEffects).append(newline);
        StringBuilder out = new StringBuilder();
        boolean inserted = false;
        for (String line : text.split("(?<=\n)", -1)) {
            String body = line.trim();
            if (INIT_SETTING.matcher(body).matches())
                continue;
            if (!inserted && MAP_NAME.matcher(body).matches()) {
                out.append(settings);
                inserted = true;
            }
            out.append(line);
        }
        if (!inserted)
            out.insert(0, settings);
        return out.toString();
    }

    static int detectProfile(String config, String init) {
        Map<String, String> values = new LinkedHashMap<>();
        values.put("vsync", "true");
        values.put("render_scale", "1.0");
        values.put("max_fps", "0");
        values.putAll(displaySettings(config));
        if (values.containsKey("resolution_limit") && !unquote(values.get("resolution_limit")).equals("480"))
            return -1;
        Map<String, Boolean> commands = new LinkedHashMap<>();
        for (String key : INIT_SETTINGS)
            commands.put(key, true);
        for (String line : init.split("\\r?\\n")) {
            Matcher assignment = INIT_SETTING.matcher(line);
            if (assignment.matches()) {
                String key = assignment.group(1) != null ? assignment.group(1) : assignment.group(3);
                String value = assignment.group(1) != null ? assignment.group(2) : assignment.group(4);
                commands.put(key.toLowerCase(java.util.Locale.ROOT), value.equalsIgnoreCase("true") || value.equals("1"));
            }
        }
        for (int i = 0; i < PROFILES.length; i++) {
            boolean effectsMatch = true;
            for (String key : INIT_SETTINGS)
                effectsMatch &= commands.get(key) == (key.equals("render_shadows")
                    ? PROFILES[i].shadows : !PROFILES[i].reducedEffects);
            if (effectsMatch && values.entrySet().containsAll(PROFILES[i].settings.entrySet()))
                return i;
        }
        return -1;
    }

    static Map<String, String> displaySettings(String config) {
        Map<String, String> values = new LinkedHashMap<>();
        boolean display = false;
        String multiline = null;
        for (String line : config.split("\\r?\\n")) {
            String previous = multiline;
            multiline = multilineAfter(line, multiline);
            if (previous != null)
                continue;
            Matcher section = SECTION.matcher(line);
            if (section.matches()) {
                display = unquote(section.group(1).trim()).equals("display");
            } else if (display) {
                Matcher setting = SETTING.matcher(line);
                if (setting.matches()) {
                    String value = setting.group(4);
                    int comment = commentStart(value);
                    values.put(unquote(setting.group(2)), (comment >= 0 ? value.substring(0, comment) : value).trim());
                }
            }
        }
        return values;
    }

    private static String unquote(String value) {
        if (value.length() > 1 && ((value.startsWith("\"") && value.endsWith("\""))
            || (value.startsWith("'") && value.endsWith("'"))))
            return value.substring(1, value.length() - 1);
        return value;
    }

    private static int commentStart(String value) {
        char quote = 0;
        boolean escaped = false;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (escaped) {
                escaped = false;
            } else if (quote == '"' && c == '\\') {
                escaped = true;
            } else if (quote != 0) {
                if (c == quote)
                    quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '#') {
                return i;
            }
        }
        return -1;
    }

    private static String multilineAfter(String line, String delimiter) {
        for (int i = 0; i < line.length(); i++) {
            if (delimiter != null) {
                int end = line.indexOf(delimiter, i);
                if (end < 0)
                    return delimiter;
                i = end + 2;
                delimiter = null;
            } else {
                char quote = line.charAt(i);
                if (quote == '#')
                    break;
                if (quote != '"' && quote != '\'')
                    continue;
                if (i + 2 < line.length() && line.charAt(i + 1) == quote && line.charAt(i + 2) == quote) {
                    delimiter = line.substring(i, i + 3);
                    i += 2;
                } else {
                    while (++i < line.length() && line.charAt(i) != quote) {
                        if (quote == '"' && line.charAt(i) == '\\')
                            i++;
                    }
                }
            }
        }
        return delimiter;
    }

    private static void appendSettings(StringBuilder out, Map<String, String> values, String newline) {
        if (!values.isEmpty())
            finishLine(out, newline);
        for (Map.Entry<String, String> entry : values.entrySet())
            out.append(entry.getKey()).append(" = ").append(entry.getValue()).append(newline);
        values.clear();
    }

    private static void finishLine(StringBuilder out, String newline) {
        if (out.length() > 0 && out.charAt(out.length() - 1) != '\n')
            out.append(newline);
    }

    static String read(File file) throws IOException {
        return file.exists() ? new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8) : "";
    }

    private static void backup(File file, String text) throws IOException {
        File backup = new File(file.getParentFile(), file.getName() + ".before-profiles");
        if (file.exists() && !backup.exists())
            writeAtomic(backup, text);
    }

    static void writeAtomic(File file, String text) throws IOException {
        File temporary = File.createTempFile(file.getName() + ".profiles-", ".tmp", file.getParentFile());
        try {
            try (FileOutputStream out = new FileOutputStream(temporary)) {
                out.write(text.getBytes(StandardCharsets.UTF_8));
                out.getFD().sync();
            }
            try {
                Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(temporary.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
        } finally {
            Files.deleteIfExists(temporary.toPath());
        }
    }
}
