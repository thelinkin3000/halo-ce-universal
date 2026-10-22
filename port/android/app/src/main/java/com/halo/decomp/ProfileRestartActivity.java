package com.halo.decomp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

/** Stops only the game process, restarting it for graphics changes or leaving after a saved checkpoint. */
public final class ProfileRestartActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int gamePid = getIntent().getIntExtra("game-pid", -1);
        if (gamePid > 0 && gamePid != Process.myPid())
            Process.killProcess(gamePid);
        if (getIntent().getBooleanExtra("exit-only", false)) {
            finishAndRemoveTask();
            return;
        }
        startActivity(new Intent(this, LauncherActivity.class)
            .putExtra("resume-after-graphics", true)
            .putExtra("launch-demo", getIntent().getBooleanExtra("launch-demo", false)));
        finish();
    }
}
