package com.halo.decomp;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.concurrent.atomic.AtomicBoolean;

/** Archived OXM Xbox promotional demo; no retail game data is downloaded or bundled. */
final class DemoPackage {
    // Source: https://digiex.net/threads/halo-xbox-demo-download.8423/
    static final String URL = "https://download.digiex.net/Consoles/Xbox/Demos/Halo-OXM-Demo.zip";
    static final long SIZE = 397660357L;
    static final String SHA256 = "097ee0d4530c70e90d7146debf7b7ecd5b83b04fc7a465f5d040aa24ccb5c91a";

    private DemoPackage() {}

    static void verify(File archive, AtomicBoolean canceled) throws IOException {
        checkCanceled(canceled);
        if (archive.length() != SIZE || !SHA256.equals(sha256(archive, canceled)))
            throw new IOException("La demo está incompleta, dañada o cambió en el servidor. No se instaló.");
    }

    static String sha256(File file, AtomicBoolean canceled) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException error) {
            throw new IOException("No se pudo verificar la demo.", error);
        }
        byte[] buffer = new byte[64 << 10];
        try (FileInputStream input = new FileInputStream(file)) {
            int count;
            while (true) {
                checkCanceled(canceled);
                count = input.read(buffer);
                if (count < 0)
                    break;
                digest.update(buffer, 0, count);
            }
        }
        checkCanceled(canceled);
        StringBuilder text = new StringBuilder(64);
        for (byte value : digest.digest()) {
            text.append(Character.forDigit((value & 255) >>> 4, 16));
            text.append(Character.forDigit(value & 15, 16));
        }
        return text.toString();
    }

    private static void checkCanceled(AtomicBoolean canceled) throws InterruptedIOException {
        if (canceled.get() || Thread.currentThread().isInterrupted())
            throw new InterruptedIOException("Verificación de demo cancelada.");
    }
}
