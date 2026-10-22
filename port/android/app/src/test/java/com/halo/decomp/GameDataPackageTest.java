package com.halo.decomp;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

import static org.junit.Assert.*;

public class GameDataPackageTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();
    private File root;

    @Before public void prepare() throws Exception { root = temporary.newFolder(); }

    private byte[] map() {
        byte[] bytes = new byte[0x800];
        ByteBuffer header = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        header.putInt(0, 0x68656164).putInt(4, 5).putInt(8, 0x800)
            .putInt(0x10, 0x800).putInt(0x14, 0).putInt(0x7fc, 0x666f6f74);
        return bytes;
    }

    private File zip(String[] names, byte[][] contents) throws Exception {
        File archive = temporary.newFile();
        try (ZipOutputStream output = new ZipOutputStream(new FileOutputStream(archive))) {
            for (int index = 0; index < names.length; index++) {
                output.putNextEntry(new ZipEntry(names[index]));
                output.write(contents[index]);
                output.closeEntry();
            }
        }
        return archive;
    }

    private File validZip() throws Exception {
        return zip(new String[] { "bundle/maps/ui.map", "bundle/maps/b30.map" }, new byte[][] { map(), map() });
    }

    private File iso() throws Exception {
        byte[] image = new byte[44 * 2048];
        ByteBuffer fields = ByteBuffer.wrap(image).order(ByteOrder.LITTLE_ENDIAN);
        byte[] magic = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(magic, 0, image, 0x10000, magic.length);
        System.arraycopy(magic, 0, image, 0x107ec, magic.length);
        fields.putInt(0x10000 + 20, 40).putInt(0x10000 + 24, 32);
        discEntry(fields, 40 * 2048, 41, 64, 0x10, "maps");
        discEntry(fields, 41 * 2048, 42, 2048, 0, "UI.MAP");
        fields.putShort(41 * 2048 + 2, (short) 8);
        discEntry(fields, 41 * 2048 + 32, 43, 2048, 0, "B30.MAP");
        System.arraycopy(map(), 0, image, 42 * 2048, 2048);
        System.arraycopy(map(), 0, image, 43 * 2048, 2048);
        File archive = temporary.newFile();
        Files.write(archive.toPath(), image);
        return archive;
    }

    private void discEntry(ByteBuffer image, int at, int sector, int size, int attributes, String name) {
        byte[] bytes = name.getBytes(StandardCharsets.US_ASCII);
        image.putInt(at + 4, sector).putInt(at + 8, size).put(at + 12, (byte) attributes)
            .put(at + 13, (byte) bytes.length);
        image.position(at + 14);
        image.put(bytes);
    }

    private String install(File archive) throws IOException {
        return GameDataPackage.install(archive, root, new AtomicBoolean(), (file, done, total) -> {});
    }

    private void rejected(File archive) throws Exception {
        try {
            install(archive);
            fail("Expected invalid package");
        } catch (IOException expected) {
            assertFalse(new File(root, "maps/ui.map").exists());
            assertNoStage();
        }
    }

    private void assertNoStage() {
        String[] stages = root.list((directory, name) -> name.startsWith(".halo-data-"));
        assertNotNull(stages);
        assertEquals(0, stages.length);
    }

    private void write(File file, String text) throws Exception {
        if (!file.getParentFile().exists())
            assertTrue(file.getParentFile().mkdirs());
        Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    private String read(File file) throws Exception {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }

    @Test public void zipInstallsOnlyMapsAndPreservesSettingsAndSaves() throws Exception {
        write(new File(root, "save/z/checkpoint.bin"), "save");
        write(new File(root, "init.txt"), "original init");
        write(new File(root, "config.toml"), "original config");
        File archive = zip(new String[] { "package/init.txt", "package/config.toml", "package/save/z/file",
            "package/maps/ui.map", "package/maps/a10.map", "package/maps/loading.tga", "package/maps/code.so" },
            new byte[][] { { 1 }, { 1 }, { 1 }, map(), map(), { 1, 2 }, { 1 } });
        assertEquals("a10", install(archive));
        assertArrayEquals(map(), Files.readAllBytes(new File(root, "maps/ui.map").toPath()));
        assertTrue(new File(root, "maps/a10.map").isFile());
        assertTrue(new File(root, "maps/loading.tga").isFile());
        assertFalse(new File(root, "maps/code.so").exists());
        assertEquals("save", read(new File(root, "save/z/checkpoint.bin")));
        assertEquals("original init", read(new File(root, "init.txt")));
        assertEquals("original config", read(new File(root, "config.toml")));
        assertNoStage();
    }

    @Test public void emptyLauncherMapsDirectoryIsReplaced() throws Exception {
        assertTrue(new File(root, "maps").mkdir());
        assertEquals("b30", install(validZip()));
        assertTrue(new File(root, "maps/ui.map").isFile());
        assertNoStage();
    }

    @Test public void demoUiAliasAndUppercaseFilesAreNormalized() throws Exception {
        assertEquals("demob30", install(zip(new String[] { "MAPS/DEMOUI.MAP", "MAPS/DEMOB30.MAP" },
            new byte[][] { map(), map() })));
        assertTrue(new File(root, "maps/ui.map").isFile());
        assertTrue(new File(root, "maps/demob30.map").isFile());
        assertFalse(new File(root, "maps/demoui.map").exists());
    }

    @Test public void existingGameIsNeverModifiedEvenWithCanceledRequest() throws Exception {
        write(new File(root, "maps/ui.map"), "existing game");
        write(new File(root, "maps/b30.map"), "existing map");
        try { install(validZip()); fail("Existing data must be retained"); }
        catch (IOException expected) { assertEquals("existing game", read(new File(root, "maps/ui.map"))); }
        assertEquals("existing map", read(new File(root, "maps/b30.map")));
        assertNoStage();
    }

    @Test public void incompleteExistingDataAlsoRemainsUntouched() throws Exception {
        write(new File(root, "maps/custom.map"), "player data");
        rejected(validZip());
        assertEquals("player data", read(new File(root, "maps/custom.map")));
    }

    @Test public void noPlayableMapIsRejected() throws Exception {
        rejected(zip(new String[] { "maps/ui.map" }, new byte[][] { map() }));
    }

    @Test public void noUiMapIsRejected() throws Exception {
        rejected(zip(new String[] { "maps/b30.map" }, new byte[][] { map() }));
    }

    @Test public void unsupportedPcMapVersionIsRejectedWithoutPublishingUi() throws Exception {
        byte[] bad = map();
        ByteBuffer.wrap(bad).order(ByteOrder.LITTLE_ENDIAN).putInt(4, 7);
        rejected(zip(new String[] { "maps/ui.map", "maps/b30.map" }, new byte[][] { map(), bad }));
    }

    @Test public void shortMapIsRejectedWithoutPublishingUi() throws Exception {
        rejected(zip(new String[] { "maps/ui.map", "maps/b30.map" }, new byte[][] { map(), new byte[16] }));
    }

    @Test public void invalidMagicFooterLengthAndTagBoundsAreRejected() throws Exception {
        for (int field : new int[] { 0, 0x7fc, 8, 0x10, 0x14 }) {
            byte[] bad = map();
            ByteBuffer.wrap(bad).order(ByteOrder.LITTLE_ENDIAN).putInt(field, field == 0x10 ? 0x900 : -1);
            rejected(zip(new String[] { "maps/ui.map", "maps/b30.map" }, new byte[][] { map(), bad }));
        }
    }

    @Test public void unterminatedMapNameIsRejected() throws Exception {
        byte[] bad = map();
        Arrays.fill(bad, 0x20, 0x40, (byte) 'x');
        rejected(zip(new String[] { "maps/ui.map", "maps/b30.map" }, new byte[][] { map(), bad }));
    }

    @Test public void compressedXboxLengthMayExceedStoredFileLength() throws Exception {
        byte[] compressedHeader = map();
        ByteBuffer.wrap(compressedHeader).order(ByteOrder.LITTLE_ENDIAN).putInt(8, 0x10000);
        assertEquals("b30", install(zip(new String[] { "maps/ui.map", "maps/b30.map" },
            new byte[][] { compressedHeader, compressedHeader })));
    }

    @Test public void traversalAndAbsolutePathsAreRejectedEvenOutsideMaps() throws Exception {
        for (String path : new String[] { "../escape", "/absolute", "C:/absolute", "maps/../ui.map",
            "folder\\maps\\ui.map", "maps//ui.map", "./maps/ui.map" }) {
            rejected(zip(new String[] { "maps/ui.map", "maps/b30.map", path },
                new byte[][] { map(), map(), { 1 } }));
        }
        assertFalse(new File(root.getParentFile(), "escape").exists());
    }

    @Test public void duplicateCaseAndDemoAliasAreRejected() throws Exception {
        for (String other : new String[] { "UI.MAP", "demoui.map" })
            rejected(zip(new String[] { "maps/ui.map", "maps/" + other, "maps/b30.map" },
                new byte[][] { map(), map(), map() }));
    }

    @Test public void multipleMapDirectoriesAreRejected() throws Exception {
        rejected(zip(new String[] { "one/maps/ui.map", "two/maps/b30.map" }, new byte[][] { map(), map() }));
    }

    @Test public void mapNameCannotInjectAnInitCommand() throws Exception {
        rejected(zip(new String[] { "maps/ui.map", "maps/b30;quit.map" }, new byte[][] { map(), map() }));
    }

    @Test public void canceledBeforeAndDuringExtractionLeavesNoMaps() throws Exception {
        File archive = validZip();
        AtomicBoolean canceled = new AtomicBoolean(true);
        try { GameDataPackage.install(archive, root, canceled, (file, done, total) -> {}); fail("Canceled"); }
        catch (InterruptedIOException expected) { assertNoStage(); }
        canceled.set(false);
        try {
            GameDataPackage.install(archive, root, canceled, (file, done, total) -> canceled.set(true));
            fail("Canceled");
        } catch (InterruptedIOException expected) {
            assertFalse(new File(root, "maps/ui.map").exists());
            assertNoStage();
        }
    }

    @Test public void currentDataAppearingDuringExtractionIsRetained() throws Exception {
        File archive = validZip();
        try {
            GameDataPackage.install(archive, root, new AtomicBoolean(), (file, done, total) -> {
                try { write(new File(root, "maps/current.map"), "keep"); }
                catch (Exception error) { throw new AssertionError(error); }
            });
            fail("Do not overwrite concurrent data");
        } catch (IOException expected) {
            assertFalse(new File(root, "maps/ui.map").exists());
            assertEquals("keep", read(new File(root, "maps/current.map")));
            assertNoStage();
        }
    }

    @Test public void truncatedZipIsRejected() throws Exception {
        File archive = validZip();
        byte[] bytes = Files.readAllBytes(archive.toPath());
        Files.write(archive.toPath(), Arrays.copyOf(bytes, bytes.length - 40));
        rejected(archive);
    }

    @Test public void wrongStoredCrcIsRejected() throws Exception {
        File archive = temporary.newFile();
        byte[] bytes = map();
        CRC32 crc = new CRC32();
        crc.update(bytes);
        try (ZipOutputStream output = new ZipOutputStream(new FileOutputStream(archive))) {
            for (String name : new String[] { "maps/ui.map", "maps/b30.map" }) {
                ZipEntry entry = new ZipEntry(name);
                entry.setMethod(ZipEntry.STORED);
                entry.setSize(bytes.length);
                entry.setCrc(crc.getValue());
                output.putNextEntry(entry);
                output.write(bytes);
                output.closeEntry();
            }
        }
        byte[] zip = Files.readAllBytes(archive.toPath());
        // Corrupt the first stored map's contents; central-directory CRC remains original.
        zip[30 + "maps/ui.map".length() + 100] ^= 1;
        Files.write(archive.toPath(), zip);
        rejected(archive);
    }

    @Test public void overFileCountIsRejected() throws Exception {
        String[] names = new String[257];
        byte[][] contents = new byte[257][];
        for (int index = 0; index < names.length; index++) {
            names[index] = "ignored/" + index;
            contents[index] = new byte[0];
        }
        rejected(zip(names, contents));
    }

    @Test public void oversizedDeclaredEntryIsRejectedBeforeExtraction() throws Exception {
        File archive = validZip();
        byte[] bytes = Files.readAllBytes(archive.toPath());
        for (int index = 0; index < bytes.length - 46; index++) {
            if (bytes[index] == 'P' && bytes[index + 1] == 'K' && bytes[index + 2] == 1 && bytes[index + 3] == 2) {
                ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putInt(index + 24, 0x20000001);
                break;
            }
        }
        Files.write(archive.toPath(), bytes);
        rejected(archive);
    }

    @Test public void totalDeclaredExpansionIsBoundedIncludingUnselectedFiles() throws Exception {
        String[] names = { "maps/ui.map", "maps/b30.map", "one", "two", "three", "four", "five" };
        byte[][] contents = { map(), map(), { 1 }, { 1 }, { 1 }, { 1 }, { 1 } };
        File archive = zip(names, contents);
        byte[] bytes = Files.readAllBytes(archive.toPath());
        int entries = 0;
        for (int index = 0; index < bytes.length - 46; index++) {
            if (bytes[index] == 'P' && bytes[index + 1] == 'K' && bytes[index + 2] == 1 && bytes[index + 3] == 2) {
                if (++entries > 2)
                    ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).putInt(index + 24, 0x20000000);
            }
        }
        assertEquals(names.length, entries);
        Files.write(archive.toPath(), bytes);
        rejected(archive);
    }

    @Test public void xboxIsoUsesTheSameIsolatedValidationAndPublish() throws Exception {
        assertEquals("b30", install(iso()));
        assertArrayEquals(map(), Files.readAllBytes(new File(root, "maps/ui.map").toPath()));
        assertArrayEquals(map(), Files.readAllBytes(new File(root, "maps/b30.map").toPath()));
        assertNoStage();
    }

    @Test public void canceledIsoRemovesItsPartialStageAndPreservesSave() throws Exception {
        write(new File(root, "save/z/checkpoint.bin"), "save");
        AtomicBoolean canceled = new AtomicBoolean();
        try {
            GameDataPackage.install(iso(), root, canceled, (name, done, total) -> canceled.set(true));
            fail("Canceled");
        } catch (InterruptedIOException expected) {
            assertFalse(new File(root, "maps/ui.map").exists());
            assertEquals("save", read(new File(root, "save/z/checkpoint.bin")));
            assertNoStage();
        }
    }

    @Test public void truncatedIsoNeverPublishesItsAlreadyCopiedUiMap() throws Exception {
        File archive = iso();
        byte[] bytes = Files.readAllBytes(archive.toPath());
        Files.write(archive.toPath(), Arrays.copyOf(bytes, 43 * 2048 + 100));
        rejected(archive);
    }

    @Test public void randomOrTruncatedImageIsRejected() throws Exception {
        File archive = temporary.newFile();
        Files.write(archive.toPath(), new byte[4096]);
        rejected(archive);
        Files.write(archive.toPath(), new byte[2]);
        rejected(archive);
    }

    private File demoZip() throws Exception {
        return zip(new String[] { "Halo Demo/maps/demoui.map", "Halo Demo/maps/demob30.map" },
            new byte[][] { map(), map() });
    }

    private void demo(File archive) throws IOException {
        GameDataPackage.installDemo(archive, root, new AtomicBoolean(), (file, done, total) -> {});
    }

    @Test public void demoAddsItsMapsAndIsReadyWithoutAnInitScript() throws Exception {
        assertFalse(GameDataPackage.demoAvailable(root));
        demo(demoZip());
        assertTrue(GameDataPackage.demoAvailable(root));
        assertFalse(new File(root, "init.txt").exists());
        assertFalse(new File(root, "maps/demoui.map").exists());
        assertNoStage();
    }

    @Test public void demoPreservesRetailUiMapsConfigAndSavedGames() throws Exception {
        assertTrue(new File(root, "maps").mkdir());
        byte[] retailUi = map();
        retailUi[100] = 9;
        Files.write(new File(root, "maps/ui.map").toPath(), retailUi);
        write(new File(root, "maps/a10.map"), "retail map");
        write(new File(root, "maps/loading.tga"), "retail loading");
        write(new File(root, "init.txt"), "player init");
        write(new File(root, "config.toml"), "player config");
        write(new File(root, "save/z/checkpoint.bin"), "checkpoint");
        demo(demoZip());
        assertArrayEquals(retailUi, Files.readAllBytes(new File(root, "maps/ui.map").toPath()));
        assertEquals("retail map", read(new File(root, "maps/a10.map")));
        assertEquals("retail loading", read(new File(root, "maps/loading.tga")));
        assertEquals("player init", read(new File(root, "init.txt")));
        assertEquals("player config", read(new File(root, "config.toml")));
        assertEquals("checkpoint", read(new File(root, "save/z/checkpoint.bin")));
        assertTrue(GameDataPackage.demoAvailable(root));
        assertNoStage();
    }

    @Test public void demoNeverReplacesAnExistingDemoMap() throws Exception {
        assertTrue(new File(root, "maps").mkdir());
        byte[] current = map();
        current[100] = 7;
        Files.write(new File(root, "maps/demob30.map").toPath(), current);
        demo(demoZip());
        assertArrayEquals(current, Files.readAllBytes(new File(root, "maps/demob30.map").toPath()));
        assertTrue(GameDataPackage.demoAvailable(root));
        assertNoStage();
    }

    @Test public void failedDemoMergeRollsBackOnlyItsOwnNewMap() throws Exception {
        write(new File(root, "maps/ui.map"), "preserve invalid player UI");
        write(new File(root, "maps/a10.map"), "keep");
        try { demo(demoZip()); fail("Do not replace existing UI"); }
        catch (IOException expected) {
            assertEquals("preserve invalid player UI", read(new File(root, "maps/ui.map")));
            assertEquals("keep", read(new File(root, "maps/a10.map")));
            assertFalse(new File(root, "maps/demob30.map").exists());
            assertFalse(GameDataPackage.demoAvailable(root));
            assertNoStage();
        }
    }

    @Test public void retailArchiveCannotBeInstalledAsTheDemo() throws Exception {
        try { demo(validZip()); fail("Must contain demob30"); }
        catch (IOException expected) {
            assertFalse(new File(root, "maps/ui.map").exists());
            assertFalse(new File(root, "maps/b30.map").exists());
            assertNoStage();
        }
    }

    @Test public void demoMergeDoesNotPublishOtherMapsFromTheArchive() throws Exception {
        demo(zip(new String[] { "maps/demoui.map", "maps/demob30.map", "maps/a10.map" },
            new byte[][] { map(), map(), map() }));
        assertTrue(GameDataPackage.demoAvailable(root));
        assertFalse(new File(root, "maps/a10.map").exists());
        assertNoStage();
    }

    @Test public void cancelingDemoExtractionLeavesCurrentGameUntouched() throws Exception {
        write(new File(root, "maps/a10.map"), "keep map");
        write(new File(root, "save/z/checkpoint.bin"), "keep save");
        AtomicBoolean canceled = new AtomicBoolean();
        try {
            GameDataPackage.installDemo(demoZip(), root, canceled, (file, done, total) -> canceled.set(true));
            fail("Canceled");
        } catch (InterruptedIOException expected) {
            assertEquals("keep map", read(new File(root, "maps/a10.map")));
            assertEquals("keep save", read(new File(root, "save/z/checkpoint.bin")));
            assertFalse(new File(root, "maps/demob30.map").exists());
            assertFalse(new File(root, "maps/ui.map").exists());
            assertNoStage();
        }
    }

    @Test public void demoReadinessChecksXboxHeadersNotOnlyFileNames() throws Exception {
        write(new File(root, "maps/ui.map"), "not a map");
        write(new File(root, "maps/demob30.map"), "not a map");
        assertFalse(GameDataPackage.demoAvailable(root));
        Files.write(new File(root, "maps/ui.map").toPath(), map());
        assertFalse(GameDataPackage.demoAvailable(root));
        Files.write(new File(root, "maps/demob30.map").toPath(), map());
        assertTrue(GameDataPackage.demoAvailable(root));
    }
}
