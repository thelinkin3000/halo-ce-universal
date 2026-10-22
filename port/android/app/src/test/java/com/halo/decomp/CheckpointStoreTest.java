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
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicInteger;

import static org.junit.Assert.*;

/** Real atomic file requests and header checks; the native engine validates image CRC and pointers. */
public class CheckpointStoreTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    private static final String ID = "0123456789abcdef0123456789abcdef";
    private static final String OTHER_ID = "fedcba9876543210fedcba9876543210";
    private File root, directory;
    private CheckpointStore store;
    private final AtomicInteger opens = new AtomicInteger();

    @Before public void prepare() throws IOException {
        root = temporary.newFolder();
        directory = new File(root, "save/z");
        assertTrue(directory.mkdirs());
        store = new CheckpointStore(root, name -> {
            opens.incrementAndGet();
            return new ByteArrayInputStream(name.getBytes(StandardCharsets.US_ASCII));
        });
    }

    private File file(String suffix) { return new File(directory, CheckpointStore.PREFIX + suffix); }

    private static void write(java.nio.file.Path path, String text) throws IOException {
        Files.write(path, text.getBytes(StandardCharsets.UTF_8));
    }

    private static String read(java.nio.file.Path path) throws IOException {
        return new String(Files.readAllBytes(path), StandardCharsets.UTF_8);
    }

    private byte[] header(int renderer) throws IOException {
        byte[] header = new byte[CheckpointStore.HEADER_SIZE];
        ByteBuffer fields = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        fields.put("OCESAVE1".getBytes(StandardCharsets.US_ASCII));
        fields.putInt(8, 1).putInt(12, header.length).putInt(16, CheckpointStore.IMAGE_SIZE)
            .putInt(20, 123).putInt(24, 2).putInt(28, renderer);
        System.arraycopy((renderer == 0 ? store.fingerprints().gl : store.fingerprints().vk)
            .getBytes(StandardCharsets.US_ASCII), 0, header, 32, 64);
        byte[] map = "levels\\b30\\b30".getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(map, 0, header, 96, map.length);
        return header;
    }

    private void slot(byte[] header) throws IOException {
        try (RandomAccessFile slot = new RandomAccessFile(file(".bin"), "rw")) {
            slot.setLength(CheckpointStore.HEADER_SIZE + CheckpointStore.IMAGE_SIZE);
            slot.write(header);
        }
    }

    @Test public void absentSlotDoesNotHashAssets() throws Exception {
        assertEquals(CheckpointStore.Kind.ABSENT, store.inspect().kind);
        assertEquals(0, opens.get());
    }

    @Test public void fingerprintsStreamImageThenRelocationsAndCacheBothRenderers() throws Exception {
        CheckpointStore.Fingerprints hashes = store.fingerprints();
        byte[] expected = MessageDigest.getInstance("SHA-256").digest(
            "halo_guest.elfhalo_guest.relocs".getBytes(StandardCharsets.US_ASCII));
        StringBuilder hex = new StringBuilder();
        for (byte value : expected)
            hex.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        assertEquals(hex.toString(), hashes.gl);
        assertNotEquals(hashes.gl, hashes.vk);
        assertSame(hashes, store.fingerprints());
        assertEquals(4, opens.get());
    }

    @Test public void compatibleSlotReadsMetadataForEitherRenderer() throws Exception {
        for (int renderer = 0; renderer < 2; renderer++) {
            slot(header(renderer));
            CheckpointStore.Candidate candidate = store.inspect();
            assertEquals(CheckpointStore.Kind.READY, candidate.kind);
            assertEquals("levels\\b30\\b30", candidate.map);
            assertEquals(2, candidate.difficulty);
        }
    }

    @Test public void configuredVulkanDoesNotRefuseGlesHostFallback() throws Exception {
        write(new File(root, "config.toml").toPath(), "[display]\nrenderer = \"vulkan\"\n");
        slot(header(0));
        assertEquals(CheckpointStore.Kind.READY, store.inspect().kind);
    }

    @Test public void differentGuestIsIncompatibleAndSlotIsUntouched() throws Exception {
        byte[] header = header(0);
        Arrays.fill(header, 32, 96, (byte) '0');
        slot(header);
        assertEquals(CheckpointStore.Kind.INCOMPATIBLE, store.inspect().kind);
        try (RandomAccessFile input = new RandomAccessFile(file(".bin"), "r")) {
            byte[] after = new byte[header.length];
            input.readFully(after);
            assertArrayEquals(header, after);
        }
    }

    @Test public void invalidHeaderFieldsAreRefused() throws Exception {
        int[][] invalid = { {8, 2}, {12, 256}, {16, 0}, {16, 128 * 1024 * 1024},
            {24, -1}, {24, 4}, {28, -1}, {28, 2} };
        for (int[] field : invalid) {
            byte[] header = header(0);
            ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN).putInt(field[0], field[1]);
            slot(header);
            assertEquals("offset " + field[0], CheckpointStore.Kind.CORRUPT, store.inspect().kind);
        }
        byte[] header = header(0);
        header[0] = 'X';
        slot(header);
        assertEquals(CheckpointStore.Kind.CORRUPT, store.inspect().kind);
        header = header(0);
        header[511] = 1;
        slot(header);
        assertEquals(CheckpointStore.Kind.CORRUPT, store.inspect().kind);
    }

    @Test public void partialOrOversizedContainerIsRefused() throws Exception {
        slot(header(0));
        for (long size : new long[] { 0, 511, 512, 512L + CheckpointStore.IMAGE_SIZE - 1,
                512L + CheckpointStore.IMAGE_SIZE + 1 }) {
            try (RandomAccessFile input = new RandomAccessFile(file(".bin"), "rw")) {
                input.setLength(size);
            }
            assertEquals(CheckpointStore.Kind.CORRUPT, store.inspect().kind);
            slot(header(0));
        }
    }

    @Test public void mapNamesNeedSafeRelativeCharactersAndNulTerminator() throws Exception {
        for (String map : new String[] { "../b30", "/levels/b30", "\\levels\\b30", "levels\nb30", "" }) {
            byte[] header = header(0);
            Arrays.fill(header, 96, 352, (byte) 0);
            byte[] bytes = map.getBytes(StandardCharsets.US_ASCII);
            System.arraycopy(bytes, 0, header, 96, bytes.length);
            slot(header);
            assertEquals(map, CheckpointStore.Kind.CORRUPT, store.inspect().kind);
        }
        byte[] header = header(0);
        Arrays.fill(header, 96, 352, (byte) 'a');
        slot(header);
        assertEquals(CheckpointStore.Kind.CORRUPT, store.inspect().kind);
    }

    @Test public void requestAndBootHaveExactTokensWithoutChangingInitOrSlot() throws Exception {
        slot(header(0));
        File init = new File(root, "init.txt");
        write(init.toPath(), "render_shadows false\nmap_name levels\\b30\\b30\n");
        String before = read(init.toPath());
        CheckpointStore.Fingerprints hash = store.fingerprints();
        store.writeRequest(ID);
        assertEquals(ID + " save " + hash.gl + " " + hash.vk + "\n", read(file(".request").toPath()));
        store.writeBoot(ID, true);
        assertEquals(ID + " continue " + hash.gl + " " + hash.vk + "\n", read(file(".boot").toPath()));
        store.writeBoot(OTHER_ID, false);
        assertEquals(OTHER_ID + " new " + hash.gl + " " + hash.vk + "\n", read(file(".boot").toPath()));
        assertEquals(before, read(init.toPath()));
        assertEquals(CheckpointStore.Kind.READY, store.inspect().kind);
        assertFalse(file(".request.tmp").exists());
        assertFalse(file(".boot.tmp").exists());
    }

    @Test public void onlyMatchingAcknowledgementAndOperationCanComplete() throws Exception {
        for (String text : new String[] { OTHER_ID + "\nsaved\n", ID + "\nresumed\n",
                ID + "\nsaved\nextra\n", ID + " saved", ID + "\nsaved \n" }) {
            write(file(".status").toPath(), text);
            assertNull(store.result(ID, false));
        }
        for (String result : CheckpointStore.SAVE_RESULTS) {
            write(file(".status").toPath(), ID + "\n" + result + "\n");
            assertEquals(result, store.result(ID, false));
        }
        for (String result : CheckpointStore.BOOT_RESULTS) {
            write(file(".status").toPath(), ID + "\n" + result);
            assertEquals(result, store.result(ID, true));
        }
    }

    @Test public void oversizedStatusIsBounded() throws Exception {
        write(file(".status").toPath(), new String(new char[129]).replace((char) 0, 'x'));
        assertThrows(IOException.class, () -> store.result(ID, false));
    }

    @Test public void cancellationCannotRemoveAnotherRequestOrOldCheckpoint() throws Exception {
        slot(header(0));
        store.writeRequest(OTHER_ID);
        store.cancelRequest(ID);
        assertTrue(file(".request").exists());
        store.cancelRequest(OTHER_ID);
        assertFalse(file(".request").exists());
        store.writeBoot(ID, true);
        store.clearBoot();
        assertFalse(file(".boot").exists());
        assertEquals(CheckpointStore.Kind.READY, store.inspect().kind);
    }

    @Test public void interruptionCannotReplaceExistingCommand() throws Exception {
        store.writeRequest(ID);
        String before = read(file(".request").toPath());
        Thread.currentThread().interrupt();
        try { assertThrows(IOException.class, () -> store.writeRequest(OTHER_ID)); }
        finally { Thread.interrupted(); }
        assertEquals(before, read(file(".request").toPath()));
        assertFalse(file(".request.tmp").exists());
    }

    @Test public void invalidIdsCannotWriteOrConsumeFiles() throws Exception {
        for (String bad : new String[] { "", "../x", ID + "\n", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" }) {
            assertThrows(IOException.class, () -> store.writeRequest(bad));
            assertThrows(IOException.class, () -> store.writeBoot(bad, true));
            assertThrows(IOException.class, () -> store.result(bad, true));
            assertThrows(IOException.class, () -> store.cancelRequest(bad));
        }
        assertFalse(file(".request").exists());
    }
}
