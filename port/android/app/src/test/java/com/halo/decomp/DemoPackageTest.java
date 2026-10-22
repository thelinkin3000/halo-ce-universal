package com.halo.decomp;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import java.io.File;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicBoolean;
import static org.junit.Assert.*;

public class DemoPackageTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test public void sourceHasPinnedSizeAndDigest() throws Exception {
        assertEquals("download.digiex.net", ControlsDownload.validateUrl(DemoPackage.URL).getHost());
        assertEquals(397660357L, DemoPackage.SIZE);
        assertTrue(DemoPackage.SHA256.matches("[0-9a-f]{64}"));
    }

    @Test public void hashMatchesKnownSha256AndChangesWithContents() throws Exception {
        File archive = temporary.newFile();
        Files.write(archive.toPath(), "abc".getBytes(StandardCharsets.US_ASCII));
        assertEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            DemoPackage.sha256(archive, new AtomicBoolean()));
        Files.write(archive.toPath(), "abcd".getBytes(StandardCharsets.US_ASCII));
        assertNotEquals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            DemoPackage.sha256(archive, new AtomicBoolean()));
    }

    @Test public void truncatedArchiveIsRejectedBeforeInstallation() throws Exception {
        try { DemoPackage.verify(temporary.newFile(), new AtomicBoolean()); fail("Must be complete"); }
        catch (IOException expected) { assertTrue(expected.getMessage().contains("No se instaló")); }
    }

    @Test public void canceledVerificationStopsBeforeReading() throws Exception {
        File archive = temporary.newFile();
        try { DemoPackage.verify(archive, new AtomicBoolean(true)); fail("Canceled"); }
        catch (InterruptedIOException expected) { /* expected */ }
        try { DemoPackage.sha256(archive, new AtomicBoolean(true)); fail("Canceled"); }
        catch (InterruptedIOException expected) { /* expected */ }
    }
}
