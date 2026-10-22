package com.halo.decomp;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.StandardCopyOption;
import java.util.Arrays;

/** One-shot demo or menu bootstrap; recover only when the engine is not running. */
final class DemoLaunch {
    private static final int MAX_INIT = 256 * 1024;
    private static final int MAGIC = 0x4f434544;
    private static final byte[] COMMAND = "map_name levels\\demob30\\demob30".getBytes(StandardCharsets.US_ASCII);
    private final File root, init, journal, status;

    DemoLaunch(File dataRoot, File privateRoot) throws IOException {
        root = directory(dataRoot);
        File files = directory(privateRoot);
        init = new File(root, "init.txt");
        journal = new File(files, "demo-launch.bin");
        status = new File(root, "save/z/" + CheckpointStore.PREFIX + ".status");
    }

    boolean available() {
        return regular(new File(root, "maps/ui.map")) && regular(new File(root, "maps/demob30.map"));
    }

    void prepare(String bootId) throws IOException {
        prepare(bootId, true);
    }

    void prepareMenu(String bootId) throws IOException {
        prepare(bootId, false);
    }

    private void prepare(String bootId, boolean demo) throws IOException {
        synchronized (DemoLaunch.class) {
            if (bootId == null || !bootId.matches("[0-9a-f]{32}"))
                throw new IOException("Identificador de demo no válido.");
            if (Files.exists(journal.toPath(), LinkOption.NOFOLLOW_LINKS))
                throw new IOException("Hay un inicio de demo pendiente de recuperar.");
            if (demo ? !available() : !regular(new File(root, "maps/ui.map")))
                throw new IOException(demo ? "Faltan los mapas de la demo." : "Falta el menú de Halo (ui.map).");
            boolean existed = Files.exists(init.toPath(), LinkOption.NOFOLLOW_LINKS);
            byte[] original = existed ? read(init, MAX_INIT) : new byte[0];
            Pending pending = new Pending(bootId, existed, original, rewrite(original, demo));
            writeAtomic(journal, encode(pending));
            try {
                writeAtomic(init, pending.prepared);
            } catch (IOException e) {
                try { restore(pending); }
                catch (IOException recovery) { e.addSuppressed(recovery); }
                throw e;
            }
        }
    }

    boolean restoreIfAcknowledged() throws IOException {
        synchronized (DemoLaunch.class) {
            Pending pending = pending();
            if (pending == null)
                return true;
            if (!Files.exists(status.toPath(), LinkOption.NOFOLLOW_LINKS)
                    || !Arrays.equals(read(status, 128), (pending.id + "\nnew\n").getBytes(StandardCharsets.US_ASCII)))
                return false;
            restore(pending);
            return true;
        }
    }

    /** Call only on a cold start, before creating another boot request. */
    void recover() throws IOException {
        synchronized (DemoLaunch.class) {
            Pending pending = pending();
            if (pending != null)
                restore(pending);
        }
    }

    private void restore(Pending pending) throws IOException {
        boolean exists = Files.exists(init.toPath(), LinkOption.NOFOLLOW_LINKS);
        byte[] current = exists ? read(init, MAX_INIT + 64) : new byte[0];
        boolean original = exists == pending.existed && Arrays.equals(current, pending.original);
        if (!original) {
            if (!exists || !Arrays.equals(current, pending.prepared))
                throw new IOException("init.txt cambió; se conservó la copia de recuperación de la demo.");
            if (pending.existed)
                writeAtomic(init, pending.original);
            else
                Files.delete(init.toPath());
        }
        Files.delete(journal.toPath());
    }

    private Pending pending() throws IOException {
        if (!Files.exists(journal.toPath(), LinkOption.NOFOLLOW_LINKS))
            return null;
        try (DataInputStream in = new DataInputStream(new ByteArrayInputStream(read(journal, MAX_INIT * 2 + 256)))) {
            if (in.readInt() != MAGIC)
                throw new IOException("La copia de recuperación de la demo no es válida.");
            String id = in.readUTF();
            boolean existed = in.readBoolean();
            byte[] original = field(in, MAX_INIT), prepared = field(in, MAX_INIT + 64);
            if (!id.matches("[0-9a-f]{32}") || (!existed && original.length != 0)
                    || in.available() != 0 || (!Arrays.equals(prepared, rewrite(original, true))
                        && !Arrays.equals(prepared, rewrite(original, false))))
                throw new IOException("La copia de recuperación de la demo no es válida.");
            return new Pending(id, existed, original, prepared);
        }
    }

    private static byte[] encode(Pending pending) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (DataOutputStream out = new DataOutputStream(bytes)) {
            out.writeInt(MAGIC);
            out.writeUTF(pending.id);
            out.writeBoolean(pending.existed);
            out.writeInt(pending.original.length);
            out.write(pending.original);
            out.writeInt(pending.prepared.length);
            out.write(pending.prepared);
        }
        return bytes.toByteArray();
    }

    private static byte[] field(DataInputStream in, int maximum) throws IOException {
        int size = in.readInt();
        if (size < 0 || size > maximum || size > in.available())
            throw new IOException("La copia de recuperación de la demo no es válida.");
        byte[] bytes = new byte[size];
        in.readFully(bytes);
        return bytes;
    }

    private static byte[] rewrite(byte[] original, boolean demo) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(original.length + 64);
        boolean replaced = false;
        for (int start = 0; start < original.length;) {
            int end = start;
            while (end < original.length && original[end] != '\r' && original[end] != '\n') {
                if (original[end] == 0)
                    throw new IOException("init.txt no es un archivo de texto válido.");
                end++;
            }
            int command = start;
            if (start == 0 && end >= 3 && original[0] == (byte) 0xef
                    && original[1] == (byte) 0xbb && original[2] == (byte) 0xbf)
                command += 3;
            while (command < end && (original[command] == ' ' || original[command] == '\t'))
                command++;
            String line = new String(original, command, end - command, StandardCharsets.US_ASCII);
            if (line.matches("(?i)(?:map_name(?:[ \\t].*)?|\\([ \\t]*map_name(?:[ \\t].*|\\)[ \\t]*))")) {
                out.write(original, start, command - start);
                if (demo && !replaced) {
                    out.write(COMMAND);
                    replaced = true;
                }
            } else {
                if (!demo && !line.startsWith("//") && !line.startsWith("#") && !line.startsWith(";")
                        && line.matches("(?i).*\\bmap_name\\b.*"))
                    throw new IOException("El inicio contiene un comando de mapa compuesto; se conservó sin cambios.");
                out.write(original, start, end - start);
            }
            int next = end;
            if (next < original.length && original[next] == '\r') next++;
            if (next < original.length && original[next] == '\n') next++;
            out.write(original, end, next - end);
            start = next;
        }
        if (demo && !replaced) {
            if (original.length > 0 && original[original.length - 1] != '\r' && original[original.length - 1] != '\n')
                out.write('\n');
            out.write(COMMAND);
            out.write('\n');
        }
        return out.toByteArray();
    }

    private static File directory(File directory) throws IOException {
        if (directory == null)
            throw new IOException("El almacenamiento de la demo no está disponible.");
        File value = directory.getCanonicalFile();
        if (value.getParentFile() == null || !value.isDirectory())
            throw new IOException("El almacenamiento de la demo no está disponible.");
        return value;
    }

    private static boolean regular(File file) {
        return Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS) && file.length() > 0;
    }

    private static byte[] read(File file, int maximum) throws IOException {
        if (!Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS) || file.length() > maximum)
            throw new IOException("Un archivo de inicio de demo tiene un tamaño o tipo no válido.");
        try (java.io.InputStream in = Files.newInputStream(file.toPath())) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            int count;
            while ((count = in.read(buffer)) != -1) {
                if (out.size() + count > maximum)
                    throw new IOException("Un archivo de inicio de demo es demasiado grande.");
                out.write(buffer, 0, count);
            }
            return out.toByteArray();
        }
    }

    private static void writeAtomic(File target, byte[] bytes) throws IOException {
        File partial = Files.createTempFile(target.getParentFile().toPath(), target.getName() + "-", ".tmp").toFile();
        try {
            try (FileOutputStream out = new FileOutputStream(partial)) {
                out.write(bytes);
                out.getFD().sync();
            }
            try {
                Files.move(partial.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException e) {
                throw new IOException("El almacenamiento no permite confirmar el inicio de demo de forma atómica.", e);
            }
        } finally {
            Files.deleteIfExists(partial.toPath());
        }
    }

    private static final class Pending {
        final String id;
        final boolean existed;
        final byte[] original, prepared;

        Pending(String id, boolean existed, byte[] original, byte[] prepared) {
            this.id = id;
            this.existed = existed;
            this.original = original;
            this.prepared = prepared;
        }
    }
}
