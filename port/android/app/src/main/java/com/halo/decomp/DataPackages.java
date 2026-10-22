package com.halo.decomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.SystemClock;
import android.text.InputType;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.net.URI;
import java.net.URISyntaxException;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;

/** Imports a player's remotely hosted game data without bundling or replacing commercial maps. */
final class DataPackages {
    private static final AtomicBoolean busy = new AtomicBoolean();
    private static final long MAX_DOWNLOAD = 8L * 1024 * 1024 * 1024;

    private DataPackages() {}

    static boolean isBusy() { return busy.get(); }

    static void playDemo(Activity activity, Runnable ready, Consumer<AlertDialog> open) {
        File root = activity.getExternalFilesDir(null);
        if (root == null || busy.get()) {
            message(activity, open, root == null ? "El almacenamiento no está disponible."
                : "Ya hay una descarga de datos en curso.");
        } else if (GameDataPackage.demoAvailable(root)) {
            ready.run();
        } else if (busy.compareAndSet(false, true)) {
            download(activity, root, DemoPackage.URL, ready, open, true);
        }
    }

    static void show(Activity activity, Runnable ready, Consumer<AlertDialog> open) {
        File root = activity.getExternalFilesDir(null);
        if (root == null || new File(root, "maps/ui.map").isFile() || busy.get()) {
            message(activity, open, root == null ? "El almacenamiento no está disponible."
                : busy.get() ? "Ya hay una descarga de datos en curso."
                : "Los datos ya están instalados. Tus mapas y partidas se conservan.");
            return;
        }
        LinearLayout content = column(activity);
        TextView help = new TextView(activity);
        help.setText("Admite ISO/XISO de Halo para Xbox desde MediaFire o un enlace HTTPS directo. "
            + "También admite ZIP con una carpeta maps. Pega el enlace público del archivo de MediaFire "
            + "o su enlace directo. Los datos se instalan en este teléfono; no vienen incluidos en la APK.");
        content.addView(help);
        EditText address = new EditText(activity);
        address.setHint("Enlace de MediaFire o URL HTTPS de ISO/XISO/ZIP");
        address.setSingleLine(true);
        address.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        content.addView(address);
        AlertDialog dialog = new AlertDialog.Builder(activity)
            .setTitle("Descargar paquete de datos").setView(content)
            .setNegativeButton("Cancelar", null).setPositiveButton("Descargar", null).create();
        open.accept(dialog);
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            String url = address.getText().toString().trim();
            if (!validAddress(url)) {
                address.setError("Escribe una URL HTTPS válida, sin usuario ni contraseña.");
                return;
            }
            if (busy.compareAndSet(false, true)) {
                download(activity, root, url, ready, open, false);
                dialog.dismiss();
            }
        });
    }

    static boolean validAddress(String text) {
        try {
            URI uri = new URI(text);
            return "https".equalsIgnoreCase(uri.getScheme()) && uri.getHost() != null
                && !uri.getHost().isEmpty() && uri.getRawUserInfo() == null && uri.getFragment() == null
                && uri.getPort() <= 65535;
        } catch (URISyntaxException e) {
            return false;
        }
    }

    private static void download(Activity activity, File root, String url, Runnable ready,
            Consumer<AlertDialog> open, boolean demo) {
        AtomicBoolean canceled = new AtomicBoolean();
        LinearLayout content = column(activity);
        TextView status = new TextView(activity);
        status.setText(demo ? "Descargando demo OXM desde Digiex · 379 MB. Mantén la aplicación abierta."
            : "Resolviendo enlace… Mantén la aplicación abierta.");
        ProgressBar bar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setIndeterminate(true);
        content.addView(status);
        content.addView(bar);
        AlertDialog progress = new AlertDialog.Builder(activity)
            .setTitle(demo ? "Descargando demo de Halo" : "Descargando datos de Halo").setView(content)
            .setNegativeButton("Cancelar", (d, w) -> canceled.set(true)).create();
        progress.setCanceledOnTouchOutside(false);
        progress.setOnCancelListener(d -> canceled.set(true));
        open.accept(progress);
        new Thread(() -> {
            File archive = new File(activity.getCacheDir(), "game-data.partial");
            long[] lastReport = {0};
            String failure = null;
            boolean installed = false;
            boolean downloaded = false;
            try {
                String source = DataDownloadSource.resolve(url, canceled);
                if (activity.isFinishing() || activity.isDestroyed())
                    canceled.set(true);
                if (canceled.get())
                    throw new InterruptedIOException("Descarga cancelada.");
                ControlsDownload.transfer(source, archive, demo ? DemoPackage.SIZE : MAX_DOWNLOAD, canceled, (done, total) ->
                    report(activity, progress, canceled, status, bar, "Descargando", done, total, lastReport));
                downloaded = true;
                GameDataPackage.Progress installation = (file, done, total) ->
                    report(activity, progress, canceled, status, bar, "Instalando " + file, done, total, lastReport);
                String map;
                if (demo) {
                    lastReport[0] = 0;
                    report(activity, progress, canceled, status, bar, "Verificando demo", 0, DemoPackage.SIZE, lastReport);
                    DemoPackage.verify(archive, canceled);
                    GameDataPackage.installDemo(archive, root, canceled, installation);
                    map = "demob30";
                } else {
                    map = GameDataPackage.install(archive, root, canceled, installation);
                }
                installed = true;
                // The demo's UI cannot select its campaign. An existing player's startup script is never replaced.
                if (!demo && "demob30".equals(map)) {
                    File init = new File(root, "init.txt");
                    if (init.createNewFile()) {
                        try (FileOutputStream out = new FileOutputStream(init)) {
                            out.write("map_name levels\\demob30\\demob30\n".getBytes(StandardCharsets.US_ASCII));
                        }
                    }
                }
            } catch (Exception e) {
                failure = e instanceof IOException ? e.getMessage() : "No se pudo instalar el paquete.";
            } finally {
                archive.delete();
                busy.set(false);
            }
            String error = failure;
            boolean success = installed;
            boolean browserFallback = !downloaded && DataDownloadSource.isMediaFirePage(url);
            activity.runOnUiThread(() -> {
                if (activity.isFinishing() || activity.isDestroyed())
                    return;
                if (success) {
                    if (demo) {
                        progress.dismiss();
                        if (!canceled.get())
                            ready.run();
                        return;
                    }
                    AlertDialog complete = new AlertDialog.Builder(activity)
                        .setTitle("Datos instalados").setMessage(error == null ? "El paquete está listo para jugar."
                            : "Los mapas están instalados, pero no se pudo preparar el inicio de la demo.\n" + error)
                        .setPositiveButton("Jugar", (d, w) -> ready.run()).create();
                    open.accept(complete);
                } else if (!canceled.get()) {
                    downloadFailure(activity, open, error, browserFallback ? url : null);
                }
                progress.dismiss();
            });
        }, "halo game data").start();
    }

    private static void report(Activity activity, AlertDialog progress, AtomicBoolean canceled,
            TextView status, ProgressBar bar, String text, long done, long total, long[] lastReport) {
        if (activity.isFinishing() || activity.isDestroyed()) {
            canceled.set(true);
            return;
        }
        long now = SystemClock.elapsedRealtime();
        if (now - lastReport[0] < 100 && done != total)
            return;
        lastReport[0] = now;
        activity.runOnUiThread(() -> {
            if (!progress.isShowing() || canceled.get())
                return;
            status.setText(text + " · " + (done >> 20) + (total > 0 ? " / " + (total >> 20) : "") + " MB");
            bar.setIndeterminate(total <= 0);
            if (total > 0)
                bar.setProgress((int) (done * 1000 / total));
        });
    }

    private static LinearLayout column(Activity activity) {
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        content.setPadding(padding, padding / 2, padding, 0);
        return content;
    }

    static void downloadFailure(Activity activity, Consumer<AlertDialog> open, String error, String page) {
        AlertDialog.Builder dialog = new AlertDialog.Builder(activity).setTitle("Paquete de datos")
            .setMessage("No se pudo instalar el paquete.\n" + error + "\nPuedes volver a intentarlo.");
        if (DataDownloadSource.isMediaFirePage(page)) {
            dialog.setMessage("MediaFire requiere abrir la descarga en el navegador.\n" + error
                + "\nSi descargaste una ISO/XISO, vuelve a Halo y pulsa «Elegir imagen de disco». "
                + "Para un ZIP, usa su enlace directo en el descargador de Halo.")
                .setPositiveButton("Abrir MediaFire", (d, w) -> {
                    try {
                        activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(page)));
                    } catch (RuntimeException e) {
                        message(activity, open, "No se pudo abrir el navegador. Abre el enlace en tu navegador "
                            + "y después elige la imagen de disco en Halo.");
                    }
                }).setNegativeButton("Cerrar", null);
        } else {
            dialog.setPositiveButton("Aceptar", null);
        }
        open.accept(dialog.create());
    }

    private static void message(Activity activity, Consumer<AlertDialog> open, String text) {
        open.accept(new AlertDialog.Builder(activity).setTitle("Paquete de datos")
            .setMessage(text).setPositiveButton("Aceptar", null).create());
    }
}
