package com.halo.decomp;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;
import java.util.zip.CRC32;

import static org.junit.Assert.*;

/** Real bounded snapshot copies and native-compatible image CRC; the engine validates pointers. */
public class CheckpointSlotsTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    private File root, working, directory;
    private CheckpointStore store;
    private CheckpointSlots slots;

    @Before public void prepare() throws Exception {
        root = temporary.newFolder();
        directory = new File(root, "save/z/mobile-checkpoint-slots");
        store = new CheckpointStore(root,
            name -> new ByteArrayInputStream(name.getBytes(StandardCharsets.US_ASCII)));
        working = store.checkpointFile();
        slots = new CheckpointSlots(root, store);
    }

    private void checkpoint(String map, int marker, boolean compatible) throws Exception {
        assertTrue(working.getParentFile().isDirectory() || working.getParentFile().mkdirs());
        byte[] header = new byte[CheckpointStore.HEADER_SIZE];
        CRC32 checksum = new CRC32();
        byte[] zeros = new byte[32768];
        zeros[0] = (byte) marker;
        for (int remaining = CheckpointStore.IMAGE_SIZE; remaining > 0; remaining -= zeros.length) {
            checksum.update(zeros, 0, Math.min(remaining, zeros.length));
            zeros[0] = 0;
        }
        ByteBuffer fields = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        fields.put("OCESAVE1".getBytes(StandardCharsets.US_ASCII));
        fields.putInt(8, 1).putInt(12, header.length).putInt(16, CheckpointStore.IMAGE_SIZE)
            .putInt(20, (int) checksum.getValue()).putInt(24, 2).putInt(28, 0);
        byte[] fingerprint = store.fingerprints().gl.getBytes(StandardCharsets.US_ASCII);
        if (!compatible)
            Arrays.fill(fingerprint, (byte) '0');
        System.arraycopy(fingerprint, 0, header, 32, fingerprint.length);
        byte[] mapName = map.getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(mapName, 0, header, 96, mapName.length);
        try (RandomAccessFile file = new RandomAccessFile(working, "rw")) {
            file.setLength(0);
            file.setLength(CheckpointStore.HEADER_SIZE + CheckpointStore.IMAGE_SIZE);
            file.write(header);
            file.write(marker);
        }
    }

    private int marker() throws Exception {
        try (RandomAccessFile file = new RandomAccessFile(working, "r")) {
            file.seek(CheckpointStore.HEADER_SIZE);
            return file.read();
        }
    }

    private File archive(CheckpointSlots.Entry entry) { return new File(directory, entry.id + ".slot"); }

    private void full() throws Exception {
        assertTrue(directory.isDirectory() || directory.mkdirs());
        while (slots.list().size() < CheckpointSlots.MAX_SLOTS) {
            File placeholder = new File(directory, CheckpointStore.newId() + ".slot");
            assertTrue(placeholder.createNewFile());
        }
    }

    @Test public void absentWorkingDoesNotCreateDirectories() throws Exception {
        assertTrue(slots.list().isEmpty());
        assertNull(slots.preserveLegacy());
        assertFalse(directory.exists());
        assertThrows(IOException.class, () -> slots.saveNew("Primera"));
        assertFalse(directory.exists());
    }

    @Test public void namesMetadataAndIndependentCheckpointsSurviveNewInstance() throws Exception {
        checkpoint("levels\\demob30\\demob30", 11, true);
        CheckpointSlots.Entry first = slots.saveNew("  Playa española  ");
        checkpoint("levels\\a10\\a10", 22, true);
        CheckpointSlots.Entry second = slots.saveNew("Nave 🚀");
        assertNotEquals(first.id, second.id);
        assertEquals("Playa española", first.name);
        assertEquals("levels\\demob30\\demob30", first.candidate.map);
        assertEquals(2, first.candidate.difficulty);
        assertTrue(first.savedAt > 0);
        List<CheckpointSlots.Entry> entries = new CheckpointSlots(root, store).list();
        assertEquals(2, entries.size());
        assertTrue(entries.get(0).savedAt >= entries.get(1).savedAt);
        assertEquals(CheckpointStore.Kind.READY, entries.get(0).candidate.kind);
        assertEquals(512L + CheckpointStore.HEADER_SIZE + CheckpointStore.IMAGE_SIZE, archive(first).length());
        slots.activate(first.id);
        assertEquals(11, marker());
        assertEquals(2, slots.list().size());
        slots.activate(second.id);
        assertEquals(22, marker());
        assertEquals(2, slots.list().size());
    }

    @Test public void replacementTouchesOnlyExplicitlySelectedSlot() throws Exception {
        checkpoint("levels\\b30\\b30", 1, true);
        CheckpointSlots.Entry first = slots.saveNew("Uno");
        CheckpointSlots.Entry second = slots.saveNew("Dos");
        checkpoint("levels\\a10\\a10", 2, true);
        CheckpointSlots.Entry replaced = slots.replace(second.id, "Dos actualizado");
        assertEquals(second.id, replaced.id);
        assertEquals("Dos actualizado", replaced.name);
        assertEquals(2, slots.list().size());
        slots.activate(first.id);
        assertEquals(1, marker());
        slots.activate(second.id);
        assertEquals(2, marker());
    }

    @Test public void activationPreservesUnarchivedWorkingAndInit() throws Exception {
        File init = new File(root, "init.txt");
        byte[] config = "map_name levels\\b30\\b30\n".getBytes(StandardCharsets.UTF_8);
        Files.write(init.toPath(), config);
        checkpoint("levels\\b30\\b30", 3, true);
        CheckpointSlots.Entry old = slots.saveNew("Inicio");
        checkpoint("levels\\a10\\a10", 4, true);
        slots.activate(old.id);
        assertEquals(3, marker());
        assertEquals(2, slots.list().size());
        CheckpointSlots.Entry preserved = slots.list().stream()
            .filter(entry -> !entry.id.equals(old.id)).findFirst().get();
        assertEquals("Partida anterior", preserved.name);
        assertEquals("levels\\a10\\a10", preserved.candidate.map);
        slots.activate(preserved.id);
        assertEquals(4, marker());
        assertArrayEquals(config, Files.readAllBytes(init.toPath()));
    }

    @Test public void preserveLegacyDeduplicatesVerifiedSnapshot() throws Exception {
        checkpoint("levels\\b30\\b30", 1, true);
        CheckpointSlots.Entry first = slots.preserveLegacy();
        assertNotNull(first);
        assertEquals(first.id, slots.preserveLegacy().id);
        assertEquals(1, slots.list().size());
    }

    @Test public void incompatibleWorkingIsPreservedButCannotBeActivated() throws Exception {
        checkpoint("levels\\b30\\b30", 8, false);
        CheckpointSlots.Entry incompatible = slots.preserveLegacy();
        assertEquals(CheckpointStore.Kind.INCOMPATIBLE, incompatible.candidate.kind);
        assertThrows(IOException.class, () -> slots.activate(incompatible.id));
        assertEquals(8, marker());
        checkpoint("levels\\a10\\a10", 9, true);
        CheckpointSlots.Entry compatible = slots.saveNew("Actual");
        checkpoint("levels\\b30\\b30", 8, false);
        slots.activate(compatible.id);
        assertEquals(9, marker());
        assertEquals(2, slots.list().size());
    }

    @Test public void corruptWorkingIsRetainedAsRecoveryBeforeActivation() throws Exception {
        checkpoint("levels\\b30\\b30", 1, true);
        CheckpointSlots.Entry first = slots.saveNew("Uno");
        byte[] corrupt = "old damaged checkpoint".getBytes(StandardCharsets.US_ASCII);
        Files.write(working.toPath(), corrupt);
        slots.activate(first.id);
        assertEquals(1, marker());
        File[] recovery = new File(working.getParentFile(), "mobile-checkpoint-recovery").listFiles();
        assertNotNull(recovery);
        assertEquals(1, recovery.length);
        assertArrayEquals(corrupt, Files.readAllBytes(recovery[0].toPath()));
        assertTrue(archive(first).isFile());
        assertEquals(1, slots.list().size());
    }

    @Test public void tamperedPayloadCannotReplaceWorkingOrSuppressPreservation() throws Exception {
        checkpoint("levels\\b30\\b30", 1, true);
        CheckpointSlots.Entry first = slots.saveNew("Uno");
        try (RandomAccessFile file = new RandomAccessFile(archive(first), "rw")) {
            file.seek(512L + CheckpointStore.HEADER_SIZE);
            file.write(9);
        }
        assertThrows(IOException.class, () -> slots.activate(first.id));
        assertEquals(1, marker());
        CheckpointSlots.Entry preserved = slots.preserveLegacy();
        assertNotEquals(first.id, preserved.id);
        assertEquals(2, slots.list().size());
    }

    @Test public void fullCollectionRecoversUnarchivedWorkingWithoutReplacingAnyNamedSlot() throws Exception {
        checkpoint("levels\\b30\\b30", 1, true);
        CheckpointSlots.Entry first = slots.saveNew("Uno");
        full();
        assertThrows(IOException.class, () -> slots.saveNew("Once"));
        assertEquals(first.id, slots.preserveLegacy().id);
        checkpoint("levels\\a10\\a10", 2, true);
        byte[] unarchived = Files.readAllBytes(working.toPath());
        slots.activate(first.id);
        assertEquals(1, marker());
        assertEquals(10, slots.list().size());
        assertEquals("Uno", slots.list().stream().filter(entry -> first.id.equals(entry.id)).findFirst().get().name);
        File[] recovery = new File(working.getParentFile(), "mobile-checkpoint-recovery").listFiles();
        assertNotNull(recovery);
        assertEquals(1, recovery.length);
        assertArrayEquals(unarchived, Files.readAllBytes(recovery[0].toPath()));
        checkpoint("levels\\a10\\a10", 2, true);
        slots.replace(first.id, "Reemplazo elegido");
        assertEquals(10, slots.list().size());
        assertEquals(first.id, slots.preserveLegacy().id);
    }

    @Test public void invalidIdsAndNamesCannotCreateOrReplaceArchives() throws Exception {
        checkpoint("levels\\b30\\b30", 1, true);
        for (String name : new String[] { null, "", "  ", "line\nbreak", "zero\u0000", " \u0000 ", "\ud800", new String(new char[161]).replace('\0', 'a') })
            assertThrows(IOException.class, () -> slots.saveNew(name));
        for (String id : new String[] { null, "", "../checkpoint", "A123456789abcdef0123456789abcdef", "0123456789abcdef0123456789abcdef\n" }) {
            assertThrows(IOException.class, () -> slots.activate(id));
            assertThrows(IOException.class, () -> slots.replace(id, "Nombre"));
        }
        assertFalse(directory.exists());
        assertEquals(1, marker());
    }

    @Test public void interruptedReplacementKeepsOriginalAndCleansStaging() throws Exception {
        checkpoint("levels\\b30\\b30", 1, true);
        CheckpointSlots.Entry first = slots.saveNew("Primera");
        checkpoint("levels\\a10\\a10", 2, true);
        Thread.currentThread().interrupt();
        try { assertThrows(IOException.class, () -> slots.replace(first.id, "Nueva")); }
        finally { Thread.interrupted(); }
        assertEquals("Primera", slots.list().get(0).name);
        assertEquals(1, directory.listFiles().length);
        slots.activate(first.id);
        assertEquals(1, marker());
    }

    @Test public void malformedContainersRemainVisibleAndAreNeverActivated() throws Exception {
        checkpoint("levels\\b30\\b30", 1, true);
        CheckpointSlots.Entry first = slots.saveNew("Primera");
        try (RandomAccessFile file = new RandomAccessFile(archive(first), "rw")) {
            file.seek(64);
            file.write(0xff);
        }
        assertEquals(CheckpointStore.Kind.CORRUPT, slots.list().get(0).candidate.kind);
        assertThrows(IOException.class, () -> slots.activate(first.id));
        assertEquals(1, marker());
        File ignored = new File(directory, "not-a-slot.tmp");
        assertTrue(ignored.createNewFile());
        assertEquals(1, slots.list().size());
    }

    @Test public void corruptNativePayloadIsRecoveredButNotPublishedAsPlayable() throws Exception {
        checkpoint("levels\\b30\\b30", 1, true);
        try (RandomAccessFile file = new RandomAccessFile(working, "rw")) {
            file.seek(CheckpointStore.HEADER_SIZE);
            file.write(7);
        }
        assertThrows(IOException.class, () -> slots.saveNew("Dañada"));
        assertNull(slots.preserveLegacy());
        assertTrue(slots.list().isEmpty());
        assertEquals(7, marker());
        File[] recovery = new File(working.getParentFile(), "mobile-checkpoint-recovery").listFiles();
        assertNotNull(recovery);
        assertEquals(1, recovery.length);
        assertEquals(working.length(), recovery[0].length());
        slots.preserveLegacy();
        assertEquals(1, new File(working.getParentFile(), "mobile-checkpoint-recovery").listFiles().length);
    }

    @Test public void oversizedCorruptWorkingFailsClosedWithoutRecovery() throws Exception {
        checkpoint("levels\\b30\\b30", 1, true);
        CheckpointSlots.Entry first = slots.saveNew("Uno");
        try (RandomAccessFile file = new RandomAccessFile(working, "rw")) {
            file.setLength(CheckpointStore.HEADER_SIZE + CheckpointStore.IMAGE_SIZE + 1L);
        }
        assertThrows(IOException.class, () -> slots.activate(first.id));
        assertEquals(CheckpointStore.HEADER_SIZE + CheckpointStore.IMAGE_SIZE + 1L, working.length());
        assertFalse(new File(working.getParentFile(), "mobile-checkpoint-recovery").exists());
        assertEquals(1, slots.list().size());
    }
}
