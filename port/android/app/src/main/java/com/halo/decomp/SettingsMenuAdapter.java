package com.halo.decomp;

import android.content.Context;
import android.graphics.Typeface;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.LinearLayout;
import android.widget.TextView;

/** Native, wrapping settings rows; item positions keep their existing actions. */
final class SettingsMenuAdapter extends BaseAdapter {
    private final Context context;
    private final String[] titles;
    private final String[] details;

    SettingsMenuAdapter(Context context, String[] titles, String[] details) {
        if (titles.length != details.length)
            throw new IllegalArgumentException("Each setting needs a description");
        this.context = context;
        this.titles = titles;
        this.details = details;
    }

    @Override public int getCount() { return titles.length; }
    @Override public String getItem(int position) { return titles[position]; }
    @Override public long getItemId(int position) { return position; }

    @Override public View getView(int position, View recycled, ViewGroup parent) {
        LinearLayout row;
        if (recycled instanceof LinearLayout) {
            row = (LinearLayout) recycled;
        } else {
            row = new LinearLayout(context);
            row.setOrientation(LinearLayout.VERTICAL);
            row.setPadding(dp(16), dp(12), dp(16), dp(12));
            row.setMinimumHeight(dp(68));
            TextView title = new TextView(context);
            title.setTextSize(16);
            title.setTextColor(0xffedf4f9);
            title.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            row.addView(title, new LinearLayout.LayoutParams(-1, -2));
            TextView detail = new TextView(context);
            detail.setTextSize(13);
            detail.setTextColor(0xffaebfce);
            detail.setPadding(0, dp(4), 0, 0);
            row.addView(detail, new LinearLayout.LayoutParams(-1, -2));
        }
        ((TextView) row.getChildAt(0)).setText(titles[position]);
        ((TextView) row.getChildAt(1)).setText(details[position]);
        return row;
    }

    private int dp(int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }
}
