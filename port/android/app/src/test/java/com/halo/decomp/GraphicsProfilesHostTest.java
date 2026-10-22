package com.halo.decomp;

import android.app.Application;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

import java.util.LinkedHashMap;
import java.util.Map;

import static org.junit.Assert.*;

/** Resolution presets keep their documented pixel counts after build 107 adds native resolution. */
@RunWith(RobolectricTestRunner.class)
@Config(application = Application.class, sdk = {28, 35})
public class GraphicsProfilesHostTest {
    @Test public void allPresetsExplicitlyResetSupportedNativeResolutionTo480() {
        String before = "# keep\r\n[display]\r\nresolution_limit = \"native\" # phone pixels\r\n"
            + "unknown = \"keep # text\"\r\n[audio]\r\nvolume = 0.6\r\n";
        for (int index = 0; index < 5; index++) {
            String changed = GraphicsProfiles.updateConfig(before, index);
            assertTrue(changed.contains("resolution_limit = \"480\" # phone pixels\r\n"));
            assertTrue(changed.contains("unknown = \"keep # text\"\r\n"));
            assertTrue(changed.endsWith("[audio]\r\nvolume = 0.6\r\n"));
            assertEquals(index, GraphicsProfiles.detectProfile(changed, GraphicsProfiles.updateInit("", index)));
            assertEquals(changed, GraphicsProfiles.updateConfig(changed, index));
            ScreenSettings.Selection selection = ScreenSettings.readSelection(changed);
            assertEquals(selection.scale, ScreenSettings.effectiveScale(selection, 2400, 1080), 0f);
        }
    }

    @Test public void nativeOrHigherResolutionIsCustomEvenWhenOtherProfileValuesMatch() {
        for (int index = 0; index < 5; index++) {
            String config = GraphicsProfiles.updateConfig("[display]\nresolution_limit = \"480\"\n", index);
            String init = GraphicsProfiles.updateInit("", index);
            assertEquals(index, GraphicsProfiles.detectProfile(config, init));
            assertEquals(-1, GraphicsProfiles.detectProfile(
                config.replace("resolution_limit = \"480\"", "resolution_limit = \"native\""), init));
            assertEquals(-1, GraphicsProfiles.detectProfile(
                config.replace("resolution_limit = \"480\"", "resolution_limit = \"720\""), init));
        }
    }

    @Test public void build106ConfigurationDoesNotGainUnsupportedResolutionLimit() {
        String before = "[display]\nrenderer = \"gl\"\n[audio]\nvolume = 0.3\n";
        for (int index = 0; index < 5; index++) {
            String config = GraphicsProfiles.updateConfig(before, index);
            assertFalse(GraphicsProfiles.displaySettings(config).containsKey("resolution_limit"));
            assertEquals(index, GraphicsProfiles.detectProfile(config, GraphicsProfiles.updateInit("", index)));
        }
    }

    @Test public void nonProfileSettingsUpdatePreservesNativeResolutionLimit() {
        String before = "[display]\nresolution_limit = \"native\" # keep\nrender_scale = 1.0\n";
        Map<String, String> settings = new LinkedHashMap<>();
        settings.put("screen_width", "640");
        String after = GraphicsProfiles.updateConfig(before, settings);
        assertTrue(after.contains("resolution_limit = \"native\" # keep\n"));
        assertEquals("\"native\"", GraphicsProfiles.displaySettings(after).get("resolution_limit"));
    }

    @Test public void otherSectionAndMultilineResolutionTextDoNotEnableNewPresetSetting() {
        String before = "[notes]\nresolution_limit = \"native\"\ntext = '''\n[display]\n"
            + "resolution_limit = \"native\"\n'''\n[display]\nrenderer = \"gl\"\n";
        String after = GraphicsProfiles.updateConfig(before, 0);
        assertFalse(GraphicsProfiles.displaySettings(after).containsKey("resolution_limit"));
        assertTrue(after.contains("text = '''\n[display]\nresolution_limit = \"native\"\n'''"));
        assertEquals(0, GraphicsProfiles.detectProfile(after, GraphicsProfiles.updateInit("", 0)));
    }
}
