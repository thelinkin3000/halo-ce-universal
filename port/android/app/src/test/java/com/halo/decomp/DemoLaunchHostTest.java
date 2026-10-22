package com.halo.decomp;

import static org.junit.Assert.*;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class DemoLaunchHostTest {
    private static final String ID = "0123456789abcdef0123456789abcdef";
    private static final String DEMO = "map_name levels\\demob30\\demob30";
    @Rule public TemporaryFolder temp = new TemporaryFolder();
    private File root, files, init, status;
    private DemoLaunch demo;

    @Before public void setUp() throws Exception {
        root = temp.newFolder("data");
        files = temp.newFolder("private");
        assertTrue(new File(root, "maps").mkdir());
        assertTrue(new File(root, "save/z").mkdirs());
        write(new File(root, "maps/ui.map"), "ui");
        write(new File(root, "maps/demob30.map"), "demo");
        write(new File(root, "config.toml"), "render_scale = 1");
        write(new File(root, "save/z/mobile-checkpoint.bin"), "existing checkpoint");
        init = new File(root, "init.txt");
        status = new File(root, "save/z/mobile-checkpoint.status");
        demo = new DemoLaunch(root, files);
    }

    @Test public void changesOnlyActiveMapCommandsAndRestoresExactOriginalBytes() throws Exception {
        String original = "\uFEFF  map_name levels\\b30\\b30\r\nrender_shadows false\r\n"
            + "// map_name keep_this\n# map_name keep_this_too\n\tMAP_NAME levels\\a10\\a10\n"
            + "render_scale 1\n// precisión";
        write(init, original);
        demo.prepare(ID);
        assertEquals("\uFEFF  " + DEMO + "\r\nrender_shadows false\r\n"
            + "// map_name keep_this\n# map_name keep_this_too\n\t\nrender_scale 1\n// precisión", read(init));
        write(status, ID + "\nnew\n");
        assertTrue(demo.restoreIfAcknowledged());
        assertEquals(original, read(init));
        assertUnchangedGameData();
    }

    @Test public void appendsMapWithoutChangingGraphicsOrSimilarCommandNames() throws Exception {
        write(init, "render_shadows false\nmap_name_extra custom\nrender_scale 1");
        demo.prepare(ID);
        assertEquals("render_shadows false\nmap_name_extra custom\nrender_scale 1\n" + DEMO + "\n", read(init));
        demo.recover();
        assertEquals("render_shadows false\nmap_name_extra custom\nrender_scale 1", read(init));
    }

    @Test public void mixedParenthesizedAndBareCommandsCannotOverrideDemo() throws Exception {
        String original = "render_shadows false\r\n \t( MAP_NAME \"levels\\a10\\a10\" )\r\n"
            + "map_name levels\\a30\\a30\n\t(map_name levels\\b30\\b30)\n// (map_name unchanged)\n";
        write(init, original);
        demo.prepare(ID);
        assertEquals("render_shadows false\r\n \t" + DEMO + "\r\n\n\t\n// (map_name unchanged)\n", read(init));
        demo.recover();
        assertEquals(original, read(init));
        assertUnchangedGameData();
    }

    @Test public void absentInitIsRemovedAfterAcknowledgement() throws Exception {
        demo.prepare(ID);
        assertEquals(DEMO + "\n", read(init));
        write(status, ID + "\nnew\n");
        assertTrue(demo.restoreIfAcknowledged());
        assertFalse(init.exists());
        assertFalse(new File(files, "demo-launch.bin").exists());
        assertUnchangedGameData();
    }

    @Test public void ignoresMissingWrongAndNonNewAcknowledgement() throws Exception {
        write(init, "map_name levels\\a10\\a10\n");
        demo.prepare(ID);
        assertFalse(demo.restoreIfAcknowledged());
        write(status, "ffffffffffffffffffffffffffffffff\nnew\n");
        assertFalse(demo.restoreIfAcknowledged());
        write(status, ID + "\nresumed\n");
        assertFalse(demo.restoreIfAcknowledged());
        assertEquals(DEMO + "\n", read(init));
        write(status, ID + "\nnew\n");
        assertTrue(demo.restoreIfAcknowledged());
        assertTrue(demo.restoreIfAcknowledged());
    }

    @Test public void coldRecoverySurvivesNewHelperInstanceWithoutAcknowledgement() throws Exception {
        write(init, "render_scale 1\nmap_name levels\\a10\\a10\n");
        demo.prepare(ID);
        new DemoLaunch(root, files).recover();
        assertEquals("render_scale 1\nmap_name levels\\a10\\a10\n", read(init));
        demo.recover();
        assertUnchangedGameData();
    }

    @Test public void noDemoMapLeavesInitAndCheckpointUntouched() throws Exception {
        Files.delete(new File(root, "maps/demob30.map").toPath());
        write(init, "original");
        assertFalse(demo.available());
        expectFailure(() -> demo.prepare(ID));
        assertEquals("original", read(init));
        assertFalse(new File(files, "demo-launch.bin").exists());
        assertEquals("existing checkpoint", read(new File(root, "save/z/mobile-checkpoint.bin")));
    }

    @Test public void missingUiAlsoDisablesDemo() throws Exception {
        Files.delete(new File(root, "maps/ui.map").toPath());
        assertFalse(demo.available());
        expectFailure(() -> demo.prepare(ID));
    }

    @Test public void rejectsMalformedTokenBeforeChangingAnything() throws Exception {
        write(init, "original");
        for (String token : new String[] { null, "", "../../init.txt", ID.toUpperCase(), ID + "\n" })
            expectFailure(() -> demo.prepare(token));
        assertEquals("original", read(init));
        assertFalse(new File(files, "demo-launch.bin").exists());
    }

    @Test public void pendingDemoCannotReplaceOriginalJournal() throws Exception {
        write(init, "original");
        demo.prepare(ID);
        byte[] before = Files.readAllBytes(new File(files, "demo-launch.bin").toPath());
        expectFailure(() -> new DemoLaunch(root, files).prepare("ffffffffffffffffffffffffffffffff"));
        assertArrayEquals(before, Files.readAllBytes(new File(files, "demo-launch.bin").toPath()));
        demo.recover();
        assertEquals("original", read(init));
    }

    @Test public void externalInitEditsArePreservedWithRecoveryJournal() throws Exception {
        write(init, "original");
        demo.prepare(ID);
        write(init, "user changed resolution\n");
        write(status, ID + "\nnew\n");
        expectFailure(() -> demo.restoreIfAcknowledged());
        expectFailure(() -> demo.recover());
        assertEquals("user changed resolution\n", read(init));
        assertTrue(new File(files, "demo-launch.bin").isFile());
        assertUnchangedGameData();
    }

    @Test public void missingPreparedInitIsNotSilentlyRecreated() throws Exception {
        write(init, "original");
        demo.prepare(ID);
        Files.delete(init.toPath());
        expectFailure(() -> demo.recover());
        assertFalse(init.exists());
        assertTrue(new File(files, "demo-launch.bin").isFile());
    }

    @Test public void alreadyRestoredOriginalAllowsJournalCleanup() throws Exception {
        write(init, "original");
        demo.prepare(ID);
        write(init, "original");
        demo.recover();
        assertEquals("original", read(init));
        assertFalse(new File(files, "demo-launch.bin").exists());
    }

    @Test public void malformedJournalCannotModifyInit() throws Exception {
        write(init, "original");
        write(new File(files, "demo-launch.bin"), "broken journal");
        expectFailure(() -> demo.recover());
        assertEquals("original", read(init));
        assertEquals("broken journal", read(new File(files, "demo-launch.bin")));
    }

    @Test public void oversizedOrBinaryInitIsRejectedBeforeJournalCreation() throws Exception {
        Files.write(init.toPath(), new byte[256 * 1024 + 1]);
        expectFailure(() -> demo.prepare(ID));
        assertFalse(new File(files, "demo-launch.bin").exists());
        write(init, "graphics\u0000invalid");
        expectFailure(() -> demo.prepare(ID));
        assertFalse(new File(files, "demo-launch.bin").exists());
    }

    @Test public void menuRemovesBareAndParenthesizedMapCommandsAndAcknowledgesExactOriginal() throws Exception {
        String original = "\uFEFF  MAP_NAME levels\\demob30\\demob30\r\nrender_scale 1\r\n"
            + "\t( map_name \"levels\\a10\\a10\" )\n// map_name keep_this\n"
            + "# (map_name keep_this_too)\nmap_name_extra custom\n// precisión";
        byte[] before = original.getBytes(StandardCharsets.UTF_8);
        Files.write(init.toPath(), before);
        demo.prepareMenu(ID);
        assertEquals("\uFEFF  \r\nrender_scale 1\r\n\t\n// map_name keep_this\n"
            + "# (map_name keep_this_too)\nmap_name_extra custom\n// precisión", read(init));
        write(status, ID + "\nnew\n");
        assertTrue(demo.restoreIfAcknowledged());
        assertArrayEquals(before, Files.readAllBytes(init.toPath()));
        assertFalse(new File(files, "demo-launch.bin").exists());
        assertUnchangedGameData();
    }

    @Test public void menuNeedsNoDemoMapAndDoesNotInsertMapIntoGraphicsOnlyInit() throws Exception {
        Files.delete(new File(root, "maps/demob30.map").toPath());
        String original = "render_shadows false\r\nrender_scale 1";
        write(init, original);
        assertFalse(demo.available());
        demo.prepareMenu(ID);
        assertEquals(original, read(init));
        write(status, ID + "\nnew\n");
        assertTrue(demo.restoreIfAcknowledged());
        assertEquals(original, read(init));
        assertFalse(new File(root, "maps/demob30.map").exists());
        assertEquals("ui", read(new File(root, "maps/ui.map")));
        assertEquals("render_scale = 1", read(new File(root, "config.toml")));
        assertEquals("existing checkpoint", read(new File(root, "save/z/mobile-checkpoint.bin")));
    }

    @Test public void menuWithoutUiCannotChangeInitOrCreateRecoveryJournal() throws Exception {
        Files.delete(new File(root, "maps/ui.map").toPath());
        write(init, "map_name levels\\a10\\a10\r\nrender_scale 1");
        byte[] before = Files.readAllBytes(init.toPath());
        expectFailure(() -> demo.prepareMenu(ID));
        assertArrayEquals(before, Files.readAllBytes(init.toPath()));
        assertFalse(new File(files, "demo-launch.bin").exists());
        assertEquals("demo", read(new File(root, "maps/demob30.map")));
        assertEquals("render_scale = 1", read(new File(root, "config.toml")));
        assertEquals("existing checkpoint", read(new File(root, "save/z/mobile-checkpoint.bin")));
    }

    @Test public void menuIgnoresWrongBootAndResumeAcknowledgements() throws Exception {
        write(init, "map_name levels\\a10\\a10\nrender_scale 1");
        demo.prepareMenu(ID);
        byte[] journal = Files.readAllBytes(new File(files, "demo-launch.bin").toPath());
        assertFalse(demo.restoreIfAcknowledged());
        for (String acknowledgement : new String[] {
                "ffffffffffffffffffffffffffffffff\nnew\n", ID + "\nresumed\n" }) {
            write(status, acknowledgement);
            assertFalse(demo.restoreIfAcknowledged());
            assertEquals("\nrender_scale 1", read(init));
            assertArrayEquals(journal, Files.readAllBytes(new File(files, "demo-launch.bin").toPath()));
        }
        write(status, ID + "\nnew\n");
        assertTrue(demo.restoreIfAcknowledged());
        assertEquals("map_name levels\\a10\\a10\nrender_scale 1", read(init));
        assertUnchangedGameData();
    }

    @Test public void menuColdRecoveryRestoresExactBytesWithoutAcknowledgement() throws Exception {
        byte[] original = "\uFEFF\t(map_name levels\\a30\\a30)\r\n// resolución\r\nrender_scale 1"
            .getBytes(StandardCharsets.UTF_8);
        Files.write(init.toPath(), original);
        demo.prepareMenu(ID);
        assertFalse(java.util.Arrays.equals(original, Files.readAllBytes(init.toPath())));
        new DemoLaunch(root, files).recover();
        assertArrayEquals(original, Files.readAllBytes(init.toPath()));
        assertFalse(new File(files, "demo-launch.bin").exists());
        assertUnchangedGameData();
    }

    @Test public void menuRemovesOriginallyAbsentInitAfterAcknowledgementOrColdRecovery() throws Exception {
        for (boolean acknowledge : new boolean[] { true, false }) {
            assertFalse(init.exists());
            Files.deleteIfExists(status.toPath());
            demo.prepareMenu(ID);
            assertTrue(init.isFile());
            assertEquals("", read(init));
            if (acknowledge) {
                write(status, ID + "\nnew\n");
                assertTrue(demo.restoreIfAcknowledged());
            } else {
                new DemoLaunch(root, files).recover();
            }
            assertFalse(init.exists());
            assertFalse(new File(files, "demo-launch.bin").exists());
            assertUnchangedGameData();
        }
    }

    @Test public void menuCannotReplaceAnExistingDemoRecoveryJournal() throws Exception {
        write(init, "map_name levels\\a10\\a10\nrender_scale 1");
        demo.prepare(ID);
        byte[] prepared = Files.readAllBytes(init.toPath());
        byte[] journal = Files.readAllBytes(new File(files, "demo-launch.bin").toPath());
        expectFailure(() -> new DemoLaunch(root, files).prepareMenu("ffffffffffffffffffffffffffffffff"));
        assertArrayEquals(prepared, Files.readAllBytes(init.toPath()));
        assertArrayEquals(journal, Files.readAllBytes(new File(files, "demo-launch.bin").toPath()));
        new DemoLaunch(root, files).recover();
        assertEquals("map_name levels\\a10\\a10\nrender_scale 1", read(init));
        assertUnchangedGameData();
    }

    @Test public void menuBootUsesNewOperationAndPreservesExistingCheckpoint() throws Exception {
        write(init, "map_name levels\\a10\\a10\nrender_scale 1");
        File campaign = new File(root, "maps/a10.map");
        File snapshot = new File(root, "save/z/mobile-checkpoint-slots/" + ID + ".slot");
        assertTrue(snapshot.getParentFile().mkdir());
        write(campaign, "campaign map");
        write(snapshot, "named checkpoint");
        CheckpointStore checkpoints = new CheckpointStore(root,
            name -> new ByteArrayInputStream(new byte[] { 1 }));
        demo.prepareMenu(ID);
        checkpoints.writeBoot(ID, false);
        CheckpointStore.Fingerprints fingerprints = checkpoints.fingerprints();
        assertEquals(ID + " new " + fingerprints.gl + " " + fingerprints.vk + "\n",
            read(new File(root, "save/z/mobile-checkpoint.boot")));
        assertEquals("\nrender_scale 1", read(init));
        assertUnchangedGameData();
        write(status, ID + "\nnew\n");
        assertTrue(demo.restoreIfAcknowledged());
        assertEquals("map_name levels\\a10\\a10\nrender_scale 1", read(init));
        assertEquals("campaign map", read(campaign));
        assertEquals("named checkpoint", read(snapshot));
        assertUnchangedGameData();
    }

    @Test public void menuRejectsNestedMapCommandsWithoutChangingStartupOrSaves() throws Exception {
        String original = "render_scale 1\n(begin (map_name levels\\a10\\a10))\n";
        write(init, original);
        expectFailure(() -> demo.prepareMenu(ID));
        assertEquals(original, read(init));
        assertFalse(new File(files, "demo-launch.bin").exists());
        assertUnchangedGameData();
    }

    private void assertUnchangedGameData() throws Exception {
        assertEquals("render_scale = 1", read(new File(root, "config.toml")));
        assertEquals("existing checkpoint", read(new File(root, "save/z/mobile-checkpoint.bin")));
        assertEquals("ui", read(new File(root, "maps/ui.map")));
        assertEquals("demo", read(new File(root, "maps/demob30.map")));
    }

    private interface Operation { void run() throws Exception; }

    private static void expectFailure(Operation operation) throws Exception {
        try { operation.run(); fail("Expected IOException"); }
        catch (IOException expected) { }
    }

    private static void write(File file, String text) throws IOException {
        Files.write(file.toPath(), text.getBytes(StandardCharsets.UTF_8));
    }

    private static String read(File file) throws IOException {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }
}
