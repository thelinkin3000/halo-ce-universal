#!/usr/bin/env python3
"""Makes every platform's icon from the logo, port/assets/icon:

    python tools/slipstream_icon.py

- the Switch's (port/switch/art/icon.jpg, 256x256) and the PS5's (port/ps5/sce_sys/icon0.png, 512x512);
- the desktop builds' window icon (port/assets/icon/slipstream-icon-256.png), the Windows executable's
  (port/windows/slipstream-icon.ico) and the web page's (port/web/site/icons): each the logo whole;
- Settings' About and Credits pictures (the logo's artwork alone, white);
- the launcher icon of the Android app of the new package name (ar.com.slipstreamce):

The logo is white artwork on a flat colour, so that colour is the adaptive icon's background layer, and the artwork,
its white taken out of the picture as coverage, its foreground and its monochrome (themed icon) layers. Launchers mask
adaptive icons to their own shapes and show only the middle 72 dp of the 108 dp layers, and promise only the circle of
66 dp, so the artwork is centred and scaled to fit that circle whatever the mask. Writes the layers for every screen
density into port/android/app/src/slipstreamce/res. The old package's bridge keeps upstream's icon, in src/bridge/res
(tools/app_icon.py). Run it again when the logo changes. Needs Pillow and NumPy.
"""

from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
LOGO = ROOT / "port/assets/icon/slipstream-logo-2048.png"
RESOURCES = ROOT / "port/android/app/src/slipstreamce/res"
SWITCH_ICON = ROOT / "port/switch/art/icon.jpg"
PS5_ICON = ROOT / "port/ps5/sce_sys/icon0.png"
WINDOW_ICON = ROOT / "port/assets/icon/slipstream-icon-256.png"
WINDOWS_ICON = ROOT / "port/windows/slipstream-icon.ico"
WINDOWS_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
WEB_ICONS = ROOT / "port/web/site/icons"
WEB_SIZES = [32, 180, 192, 512]
# Settings' About: its picture's frame (its frames' place in the panel kept: ce_menus.py made the others so), and in it
# the square the logo fills
ABOUT_PICTURE = ROOT / "port/assets/menus/ce/shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__8.png"
# (x, y, side) in the frame: the middle of the panel it is drawn in, across
# and between the panel's top and the description's text
ABOUT_SQUARE = (92, 15, 380)
# Credits' picture (port_settings.py's _credits_screen): 256 units at 4 pixels
# a unit, the artwork 160 units across in its middle
CREDITS_PICTURE = ROOT / "port/assets/menus/ce/port/main_menu/settings_select/player_setup/player_profile_edit/credits/credits_logo.png"
CREDITS_PICTURE_SIZE = 1024
CREDITS_SIDE = 640
LAYER_DP = 108
SAFE_DP = 66
DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}


def whole_logo_icons() -> None:
    """the platforms whose icons are the picture as it is"""
    logo = Image.open(LOGO).convert("RGBA")
    logo.convert("RGB").resize((256, 256), Image.LANCZOS).save(SWITCH_ICON, quality=95, subsampling=0, optimize=True)
    logo.resize((512, 512), Image.LANCZOS).save(PS5_ICON, optimize=True)
    logo.resize((256, 256), Image.LANCZOS).save(WINDOW_ICON, optimize=True)
    logo.resize((512, 512), Image.LANCZOS).save(WINDOWS_ICON, sizes=[(size, size) for size in WINDOWS_SIZES])
    for size in WEB_SIZES:
        logo.resize((size, size), Image.LANCZOS).save(WEB_ICONS / f"icon-{size}.png", optimize=True)


def main() -> None:
    whole_logo_icons()
    pixels = np.asarray(Image.open(LOGO).convert("RGB"), dtype=np.float64)
    background = pixels[4, 4]
    # how white each pixel is, from the background's colour to white, by the channel furthest from it
    channel = int(np.argmin(background))
    coverage = np.clip((pixels[..., channel] - background[channel]) / (255.0 - background[channel]), 0.0, 1.0)
    ys, xs = np.nonzero(coverage > 0.5)
    centre_x, centre_y = (xs.min() + xs.max() + 1) / 2.0, (ys.min() + ys.max() + 1) / 2.0
    radius = float(np.sqrt((xs + 0.5 - centre_x) ** 2 + (ys + 0.5 - centre_y) ** 2).max())
    art = Image.fromarray(np.uint8(np.round(coverage * 255.0)), "L")
    size_hires = 1728  # 16 pixels a dp: reduced from this, the layers' edges stay smooth
    scale = (SAFE_DP / 2.0 * size_hires / LAYER_DP) / radius
    drawn = art.resize((round(art.width * scale), round(art.height * scale)), Image.LANCZOS)
    layer = Image.new("L", (size_hires, size_hires), 0)
    layer.paste(drawn, (round(size_hires / 2.0 - centre_x * scale), round(size_hires / 2.0 - centre_y * scale)))
    for density, per_dp in DENSITIES.items():
        size = round(LAYER_DP * per_dp)
        alpha = layer.resize((size, size), Image.LANCZOS)
        image = Image.merge("RGBA", (*(Image.new("L", (size, size), 255) for _ in range(3)), alpha))
        directory = RESOURCES / f"mipmap-{density}"
        directory.mkdir(parents=True, exist_ok=True)
        image.save(directory / "ic_launcher_foreground.png", optimize=True)
        image.save(directory / "ic_launcher_monochrome.png", optimize=True)
    colour = "#%02X%02X%02X" % tuple(int(round(value)) for value in background)
    (RESOURCES / "drawable").mkdir(parents=True, exist_ok=True)
    (RESOURCES / "drawable/ic_launcher_background.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<!-- the launcher icon's background (tools/slipstream_icon.py) -->\n"
        '<shape xmlns:android="http://schemas.android.com/apk/res/android">\n'
        f'    <solid android:color="{colour}" />\n'
        "</shape>\n", encoding="utf-8", newline="\n")
    (RESOURCES / "mipmap-anydpi-v26").mkdir(parents=True, exist_ok=True)
    (RESOURCES / "mipmap-anydpi-v26/ic_launcher.xml").write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<!-- the launcher icon (tools/slipstream_icon.py, from port/assets/icon) -->\n"
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@drawable/ic_launcher_background" />\n'
        '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
        '    <monochrome android:drawable="@mipmap/ic_launcher_monochrome" />\n'
        "</adaptive-icon>\n", encoding="utf-8", newline="\n")
    # (the artwork alone, in its own square: the same layer, without the launcher's margin)
    about = Image.open(ABOUT_PICTURE).size
    x, y, side = ABOUT_SQUARE
    box = round(radius * 2)
    left, top = round(centre_x - radius), round(centre_y - radius)
    fitted = art.crop((left, top, left + box, top + box)).resize((side, side), Image.LANCZOS)
    alpha = Image.new("L", about, 0)
    alpha.paste(fitted, (x, y))
    white = Image.new("L", about, 255)
    Image.merge("RGBA", (white, white, white, alpha)).save(ABOUT_PICTURE, optimize=True)
    fitted = art.crop((left, top, left + box, top + box)).resize((CREDITS_SIDE, CREDITS_SIDE), Image.LANCZOS)
    alpha = Image.new("L", (CREDITS_PICTURE_SIZE, CREDITS_PICTURE_SIZE), 0)
    alpha.paste(fitted, ((CREDITS_PICTURE_SIZE - CREDITS_SIDE) // 2, (CREDITS_PICTURE_SIZE - CREDITS_SIDE) // 2))
    white = Image.new("L", alpha.size, 255)
    CREDITS_PICTURE.parent.mkdir(parents=True, exist_ok=True)
    Image.merge("RGBA", (white, white, white, alpha)).save(CREDITS_PICTURE, optimize=True)
    print(f"background {colour}; artwork radius {radius:.0f} px of the logo, scaled {scale:.3f}")


if __name__ == "__main__":
    main()
