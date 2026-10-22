package com.halo.decomp;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.Set;
import java.util.UUID;

/** Bounded file protocol; the engine owns and validates the actual checkpoint image. */
final class CheckpointStore {
    static final String PREFIX = "mobile-checkpoint";
    static final int HEADER_SIZE = 512;
    static final int IMAGE_SIZE = 20 * 1024 * 1024;
    static final Set<String> SAVE_RESULTS = Collections.unmodifiableSet(new HashSet<>(Arrays.asList(
        "saved", "no_checkpoint", "unsupported", "io_error")));
    static final Set<String> BOOT_RESULTS = Collections.unmodifiableSet(new HashSet<>(Arrays.asList(
        "resumed", "corrupt", "incompatible", "io_error", "unsupported")));

    interface Assets { InputStream open(String name) throws IOException; }

    enum Kind { ABSENT, READY, CORRUPT, INCOMPATIBLE }

    static final class Candidate {
        final Kind kind;
        final String map;
        final int difficulty;

        Candidate(Kind kind, String map, int difficulty) {
            this.kind = kind;
            this.map = map;
            this.difficulty = difficulty;
        }
    }

    static final class Fingerprints {
        final String gl, vk;

        Fingerprints(String gl, String vk) {
            if (!hash(gl) || !hash(vk))
                throw new IllegalArgumentException("Invalid engine fingerprint");
            this.gl = gl;
            this.vk = vk;
        }
    }

    private final File directory;
    private final Assets assets;
    private Fingerprints fingerprints;

    CheckpointStore(File dataRoot, Assets assets) throws IOException {
        if (dataRoot == null)
            throw new IOException("El almacenamiento del juego no está disponible.");
        directory = new File(dataRoot, "save/z");
        this.assets = assets;
    }

    static String newId() { return UUID.randomUUID().toString().replace("-", ""); }

    private static boolean id(String value) { return value != null && value.matches("[0-9a-f]{32}"); }
    private static boolean hash(String value) { return value != null && value.matches("[0-9a-f]{64}"); }

    synchronized Fingerprints fingerprints() throws IOException {
        if (fingerprints == null)
            fingerprints = new Fingerprints(digest("halo_guest"), digest("halo_guest_vk"));
        return fingerprints;
    }

    private String digest(String base) throws IOException {
        final MessageDigest digest;
        try { digest = MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException e) { throw new IOException("SHA-256 no está disponible.", e); }
        byte[] buffer = new byte[32768];
        for (String suffix : new String[] { ".elf", ".relocs" }) {
            try (InputStream in = assets.open(base + suffix)) {
                int count;
                while ((count = in.read(buffer)) != -1) {
                    interrupted();
                    digest.update(buffer, 0, count);
                }
            }
        }
        StringBuilder hex = new StringBuilder(64);
        for (byte value : digest.digest()) {
            hex.append(Character.forDigit((value >>> 4) & 15, 16));
            hex.append(Character.forDigit(value & 15, 16));
        }
        return hex.toString();
    }

    Candidate inspect() throws IOException {
        return inspect(file(".bin"), 0);
    }

    File checkpointFile() { return file(".bin"); }

    Candidate inspect(File slot, long offset) throws IOException {
        if (offset < 0)
            throw new IOException("Desplazamiento de guardado no válido.");
        if (!slot.exists())
            return new Candidate(Kind.ABSENT, "", 0);
        if (!slot.isFile() || slot.length() - offset < HEADER_SIZE)
            return new Candidate(Kind.CORRUPT, "", 0);
        byte[] header = new byte[HEADER_SIZE];
        try (FileInputStream in = new FileInputStream(slot)) {
            in.getChannel().position(offset);
            int read = 0, count;
            while (read < header.length && (count = in.read(header, read, header.length - read)) > 0)
                read += count;
            if (read != HEADER_SIZE)
                return new Candidate(Kind.CORRUPT, "", 0);
        }
        ByteBuffer fields = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        long imageSize = Integer.toUnsignedLong(fields.getInt(16));
        int difficulty = fields.getInt(24), renderer = fields.getInt(28), end = 96;
        while (end < 352 && header[end] != 0)
            end++;
        String map = new String(header, 96, end - 96, StandardCharsets.US_ASCII);
        String fingerprint = new String(header, 32, 64, StandardCharsets.US_ASCII);
        boolean zeroTail = true;
        for (int index = end; index < header.length; index++)
            zeroTail &= header[index] == 0;
        if (!Arrays.equals(Arrays.copyOf(header, 8), "OCESAVE1".getBytes(StandardCharsets.US_ASCII))
                || fields.getInt(8) != 1 || fields.getInt(12) != HEADER_SIZE
                || imageSize != IMAGE_SIZE || slot.length() - offset != HEADER_SIZE + imageSize
                || difficulty < 0 || difficulty > 3 || renderer < 0 || renderer > 1
                || end == 352 || !zeroTail || !map.matches("(?:levels|custom_maps)\\\\[A-Za-z0-9_\\\\-]+")
                || map.endsWith("\\") || !hash(fingerprint))
            return new Candidate(Kind.CORRUPT, "", 0);
        Fingerprints expected = fingerprints();
        // The host can fall back from Vulkan to GLES. Only the running guest can verify its renderer.
        Kind kind = fingerprint.equals(renderer == 0 ? expected.gl : expected.vk)
            ? Kind.READY : Kind.INCOMPATIBLE;
        return new Candidate(kind, map, difficulty);
    }

    void writeRequest(String requestId) throws IOException { writeCommand(".request", requestId, "save"); }

    void writeBoot(String requestId, boolean resume) throws IOException {
        writeCommand(".boot", requestId, resume ? "continue" : "new");
    }

    private void writeCommand(String suffix, String requestId, String operation) throws IOException {
        if (!id(requestId))
            throw new IOException("Identificador de guardado no válido.");
        Fingerprints value = fingerprints();
        interrupted();
        writeAtomic(file(suffix), requestId + " " + operation + " " + value.gl + " " + value.vk + "\n");
    }

    String result(String requestId, boolean resume) throws IOException {
        if (!id(requestId))
            throw new IOException("Identificador de guardado no válido.");
        String text = readBounded(file(".status"), 128);
        String[] lines = text.split("\n", -1);
        if ((lines.length != 2 && (lines.length != 3 || !lines[2].isEmpty()))
                || !lines[0].equals(requestId))
            return null;
        return (resume ? BOOT_RESULTS : SAVE_RESULTS).contains(lines[1]) ? lines[1] : null;
    }

    void clearBoot() throws IOException { Files.deleteIfExists(file(".boot").toPath()); }

    void clearAbandonedRequests() throws IOException {
        clearBoot();
        Files.deleteIfExists(file(".request").toPath());
    }

    void cancelRequest(String requestId) throws IOException {
        if (!id(requestId))
            throw new IOException("Identificador de guardado no válido.");
        File request = file(".request");
        if (readBounded(request, 256).startsWith(requestId + " save "))
            Files.deleteIfExists(request.toPath());
    }

    private File file(String suffix) { return new File(directory, PREFIX + suffix); }

    private static void interrupted() throws InterruptedIOException {
        if (Thread.currentThread().isInterrupted())
            throw new InterruptedIOException("Operación cancelada.");
    }

    static String readBounded(File file, int maximum) throws IOException {
        if (!file.exists())
            return "";
        if (!file.isFile() || file.length() > maximum)
            throw new IOException("El archivo de guardado tiene un tamaño no válido.");
        byte[] buffer = new byte[maximum + 1];
        int offset = 0, count;
        try (FileInputStream in = new FileInputStream(file)) {
            while (offset < buffer.length && (count = in.read(buffer, offset, buffer.length - offset)) > 0)
                offset += count;
        }
        if (offset > maximum)
            throw new IOException("El archivo de guardado tiene un tamaño no válido.");
        return new String(buffer, 0, offset, StandardCharsets.UTF_8);
    }

    static void writeAtomic(File target, String text) throws IOException {
        File parent = target.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs()))
            throw new IOException("No se pudo preparar la carpeta de guardado.");
        File partial = new File(parent, target.getName() + ".tmp");
        try {
            try (FileOutputStream out = new FileOutputStream(partial)) {
                out.write(text.getBytes(StandardCharsets.US_ASCII));
                out.getFD().sync();
            }
            interrupted();
            try {
                Files.move(partial.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException e) {
                if (!partial.renameTo(target))
                    throw new IOException("No se pudo confirmar el archivo de guardado.", e);
            }
        } finally {
            Files.deleteIfExists(partial.toPath());
        }
    }
}
