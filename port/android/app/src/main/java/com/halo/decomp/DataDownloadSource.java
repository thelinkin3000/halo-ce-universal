package com.halo.decomp;

import java.io.IOException;
import java.io.InterruptedIOException;
import java.net.URI;
import java.net.URISyntaxException;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Resolves public file pages without executing scripts or following advertisements. */
final class DataDownloadSource {
    interface HtmlSource {
        String read(String address, int maxBytes, AtomicBoolean canceled) throws IOException;
    }

    static final int MAX_HTML_BYTES = 2 * 1024 * 1024;
    private static final Pattern IGNORED = Pattern.compile(
        "<!--.*?(?:-->|\\z)|<script\\b[^>]*>.*?(?:</script\\s*>|\\z)|<style\\b[^>]*>.*?(?:</style\\s*>|\\z)",
        Pattern.CASE_INSENSITIVE | Pattern.DOTALL);
    private static final Pattern ANCHOR = Pattern.compile("<a(?=[\\s/>])", Pattern.CASE_INSENSITIVE);
    private static final Pattern ATTRIBUTE = Pattern.compile(
        "([a-zA-Z_:][a-zA-Z0-9_:.-]*)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"'=<>`]+))");
    private static final Pattern ENTITY = Pattern.compile("&(#(?:[xX][0-9a-fA-F]+|[0-9]+)|[a-zA-Z]+);");
    private static final Pattern CHALLENGE = Pattern.compile(
        "<(?:form|iframe|div)\\b[^>]*(?:g-recaptcha|h-captcha|cf-turnstile|challenge-platform)[^>]*>",
        Pattern.CASE_INSENSITIVE | Pattern.DOTALL);

    private DataDownloadSource() {}

    static String resolve(String address, AtomicBoolean canceled) throws IOException {
        return resolve(address, canceled, ControlsDownload::readText);
    }

    static boolean isMediaFirePage(String address) {
        try {
            return isMediaFirePage(https(address));
        } catch (IOException error) {
            return false;
        }
    }

    private static boolean isMediaFirePage(URI page) {
        String host = page.getHost().toLowerCase(Locale.ROOT);
        return (host.equals("mediafire.com") || host.equals("www.mediafire.com"))
            && (page.getPort() == -1 || page.getPort() == 443)
            && page.getRawPath().matches("/(?:file|download)/[a-zA-Z0-9]+(?:/[^\\s]*)?");
    }

    static String resolve(String address, AtomicBoolean canceled, HtmlSource source) throws IOException {
        checkCanceled(canceled);
        URI page = https(address);
        String host = page.getHost().toLowerCase(Locale.ROOT);
        if (!host.equals("mediafire.com") && !host.equals("www.mediafire.com"))
            return address;
        if (!isMediaFirePage(page))
            throw new IOException("Usa el enlace público de un archivo de MediaFire.");
        String html = source.read(address, MAX_HTML_BYTES, canceled);
        checkCanceled(canceled);
        if (html.length() > MAX_HTML_BYTES)
            throw new IOException("La página supera el límite permitido.");
        html = IGNORED.matcher(html).replaceAll("");
        if (CHALLENGE.matcher(html).find())
            throw new IOException("MediaFire requiere una comprobación en el navegador.");
        Matcher anchors = ANCHOR.matcher(html);
        String result = null;
        while (anchors.find()) {
            int end = tagEnd(html, anchors.end());
            Map<String, String> attributes = attributes(html.substring(anchors.start(), end));
            anchors.region(end, html.length());
            if (!"downloadButton".equals(attributes.get("id")))
                continue;
            String href = attributes.get("href");
            if (href == null)
                throw new IOException("MediaFire no presentó un enlace de descarga público.");
            String candidate = decodeEntities(href);
            URI download = https(candidate);
            if (!download.getHost().toLowerCase(Locale.ROOT).matches("download[0-9]+\\.mediafire\\.com")
                    || (download.getPort() != -1 && download.getPort() != 443)
                    || download.getRawPath().isEmpty() || download.getRawPath().equals("/"))
                throw new IOException("MediaFire presentó un destino de descarga no válido.");
            if (result != null && !result.equals(candidate))
                throw new IOException("MediaFire presentó varios destinos de descarga.");
            result = candidate;
        }
        if (result == null)
            throw new IOException("MediaFire no presentó un enlace público. Abre la página en el navegador.");
        checkCanceled(canceled);
        return result;
    }

    private static URI https(String address) throws IOException {
        if (address == null)
            throw new IOException("La dirección de descarga no es válida.");
        try {
            URI uri = new URI(address);
            if (!"https".equalsIgnoreCase(uri.getScheme()) || uri.getHost() == null
                    || uri.getRawUserInfo() != null || uri.getRawFragment() != null)
                throw new IOException("La descarga necesita una dirección HTTPS sin credenciales ni fragmentos.");
            return uri;
        } catch (URISyntaxException | IllegalArgumentException error) {
            throw new IOException("La dirección de descarga no es válida.", error);
        }
    }

    private static int tagEnd(String html, int start) throws IOException {
        char quote = 0;
        for (int index = start; index < html.length() && index - start < 16 * 1024; index++) {
            char current = html.charAt(index);
            if (quote != 0) {
                if (current == quote)
                    quote = 0;
            } else if (current == '\'' || current == '"') {
                quote = current;
            } else if (current == '>') {
                return index + 1;
            }
        }
        throw new IOException("La página contiene un enlace incompleto o demasiado grande.");
    }

    private static Map<String, String> attributes(String tag) throws IOException {
        Map<String, String> values = new HashMap<>();
        Matcher attributes = ATTRIBUTE.matcher(tag);
        while (attributes.find()) {
            String name = attributes.group(1).toLowerCase(Locale.ROOT);
            String value = attributes.group(2) != null ? attributes.group(2)
                : attributes.group(3) != null ? attributes.group(3) : attributes.group(4);
            if (values.put(name, value) != null && (name.equals("id") || name.equals("href")))
                throw new IOException("La página contiene un enlace ambiguo.");
        }
        return values;
    }

    private static String decodeEntities(String value) throws IOException {
        Matcher entities = ENTITY.matcher(value);
        StringBuffer decoded = new StringBuffer();
        while (entities.find()) {
            String entity = entities.group(1);
            String replacement;
            switch (entity) {
                case "amp": replacement = "&"; break;
                case "quot": replacement = "\""; break;
                case "apos": replacement = "'"; break;
                case "lt": replacement = "<"; break;
                case "gt": replacement = ">"; break;
                default:
                    try {
                        if (!entity.startsWith("#"))
                            throw new NumberFormatException();
                        boolean hex = entity.startsWith("#x") || entity.startsWith("#X");
                        int code = Integer.parseInt(entity.substring(hex ? 2 : 1), hex ? 16 : 10);
                        if (!Character.isValidCodePoint(code) || code < 32
                                || code >= 0xd800 && code <= 0xdfff)
                            throw new NumberFormatException();
                        replacement = new String(Character.toChars(code));
                    } catch (IllegalArgumentException error) {
                        throw new IOException("El enlace contiene una entidad HTML no válida.", error);
                    }
            }
            entities.appendReplacement(decoded, Matcher.quoteReplacement(replacement));
        }
        entities.appendTail(decoded);
        return decoded.toString();
    }

    private static void checkCanceled(AtomicBoolean canceled) throws InterruptedIOException {
        if (canceled.get() || Thread.currentThread().isInterrupted())
            throw new InterruptedIOException("Descarga cancelada.");
    }
}
