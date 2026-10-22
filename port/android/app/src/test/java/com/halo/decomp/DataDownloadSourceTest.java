package com.halo.decomp;

import org.junit.Test;

import java.io.IOException;
import java.io.InterruptedIOException;
import java.util.concurrent.atomic.AtomicBoolean;

import static org.junit.Assert.*;

public class DataDownloadSourceTest {
    private static final String PAGE = "https://www.mediafire.com/file/example123/package.iso/file";
    private static final String FILE = "https://download123.mediafire.com/token/key/package.iso";

    private static String resolve(String html) throws IOException {
        return DataDownloadSource.resolve(PAGE, new AtomicBoolean(), (url, limit, canceled) -> {
            assertEquals(PAGE, url);
            assertEquals(2 * 1024 * 1024, limit);
            assertFalse(canceled.get());
            return html;
        });
    }

    @Test public void resolvesOnlyNormalPublicDownloadAnchor() throws Exception {
        assertEquals(FILE, resolve("<a href='https://advertisement.example/ad'>Download</a>"
            + "<a class='input' href='" + FILE + "' id='downloadButton'>Download</a>"));
    }

    @Test public void acceptsAttributeOrderCaseAndUnquotedValues() throws Exception {
        assertEquals(FILE, resolve("<A ID=downloadButton data-title='one > two' HREF=\"" + FILE + "\">"));
    }

    @Test public void acceptsFileAndDownloadPagePaths() throws Exception {
        for (String page : new String[] {"https://mediafire.com/file/example123",
                "https://mediafire.com/download/example123/package.zip"}) {
            assertEquals(FILE, DataDownloadSource.resolve(page, new AtomicBoolean(),
                (url, max, canceled) -> "<a id='downloadButton' href='" + FILE + "'>Download</a>"));
        }
    }

    @Test public void identifiesOnlyPublicMediaFireFilePageRoutes() {
        for (String address : new String[] {PAGE, "https://mediafire.com/download/example123/package.zip",
                "https://www.mediafire.com:443/file/example123?download=1"}) {
            assertTrue(address, DataDownloadSource.isMediaFirePage(address));
        }
    }

    @Test public void invalidAndOtherUrlsAreNotMediaFirePages() {
        for (String address : new String[] {null, "", "http://mediafire.com/file/example123",
                "https://user@mediafire.com/file/example123", PAGE + "#fragment",
                "https://mediafire.com:8443/file/example123", "https://mediafire.com/folder/example123",
                "https://mediafire.com/file/", "https://mediafire.com.evil.example/file/example123",
                "https://example.test/package.zip", FILE}) {
            assertFalse(String.valueOf(address), DataDownloadSource.isMediaFirePage(address));
        }
    }

    @Test public void directHttpsDownloadsDoNotFetchHtml() throws Exception {
        for (String address : new String[] {"https://example.test/maps.zip", FILE}) {
            assertEquals(address, DataDownloadSource.resolve(address, new AtomicBoolean(),
                (url, max, canceled) -> { throw new AssertionError("Unexpected page fetch"); }));
        }
    }

    @Test public void decodesEntitiesWithoutDroppingQueryParameters() throws Exception {
        assertEquals(FILE + "?key=one&token=two&extra=three", resolve("<a id='downloadButton' href='"
            + FILE + "?key=one&amp;token=two&#38;extra=three'>Download</a>"));
        assertEquals(FILE + "?name=abc&key=123", resolve("<a id='downloadButton' href='"
            + FILE + "?name=abc&#x26;key=123'>Download</a>"));
    }

    @Test public void ignoresAnchorsInsideScriptsStylesAndComments() throws Exception {
        String fake = "<a id='downloadButton' href='https://evil.example/payload'>Download</a>";
        assertEquals(FILE, resolve("<!--" + fake + "--><script>const text=\"" + fake + "\";</script>"
            + "<style>/*" + fake + "*/</style><a id='downloadButton' href='" + FILE + "'>Download</a>"));
        assertThrows(IOException.class, () -> resolve("<script>const text=\"" + fake + "\";</script>"));
        assertThrows(IOException.class, () -> resolve("<script>" + fake));
    }

    @Test public void refusesOtherAnchorsAndJavascriptOnlyPages() {
        for (String html : new String[] {"<a href='" + FILE + "'>Download</a>",
                "<script>location.href='" + FILE + "'</script>",
                "<a id='downloadButton'>Download</a>", "<html>File removed</html>"}) {
            assertThrows(IOException.class, () -> resolve(html));
        }
    }

    @Test public void refusesDestinationsOutsideThePublicMediafireDownloadHost() {
        for (String address : new String[] {"http://download123.mediafire.com/a", "javascript:download()",
                "https://mediafire.com/a", "https://evil.example/a", "https://127.0.0.1/a",
                "https://download123.mediafire.com.evil.example/a", "https://evilmediafire.com/a",
                "https://user@download123.mediafire.com/a", "https://download123.mediafire.com/a#fragment",
                "https://download123.mediafire.com:8443/a", "https://download123.mediafire.com/",
                "//download123.mediafire.com/a"}) {
            assertThrows(address, IOException.class,
                () -> resolve("<a id='downloadButton' href='" + address + "'>Download</a>"));
        }
    }

    @Test public void refusesUnsupportedPagePathsAndInvalidInputBeforeNetwork() {
        for (String address : new String[] {null, "http://www.mediafire.com/file/example123",
                "https://user@www.mediafire.com/file/example123", PAGE + "#fragment",
                "https://mediafire.com/folder/example123", "https://mediafire.com:8443/file/example123",
                "https://mediafire.com/file/", "https://mediafire.com/file/key with spaces"}) {
            assertThrows(address, IOException.class, () -> DataDownloadSource.resolve(address,
                new AtomicBoolean(), (url, max, canceled) -> { throw new AssertionError("Unexpected fetch"); }));
        }
    }

    @Test public void refusesChallengePagesWithoutSolvingThem() {
        for (String challenge : new String[] {"<div class='g-recaptcha'>", "<iframe src='/h-captcha'>",
                "<div class='cf-turnstile'>"}) {
            assertThrows(IOException.class, () -> resolve(challenge
                + "<a id='downloadButton' href='" + FILE + "'>Download</a>"));
        }
    }

    @Test public void refusesAmbiguousOrDuplicateDownloadAttributes() {
        for (String html : new String[] {"<a id='downloadButton' id='other' href='" + FILE + "'>",
                "<a id='downloadButton' href='" + FILE + "' href='https://evil.example/a'>",
                "<a id='downloadButton' href='" + FILE + "'><a id='downloadButton' href='" + FILE + "2'>"}) {
            assertThrows(IOException.class, () -> resolve(html));
        }
    }

    @Test public void refusesMalformedAndControlCharacterEntities() {
        for (String entity : new String[] {"&#0;", "&#xD800;", "&#1114112;", "&#99999999999;", "&unknown;"}) {
            assertThrows(IOException.class,
                () -> resolve("<a id='downloadButton' href='" + FILE + entity + "'>Download</a>"));
        }
    }

    @Test public void enforcesHtmlLimitAndPropagatesFetchFailure() {
        char[] body = new char[DataDownloadSource.MAX_HTML_BYTES + 1];
        assertThrows(IOException.class, () -> resolve(new String(body)));
        assertThrows(IOException.class, () -> DataDownloadSource.resolve(PAGE, new AtomicBoolean(),
            (url, max, canceled) -> { throw new IOException("HTTP 429"); }));
    }

    @Test public void rejectsUnterminatedAndOversizedTagsWithoutRecursiveParsing() {
        assertThrows(IOException.class, () -> resolve("<a id='downloadButton' href='" + FILE));
        char[] large = new char[100 * 1024];
        assertThrows(IOException.class, () -> resolve("<a title='" + new String(large) + "' href='" + FILE + "'>"));
    }

    @Test public void cancellationPreventsFetchAndPublication() {
        assertThrows(InterruptedIOException.class, () -> DataDownloadSource.resolve(PAGE,
            new AtomicBoolean(true), (url, max, canceled) -> { throw new AssertionError("Unexpected fetch"); }));
        AtomicBoolean canceled = new AtomicBoolean();
        assertThrows(InterruptedIOException.class, () -> DataDownloadSource.resolve(PAGE, canceled,
            (url, max, flag) -> {
                flag.set(true);
                return "<a id='downloadButton' href='" + FILE + "'>Download</a>";
            }));
    }
}
