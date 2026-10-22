package com.halo.decomp;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.atomic.AtomicBoolean;

/** Bounded HTTPS transfers shared by the data importer and this edition's updater. */
final class ControlsDownload {
    interface Progress { void update(long received, long total); }
    interface Connections { HttpURLConnection open(URL url) throws IOException; }
    private static final Connections NETWORK = url -> (HttpURLConnection) url.openConnection();

    private ControlsDownload() {}

    static URL validateUrl(String address) throws IOException {
        URL url = new URL(address);
        if (!"https".equalsIgnoreCase(url.getProtocol()) || url.getHost().isEmpty() || url.getUserInfo() != null)
            throw new IOException("La descarga necesita una dirección HTTPS sin credenciales.");
        return url;
    }

    private static void checkCanceled(AtomicBoolean canceled) throws InterruptedIOException {
        if (canceled.get() || Thread.currentThread().isInterrupted())
            throw new InterruptedIOException("Descarga cancelada.");
    }

    private static HttpURLConnection open(String address, AtomicBoolean canceled, Connections connections)
            throws IOException {
        URL url = validateUrl(address);
        for (int redirects = 0; redirects <= 5; redirects++) {
            checkCanceled(canceled);
            HttpURLConnection connection = connections.open(url);
            try {
                connection.setConnectTimeout(20_000);
                connection.setReadTimeout(20_000);
                connection.setInstanceFollowRedirects(false);
                connection.setRequestProperty("User-Agent", "OpenCE-Controls");
                connection.setRequestProperty("Accept-Encoding", "identity");
                int code = connection.getResponseCode();
                if (code == HttpURLConnection.HTTP_OK)
                    return connection;
                if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                    String location = connection.getHeaderField("Location");
                    if (location == null || redirects == 5)
                        throw new IOException("La redirección de descarga no es válida.");
                    url = validateUrl(new URL(url, location).toString());
                } else {
                    throw new IOException("El servidor respondió HTTP " + code + ".");
                }
            } catch (IOException | RuntimeException error) {
                connection.disconnect();
                throw error;
            }
            connection.disconnect();
        }
        throw new IOException("Demasiadas redirecciones.");
    }

    static String readText(String address, int maxBytes, AtomicBoolean canceled) throws IOException {
        return readText(address, maxBytes, canceled, NETWORK);
    }

    static String readText(String address, int maxBytes, AtomicBoolean canceled, Connections connections)
            throws IOException {
        if (maxBytes <= 0)
            throw new IOException("Límite de descarga no válido.");
        HttpURLConnection connection = open(address, canceled, connections);
        try {
            long total = connection.getContentLengthLong();
            if (total > maxBytes)
                throw new IOException("La respuesta supera el límite permitido.");
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            try (InputStream input = connection.getInputStream()) {
                copy(input, output, total, maxBytes, canceled, (received, length) -> {});
            }
            return new String(output.toByteArray(), StandardCharsets.UTF_8);
        } finally {
            connection.disconnect();
        }
    }

    static long transfer(String address, File destination, long maxBytes, AtomicBoolean canceled, Progress progress)
            throws IOException {
        return transfer(address, destination, maxBytes, canceled, progress, NETWORK);
    }

    static long transfer(String address, File destination, long maxBytes, AtomicBoolean canceled, Progress progress,
            Connections connections) throws IOException {
        if (maxBytes <= 0)
            throw new IOException("Límite de descarga no válido.");
        File parent = destination.getAbsoluteFile().getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs()))
            throw new IOException("No se pudo crear la carpeta de descarga.");
        File partial = File.createTempFile("download-", ".partial", parent);
        HttpURLConnection connection = null;
        try {
            connection = open(address, canceled, connections);
            String type = connection.getContentType();
            if (type != null) {
                type = type.split(";", 2)[0].trim();
                if (type.equalsIgnoreCase("text/html") || type.equalsIgnoreCase("application/xhtml+xml"))
                    throw new IOException("El enlace devuelve una página web, no un archivo. "
                        + "Usa un enlace directo o una página pública de MediaFire.");
            }
            long total = connection.getContentLengthLong();
            if (total > maxBytes)
                throw new IOException("El paquete supera el tamaño permitido.");
            long received;
            try (InputStream input = connection.getInputStream(); FileOutputStream output = new FileOutputStream(partial)) {
                received = copy(input, output, total, maxBytes, canceled, progress);
                output.getFD().sync();
            }
            checkCanceled(canceled);
            Files.move(partial.toPath(), destination.toPath(), StandardCopyOption.REPLACE_EXISTING);
            return received;
        } finally {
            if (connection != null)
                connection.disconnect();
            partial.delete();
        }
    }

    private static long copy(InputStream input, java.io.OutputStream output, long total, long limit,
            AtomicBoolean canceled, Progress progress) throws IOException {
        byte[] buffer = new byte[32 * 1024];
        long received = 0;
        long lastProgressBytes = 0, lastProgressTime = 0;
        while (true) {
            checkCanceled(canceled);
            int count = input.read(buffer);
            if (count < 0)
                break;
            if (count == 0)
                continue;
            if (received > limit - count)
                throw new IOException("El paquete supera el tamaño permitido.");
            output.write(buffer, 0, count);
            received += count;
            long now = System.nanoTime();
            if (received == total || received - lastProgressBytes >= 256 * 1024
                    || now - lastProgressTime >= 100_000_000L) {
                progress.update(received, total);
                lastProgressBytes = received;
                lastProgressTime = now;
            }
        }
        checkCanceled(canceled);
        if (total >= 0 && received != total)
            throw new IOException("La descarga quedó incompleta.");
        if (received != lastProgressBytes)
            progress.update(received, total);
        checkCanceled(canceled);
        return received;
    }
}
