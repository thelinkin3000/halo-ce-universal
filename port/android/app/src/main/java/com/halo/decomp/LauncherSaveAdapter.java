package com.halo.decomp;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.List;

/** The saved-game picker keeps the native list interaction and readable, wrapping rows. */
final class LauncherSaveAdapter extends BaseAdapter {
    private final Context context;
    private final List<CheckpointSlots.Entry> entries;
    private final String[] labels;
    private final String[] details;

    LauncherSaveAdapter(Context context, List<CheckpointSlots.Entry> entries) {
        this.context = context;
        this.entries = entries;
        labels = new String[entries.size()];
        details = new String[entries.size()];
        for (int i = 0; i < entries.size(); i++) {
            CheckpointSlots.Entry entry = entries.get(i);
            String label = SaveLabels.label(entry);
            labels[i] = label;
            int line = label.indexOf('\n', entry.name.length());
            details[i] = line >= 0
                ? label.substring(line + 1) + "\n" + label.substring(entry.name.length() + 3, line)
                : label;
        }
    }

    @Override public int getCount() { return entries.size(); }
    @Override public CheckpointSlots.Entry getItem(int position) { return entries.get(position); }
    @Override public long getItemId(int position) { return position; }

    @Override public View getView(int position, View convertView, ViewGroup parent) {
        LinearLayout row;
        if (convertView instanceof LinearLayout) {
            row = (LinearLayout) convertView;
        } else {
            row = new LinearLayout(context);
            row.setOrientation(LinearLayout.VERTICAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(12), dp(14), dp(12), dp(14));
            row.setMinimumHeight(dp(76));
            row.setLayoutParams(new android.widget.AbsListView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            row.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
            row.setScreenReaderFocusable(true);
            row.setFocusable(false);

            TextView name = text(18);
            name.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
            row.addView(name, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            TextView metadata = text(13);
            metadata.setLineSpacing(dp(3), 1);
            LinearLayout.LayoutParams metadataLayout = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            metadataLayout.topMargin = dp(6);
            row.addView(metadata, metadataLayout);
        }
        boolean ready = getItem(position).candidate.kind == CheckpointStore.Kind.READY;
        TextView name = (TextView) row.getChildAt(0);
        TextView metadata = (TextView) row.getChildAt(1);
        name.setText(getItem(position).name);
        name.setTextColor(ready ? Color.rgb(237, 244, 249) : Color.rgb(145, 158, 172));
        metadata.setText(details[position]);
        metadata.setTextColor(ready ? Color.rgb(162, 181, 197) : Color.rgb(124, 143, 160));
        row.setContentDescription(labels[position]);
        return row;
    }

    private TextView text(int size) {
        TextView text = new TextView(context);
        text.setTextSize(size);
        text.setIncludeFontPadding(false);
        text.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        text.setFocusable(false);
        return text;
    }

    private int dp(int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }
}
