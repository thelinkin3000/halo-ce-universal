package com.halo.decomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;
import java.util.HashSet;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;

/** Updates the separately signed Controls edition without replacing it with upstream's package. */
final class ControlsUpdater {
    private static final String REPOSITORY = "dorlanpabon/halo-ce-universal";
    private static final String RELEASES = "https://api.github.com/repos/" + REPOSITORY + "/releases?per_page=100";
    static final long MAX_APK_BYTES = 256L * 1024 * 1024;

    static final class Release {
        final int version;
        final long size;
        final String tag, address;
        Release(int version, long size, String tag, String address) {
            this.version = version;
            this.size = size;
            this.tag = tag;
            this.address = address;
        }
    }

    private ControlsUpdater() {}

    static Release selectRelease(String json, long installedVersion) throws JSONException {
        JSONArray releases = new JSONArray(json);
        Release latest = null;
        for (int index = 0; index < releases.length(); index++) {
            JSONObject item = releases.optJSONObject(index);
            if (item == null || item.optBoolean("draft") || item.optBoolean("prerelease"))
                continue;
            String tag = item.optString("tag_name");
            if (!tag.matches("controles-[1-9][0-9]{0,5}"))
                continue;
            int version = 10600 + Integer.parseInt(tag.substring("controles-".length()));
            if (version <= installedVersion || (latest != null && version <= latest.version))
                continue;
            JSONArray assets = item.optJSONArray("assets");
            if (assets == null)
                continue;
            String expected = "https://github.com/" + REPOSITORY + "/releases/download/" + tag + "/halo-controls.apk";
            for (int assetIndex = 0; assetIndex < assets.length(); assetIndex++) {
                JSONObject asset = assets.optJSONObject(assetIndex);
                if (asset == null || !"halo-controls.apk".equals(asset.optString("name")))
                    continue;
                long size = asset.optLong("size", -1);
                if (size > 0 && size <= MAX_APK_BYTES && expected.equals(asset.optString("browser_download_url")))
                    latest = new Release(version, size, tag, expected);
            }
        }
        return latest;
    }

    static void validateArchive(PackageInfo installed, PackageInfo archive, int releaseVersion) throws IOException {
        if (installed == null || archive == null || installed.packageName == null
                || !installed.packageName.endsWith(".controls") || !installed.packageName.equals(archive.packageName))
            throw new IOException("El paquete no corresponde a esta edición de Halo.");
        if (archive.getLongVersionCode() <= installed.getLongVersionCode()
                || archive.getLongVersionCode() != releaseVersion)
            throw new IOException("La versión del APK no coincide con la actualización.");
        Signature[] current = installed.signingInfo == null ? null : installed.signingInfo.getApkContentsSigners();
        Signature[] incoming = archive.signingInfo == null ? null : archive.signingInfo.getApkContentsSigners();
        if (current == null || incoming == null || current.length == 0 || incoming.length != current.length
                || !new HashSet<>(Arrays.asList(current)).equals(new HashSet<>(Arrays.asList(incoming))))
            throw new IOException("La firma del APK es diferente. La actualización se rechazó para proteger tus datos.");
    }

    static void show(Activity activity, Consumer<AlertDialog> openDialog) {
        if (!activity.getPackageName().endsWith(".controls")) {
            message(activity, openDialog, "Esta opción corresponde a la edición Controles. Usa el actualizador de tu edición.");
            return;
        }
        AtomicBoolean canceled = new AtomicBoolean();
        AlertDialog checking = new AlertDialog.Builder(activity).setTitle("Actualizar APK")
            .setMessage("Buscando una actualización de la edición Controles…")
            .setNegativeButton("Cancelar", (dialog, which) -> canceled.set(true))
            .setOnCancelListener(dialog -> canceled.set(true)).create();
        openDialog.accept(checking);
        new Thread(() -> {
            try {
                PackageInfo installed = activity.getPackageManager().getPackageInfo(activity.getPackageName(),
                    PackageManager.GET_SIGNING_CERTIFICATES);
                Release release = selectRelease(ControlsDownload.readText(RELEASES, 2 * 1024 * 1024, canceled),
                    installed.getLongVersionCode());
                activity.runOnUiThread(() -> {
                    if (!available(activity, canceled)) return;
                    checking.dismiss();
                    if (release == null) {
                        message(activity, openDialog, "No hay una actualización más reciente publicada para esta edición.");
                    } else {
                        openDialog.accept(new AlertDialog.Builder(activity).setTitle("Actualización disponible")
                            .setMessage(release.tag + " · " + ((release.size + 1048575) / 1048576)
                                + " MB\nSe comprobarán el paquete y la firma antes de abrir el instalador de Android. Tus datos se conservan.")
                            .setPositiveButton("Descargar", (dialog, which) -> download(activity, openDialog, release))
                            .setNegativeButton("Ahora no", null).create());
                    }
                });
            } catch (Exception error) {
                activity.runOnUiThread(() -> {
                    if (!available(activity, canceled)) return;
                    checking.dismiss();
                    message(activity, openDialog, "No se pudo consultar la actualización. Revisa la conexión e inténtalo de nuevo.");
                });
            }
        }, "controls-update-check").start();
    }

    private static boolean available(Activity activity, AtomicBoolean canceled) {
        return !canceled.get() && !activity.isFinishing() && !activity.isDestroyed();
    }

    private static void download(Activity activity, Consumer<AlertDialog> openDialog, Release release) {
        AtomicBoolean canceled = new AtomicBoolean();
        LinearLayout layout = new LinearLayout(activity);
        layout.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(20 * activity.getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding / 2, padding, 0);
        TextView status = new TextView(activity);
        status.setText("Descargando " + release.tag + "…");
        ProgressBar progress = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        layout.addView(status);
        layout.addView(progress);
        AlertDialog dialog = new AlertDialog.Builder(activity).setTitle("Actualizar APK").setView(layout)
            .setNegativeButton("Cancelar", (view, which) -> canceled.set(true))
            .setOnCancelListener(view -> canceled.set(true)).create();
        openDialog.accept(dialog);
        new Thread(() -> {
            File incoming = null;
            try {
                File directory = new File(activity.getCacheDir(), UpdateProvider.DIRECTORY);
                if (!directory.isDirectory() && !directory.mkdirs())
                    throw new IOException("No se pudo crear la carpeta de actualización.");
                incoming = File.createTempFile("controls-", ".apk", directory);
                long received = ControlsDownload.transfer(release.address, incoming, MAX_APK_BYTES, canceled,
                    (bytes, total) -> activity.runOnUiThread(() -> {
                        if (!available(activity, canceled)) return;
                        progress.setProgress((int) Math.min(1000, bytes * 1000 / release.size));
                        status.setText("Descargando " + release.tag + "… " + (bytes >> 20) + " / " + (release.size >> 20) + " MB");
                    }));
                if (received != release.size)
                    throw new IOException("El tamaño del APK no coincide con la publicación.");
                PackageManager packages = activity.getPackageManager();
                validateArchive(packages.getPackageInfo(activity.getPackageName(), PackageManager.GET_SIGNING_CERTIFICATES),
                    packages.getPackageArchiveInfo(incoming.getAbsolutePath(), PackageManager.GET_SIGNING_CERTIFICATES), release.version);
                if (!available(activity, canceled)) return;
                Files.move(incoming.toPath(), new File(directory, UpdateProvider.APK).toPath(), StandardCopyOption.REPLACE_EXISTING);
                activity.runOnUiThread(() -> {
                    if (!available(activity, canceled)) return;
                    dialog.dismiss();
                    try {
                        Intent install = new Intent(Intent.ACTION_VIEW);
                        install.setDataAndType(Uri.parse("content://" + UpdateProvider.AUTHORITY + "/" + UpdateProvider.APK),
                            "application/vnd.android.package-archive");
                        install.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
                        activity.startActivity(install);
                    } catch (RuntimeException error) {
                        message(activity, openDialog, "No se pudo abrir el instalador de Android. Permite instalar aplicaciones desde Halo e inténtalo de nuevo.");
                    }
                });
            } catch (Exception error) {
                activity.runOnUiThread(() -> {
                    if (!available(activity, canceled)) return;
                    dialog.dismiss();
                    message(activity, openDialog, error instanceof IOException ? error.getMessage()
                        : "No se pudo verificar el APK. Inténtalo de nuevo.");
                });
            } finally {
                if (incoming != null) incoming.delete();
            }
        }, "controls-update-download").start();
    }

    private static void message(Activity activity, Consumer<AlertDialog> openDialog, String message) {
        openDialog.accept(new AlertDialog.Builder(activity).setTitle("Actualizar APK").setMessage(message)
            .setPositiveButton("Aceptar", null).create());
    }
}
