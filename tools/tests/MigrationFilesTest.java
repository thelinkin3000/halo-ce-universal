package com.halo.decomp;

/** What the move to the new package name copies (MigrationFiles.java). */
public final class MigrationFilesTest {
    private static void check(boolean condition, String what) {
        if (!condition)
            throw new AssertionError(what);
    }

    public static void main(String[] arguments) {
        for (String path : new String[] { "config.toml", "save/profile/blam.sav", "mods/hd/mod.toml", "overlay/a.toml",
            "replays/a.ocreplay", "custom_maps/packA/foo.map", "maps/ui.map", "shared_prefs/touch-layout.xml" })
            check(MigrationFiles.copied(path), path + " is the player's");
        for (String path : new String[] { "debug.txt", "halo.log", "hardware_id.txt", "join_link.txt", ".migrated",
            ".migrating", "halo_profiles/run/x.bin", "maps/a10.map.part", "config.toml.tmp", ".import/a/mod.toml",
            "shared_prefs/x.bak", "shared_prefs/a/b.xml" })
            check(!MigrationFiles.copied(path), path + " is not to be copied");
        for (String path : new String[] { "", "/etc/hosts", "../secret", "save/../../secret", "save//a", "a\\b", "save/./a",
            "shared_prefs/../../x.xml" })
            check(!MigrationFiles.copied(path), path + " leaves its folder");
        check(!MigrationFiles.copied(null), "no path");
        check(MigrationFiles.remade("maps/a10.map") && !MigrationFiles.remade("custom_maps/a.map"), "the ISO's maps");
        check(MigrationFiles.large("custom_maps/a.map") && !MigrationFiles.large("save/a"), "custom maps");
        check(MigrationFiles.preferences("shared_prefs/a.xml") && !MigrationFiles.preferences("save/a.xml"), "preferences");
    }
}
