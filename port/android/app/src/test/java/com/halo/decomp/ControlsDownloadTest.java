package com.halo.decomp;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayDeque;
import java.util.Queue;
import java.util.concurrent.atomic.AtomicBoolean;

import static org.junit.Assert.*;

public class ControlsDownloadTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    private static final class Response extends HttpURLConnection {
        final byte[] bytes;
        final long length;
        final int code;
        String location;
        String contentType;
        boolean disconnected;
        boolean streamOpened;
        InputStream stream;
        Response(int code, String body, long length) throws Exception {
            super(new URL("https://example.test/package"));
            this.code = code;
            this.bytes = body.getBytes(StandardCharsets.UTF_8);
            this.length = length;
        }
        @Override public int getResponseCode() { return code; }
        @Override public long getContentLengthLong() { return length; }
        @Override public String getContentType() { return contentType; }
        @Override public String getHeaderField(String name) { return "Location".equals(name) ? location : null; }
        @Override public InputStream getInputStream() {
            streamOpened = true;
            return stream == null ? new ByteArrayInputStream(bytes) : stream;
        }
        @Override public void disconnect() { disconnected = true; }
        @Override public boolean usingProxy() { return false; }
        @Override public void connect() {}
    }

    private static final class Network implements ControlsDownload.Connections {
        final Queue<Response> replies = new ArrayDeque<>();
        int requests;
        @Override public HttpURLConnection open(URL url) throws IOException {
            assertEquals("https", url.getProtocol());
            requests++;
            Response response = replies.poll();
            if (response == null) throw new IOException("Unexpected request");
            return response;
        }
    }

    private File existing() throws Exception {
        File destination = temporary.newFile("package.zip");
        Files.write(destination.toPath(), "old".getBytes(StandardCharsets.UTF_8));
        return destination;
    }

    private static String contents(File file) throws Exception {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }

    private void assertNoPartial() {
        assertEquals(0, temporary.getRoot().listFiles((dir, name) -> name.endsWith(".partial")).length);
    }

    @Test public void publishesOnlyAfterCompleteAndDisconnects() throws Exception {
        File destination = existing();
        Network network = new Network();
        Response response = new Response(200, "new package", 11);
        network.replies.add(response);
        long size = ControlsDownload.transfer("https://example.test/package", destination, 100, new AtomicBoolean(),
            (received, total) -> {
                try { assertEquals("old", contents(destination)); }
                catch (Exception error) { throw new AssertionError(error); }
                assertEquals(11, total);
            }, network);
        assertEquals(11, size);
        assertEquals("new package", contents(destination));
        assertEquals(20_000, response.getConnectTimeout());
        assertEquals(20_000, response.getReadTimeout());
        assertFalse(response.getInstanceFollowRedirects());
        assertTrue(response.disconnected);
        assertNoPartial();
    }

    @Test public void refusesHttpAndEmbeddedCredentialsBeforeNetwork() throws Exception {
        Network network = new Network();
        File destination = existing();
        for (String address : new String[] {"http://example.test/a", "https://user:pass@example.test/a"}) {
            assertThrows(IOException.class, () -> ControlsDownload.transfer(address, destination, 100,
                new AtomicBoolean(), (received, total) -> {}, network));
        }
        assertEquals(0, network.requests);
        assertEquals("old", contents(destination));
        assertNoPartial();
    }

    @Test public void redirectMustAlsoUseHttps() throws Exception {
        File destination = existing();
        Network network = new Network();
        Response redirect = new Response(302, "", 0);
        redirect.location = "http://example.test/insecure";
        network.replies.add(redirect);
        assertThrows(IOException.class, () -> ControlsDownload.transfer("https://example.test/package", destination,
            100, new AtomicBoolean(), (received, total) -> {}, network));
        assertEquals(1, network.requests);
        assertTrue(redirect.disconnected);
        assertEquals("old", contents(destination));
        assertNoPartial();
    }

    @Test public void followsRelativeRedirectAndStopsLoops() throws Exception {
        Network network = new Network();
        Response redirect = new Response(307, "", 0);
        redirect.location = "/next";
        Response ok = new Response(200, "{}", 2);
        network.replies.add(redirect);
        network.replies.add(ok);
        assertEquals("{}", ControlsDownload.readText("https://example.test/start", 10, new AtomicBoolean(), network));
        assertTrue(redirect.disconnected);
        assertTrue(ok.disconnected);
        Network loop = new Network();
        for (int index = 0; index < 6; index++) {
            Response again = new Response(302, "", 0);
            again.location = "/again";
            loop.replies.add(again);
        }
        assertThrows(IOException.class, () -> ControlsDownload.readText("https://example.test/start", 10,
            new AtomicBoolean(), loop));
        assertEquals(6, loop.requests);
    }

    @Test public void rejectsStatusDeclaredOversizeAndStreamingOversize() throws Exception {
        for (Response response : new Response[] {new Response(404, "missing", 7),
                new Response(200, "body", 101), new Response(200, "too long", -1)}) {
            Network network = new Network();
            network.replies.add(response);
            File destination = new File(temporary.getRoot(), "preserved.zip");
            Files.write(destination.toPath(), "old".getBytes(StandardCharsets.UTF_8));
            assertThrows(IOException.class, () -> ControlsDownload.transfer("https://example.test/package", destination,
                5, new AtomicBoolean(), (received, total) -> {}, network));
            assertEquals("old", contents(destination));
            assertTrue(response.disconnected);
            assertNoPartial();
        }
    }

    @Test public void truncatedBodyAndMidstreamErrorDoNotReplaceExistingPackage() throws Exception {
        for (boolean streamError : new boolean[] {false, true}) {
            Network network = new Network();
            Response response = new Response(200, "short", 10);
            if (streamError) response.stream = new InputStream() {
                @Override public int read() throws IOException { throw new IOException("connection lost"); }
            };
            network.replies.add(response);
            File destination = new File(temporary.getRoot(), "preserved.zip");
            Files.write(destination.toPath(), "old".getBytes(StandardCharsets.UTF_8));
            assertThrows(IOException.class, () -> ControlsDownload.transfer("https://example.test/package", destination,
                100, new AtomicBoolean(), (received, total) -> {}, network));
            assertEquals("old", contents(destination));
            assertTrue(response.disconnected);
            assertNoPartial();
        }
    }

    @Test public void cancellationDuringTransferDiscardsPartial() throws Exception {
        File destination = existing();
        Network network = new Network();
        Response response = new Response(200, "new package", 11);
        network.replies.add(response);
        AtomicBoolean canceled = new AtomicBoolean();
        assertThrows(IOException.class, () -> ControlsDownload.transfer("https://example.test/package", destination,
            100, canceled, (received, total) -> canceled.set(true), network));
        assertEquals("old", contents(destination));
        assertTrue(response.disconnected);
        assertNoPartial();
    }

    @Test public void textResponsesHaveStreamingBoundsAndLengthChecksToo() throws Exception {
        for (Response response : new Response[] {new Response(200, "123456", -1), new Response(200, "a", 3)}) {
            Network network = new Network();
            network.replies.add(response);
            assertThrows(IOException.class, () -> ControlsDownload.readText("https://example.test/releases", 5,
                new AtomicBoolean(), network));
            assertTrue(response.disconnected);
        }
    }

    @Test public void webPagesAreRejectedBeforeReadingAndPreserveExistingPackage() throws Exception {
        for (String type : new String[] {"text/html", "Text/HTML; charset=UTF-8", "application/xhtml+xml"}) {
            Network network = new Network();
            Response response = new Response(200, "<html>landing page</html>", -1);
            response.contentType = type;
            network.replies.add(response);
            File destination = new File(temporary.getRoot(), "preserved.zip");
            Files.write(destination.toPath(), "old".getBytes(StandardCharsets.UTF_8));
            assertThrows(IOException.class, () -> ControlsDownload.transfer("https://example.test/package", destination,
                100, new AtomicBoolean(), (received, total) -> {}, network));
            assertFalse(response.streamOpened);
            assertTrue(response.disconnected);
            assertEquals("old", contents(destination));
            assertNoPartial();
        }
    }

    @Test public void binaryContentTypesRemainSupported() throws Exception {
        for (String type : new String[] {null, "application/octet-stream", "application/zip", "application/x-iso9660-image"}) {
            Network network = new Network();
            Response response = new Response(200, "data", 4);
            response.contentType = type;
            network.replies.add(response);
            File destination = new File(temporary.getRoot(), "package.iso");
            assertEquals(4, ControlsDownload.transfer("https://example.test/package", destination,
                100, new AtomicBoolean(), (received, total) -> {}, network));
            assertEquals("data", contents(destination));
            assertNoPartial();
        }
    }

    @Test public void boundedTextReaderStillAcceptsLandingPageHtml() throws Exception {
        Network network = new Network();
        Response response = new Response(200, "<a id='downloadButton'>Download</a>", -1);
        response.contentType = "text/html; charset=UTF-8";
        network.replies.add(response);
        assertEquals(new String(response.bytes, StandardCharsets.UTF_8), ControlsDownload.readText(
            "https://www.mediafire.com/file/example", 100, new AtomicBoolean(), network));
        assertTrue(response.disconnected);
    }
}
