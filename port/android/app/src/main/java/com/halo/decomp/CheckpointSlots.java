package com.halo.decomp;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.zip.CRC32;

/** Named snapshots; each atomic container commits its label and checkpoint together. */
final class CheckpointSlots {
    static final int MAX_SLOTS = 10;
    private static final int META_SIZE = 512, MAX_NAME_BYTES = 160;
    private static final long CHECKPOINT_SIZE = CheckpointStore.HEADER_SIZE + CheckpointStore.IMAGE_SIZE;
    private static final byte[] MAGIC = "OCESLOT1".getBytes(StandardCharsets.US_ASCII);

    static final class Entry {
        final String id, name;
        final long savedAt;
        final CheckpointStore.Candidate candidate;
        private final byte[] digest;

        Entry(String id, String name, long savedAt, CheckpointStore.Candidate candidate, byte[] digest) {
            this.id = id;
            this.name = name;
            this.savedAt = savedAt;
            this.candidate = candidate;
            this.digest = digest;
        }
    }

    private final File directory;
    private final CheckpointStore store;

    CheckpointSlots(File dataRoot, CheckpointStore store) throws IOException {
        if (dataRoot == null || store == null)
            throw new IOException("El almacenamiento del juego no está disponible.");
        directory = new File(dataRoot, "save/z/mobile-checkpoint-slots");
        this.store = store;
    }

    synchronized List<Entry> list() throws IOException {
        if (!directory.exists())
            return Collections.emptyList();
        File[] files = directory.listFiles(file -> file.getName().matches("[0-9a-f]{32}\\.slot"));
        if (files == null)
            throw new IOException("No se pudo leer la lista de partidas.");
        List<Entry> result = new ArrayList<>();
        for (File file : files) {
            interrupted();
            result.add(read(file));
        }
        result.sort((left, right) -> {
            int date = Long.compare(right.savedAt, left.savedAt);
            return date != 0 ? date : left.id.compareTo(right.id);
        });
        return Collections.unmodifiableList(result);
    }

    synchronized Entry saveNew(String name) throws IOException {
        String validName = name(name);
        if (list().size() >= MAX_SLOTS)
            throw new IOException("Ya hay 10 partidas guardadas. Elige una partida para reemplazarla.");
        String id = CheckpointStore.newId();
        File target = file(id);
        if (target.exists())
            throw new IOException("La partida ya existe. Inténtalo de nuevo.");
        return archive(target, validName);
    }

    synchronized Entry replace(String id, String name) throws IOException {
        File target = file(id);
        if (!target.isFile())
            throw new IOException("La partida seleccionada ya no está disponible.");
        return archive(target, name(name));
    }

    /** Refuse a switch if the working checkpoint cannot be safely retained. */
    synchronized Entry preserveLegacy() throws IOException {
        CheckpointStore.Candidate current = store.inspect();
        if (current.kind == CheckpointStore.Kind.ABSENT)
            return null;
        if (current.kind == CheckpointStore.Kind.CORRUPT || !payloadValid(store.checkpointFile(), 0)) {
            preserveRecovery();
            return null;
        }
        validWorking(current);
        byte[] currentDigest = digest(store.checkpointFile(), 0);
        List<Entry> entries = list();
        for (Entry entry : entries) {
            if (entry.digest != null && Arrays.equals(currentDigest, entry.digest)
                    && Arrays.equals(entry.digest, digest(file(entry.id), META_SIZE)))
                return entry;
        }
        if (entries.size() >= MAX_SLOTS) {
            preserveRecovery();
            return null;
        }
        return saveNew("Partida anterior");
    }

    synchronized void activate(String id) throws IOException {
        File source = file(id);
        Entry entry = read(source);
        if (entry.candidate.kind != CheckpointStore.Kind.READY || entry.digest == null)
            throw new IOException("Esta partida está dañada o pertenece a otra versión del juego.");
        File target = store.checkpointFile(), partial = partial(target);
        try {
            copy(source, META_SIZE, partial, null, entry.digest);
            if (store.inspect(partial, 0).kind != CheckpointStore.Kind.READY || !payloadValid(partial, 0))
                throw new IOException("No se pudo verificar la partida seleccionada.");
            preserveLegacy();
            interrupted();
            commit(partial, target);
        } finally {
            Files.deleteIfExists(partial.toPath());
        }
    }

    private Entry archive(File target, String name) throws IOException {
        validWorking(store.inspect());
        if (!payloadValid(store.checkpointFile(), 0))
            throw new IOException("El punto de control está dañado. La partida actual se conserva.");
        byte[] digest = digest(store.checkpointFile(), 0);
        byte[] label = name.getBytes(StandardCharsets.UTF_8);
        byte[] metadata = new byte[META_SIZE];
        ByteBuffer header = ByteBuffer.wrap(metadata).order(ByteOrder.LITTLE_ENDIAN);
        header.put(MAGIC).putInt(1).putInt(label.length).putLong(System.currentTimeMillis()).put(digest);
        System.arraycopy(label, 0, metadata, 64, label.length);
        File partial = partial(target);
        try {
            copy(store.checkpointFile(), 0, partial, metadata, digest);
            Entry staged = read(partial);
            if (staged.candidate.kind != CheckpointStore.Kind.READY
                    && staged.candidate.kind != CheckpointStore.Kind.INCOMPATIBLE)
                throw new IOException("No se pudo verificar la copia de la partida.");
            interrupted();
            commit(partial, target);
            return read(target);
        } finally {
            Files.deleteIfExists(partial.toPath());
        }
    }

    private static void validWorking(CheckpointStore.Candidate candidate) throws IOException {
        if (candidate.kind == CheckpointStore.Kind.ABSENT)
            throw new IOException("Todavía no hay un punto de control para guardar.");
        if (candidate.kind == CheckpointStore.Kind.CORRUPT)
            throw new IOException("La partida actual está dañada. Se conserva sin reemplazarla.");
    }

    private Entry read(File file) throws IOException {
        String id = file.getName().substring(0, 32);
        byte[] metadata = new byte[META_SIZE];
        if (!file.isFile() || file.length() != META_SIZE + CHECKPOINT_SIZE)
            return corrupt(id, file.lastModified());
        try (FileInputStream input = new FileInputStream(file)) {
            int offset = 0, count;
            while (offset < metadata.length && (count = input.read(metadata, offset, metadata.length - offset)) > 0)
                offset += count;
            if (offset != metadata.length)
                return corrupt(id, file.lastModified());
        }
        ByteBuffer header = ByteBuffer.wrap(metadata).order(ByteOrder.LITTLE_ENDIAN);
        int length = header.getInt(12);
        long savedAt = header.getLong(16);
        if (!Arrays.equals(MAGIC, Arrays.copyOf(metadata, 8)) || header.getInt(8) != 1
                || length < 1 || length > MAX_NAME_BYTES || savedAt <= 0)
            return corrupt(id, file.lastModified());
        for (int index = 56; index < metadata.length; index++) {
            if ((index < 64 || index >= 64 + length) && metadata[index] != 0)
                return corrupt(id, file.lastModified());
        }
        final String name;
        try {
            name = StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(metadata, 64, length)).toString();
            if (!name.equals(name(name)))
                return corrupt(id, file.lastModified());
        } catch (CharacterCodingException | IllegalArgumentException e) {
            return corrupt(id, file.lastModified());
        } catch (IOException e) {
            return corrupt(id, file.lastModified());
        }
        return new Entry(id, name, savedAt, store.inspect(file, META_SIZE), Arrays.copyOfRange(metadata, 24, 56));
    }

    private static Entry corrupt(String id, long timestamp) {
        return new Entry(id, "Partida dañada", timestamp,
            new CheckpointStore.Candidate(CheckpointStore.Kind.CORRUPT, "", 0), null);
    }

    private File file(String id) throws IOException {
        if (id == null || !id.matches("[0-9a-f]{32}"))
            throw new IOException("Identificador de partida no válido.");
        return new File(directory, id + ".slot");
    }

    private static String name(String value) throws IOException {
        if (value == null)
            throw new IOException("Escribe un nombre para la partida.");
        for (int index = 0; index < value.length(); index++) {
            if (Character.isISOControl(value.charAt(index)))
                throw new IOException("El nombre de la partida contiene caracteres no válidos.");
        }
        String result = value.trim();
        if (result.isEmpty() || result.getBytes(StandardCharsets.UTF_8).length > MAX_NAME_BYTES)
            throw new IOException("El nombre debe tener entre 1 y 160 bytes.");
        try {
            StandardCharsets.UTF_8.newEncoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).encode(java.nio.CharBuffer.wrap(result));
        } catch (CharacterCodingException e) {
            throw new IOException("El nombre de la partida contiene caracteres no válidos.", e);
        }
        return result;
    }

    private static File partial(File target) throws IOException {
        File parent = target.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs()))
            throw new IOException("No se pudo preparar la carpeta de partidas.");
        return new File(parent, target.getName() + "." + CheckpointStore.newId() + ".tmp");
    }

    private static MessageDigest sha256() throws IOException {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException e) { throw new IOException("SHA-256 no está disponible.", e); }
    }

    private static byte[] digest(File file, long offset) throws IOException {
        return digest(file, offset, CHECKPOINT_SIZE);
    }

    private static byte[] digest(File file, long offset, long size) throws IOException {
        MessageDigest hash = sha256();
        try (FileInputStream input = new FileInputStream(file)) {
            if (file.length() - offset != size)
                throw new IOException("La partida cambió durante la copia. Inténtalo de nuevo.");
            input.getChannel().position(offset);
            byte[] buffer = new byte[32768];
            int count;
            long total = 0;
            while ((count = input.read(buffer)) != -1) {
                interrupted();
                total += count;
                if (total > size)
                    throw new IOException("La partida cambió durante la copia. Inténtalo de nuevo.");
                hash.update(buffer, 0, count);
            }
            if (total != size)
                throw new IOException("La partida cambió durante la copia. Inténtalo de nuevo.");
        }
        return hash.digest();
    }

    private static void copy(File source, long offset, File target, byte[] metadata, byte[] expected) throws IOException {
        copy(source, offset, target, metadata, expected, CHECKPOINT_SIZE);
    }

    private static void copy(File source, long offset, File target, byte[] metadata, byte[] expected, long size) throws IOException {
        MessageDigest hash = sha256();
        try (FileInputStream input = new FileInputStream(source);
                FileOutputStream output = new FileOutputStream(target)) {
            if (source.length() - offset != size)
                throw new IOException("La partida tiene un tamaño no válido.");
            input.getChannel().position(offset);
            if (metadata != null)
                output.write(metadata);
            byte[] buffer = new byte[32768];
            int count;
            long total = 0;
            while ((count = input.read(buffer)) != -1) {
                interrupted();
                total += count;
                if (total > size)
                    throw new IOException("La partida cambió durante la copia. Inténtalo de nuevo.");
                output.write(buffer, 0, count);
                hash.update(buffer, 0, count);
            }
            if (total != size || !Arrays.equals(expected, hash.digest()))
                throw new IOException("No se pudo verificar la copia de la partida.");
            output.getFD().sync();
        }
        if (!Arrays.equals(expected, digest(target, metadata == null ? 0 : META_SIZE, size)))
            throw new IOException("No se pudo verificar la copia de la partida.");
    }

    private void preserveRecovery() throws IOException {
        File source = store.checkpointFile();
        long size = source.length();
        if (!source.isFile() || size > CHECKPOINT_SIZE)
            throw new IOException("La partida actual tiene un tamaño no válido. Se conserva sin reemplazarla.");
        byte[] hash = digest(source, 0, size);
        StringBuilder hex = new StringBuilder(64);
        for (byte value : hash) {
            hex.append(Character.forDigit((value >>> 4) & 15, 16));
            hex.append(Character.forDigit(value & 15, 16));
        }
        File recovery = new File(store.checkpointFile().getParentFile(), "mobile-checkpoint-recovery");
        File target = new File(recovery, hex + ".bin");
        if (target.exists()) {
            if (!target.isFile() || target.length() != size || !Arrays.equals(hash, digest(target, 0, size)))
                throw new IOException("No se pudo verificar la copia de recuperación de la partida.");
            return;
        }
        File partial = partial(target);
        try {
            copy(source, 0, partial, null, hash, size);
            interrupted();
            commit(partial, target);
        } finally {
            Files.deleteIfExists(partial.toPath());
        }
    }

    private static boolean payloadValid(File file, long offset) throws IOException {
        if (!file.isFile() || file.length() - offset != CHECKPOINT_SIZE)
            return false;
        CRC32 checksum = new CRC32();
        try (FileInputStream input = new FileInputStream(file)) {
            input.getChannel().position(offset);
            byte[] header = new byte[32];
            int read = 0, count;
            while (read < header.length && (count = input.read(header, read, header.length - read)) > 0)
                read += count;
            if (read != header.length)
                return false;
            long expected = Integer.toUnsignedLong(ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN).getInt(20));
            input.getChannel().position(offset + CheckpointStore.HEADER_SIZE);
            byte[] buffer = new byte[32768];
            long total = 0;
            while ((count = input.read(buffer)) != -1) {
                interrupted();
                total += count;
                if (total > CheckpointStore.IMAGE_SIZE)
                    return false;
                checksum.update(buffer, 0, count);
            }
            return total == CheckpointStore.IMAGE_SIZE && checksum.getValue() == expected;
        }
    }

    private static void commit(File partial, File target) throws IOException {
        try {
            Files.move(partial.toPath(), target.toPath(), StandardCopyOption.ATOMIC_MOVE,
                StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException e) {
            throw new IOException("El almacenamiento no permite guardar la partida de forma segura.", e);
        }
    }

    private static void interrupted() throws InterruptedIOException {
        if (Thread.currentThread().isInterrupted())
            throw new InterruptedIOException("Operación cancelada.");
    }
}
