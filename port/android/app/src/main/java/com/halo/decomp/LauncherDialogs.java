package com.halo.decomp;

import android.app.AlertDialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.util.DisplayMetrics;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.Button;
import android.widget.ListView;
import android.widget.TextView;

/** Styling applied after show(), retaining the platform dialog's list and focus behavior. */
final class LauncherDialogs {
    private LauncherDialogs() { }

    static void style(AlertDialog dialog) {
        Context context = dialog.getContext();
        Window window = dialog.getWindow();
        if (window == null) return;
        DisplayMetrics metrics = context.getResources().getDisplayMetrics();
        GradientDrawable background = new GradientDrawable();
        background.setColor(Color.rgb(16, 27, 39));
        background.setCornerRadius(dp(context, 20));
        background.setStroke(dp(context, 1), Color.rgb(38, 56, 72));
        window.setBackgroundDrawable(background);
        int availableWidth = Math.max(1, metrics.widthPixels - dp(context, 48));
        window.setLayout(Math.min(dp(context, 600), availableWidth), ViewGroup.LayoutParams.WRAP_CONTENT);
        window.setDimAmount(.72f);

        int titleId = context.getResources().getIdentifier("alertTitle", "id", "android");
        TextView title = titleId == 0 ? null : dialog.findViewById(titleId);
        if (title != null) {
            title.setTextColor(Color.rgb(237, 244, 249));
            title.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            title.setTextSize(21);
            title.setSingleLine(false);
            title.setMaxLines(Integer.MAX_VALUE);
            title.setEllipsize(null);
        }
        TextView message = dialog.findViewById(android.R.id.message);
        if (message != null) {
            message.setTextColor(Color.rgb(174, 191, 206));
            message.setTextSize(16);
            message.setLineSpacing(dp(context, 3), 1);
        }
        ListView list = dialog.getListView();
        if (list != null) {
            list.setDivider(new ColorDrawable(Color.rgb(36, 52, 68)));
            list.setDividerHeight(dp(context, 1));
            list.setPadding(dp(context, 12), 0, dp(context, 12), dp(context, 8));
            list.setClipToPadding(false);
            list.setSelector(new RippleDrawable(ColorStateList.valueOf(Color.rgb(42, 67, 86)),
                new ColorDrawable(Color.TRANSPARENT), null));
        }
        styleButton(dialog.getButton(AlertDialog.BUTTON_POSITIVE), context);
        styleButton(dialog.getButton(AlertDialog.BUTTON_NEGATIVE), context);
        styleButton(dialog.getButton(AlertDialog.BUTTON_NEUTRAL), context);
    }

    private static void styleButton(Button button, Context context) {
        if (button == null) return;
        button.setTextColor(new ColorStateList(new int[][] {
            new int[] {-android.R.attr.state_enabled}, new int[0]
        }, new int[] {Color.rgb(103, 123, 141), Color.rgb(124, 218, 236)}));
        button.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        button.setTextSize(14);
        button.setAllCaps(false);
        button.setSingleLine(false);
        button.setMinHeight(dp(context, 48));
        button.setMinimumHeight(dp(context, 48));
        button.setPadding(dp(context, 16), dp(context, 10), dp(context, 16), dp(context, 10));
    }

    private static int dp(Context context, int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }
}
