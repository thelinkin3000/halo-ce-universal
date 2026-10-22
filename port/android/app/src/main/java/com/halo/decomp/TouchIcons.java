package com.halo.decomp;

import android.content.res.Resources;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.drawable.Drawable;

/** Cached Game Icons silhouettes; attribution: assets/notices/game-icons.txt. */
final class TouchIcons {
    private static final int[] RESOURCES = {
        R.drawable.touch_jump, R.drawable.touch_grenade_type, R.drawable.touch_use,
        R.drawable.touch_weapon, R.drawable.touch_melee, R.drawable.touch_flashlight,
        R.drawable.touch_grenade, R.drawable.touch_fire, R.drawable.touch_pause,
        R.drawable.touch_back, R.drawable.touch_crouch, R.drawable.touch_zoom
    };
    private final Drawable[] icons = new Drawable[RESOURCES.length];

    TouchIcons(Resources resources) {
        for (int i = 0; i < icons.length; i++) {
            icons[i] = resources.getDrawable(RESOURCES[i], null).mutate();
            icons[i].setBounds(0, 0, 512, 512);
        }
    }

    /** Keep the profile's Xbox button letter legible below its action silhouette. */
    void drawCaption(Canvas canvas, Paint paint, float x, float y, float radius,
            String button, String action) {
        int icon = "SALTO".equals(action) ? 0 : "GOLPE".equals(action) ? 4
            : "USAR".equals(action) ? 2 : 3;
        drawIcon(canvas, paint, x, y - radius * 0.17f, radius * 0.61f, icon);
        paint.setStyle(Paint.Style.FILL);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(radius * 0.27f);
        // Fit both the button letter and action inside the existing touch circle.
        float gap = radius * 0.12f;
        float width = paint.measureText(button) + gap + paint.measureText(action);
        if (width > radius * 1.4f)
            paint.setTextSize(paint.getTextSize() * radius * 1.4f / width);
        float buttonWidth = paint.measureText(button);
        width = buttonWidth + gap + paint.measureText(action);
        paint.setTextAlign(Paint.Align.LEFT);
        canvas.drawText(button, x - width / 2, y + radius * 0.72f, paint);
        canvas.drawText(action, x - width / 2 + buttonWidth + gap, y + radius * 0.72f, paint);
    }

    /** Pictograms carry the action; keep the existing caller free of duplicate text. */
    void drawLabel(Canvas canvas, Paint paint, float x, float y, float radius, int action) {
    }

    void draw(Canvas canvas, Paint paint, float x, float y, float radius, int action) {
        drawIcon(canvas, paint, x, y, radius * 0.73f, action);
    }

    private void drawIcon(Canvas canvas, Paint paint, float x, float y, float radius, int action) {
        if (action < 0 || action >= icons.length || !Float.isFinite(radius) || radius <= 0)
            return;
        Drawable icon = icons[action];
        icon.setAlpha(paint.getAlpha());
        canvas.save();
        canvas.translate(x - radius, y - radius);
        canvas.scale(radius / 256f, radius / 256f);
        icon.draw(canvas);
        canvas.restore();
    }
}
