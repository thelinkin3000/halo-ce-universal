package com.halo.decomp;

/**
 * What the move to the new package name copies (Migration.java, MigrationProvider.java), by a file's path relative to
 * the data folder, or under shared_prefs/ for the touch layouts. No Android here: tools/tests/MigrationFilesTest.java.
 */
final class MigrationFiles {
    /** the name a file of the app's private preferences goes by */
    static final String PREFERENCES = "shared_prefs/";

    private static final String[] LEFT_TOP_FILES = { "debug.txt", "halo.log", "hardware_id.txt", "join_link.txt",
        "profile_folder.txt", "benchmark.txt", "auto_configure.txt", ".migrated", ".migrating" };
    private static final String[] LEFT_FOLDERS = { "halo_profiles/", "profiles/", ".import/" };
    private static final String[] LEFT_ENDINGS = { ".part", ".tmp" };

    private MigrationFiles() {
    }

    /** a path that stays inside its folder: nothing absolute, no way up */
    static boolean safe(String path) {
        if (path == null || path.isEmpty() || path.startsWith("/") || path.indexOf('\\') >= 0 || path.indexOf('\0') >= 0)
            return false;
        for (String part : path.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals(".."))
                return false;
        }
        return true;
    }

    /** a preference file's path, directly in shared_prefs */
    static boolean preferences(String path) {
        return path != null && path.startsWith(PREFERENCES) && path.endsWith(".xml")
            && path.indexOf('/', PREFERENCES.length()) < 0;
    }

    /** whether the file is the player's, to be copied: not a log, what a run made for itself, or half a copy */
    static boolean copied(String path) {
        if (!safe(path))
            return false;
        if (path.startsWith(PREFERENCES))
            return preferences(path);
        for (String name : LEFT_TOP_FILES) {
            if (path.equals(name))
                return false;
        }
        for (String folder : LEFT_FOLDERS) {
            if (path.startsWith(folder))
                return false;
        }
        for (String ending : LEFT_ENDINGS) {
            if (path.endsWith(ending))
                return false;
        }
        return true;
    }

    /** the game's own maps, which the ISO can make again: first to be left out where there is no room */
    static boolean remade(String path) {
        return path.startsWith("maps/");
    }

    /** custom maps, the player's but large: next to be left out, the player told */
    static boolean large(String path) {
        return path.startsWith("custom_maps/");
    }
}
