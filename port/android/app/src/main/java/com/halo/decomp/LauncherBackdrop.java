package com.halo.decomp;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.view.View;

/** Original, static orbital artwork. Geometry and shaders are prepared only on resize. */
final class LauncherBackdrop extends View {
    private static final int NAVY = Color.rgb(8, 14, 22);
    private static final float[] STARS = {
        .04f, .12f, .8f, .13f, .20f, .5f, .27f, .07f, .7f,
        .38f, .16f, .4f, .47f, .08f, .6f, .08f, .35f, .4f,
        .19f, .40f, .7f, .33f, .31f, .5f, .45f, .36f, .4f,
        .52f, .23f, .4f, .03f, .63f, .6f, .11f, .77f, .5f,
        .29f, .84f, .6f, .40f, .73f, .4f, .49f, .61f, .5f,
        .06f, .92f, .4f, .23f, .95f, .7f, .43f, .94f, .5f
    };

    private final Paint atmosphere = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint planet = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint surface = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ringEdge = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint star = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fade = new Paint();
    private final Path ringBack = new Path();
    private final Path ringFront = new Path();
    private final Path planetOutline = new Path();
    private final Path surfaceBands = new Path();
    private final RectF orbitBounds = new RectF();
    private final Matrix orbitTransform = new Matrix();
    private final float[] starPositions = new float[STARS.length / 3 * 2];
    private float centerX;
    private float centerY;
    private float radius;
    private float starScale;

    LauncherBackdrop(Context context) {
        super(context);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        setFocusable(false);
        setClickable(false);
        ring.setStyle(Paint.Style.STROKE);
        ringEdge.setStyle(Paint.Style.STROKE);
        ringEdge.setColor(Color.rgb(139, 176, 190));
        surface.setStyle(Paint.Style.STROKE);
        surface.setColor(Color.argb(29, 120, 159, 171));
        star.setColor(Color.rgb(189, 211, 222));
    }

    @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        super.onSizeChanged(width, height, oldWidth, oldHeight);
        if (width <= 0 || height <= 0) return;
        boolean landscape = width > height;
        float scale = landscape ? height : width;
        centerX = width * (landscape ? .17f : .25f);
        centerY = height * (landscape ? .65f : .34f);
        radius = scale * .43f;
        starScale = Math.max(.7f, getResources().getDisplayMetrics().density);

        atmosphere.setShader(new RadialGradient(centerX - radius * .15f,
            centerY - radius * .35f, radius * 1.8f,
            new int[] {Color.rgb(20, 42, 56), NAVY}, null, Shader.TileMode.CLAMP));
        planet.setShader(new RadialGradient(centerX - radius * .49f,
            centerY - radius * .62f, radius * 1.7f,
            new int[] {Color.rgb(65, 91, 106), Color.rgb(25, 44, 58),
                Color.rgb(8, 15, 24), Color.rgb(4, 9, 16)},
            new float[] {0, .42f, .78f, 1}, Shader.TileMode.CLAMP));
        ring.setShader(new LinearGradient(centerX - radius, centerY - radius,
            centerX + radius * 1.8f, centerY + radius,
            new int[] {Color.rgb(29, 48, 64), Color.rgb(142, 176, 186),
                Color.rgb(39, 65, 82)}, new float[] {0, .55f, 1}, Shader.TileMode.CLAMP));
        ring.setStrokeWidth(scale * .019f);
        ringEdge.setStrokeWidth(Math.max(1, scale * .0017f));
        surface.setStrokeWidth(scale * .015f);

        orbitBounds.set(-scale * .94f, -scale * .18f, scale * .94f, scale * .18f);
        orbitTransform.reset();
        orbitTransform.setRotate(-38);
        orbitTransform.postTranslate(centerX, centerY);
        ringBack.reset();
        ringBack.addOval(orbitBounds, Path.Direction.CW);
        ringBack.transform(orbitTransform);
        ringFront.reset();
        ringFront.addArc(orbitBounds, 0, 180);
        ringFront.transform(orbitTransform);
        planetOutline.reset();
        planetOutline.addCircle(centerX, centerY, radius, Path.Direction.CW);
        surfaceBands.reset();
        surfaceBands.moveTo(centerX - radius * 1.1f, centerY - radius * .15f);
        surfaceBands.cubicTo(centerX - radius * .35f, centerY - radius * .7f,
            centerX + radius * .1f, centerY + radius * .16f,
            centerX + radius, centerY - radius * .35f);
        surfaceBands.moveTo(centerX - radius, centerY + radius * .25f);
        surfaceBands.cubicTo(centerX - radius * .25f, centerY - radius * .1f,
            centerX + radius * .38f, centerY + radius * .65f,
            centerX + radius, centerY + radius * .24f);

        if (landscape) {
            fade.setShader(new LinearGradient(width * .23f, 0, width * .59f, 0,
                Color.argb(0, 8, 14, 22), NAVY, Shader.TileMode.CLAMP));
        } else {
            fade.setShader(new LinearGradient(0, height * .24f, 0, height * .61f,
                Color.argb(0, 8, 14, 22), NAVY, Shader.TileMode.CLAMP));
        }
        for (int i = 0, target = 0; i < STARS.length; i += 3) {
            starPositions[target++] = STARS[i] * width;
            starPositions[target++] = STARS[i + 1] * (landscape ? height : height * .62f);
        }
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        canvas.drawColor(NAVY);
        if (radius <= 0) return;
        canvas.drawRect(0, 0, getWidth(), getHeight(), atmosphere);
        for (int i = 0, point = 0; i < STARS.length; i += 3, point += 2) {
            star.setAlpha((int) (STARS[i + 2] * 170));
            canvas.drawCircle(starPositions[point], starPositions[point + 1],
                STARS[i + 2] * starScale, star);
        }
        canvas.drawPath(ringBack, ring);
        canvas.drawPath(ringBack, ringEdge);
        canvas.drawCircle(centerX, centerY, radius, planet);
        int saved = canvas.save();
        canvas.clipPath(planetOutline);
        canvas.drawPath(surfaceBands, surface);
        canvas.restoreToCount(saved);
        canvas.drawPath(ringFront, ring);
        canvas.drawPath(ringFront, ringEdge);
        canvas.drawRect(0, 0, getWidth(), getHeight(), fade);
    }
}
