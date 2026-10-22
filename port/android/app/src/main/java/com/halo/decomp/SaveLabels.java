package com.halo.decomp;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/** Shared labels for the Android save and continue pickers. */
final class SaveLabels {
    private SaveLabels() { }

    static String label(CheckpointSlots.Entry entry) {
        String map = entry.candidate.map;
        int slash = map.lastIndexOf('\\');
        if (slash >= 0)
            map = map.substring(slash + 1);
        if ("demob30".equals(map))
            map = "Demo · El cartógrafo silencioso";
        String state = entry.candidate.kind == CheckpointStore.Kind.READY
            ? new String[] {"Fácil", "Normal", "Heroica", "Legendaria"}[entry.candidate.difficulty]
            : entry.candidate.kind == CheckpointStore.Kind.INCOMPATIBLE ? "Versión incompatible" : "Archivo dañado";
        return entry.name + " · " + new SimpleDateFormat("dd/MM/yyyy HH:mm", Locale.getDefault())
            .format(new Date(entry.savedAt)) + "\n" + map + " · " + state;
    }
}
