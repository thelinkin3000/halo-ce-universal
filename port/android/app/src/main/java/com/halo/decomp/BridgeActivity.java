package com.halo.decomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

/**
 * All the bridge is (Migration.java): no game, only a notice that the game has a new name and a new app, which it
 * downloads and hands to the installer at once, and offers to open once it is there. The player's files stay where they
 * are for the new app to take (MigrationProvider): the notice says not to remove this app first.
 */
public class BridgeActivity extends Activity {
    private TextView message;
    private Button action;
    private boolean offered;

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics());
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout layout = new LinearLayout(this);
        TextView title = new TextView(this);

        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setPadding(dp(32), dp(16), dp(32), dp(16));
        layout.setBackgroundColor(Color.BLACK);
        title.setText("thelinkin3000's OpenCE fork is now SlipstreamCE");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 26);
        title.setGravity(Gravity.CENTER);
        message = new TextView(this);
        message.setTextColor(Color.LTGRAY);
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        message.setGravity(Gravity.CENTER);
        message.setPadding(0, dp(16), 0, dp(24));
        action = new Button(this);
        layout.addView(title);
        layout.addView(message);
        layout.addView(action);
        setContentView(layout);
    }

    /** on each return here: the installer may have put the new app in, or been refused */
    @Override
    protected void onResume() {
        super.onResume();
        Intent open = getPackageManager().getLaunchIntentForPackage(BuildConfig.HALO_NEW_PACKAGE);

        if (open != null) {
            message.setText("The new game is installed. Open it, and it brings over your saves, settings, mods and maps. "
                + "It will offer to remove this app afterwards.");
            action.setText("Open SlipstreamCE");
            action.setOnClickListener(view -> {
                startActivity(open);
                finish();
            });
            return;
        }
        message.setText("We're rebranding, and the game has moved to a new app. This one no longer plays.\n\n"
            + "Install SlipstreamCE, then open it to move all your data.\n"
            + "Don't remove this app before then: your saves are in it.");
        action.setText("Get SlipstreamCE");
        action.setOnClickListener(view -> download());
        if (!offered) {
            offered = true;
            download();
        }
    }

    private void download() {
        Updater.downloadNewApp(this, () -> Toast.makeText(this, "The new game could not be found. Check your connection "
            + "and try again.", Toast.LENGTH_LONG).show());
    }
}
