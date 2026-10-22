package com.halo.decomp;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Installs only Xbox maps from the player's own archive, without replacing existing data. */
final class GameDataPackage {
    private static final long MAX_FILE = 512L << 20;
    private static final long MAX_TOTAL = 2L << 30;
    private static final int MAX_FILES = 256;
    private static final int HEADER_SIZE = 0x800;

    interface Progress {
        void report(String file, long done, long total);
    }

    private static final class ExtractionStopped extends RuntimeException {
        final IOException reason;
        ExtractionStopped(IOException reason) { this.reason = reason; }
    }

    /** Returns a playable map's plain basename; never imports configuration or save files. */
    static synchronized String install(File archive, File dataRoot, AtomicBoolean canceled, Progress progress)
        throws IOException {
        checkCanceled(canceled);
        File root = dataRoot.getCanonicalFile();
        if (!root.isDirectory())
            throw new IOException("No se encuentra la carpeta del juego.");
        File target = new File(root, "maps");
        requireEmpty(target);
        File stage = Files.createTempDirectory(root.toPath(), ".halo-data-").toFile();
        try {
            byte[] signature = new byte[4];
            try (FileInputStream input = new FileInputStream(archive)) {
                if (input.read(signature) != signature.length)
                    throw new IOException("El paquete está incompleto.");
            }
            if (signature[0] == 'P' && signature[1] == 'K') {
                extractZip(archive, stage, canceled, progress);
            } else {
                extractIso(archive, stage, canceled, progress);
            }
            File maps = new File(stage, "maps");
            String preferred = validateMaps(maps, canceled);
            checkCanceled(canceled);
            requireEmpty(target);
            boolean hadEmptyDirectory = target.exists();
            if (hadEmptyDirectory && !target.delete())
                throw new IOException("No se pudo preparar la carpeta de mapas.");
            if (!maps.renameTo(target)) {
                if (hadEmptyDirectory)
                    target.mkdir();
                throw new IOException("No se pudieron instalar los mapas.");
            }
            return preferred;
        } finally {
            deleteStage(stage);
        }
    }

    static boolean demoAvailable(File dataRoot) {
        try {
            File maps = new File(dataRoot.getCanonicalFile(), "maps");
            requirePlainMaps(maps);
            for (String name : new String[] { "ui.map", "demob30.map" }) {
                File map = new File(maps, name);
                if (!Files.isRegularFile(map.toPath(), LinkOption.NOFOLLOW_LINKS))
                    return false;
                validateHeader(map);
            }
            return true;
        } catch (IOException error) {
            return false;
        }
    }

    /** Adds only missing demo maps; retail maps, settings and saves are never replaced. */
    static synchronized void installDemo(File archive, File dataRoot, AtomicBoolean canceled, Progress progress)
            throws IOException {
        checkCanceled(canceled);
        File root = dataRoot.getCanonicalFile();
        if (!root.isDirectory())
            throw new IOException("No se encuentra la carpeta del juego.");
        File target = new File(root, "maps");
        requirePlainMaps(target);
        File stage = Files.createTempDirectory(root.toPath(), ".halo-data-").toFile();
        List<File> published = new ArrayList<>();
        try {
            extractZip(archive, stage, canceled, progress);
            File maps = new File(stage, "maps");
            validateMaps(maps, canceled);
            if (!new File(maps, "demob30.map").isFile())
                throw new IOException("El paquete no contiene la demo de Xbox.");
            requirePlainMaps(target);
            if (!target.isDirectory() && !target.mkdir())
                throw new IOException("No se pudo crear la carpeta de mapas.");
            for (String name : new String[] { "demob30.map", "loading.tga", "ui.map" }) {
                checkCanceled(canceled);
                File source = new File(maps, name);
                if (!source.isFile())
                    continue;
                File destination = new File(target, name);
                if (Files.exists(destination.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                    if (!Files.isRegularFile(destination.toPath(), LinkOption.NOFOLLOW_LINKS))
                        throw new IOException("Hay un archivo de demo no válido. Se conservó sin reemplazarlo.");
                    if (name.endsWith(".map"))
                        validateHeader(destination);
                    continue;
                }
                Files.move(source.toPath(), destination.toPath());
                published.add(destination);
            }
            checkCanceled(canceled);
        } catch (IOException | RuntimeException error) {
            for (File file : published)
                Files.deleteIfExists(file.toPath());
            throw error;
        } finally {
            deleteStage(stage);
        }
    }

    private static void requirePlainMaps(File target) throws IOException {
        if (Files.isSymbolicLink(target.toPath()) || !target.getCanonicalFile().equals(target.getAbsoluteFile())
                || target.exists() && !target.isDirectory())
            throw new IOException("La carpeta de mapas no es válida.");
    }

    private static void requireEmpty(File target) throws IOException {
        if (Files.isSymbolicLink(target.toPath()) || !target.getCanonicalFile().equals(target.getAbsoluteFile()))
            throw new IOException("La carpeta de mapas no es válida.");
        if (!target.exists())
            return;
        String[] children = target.list();
        if (children == null || children.length != 0)
            throw new IOException("Ya hay datos del juego. Se conservaron los mapas y las partidas actuales.");
    }

    private static void checkCanceled(AtomicBoolean canceled) throws InterruptedIOException {
        if (canceled.get() || Thread.currentThread().isInterrupted())
            throw new InterruptedIOException("Descarga o instalación cancelada.");
    }

    private static String[] safePath(String name, boolean directory) throws IOException {
        if (name.isEmpty() || name.startsWith("/") || name.indexOf('\\') >= 0)
            throw new IOException("El ZIP contiene una ruta no válida.");
        if (directory && name.endsWith("/"))
            name = name.substring(0, name.length() - 1);
        String[] parts = name.split("/", -1);
        if (parts.length > 32)
            throw new IOException("El ZIP contiene una ruta demasiado larga.");
        for (String part : parts) {
            if (!XisoExtractor.isPlainName(part))
                throw new IOException("El ZIP contiene una ruta no válida.");
        }
        return parts;
    }

    private static String selectedName(String name) throws IOException {
        String lower = name.toLowerCase(Locale.ROOT);
        if (!lower.endsWith(".map") && !lower.equals("loading.tga"))
            return null;
        if (!lower.matches("[a-z0-9_][a-z0-9_.-]*\\.map") && !lower.equals("loading.tga"))
            throw new IOException("El paquete contiene un nombre de mapa no válido.");
        return lower.equals("demoui.map") ? "ui.map" : lower;
    }

    private static void extractZip(File archive, File stage, AtomicBoolean canceled, Progress progress)
        throws IOException {
        File maps = new File(stage, "maps");
        if (!maps.mkdir())
            throw new IOException("No se pudo preparar la instalación.");
        try (ZipFile zip = new ZipFile(archive)) {
            List<ZipEntry> selected = new ArrayList<>();
            Set<String> names = new HashSet<>();
            String mapDirectory = null;
            long total = 0, expanded = 0;
            int files = 0, entries = 0;
            Enumeration<? extends ZipEntry> enumeration = zip.entries();
            while (enumeration.hasMoreElements()) {
                checkCanceled(canceled);
                ZipEntry entry = enumeration.nextElement();
                if (++entries > 4096)
                    throw new IOException("El ZIP contiene demasiadas entradas.");
                String[] parts = safePath(entry.getName(), entry.isDirectory());
                if (entry.isDirectory())
                    continue;
                if (++files > MAX_FILES || entry.getSize() < 0 || entry.getSize() > MAX_FILE)
                    throw new IOException("El paquete supera los límites de tamaño o archivos.");
                expanded += entry.getSize();
                if (expanded > MAX_TOTAL)
                    throw new IOException("El paquete supera 2 GB descomprimidos.");
                int at = -1;
                for (int index = 0; index < parts.length - 1; index++) {
                    if (parts[index].equalsIgnoreCase("maps")) {
                        at = index;
                        break;
                    }
                }
                if (at < 0 || at != parts.length - 2)
                    continue;
                String name = selectedName(parts[parts.length - 1]);
                if (name == null)
                    continue;
                String prefix = entry.getName().substring(0,
                    entry.getName().length() - parts[parts.length - 1].length());
                if (mapDirectory != null && !mapDirectory.equalsIgnoreCase(prefix))
                    throw new IOException("El ZIP contiene más de una carpeta de mapas.");
                mapDirectory = prefix;
                if (!names.add(name))
                    throw new IOException("El ZIP contiene mapas duplicados.");
                total += entry.getSize();
                if (total > MAX_TOTAL)
                    throw new IOException("El paquete supera 2 GB de mapas.");
                selected.add(entry);
            }
            long done = 0;
            byte[] buffer = new byte[64 << 10];
            for (ZipEntry entry : selected) {
                String name = selectedName(entry.getName().substring(entry.getName().lastIndexOf('/') + 1));
                CRC32 crc = new CRC32();
                long count = 0;
                try (InputStream input = zip.getInputStream(entry);
                     FileOutputStream output = new FileOutputStream(new File(maps, name))) {
                    int length;
                    while ((length = input.read(buffer)) != -1) {
                        checkCanceled(canceled);
                        count += length;
                        if (count > entry.getSize() || count > MAX_FILE)
                            throw new IOException("Un archivo del ZIP supera el tamaño declarado.");
                        output.write(buffer, 0, length);
                        crc.update(buffer, 0, length);
                        done += length;
                        progress.report(name, done, total);
                    }
                }
                if (count != entry.getSize() || crc.getValue() != entry.getCrc())
                    throw new IOException("Un archivo del ZIP está incompleto o dañado.");
            }
        }
    }

    private static void extractIso(File archive, File stage, AtomicBoolean canceled, Progress progress)
        throws IOException {
        final String[] current = { null };
        final long[] previous = { 0 }, fileDone = { 0 };
        try (FileInputStream input = new FileInputStream(archive)) {
            XisoExtractor.extractMaps(input.getChannel(), stage, (name, done, total) -> {
                try {
                    checkCanceled(canceled);
                    if (!name.equals(current[0])) {
                        current[0] = name;
                        fileDone[0] = 0;
                    }
                    fileDone[0] += done - previous[0];
                    previous[0] = done;
                    if (total > MAX_TOTAL || fileDone[0] > MAX_FILE)
                        throw new IOException("La imagen supera los límites de tamaño.");
                    progress.report(name, done, total);
                } catch (IOException error) {
                    throw new ExtractionStopped(error);
                }
            });
        } catch (ExtractionStopped error) {
            throw error.reason;
        }
    }

    private static String validateMaps(File maps, AtomicBoolean canceled) throws IOException {
        File[] files = maps.listFiles();
        if (files == null || files.length > MAX_FILES)
            throw new IOException("El paquete no contiene mapas válidos.");
        List<String> playable = new ArrayList<>();
        Set<String> names = new HashSet<>();
        long total = 0;
        for (File file : files) {
            checkCanceled(canceled);
            if (!file.isFile() || file.length() > MAX_FILE || (total += file.length()) > MAX_TOTAL)
                throw new IOException("El paquete supera los límites de tamaño.");
            String name = selectedName(file.getName());
            if (name == null) {
                Files.delete(file.toPath());
                continue;
            }
            if (!names.add(name))
                throw new IOException("El paquete contiene mapas duplicados.");
            if (!file.getName().equals(name)) {
                File normalized = new File(maps, name);
                if ((normalized.exists() && !normalized.getCanonicalFile().equals(file.getCanonicalFile())) ||
                    !file.renameTo(normalized))
                    throw new IOException("No se pudo preparar un mapa.");
                file = normalized;
            }
            if (name.endsWith(".map")) {
                validateHeader(file);
                if (!name.equals("ui.map"))
                    playable.add(name.substring(0, name.length() - 4));
            }
        }
        if (!names.contains("ui.map") || playable.isEmpty())
            throw new IOException("Falta ui.map o un mapa jugable de Xbox.");
        Collections.sort(playable);
        return playable.contains("a10") ? "a10" : playable.contains("demob30") ? "demob30" : playable.get(0);
    }

    private static void validateHeader(File map) throws IOException {
        byte[] header = new byte[HEADER_SIZE];
        try (RandomAccessFile input = new RandomAccessFile(map, "r")) {
            input.readFully(header);
        }
        ByteBuffer fields = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        int size = fields.getInt(8), offset = fields.getInt(0x10), tags = fields.getInt(0x14);
        boolean terminated = false;
        for (int index = 0x20; index < 0x40; index++)
            terminated |= header[index] == 0;
        // Matches source/cache/cache_files.c; length is decompressed, not the file's compressed size.
        if (fields.getInt(0) != 0x68656164 || fields.getInt(0x7fc) != 0x666f6f74 ||
            fields.getInt(4) != 5 || size < HEADER_SIZE || size > 0x11600000 ||
            offset < 0 || tags < 0 || tags > 0x1600000 || offset > size - tags || !terminated)
            throw new IOException("El mapa " + map.getName() + " no es un mapa de Xbox compatible (versión 5).");
    }

    private static void deleteStage(File file) throws IOException {
        if (!Files.isSymbolicLink(file.toPath()) && file.isDirectory()) {
            File[] children = file.listFiles();
            if (children == null)
                throw new IOException("No se pudo limpiar la instalación temporal.");
            for (File child : children)
                deleteStage(child);
        }
        Files.deleteIfExists(file.toPath());
    }
}
