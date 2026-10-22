package com.halo.decomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.content.pm.PackageInfo;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowAlertDialog;

import java.io.IOException;

import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35})
public class ControlsUpdaterHostTest {
    public static class UpstreamActivity extends Activity {
        @Override public String getPackageName() { return "com.halo.decomp"; }
    }
    private static JSONObject release(int number, long size) throws Exception {
        String tag = "controles-" + number;
        JSONObject asset = new JSONObject().put("name", "halo-controls.apk").put("size", size)
            .put("browser_download_url", "https://github.com/dorlanpabon/halo-ce-universal/releases/download/"
                + tag + "/halo-controls.apk");
        return new JSONObject().put("tag_name", tag).put("assets", new JSONArray().put(asset));
    }

    private static PackageInfo packageInfo(String id, int version, String... certificates) {
        PackageInfo info = new PackageInfo();
        info.packageName = id;
        info.setLongVersionCode(version);
        info.signingInfo = new SigningInfo();
        Signature[] signatures = new Signature[certificates.length];
        for (int index = 0; index < signatures.length; index++)
            signatures[index] = new Signature(certificates[index]);
        shadowOf(info.signingInfo).setSignatures(signatures);
        return info;
    }

    @Test public void selectsHighestNewerCompatibleChannelRegardlessOfReleaseOrder() throws Exception {
        JSONArray releases = new JSONArray().put(release(16, 100)).put(release(18, 200)).put(release(17, 300));
        ControlsUpdater.Release candidate = ControlsUpdater.selectRelease(releases.toString(), 10616);
        assertNotNull(candidate);
        assertEquals(10618, candidate.version);
        assertEquals(200, candidate.size);
        assertTrue(candidate.address.endsWith("/controles-18/halo-controls.apk"));
        assertNull(ControlsUpdater.selectRelease(releases.toString(), 10618));
    }

    @Test public void ignoresDraftPrereleaseUpstreamForeignUrlAndOversizeAssets() throws Exception {
        JSONObject foreign = release(20, 100);
        foreign.getJSONArray("assets").getJSONObject(0).put("browser_download_url", "https://example.test/halo-controls.apk");
        JSONObject upstream = release(21, 100).put("tag_name", "build-107");
        JSONObject wrongAsset = release(22, 100);
        wrongAsset.getJSONArray("assets").getJSONObject(0).put("name", "halo-android-release.zip");
        JSONArray releases = new JSONArray().put(release(16, 100))
            .put(release(17, 100).put("draft", true)).put(release(18, 100).put("prerelease", true))
            .put(release(19, ControlsUpdater.MAX_APK_BYTES + 1)).put(foreign).put(upstream).put(wrongAsset)
            .put(release(23, 0));
        assertNull(ControlsUpdater.selectRelease(releases.toString(), 10616));
    }

    @Test public void acceptsOnlyNewerArchiveWithExactIdVersionAndCurrentSignerSet() throws Exception {
        PackageInfo installed = packageInfo("com.halo.decomp.controls", 10616, "0102", "0304");
        PackageInfo archive = packageInfo("com.halo.decomp.controls", 10617, "0304", "0102");
        ControlsUpdater.validateArchive(installed, archive, 10617);
    }

    @Test public void wrongPackageCannotReplaceOrForkInstalledEdition() throws Exception {
        PackageInfo installed = packageInfo("com.halo.decomp.controls", 10616, "0102");
        for (String id : new String[] {"com.halo.decomp", "com.halo.decomp.other", "com.other.controls"})
            assertThrows(IOException.class, () -> ControlsUpdater.validateArchive(installed, packageInfo(id, 10617, "0102"), 10617));
        assertThrows(IOException.class, () -> ControlsUpdater.validateArchive(
            packageInfo("com.halo.decomp", 106, "0102"), packageInfo("com.halo.decomp", 107, "0102"), 107));
    }

    @Test public void rejectsDowngradeSameVersionAndMislabelledRelease() throws Exception {
        PackageInfo installed = packageInfo("com.halo.decomp.controls", 10616, "0102");
        for (int version : new int[] {10615, 10616, 10618})
            assertThrows(IOException.class, () -> ControlsUpdater.validateArchive(installed,
                packageInfo(installed.packageName, version, "0102"), 10617));
    }

    @Test public void rejectsUnknownDifferentOrPartialSignerSetAndUnparseableArchive() throws Exception {
        PackageInfo installed = packageInfo("com.halo.decomp.controls", 10616, "0102", "0304");
        assertThrows(IOException.class, () -> ControlsUpdater.validateArchive(installed, null, 10617));
        for (String[] signers : new String[][] {{}, {"0102"}, {"0102", "0506"}})
            assertThrows(IOException.class, () -> ControlsUpdater.validateArchive(installed,
                packageInfo(installed.packageName, 10617, signers), 10617));
        PackageInfo unsigned = packageInfo(installed.packageName, 10617, "0102", "0304");
        unsigned.signingInfo = null;
        assertThrows(IOException.class, () -> ControlsUpdater.validateArchive(installed, unsigned, 10617));
    }

    @Test public void unsupportedEditionShowsMessageWithoutStartingNetwork() {
        Activity activity = Robolectric.buildActivity(UpstreamActivity.class).setup().get();
        ControlsUpdater.show(activity, AlertDialog::show);
        AlertDialog dialog = ShadowAlertDialog.getLatestAlertDialog();
        assertTrue(shadowOf(dialog).getMessage().toString().contains("edición Controles"));
        dialog.dismiss();
        activity.finish();
    }
}
