"""The port's settings screens, which tools/ce_menus.py writes in place of the
PC version's (port/assets/menus/ce): laid out as its are (rows of a label and
a spinner over its row backgrounds, a line of help for the row chosen,
Defaults, OK and Cancel), with what this port has to set instead of what the
PC version had (no connection speed or texture quality: a window or the full
screen, the frame rate, the keyboard's own controls).

A row's spinner sets one of config.toml's settings (setting=, values=), or of
the profile being edited ("profile.<field>": the controller's settings), by
port/linux/game/menu_functions.c: "port setting load" shows its value,
"port settings save" (OK) writes those changed and applies them, "port
settings defaults" shows the defaults. Controls Setup binds the keyboard and
mouse's controls, by the functions the PC version names for it.
"""

import re
import sys
from pathlib import Path
from xml.sax.saxutils import quoteattr

PE = "main_menu/settings_select/player_setup/player_profile_edit"
# the mods' screen, and its rows (_mods); the main menu's items
# (ce/main_menu.xml), MAIN_MENU_ITEM_SPACING apart (the PC version's 36
# leaves no room below Quit for a seventh): Campaign and Multiplayer, THEATER
# below it (_theater), the PC version's Profiles and Settings, MODS in
# Credits' place (Credits is left out: tools/ce_menus.py's LEFT_OUT), and
# Quit (their CHILD_OFFSETS); the list's order is its up and down's
MODS = "main_menu/mods"
MOD_ROWS = 11
MAIN_MENU_ITEM_TOP = 247
MAIN_MENU_ITEM_SPACING = 32
MAIN_MENU_ITEMS = ["load_camp", "multiplayer", "theater", "profile", "settings", "mods", "quit_game"]


def main_menu_item_place(item: str) -> tuple:
    """where the main menu's item is (Campaign's 2 units right of the rest,
    as the PC version has it)"""
    return (194 if item == "load_camp" else 192,
            MAIN_MENU_ITEM_TOP + MAIN_MENU_ITEM_SPACING * MAIN_MENU_ITEMS.index(item))


MODS_ITEM_PLACE = main_menu_item_place("mods")
THEATER_ITEM_PLACE = main_menu_item_place("theater")
QUIT_ITEM_PLACE = main_menu_item_place("quit_game")
# the Theater (port/linux/REPLAY.md): the replays a host records, its rows
# (_theater)
THEATER = "main_menu/theater"
# the profile settings' rows, in order: the PC version's nine and Gameplay,
# second to last, all in the nine's room (78 to About's top, 342) so the
# last one's 32-unit highlight stays beside the description panel. Each is
# moved from its tag's top (the second number; Gameplay's is ours) by its
# list's child offset (tools/ce_menus.py's CHILD_OFFSETS)
PROFILE_ROWS = [("name_profile_item", 78), ("controller_setup_profile_item", 111),
                ("gamepads_profile_item", 144), ("mouse_settings_profile_item", 177),
                ("audio_settings_profile_item", 210), ("video_settings_profile_item", 243),
                ("network_settings_profile_item", 277), ("color_profile_item", 277),
                ("gameplay_profile_item", None), ("about_item", 310)]
PROFILE_ROWS_TOP = 78
PROFILE_ROWS_LAST_TOP = 342


def profile_row_top(row: str) -> int:
    """where a profile settings row is: the rows evenly spaced"""
    index = [name for name, _ in PROFILE_ROWS].index(row)
    return PROFILE_ROWS_TOP + round(index * (PROFILE_ROWS_LAST_TOP - PROFILE_ROWS_TOP) / (len(PROFILE_ROWS) - 1))


def profile_row_offset(row: str) -> int:
    """a PC row's child offset in the list, to its place"""
    return profile_row_top(row) - dict(PROFILE_ROWS)[row]


YES_NO = [("YES", "true"), ("NO", "false")]
ON_OFF = [("ON", "true"), ("OFF", "false")]
# the multiplayer armor colors, in the profile's order (player_profile.c)
SHIELD_COLORS = ["white", "black", "red", "blue", "gray", "yellow", "green", "pink", "purple", "cyan", "cobalt",
                 "orange", "teal", "sage", "brown", "tan", "maroon", "salmon"]
SENSITIVITIES = [(f"{value:g}", f"{value:g}") for value in (0.1, 0.15, 0.25, 0.4, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3, 4)]
VOLUMES = [(str(step), f"{step / 10:g}") for step in range(11)]
# an HDR display's paper white and peak in nits (display.hdr_paper_white,
# display.hdr_peak_brightness), in steps fine enough for HDR Calibration's
# patterns (and BT.2408's reference white, 203); Extra Effects' rows have the
# same, so they show what it chose
PAPER_WHITES = [("AUTO", "0")] + [(str(nits), str(nits)) for nits in sorted({*range(80, 200, 10), 200, 203, *range(220, 401, 20), 250})]
PEAK_BRIGHTNESSES = [("AUTO", "0")] + [(str(nits), str(nits)) for nits in
                                       [*range(250, 600, 25), *range(600, 1200, 50), *range(1200, 2001, 100),
                                        2500, 3000, 4000, 5000, 6000, 8000, 10000]]

# Gamepads' BUTTON LAYOUT's values (profile.button_preset)
BUTTON_LAYOUTS = [("DEFAULT", "0"), ("SWAP TRIGGERS", "1"), ("SWAP A, L TRIGGER", "2"), ("SWAP B, L TRIGGER", "3"),
                  ("SWAP B, R STICK", "4"),
                  # (the port's: the later games' layouts as Halo: CE's controls
                  # go, source/interface/player_ui.c; upstream's Fishsticks and
                  # Recon after this fork's, whose numbers saved profiles keep)
                  ("BOXER", "5"), ("GREEN THUMB", "6"), ("BUMPER JUMPER", "7"), ("RECLAIMER", "8"),
                  ("FISHSTICKS", "9"), ("RECON", "10")]

# each screen: its folder below PE, its screen's widget (the name the profile
# menu opens), its header (widget, bitmap), the row spacing, and its rows:
# (label, setting, [(shown, value)], help, platform[, key]): platform is None
# for every platform, else the platforms the row is for, separated by spaces
# ("desktop", "android" (the Android app), "switch", "ps5", "web": the menus'
# platform attribute, port/linux/src/menu_files.c); key names the row's widgets where
# two rows set one setting (one for each platform), else the setting's name does
SCREENS = {
    "video_settings": {
        "screen": "video_settings_screen",
        "header": ("header_profile_video_settings", f"{PE}/video_settings/header_profile_video_settings"),
        # the other screens' 30px step. each platform packs its own rows, so
        # a row hidden on this machine does not leave a gap
        "spacing": 30,
        "help_top": 364,
        "platform_places": True,
        # (rows in the place of the row before them: Window Size in
        # Resolution's, port/linux/game/menu_functions.c showing the one the
        # display mode chosen uses; Android's anti-aliasing in the desktop's.
        # Android's backend and Vulkan driver take places of their own:
        # platform_places packs each platform's rows)
        "same_place": ["display.window_size", "anti_aliasing_android"],
        # lines of text among the rows, in a place no row of theirs uses:
        # (text, platform, place), None the place after the platform's last
        # row. Below Android's rows, wherever they end (place 6 was below
        # them until Maximum Resolution and Extra Effects came)
        "notes": [("The graphics backend and driver change when the game restarts.", "android", None)],
        # (buttons of the bar's own, between DEFAULTS and OK: (key, words,
        # menu function, platform, x). Android's IMPORT DRIVER, the app's file
        # picker for a driver archive copied into freedreno_drivers:
        # menu_functions.c, HaloActivity.importVkDrivers)
        "bar_buttons": [("button_vk_driver_import", "IMPORT DRIVER", "port vk driver import", "android", 130)],
        "rows": [
            ("DISPLAY MODE:", "display.mode",
             [("FULLSCREEN", "fullscreen"), ("BORDERLESS", "borderless"), ("WINDOWED", "windowed")],
             "Fullscreen takes the display; borderless covers it\nwith a window. F11 switches to the window.",
             "desktop"),
            # (read by the Android app as the game starts, which loads the game
            # image built with that renderer: port/android/host/host_main.c)
            ("GRAPHICS BACKEND:", "display.renderer", [("VULKAN", "vulkan"), ("OPENGL", "gl")],
             "How the game draws: Vulkan, or OpenGL ES. Takes\neffect the next time the game starts.",
             "android"),
            # (port/linux/game/menu_tags.c adds the display's resolutions)
            ("RESOLUTION:", "display.resolution", [("NATIVE", "native")],
             "What fullscreen and borderless draw at. Fullscreen\nsets the display to it; borderless scales it.",
             "desktop"),
            # (port/linux/game/menu_tags.c puts the sizes that fit the desktop
            # in place of this one)
            ("WINDOW SIZE:", "display.window_size", [("1280 x 960", "1280x960")],
             "The window's size: 4:3, then 16:10, 16:9 and 21:9\n(its edges can also be dragged).", "desktop"),
            # (port/linux/game/menu_functions.c shows it while the backend
            # shown is Vulkan, and lists the archives in freedreno_drivers
            # after STOCK, as source/interface/ui_widget.c's extra items: the
            # Turnip build the launcher downloads among them; an archive's
            # name left there by hand is kept unless another is chosen:
            # port/android/host/host_vk_driver.c)
            ("VULKAN DRIVER:", "display.vk_driver", [("STOCK", "")],
             "The phone's own driver, or a zip from the drivers\nfolder (Turnip's, or one IMPORT DRIVER adds).",
             "android"),
            ("RESOLUTION SCALING:", "display.resolution_scaling", [("NATIVE", "native"), ("ORIGINAL", "original")],
             "Native draws at the resolution; Original draws\nthe Xbox's 640x480 and scales it up.", "desktop web"),
            # (the PS5's own, first: what the game draws at, no more than the
            # display's mode, port/ps5/host/host_main.c; the Switch's row
            # below has the same setting with its own values)
            ("MAXIMUM RESOLUTION:", "display.resolution_limit",
             [("2160P (4K)", "2160"), ("1440P (2K)", "1440"), ("1080P (FHD)", "1080")],
             "The most lines the game draws at, no more than\nthe display shows. Changes at once.", "ps5",
             "resolution_limit_ps5"),
            # (the phone's: sdl_platform.c's platform_resolution_scale)
            ("MAXIMUM RESOLUTION:", "display.resolution_limit",
             [("NATIVE", "native"), ("2160P (4K)", "2160"), ("1440P (2K)", "1440"), ("1080P (FHD)", "1080"),
              ("720P", "720"), ("480P", "480")],
             "The most lines the game draws at, no more than\nthe screen has. More are sharper, and slower.", "android",
             "resolution_limit_android"),
            ("V-SYNC:", "display.vsync", ON_OFF,
             "Wait for the display between frames, so that the\npicture never tears.", None),
            ("FRAME RATE LIMIT:", "display.max_fps",
             [("AUTO", "0"), ("30", "30"), ("60", "60"), ("120", "120"), ("144", "144"), ("165", "165"),
              ("240", "240"), ("NONE", "-1")],
             "With V-Sync off, the most frames a second. Auto:\ntwice the display's refresh rate.", "desktop web"),
            ("FRAME RATE LIMIT:", "display.max_fps",
             [("AUTO", "0"), ("30", "30"), ("60", "60"), ("120", "120"), ("NONE", "-1")],
             "With V-Sync off, the most frames a second. Auto:\ntwice the display's refresh rate.", "android",
             "max_fps_android"),
            ("SMOOTH MOTION:", "display.interpolation", ON_OFF,
             "Draw a frame for every display refresh, blending\nbetween the game's 30 ticks a second.", None),
            ("INSTANT AIM:", "display.direct_camera", ON_OFF,
             "In first person, turn the view the moment the\nmouse moves, not up to two ticks later.", "desktop web"),
            # (the deko3d renderer's screen scale: port/switch/guest/d3d8_dk.c)
            ("RESOLUTION LIMIT:", "display.resolution_limit",
             [("480P", "480"), ("720P", "720"), ("900P", "900"), ("1080P", "1080")],
             "What the game draws at. Shown at 720p in handheld\nand 1080p docked; handheld draws 720p at most.", "switch"),
            # (port/switch/guest/d3d8_dk.c's dynamic_resolution)
            ("DYNAMIC RESOLUTION:", "display.dynamic_resolution", ON_OFF,
             "Draw one step lower while the frame rate would\nfall under 55, and back up once it fits again.", "switch"),
            # (Graphics', with the HUD's rows: Video Setup's own has no room
            # left above its help on the desktop. The game's own counter, main.c's main_framerate_render, which
            # display.show_fps turns on: port/linux/game/frame_rate_display.c)
            ("SHOW FRAMES:", "display.show_fps", ON_OFF,
             "Draw the frame rate at the bottom right of a game,\ncounted over half a second.", None),
            ("HIGH-RES HUD:", "display.high_res_hud", ON_OFF,
             "Draw the HUD from the high-res redraws; off\ndraws the game's own pictures.", None),
            ("BUTTON STYLE:", "display.hud_button_style",
             [("CLASSIC", "classic"), ("GEMS", "gems"), ("DARK GEMS", "menu_gems")],
             "Classic: plain Xbox buttons. Gems: shaded Xbox\nbuttons. Dark Gems: the menu's button artwork.", None),
            ("HIGH-RES TEXT:", "display.high_res_text", ON_OFF,
             "Draw text and titles with high-res fonts; off\ndraws the game's own.", None),
            ("ANTI-ALIASING:", "display.anti_aliasing",
             [("OFF", "off"), ("FXAA", "fxaa"), ("SMAA", "smaa"), ("SSAA 2X", "ssaa2x"), ("MSAA 2X", "msaa2x"),
              ("MSAA 4X", "msaa4x"), ("MSAA 8X", "msaa8x")],
             "Smooth jagged edges, which the Xbox did not. FXAA\nand SMAA are cheap; SSAA and MSAA are sharper.",
             "desktop"),
            ("ANTI-ALIASING:", "display.anti_aliasing",
             [("OFF", "off"), ("FXAA", "fxaa"), ("MSAA 2X", "msaa2x"), ("MSAA 4X", "msaa4x")],
             "Smooth jagged edges, which the Xbox did not. FXAA\nis cheap; MSAA is sharper.",
             "android web ps5", "anti_aliasing_android"),
            ("TEXTURE FILTERING:", "display.anisotropic_filtering",
             [("OFF", "0"), ("2X", "2"), ("4X", "4"), ("8X", "8"), ("16X", "16")],
             "Keep floors and walls sharp at a grazing angle,\nwhich the Xbox did not. Off: as the Xbox.",
             "desktop android web"),
            ("SHADOW RESOLUTION:", "display.shadow_resolution",
             [("128", "128"), ("256", "256"), ("512", "512"), ("1024", "1024")],
             "The size objects' shadows are drawn at: 128 as on\nthe Xbox; larger for smoother, as soft, edges.",
             "desktop android web ps5"),
            # (neither is the Switch's: its deko3d renderer draws the Xbox's
            # shadow maps and lights each vertex, port/switch/guest/d3d8_dk.c)
            ("PER-PIXEL LIGHTING:", "display.per_pixel_lighting", ON_OFF,
             "Light models for each pixel, without the facets\nof the Xbox's lighting for each vertex.",
             "desktop android web ps5"),
            # (the OpenGL renderer's: the deko3d and Vulkan ones do not draw
            # its fade up the body, d3d8_gl_set_height_fade)
            ("OVERSHIELD:", "display.overshield_effect", ON_OFF,
             "The overshield rises up the body and stays lit,\nin its own color. Online: the host's choice.",
             "desktop android web"),
            ("MODEL DETAIL:", "display.model_detail", [("STOCK", "stock"), ("NATIVE", "native"), ("HIGHEST", "highest")],
             "Stock picks models' detail by the Xbox's 480 lines;\nnative by this display's; highest, always the best.", None),
            ("SHIELD GLOW:", "display.shield_glow", ON_OFF,
             "Shields light their surroundings as they flare.\nOnline: the host's choice.", None),
            ("GLOW INTENSITY:", "display.shield_glow_intensity", [("DEFAULT", "default"), ("LIGHT SHOW", "light_show")],
             "Light Show: three times as far, at full from a\nthird of the flare. Online: the host's choice.", None),
            # (the OpenGL renderer's alone, as texture filtering: the deko3d
            # and Vulkan ones draw the Xbox's range)
            ("HDR:", "display.hdr", ON_OFF,
             "Let explosions, plasma and the sun shine brighter\nthan white, then fit them to the screen.",
             "desktop android web"),
            ("HDR DISPLAY:", "display.hdr_output", ON_OFF,
             "On an HDR display (Windows HDR on), show that\nlight truly brighter. From the next start.", "desktop"),
            ("TONE CURVE:", "display.hdr_tone_mapping",
             [("GT7", "gt7"), ("NEUTRAL", "neutral"), ("ACES", "aces"), ("REINHARD", "reinhard")],
             "GT7: bright light turns white, as on film. Neutral:\nas drawn. ACES: more contrast. Reinhard: softer.",
             "desktop android web"),
            ("PAPER WHITE:", "display.hdr_paper_white", PAPER_WHITES,
             "With HDR Display, how bright white is, in nits.\nAuto: Windows' SDR content brightness.", "desktop"),
            ("PEAK BRIGHTNESS:", "display.hdr_peak_brightness", PEAK_BRIGHTNESSES,
             "With HDR Display, your display's brightest, in\nnits. Auto: Windows' HDR calibration.", "desktop"),
        ],
    },
    "mouse_settings": {
        "screen": "mouse_settings_screen",
        "header": ("header_profile_mouse_settings", f"{PE}/mouse_settings/header_profile_mouse_settings"),
        "spacing": 30,
        "rows": [
            ("HORIZONTAL SENSITIVITY:", "input.mouse_sensitivity", SENSITIVITIES,
             "How fast the view turns side to side for the\nmouse's movement.", None),
            ("VERTICAL SENSITIVITY:", "input.mouse_vertical_sensitivity", [("SAME", "0")] + SENSITIVITIES,
             "How fast the view turns up and down; Same\nturns it as fast as side to side.", None),
            ("INVERT VERTICAL AXIS:", "input.invert_mouse", YES_NO,
             "Moving the mouse forward looks down.", None),
            ("AIM ASSIST:", "input.mouse_aim_assist", ON_OFF,
             "Slow and drag the view along with a target while\naiming with the mouse, as with a controller.", None),
            # (port/linux/src/sdl_platform.c's menu_cursor_size; last, as the
            # other platforms have no row in its place)
            ("MENU CURSOR:", "display.menu_cursor",
             [("OFF", "off"), ("SMALL", "small"), ("MEDIUM", "medium"), ("LARGE", "large"),
              ("EXTRA LARGE", "extra_large")],
             "The menus' blue arrow for the mouse, and its size;\nOff shows the system's pointer.", "desktop"),
        ],
    },
    "audio_settings": {
        "screen": "audio_settings_screen",
        "header": ("header_profile_audio_settings", f"{PE}/audio_settings/header_profile_audio_settings"),
        "spacing": 30,
        # (the devices, desktop only, first: Android's rows from the top)
        "platform_places": True,
        "rows": [
            # (port/linux/game/menu_tags.c adds the devices SDL finds)
            ("OUTPUT DEVICE:", "audio.output_device", [("SYSTEM DEFAULT", "default")],
             "Where the game's sound and the voices play.", "desktop"),
            ("INPUT DEVICE:", "audio.input_device", [("SYSTEM DEFAULT", "default")],
             "The microphone voice chat listens to.", "desktop"),
            ("MASTER VOLUME:", "audio.volume", VOLUMES, "The volume of everything.", None),
            ("MUSIC VOLUME:", "audio.music_volume", VOLUMES, "The music's volume.", None),
            ("EFFECTS VOLUME:", "audio.effects_volume", VOLUMES,
             "The volume of every other sound: effects and\nspeech.", None),
            ("REVERB:", "audio.reverb", ON_OFF,
             "Echo sounds as the place you are in does, and\nmuffle those behind walls, as the Xbox did.", None),
            ("SOUND:", "audio.enabled", ON_OFF,
             "Play sound at all; from the next time the game\nstarts.", None),
            # voice chat: this machine's own (port/linux/game/network_voice.c)
            ("VOICE CHAT:", "audio.voice_chat",
             [("PUSH TO TALK", "push_to_talk"), ("OPEN MIC", "open_mic"), ("OFF", "off")],
             "Talk in network games: while PUSH TO TALK is held\n(Controls), whenever you speak, or never.", None),
            ("VOICE VOLUME:", "audio.voice_volume", VOLUMES, "The other players' voices.", None),
        ],
    },
    "network_setup": {
        "screen": "network_settings_screen",
        "header": ("header_profile_network_settings", f"{PE}/network_setup/header_profile_network_settings"),
        "spacing": 30,
        # (the router, the clipboard and the updates, which the web build
        # has not: the other rows from the top there)
        "platform_places": True,
        "rows": [
            ("INTERNET PLAY:", "network.online", ON_OFF,
             "Host and join games over the internet by invite\nlinks; off keeps to the local network.",
             "desktop android switch web ps5"),
            ("UPNP PORT FORWARDING:", "network.allow_upnp", ON_OFF,
             "Let internet play ask the router to forward its\nport, for networks that stop connections.",
             "desktop android switch ps5"),
            # (neither of these two is the PS5's: nothing outside a title
            # can put text on its clipboard, and a title is updated by
            # installing it again, port/ps5/host/host_update_none.c)
            ("JOIN FROM CLIPBOARD:", "network.join_from_clipboard", ON_OFF,
             "Join the game of an invite link copied before\nswitching to the game.", "desktop android switch"),
            ("CHECK FOR UPDATES:", "update.auto", ON_OFF,
             "Look for a new version when the game starts.", "desktop android switch"),
            ("PLAYER NAMES:", "display.player_names",
             [("ALL", "all"), ("ALLIES", "allies"), ("ENEMIES", "enemies"), ("NONE", "none")],
             "In multiplayer, whose names are drawn above their\nheads.", None),
            ("PLAYER NAME SIZE:", "display.player_name_scale",
             [(f"{value:g}x", f"{value:g}") for value in (0.5, 0.75, 1, 1.25, 1.5, 2)],
             "How large the players' names are drawn.", None),
            ("SCOREBOARD LAYOUT:", "display.scoreboard_team_layout", [("TEAMS", "teams"), ("BY SCORE", "score")],
             "A team game's scoreboard: a column for each team,\nor every player in order of score.", None),
            ("SCOREBOARD PANEL:", "display.scoreboard_background", ON_OFF,
             "Draw a panel behind the scoreboard, for clearer\ntext.", None),
        ],
    },
    "gamepad_setup": {
        "screen": "gamepad_setup_screen",
        "header": ("header_profile_controller_setting", f"{PE}/controller_edit/header_profile_controller_setting"),
        "spacing": 30,
        "rows": [
            ("LOOK SENSITIVITY:", "profile.look_sensitivity", [(str(step), str(step)) for step in range(1, 11)],
             "How fast the right stick turns the view.", None),
            ("INVERT LOOK:", "profile.invert_look", YES_NO, "Pushing the right stick up looks down.", None),
            ("INVERT FLIGHT:", "profile.flight_inversion", YES_NO,
             "Flying vehicles pitch as aircraft do: pushing up\nflies down.", None),
            ("AUTO-CENTER LOOK:", "profile.autocenter", YES_NO,
             "Level the view while walking.", None),
            ("BUTTON LAYOUT:", "profile.button_preset", BUTTON_LAYOUTS,
             "Which of the controller's buttons does what (not\nthe keyboard's: Controls Setup sets those).",
             "desktop android web"),
            # (the Switch and the PS5 have no keyboard, nor Controls Setup:
            # menu_tags.c's platform_hidden_profile_items)
            ("BUTTON LAYOUT:", "profile.button_preset", BUTTON_LAYOUTS,
             "Which of the controller's buttons does what.", "switch ps5", "button_preset_console"),
            ("STICK LAYOUT:", "profile.joystick_preset",
             [("DEFAULT", "0"), ("SOUTHPAW", "1"), ("LEGACY", "2"), ("LEGACY SOUTHPAW", "3")],
             "Which stick moves and which looks.", None),
            ("VIBRATION:", "profile.vibration", ON_OFF, "Rumble the controller.", None),
            ("IN-GAME HELP:", "profile.ingame_help", ON_OFF, "Show the game's hints about its controls.", None),
        ],
    },
}

# Gyro Aim is its own screen, opened from Gamepads: the controller's gyroscope
# on the Switch and the PS5, the phone's on Android. The other platforms do not show the
# row, and it takes the place after the last row, so they have no gap.
_gamepads = SCREENS["gamepad_setup"]
_gamepads["categories"] = [
    ("GYRO AIM:", "gamepad_setup/gyro",
     "Turn the view with the gyroscope,\nas well as the stick.", "android switch ps5"),
]
SCREENS["gamepad_setup/gyro"] = {
    "screen": "gyro_aim_screen", "header": _gamepads["header"], "spacing": 30,
    "rows": [
        ("GYRO AIM:", "input.gyro_aim", ON_OFF + [("ONLY ON ZOOM", "zoom")],
         "Turn the view with the gyroscope,\nalways, or only while zoomed in.", None),
        ("HORIZONTAL SENSITIVITY:", "input.gyro_sensitivity_horizontal",
         [(f"{value:g}", f"{value:g}") for value in
          (0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 2.5, 3, 4, 5, 6, 7, 8, 9, 10)],
         "How far the view turns left and right.\n1 is the sensor's own turn.", None),
        ("VERTICAL SENSITIVITY:", "input.gyro_sensitivity_vertical",
         [(f"{value:g}", f"{value:g}") for value in
          (0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 2.5, 3, 4, 5, 6, 7, 8, 9, 10)],
         "How far the view turns up and down.\n1 is the sensor's own turn.", None),
        ("INVERT HORIZONTAL:", "input.gyro_invert_horizontal", YES_NO,
         "Turning it right looks left.", None),
        ("INVERT VERTICAL:", "input.gyro_invert_vertical", YES_NO,
         "Tilting it up looks down.", None),
    ],
    # The other controller rows are profile fields, written when the profile
    # is saved. These are the profile's own settings.toml (config.toml's where
    # it has none: port/linux/src/port_config.h), written at once, and B (and
    # Cancel) only went back, so a change was kept for this run and gone after
    # quitting.
    "save_on_leave": True,
}

# BUTTON LAYOUT is its own screen, opened from Gamepads: the button and stick
# layouts, and below them the overlay of the layout shown, a picture of the
# controller with what the layout puts by each button (_layout_overlays)
LAYOUT = f"{PE}/gamepad_setup/layout"
_layout_rows = {"profile.button_preset", "profile.joystick_preset"}
_gamepads["categories"].insert(0, (
    "BUTTON LAYOUT:", "gamepad_setup/layout", "See what each button does in each layout,\nand choose one."))
SCREENS["gamepad_setup/layout"] = {
    "screen": "button_layout_screen", "header": _gamepads["header"], "spacing": 30, "help_top": 133,
    "rows": [row for row in _gamepads["rows"] if row[1] in _layout_rows],
    # (the consoles' BUTTON LAYOUT in the other's place)
    "same_place": ["button_preset_console"],
}
_gamepads["rows"] = [row for row in _gamepads["rows"] if row[1] not in _layout_rows]

# the overlay's controllers: AL2009man's Gamepad Asset Pack's
# (port/assets/menus/controllers, MIT), the platform's, and where each of its
# buttons is (a fraction of the picture's width and height), by the Xbox's
# names (the others' by place, as xinput_sdl.c reads them: the PS5's cross is
# a, the Switch's B is a); a trigger and a bumper where they show, above the
# controller. Back and Start in the middle are labelled above it (top), not
# across the buttons by them
CONTROLLERS = {
    "xbox_series": {"platform": "desktop android web", "aspect": 1.6087, "top": ("back", "start"), "buttons": {
        "a": (0.767, 0.538), "b": (0.838, 0.426), "x": (0.697, 0.437), "y": (0.770, 0.326),
        "trigger_left": (0.198, 0.04), "trigger_right": (0.792, 0.04), "white": (0.19, 0.15), "black": (0.81, 0.15),
        "back": (0.422, 0.440), "start": (0.576, 0.440), "stick_left": (0.229, 0.464),
        "stick_right": (0.636, 0.703), "dpad": (0.363, 0.686)}},
    "dualsense": {"platform": "ps5", "aspect": 1.7981, "buttons": {
        "a": (0.820, 0.636), "b": (0.897, 0.522), "x": (0.746, 0.527), "y": (0.824, 0.411),
        "trigger_left": (0.184, 0.05), "trigger_right": (0.815, 0.05), "white": (0.178, 0.17), "black": (0.822, 0.17),
        "back": (0.256, 0.335), "start": (0.744, 0.335), "stick_left": (0.335, 0.776),
        "stick_right": (0.665, 0.777), "dpad": (0.178, 0.524)}},
    "switch_pro": {"platform": "switch", "aspect": 1.3762, "top": ("back", "start"), "buttons": {
        "a": (0.762, 0.413), "b": (0.841, 0.319), "x": (0.685, 0.319), "y": (0.762, 0.224),
        "trigger_left": (0.23, 0.03), "trigger_right": (0.77, 0.03), "white": (0.20, 0.11), "black": (0.80, 0.11),
        "back": (0.377, 0.215), "start": (0.621, 0.215), "stick_left": (0.224, 0.318),
        "stick_right": (0.630, 0.510), "dpad": (0.347, 0.510)}},
}
# the labels each side of the controller, ordered top to bottom by their
# buttons, and the lines each has room for (a stick's: what it does, and
# its button's; the D-pad's: its directions' a layout gives a control)
LAYOUT_LABELS = {"left": {"trigger_left": 1, "white": 1, "back": 1, "stick_left": 2, "dpad": 2},
                 "right": {"trigger_right": 1, "black": 1, "y": 1, "b": 1, "x": 1, "a": 1, "start": 1,
                           "stick_right": 2}}
# the overlays: Gamepad Setup's BUTTON LAYOUT's, below its rows and their
# help, and the profile settings' picture's (its 279 by 202, where Gamepad
# Setup's row shows it: menu_functions.c's profile_gamepad_layout), which is
# the controller alone, no labels. The area (left, top, width, height), the
# controller's width and how far right of the middle it is, the labels'
# widths (each side's) and font, a line's height and the text's top in it.
# large_ui takes about 10.5 units a letter and its lines are 21 apart (a
# Switch's screenshots): the left's longest is a stick's PRESS: SWITCH
# GRENADE (Reclaimer's), the right's ACTION / RELOAD (its stick's button is
# only ever zoom or melee)
LAYOUT_OVERLAYS = {
    "overlay": {"area": (0, 172, 640, 236), "width": 200, "shift": 34,
                "label_width": {"left": 240, "right": 172}, "font": "ui\\large_ui", "line": 21, "text_y": 2},
    "overlay_small": {"area": (288, 85, 279, 202), "width": 250, "labels": False},
}
# their words: the game's controls (input_abstraction.c's), what a stick
# does, and the start of a line of a stick's button or a D-pad direction
# (menu_functions.c's _layout_word_*)
LAYOUT_WORDS = ["JUMP", "SWITCH GRENADE", "ACTION / RELOAD", "SWITCH WEAPON", "MELEE", "FLASHLIGHT",
                "THROW GRENADE", "FIRE", "PAUSE", "SCORES", "CROUCH", "ZOOM",
                "MOVE", "LOOK", "MOVE / TURN", "STRAFE / LOOK", "PRESS:", "UP:", "DOWN:", "LEFT:", "RIGHT:"]
# the controller tinted as the Xbox's pictures of its controller are: its
# lightness from the dark blue to the light
LAYOUT_TINT = ((0.02, 0.09, 0.20), (0.62, 0.84, 1.0))
LAYOUT_LINE_COLOR = "#2896FF"
# (BUTTON LAYOUT's, each platform's controller)
SCREENS["gamepad_setup/layout"]["screen_children"] = [
    f'<child widget="{LAYOUT}/{controller}/overlay" platform="{spec["platform"]}"/>'
    for controller, spec in CONTROLLERS.items()]

# Video Setup's categories open ordinary settings screens. Each has the same
# pending edits, Defaults, OK and Cancel as the other settings screens.
_video = SCREENS["video_settings"]
_graphics = {"display.show_fps", "display.high_res_hud", "display.hud_button_style",
             "display.high_res_text", "display.anti_aliasing",
             "display.anisotropic_filtering", "display.shadow_resolution", "display.per_pixel_lighting",
             "display.model_detail"}
# (the visual modifiers: a screen of their own, Extra Effects)
_extra_effects = {"display.shield_glow", "display.shield_glow_intensity",
                  "display.overshield_effect", "display.hdr", "display.hdr_output", "display.hdr_tone_mapping",
                  "display.hdr_paper_white", "display.hdr_peak_brightness"}
# (each platform's rows packed: the Switch has none of the renderer's rows
# between the text and the model detail, which left it gaps; the two
# anti-aliasing rows land in the same place, each its platforms' next)
SCREENS["video_settings/graphics"] = {
    "screen": "graphics_settings_screen", "header": _video["header"], "spacing": 30,
    "platform_places": True,
    "rows": [row for row in _video["rows"] if row[1] in _graphics],
}
SCREENS["video_settings/extra_effects"] = {
    "screen": "extra_effects_settings_screen", "header": _video["header"], "spacing": 30,
    "platform_places": True,
    "rows": [row for row in _video["rows"] if row[1] in _extra_effects],
    # (shown only with an HDR display: menu_functions.c's
    # hdr_calibration_row_show)
    "categories": [
        ("HDR CALIBRATION:", "video_settings/hdr_calibration",
         "Set Peak Brightness and Paper White by eye,\nwith patterns brighter than white.", "desktop"),
    ],
}
# HDR Calibration: the peak, then paper white, each row's pattern drawn by
# the renderer between the rows and the help, at the values shown, which the
# display takes while the screen is open (menu_functions.c's "port hdr
# calibration", d3d8_gl.c's hdr_calibration_draw)
SCREENS["video_settings/hdr_calibration"] = {
    "screen": "hdr_calibration_settings_screen", "header": _video["header"], "spacing": 30,
    "inputs": ["port hdr calibration"],
    "rows": [
        ("PEAK BRIGHTNESS:", "display.hdr_peak_brightness", PEAK_BRIGHTNESSES,
         "Raise it until the ring just disappears into\nthe square: the brightest your display shows.", None),
        ("PAPER WHITE:", "display.hdr_paper_white", PAPER_WHITES,
         "How bright white is (the menus, the HUD, the\ngrey bars), in nits. Choose what looks right.", None),
    ],
}
_video["rows"] = [row for row in _video["rows"] if row[1] not in _graphics | _extra_effects]
_video["categories"] = [
    ("GRAPHICS:", "video_settings/graphics", "The HUD, text, anti-aliasing, shadows,\nlighting and HDR."),
    ("FOV AND VIEWMODELS:", "video_settings/fov_viewmodels",
     "The field of view and the first-person\nweapon. Their defaults keep the stock view."),
    ("PERFORMANCE:", "video_settings/performance",
     "How fast the console runs the game: a steadier\nframe rate, or a cooler console.", "switch"),
    ("EXTRA EFFECTS:", "video_settings/extra_effects",
     "Visual modifiers the stock game does not\nhave, such as the shield glow."),
]
# the Switch's clocks (port/switch/host/host_sdl2.c)
SCREENS["video_settings/performance"] = {
    "screen": "performance_settings_screen", "header": _video["header"], "spacing": 30,
    "rows": [
        ("CPU CLOCK:", "performance.cpu_clock", [("STOCK", "stock"), ("1 GHZ", "1020"), ("1.7 GHZ", "1785")],
         "1.7 GHz is the fastest Nintendo's games use.\n1 GHz and stock run cooler, and the battery lasts.", None),
    ],
}
SCREENS["video_settings/fov_viewmodels"] = {
    "screen": "fov_viewmodel_settings_screen", "header": _video["header"], "spacing": 30,
    "rows": [
        ("FOV:", "display.fov", [("DEFAULT", "0")] + [(str(n), str(n)) for n in range(80, 151, 5)],
         "On foot, horizontal at 16:9, in degrees.\nDefault keeps the stock view.", None),
        ("VIEWMODEL FOV:", "display.viewmodel_fov", [("DEFAULT", "0")] + [(str(n), str(n)) for n in range(80, 151, 5)],
         "The weapon and hands, horizontal at 16:9.\nDefault keeps the weapon's stock view.", None),
        ("VIEWMODELS:", "display.viewmodel_visible", ON_OFF,
         "Draw first-person weapons, hands and attached\nvisuals. Gameplay and sound continue when off.", None),
        ("PASSENGER THIRD PERSON:", "display.passenger_third_person", ON_OFF,
         "Third-person Warthog and Scorpion passengers in\nthe campaign and co-op. Weapon zoom is disabled.", None),
        ("ARM SHIELDS:", "display.viewmodel_shield", ON_OFF,
         "The energy shield's flare on your first-person\narms too, as on your body.", None),
        ("LEGS:", "display.first_person_legs", ON_OFF,
         "Your legs in first person, seen looking down,\nmoving as you run. Your body as others see it.", None),
        ("SHIELD COLOR:", "display.shield_color", [("DEFAULT", "default")] + [(c.upper(), c) for c in SHIELD_COLORS],
         "Your shield's flare in an armor color, whatever\nyour armor. Only you see it. Default: its own.", None),
    ],
}

# Audio Setup's sound processing (port/linux/src/dsound_sdl.c): the reverb
# and the resampler, a screen of their own (a category, as Video Setup's);
# the devices and the volumes stay on Audio Setup
_audio = SCREENS["audio_settings"]
_processing = {"audio.reverb"}
SCREENS["audio_settings/sound_processing"] = {
    "screen": "sound_processing_settings_screen", "header": _audio["header"], "spacing": 30,
    "rows": [row for row in _audio["rows"] if row[1] in _processing] + [
        ("RESAMPLER:", "audio.resampler", [("SINC", "sinc"), ("LINEAR", "linear")],
         "How sounds are converted to the output's rate:\nsinc is clean; linear is brighter, with aliasing.",
         None),
    ],
}
_audio["rows"] = [row for row in _audio["rows"] if row[1] not in _processing]

# (voice chat stays on Audio Setup; the text chat's rows are the multiplayer
# HUD's, on Network Setup)
_audio["categories"] = [
    ("SOUND PROCESSING:", "audio_settings/sound_processing",
     "The reverb, and how sounds are converted to the\noutput's rate."),
]

# Network Setup's map sharing (port/linux/game/map_share.c): this machine's
# part in another's game, a screen of its own (a category, as Video Setup's)
SEED_UPLOADS = (64, 128, 256, 512, 1024, 2048, 4096, 8192, 10240)
_network = SCREENS["network_setup"]
# (and the multiplayer HUD's rows, and streamer mode, screens of their own:
# Network Setup has room for nine places)
_hud_settings = ("display.player_names", "display.player_name_scale", "display.scoreboard_team_layout",
                 "display.scoreboard_background")
_hud_rows = [row for row in _network["rows"] if row[1] in _hud_settings]
_network["rows"] = [row for row in _network["rows"] if row[1] not in _hud_settings]
_network["categories"] = [
    ("MAP SHARING:", "network_setup/map_sharing",
     "Downloading Custom Edition maps you do not have,\nand helping send them to other players.",
     "desktop android ps5"),
    ("MULTIPLAYER HUD:", "network_setup/multiplayer_hud",
     "Players' names above their heads, the\nscoreboard, the text chat and the match clock."),
    ("STREAMER MODE:", "network_setup/streamer_mode",
     "Hide offensive words in names and text from\nother players, maps and mods, as stars."),
    ("SERVER BROWSER:", "network_setup/server_browser",
     "What the Server Browser lists and its order: the\nsame settings as its Filters screen."),
    ("DISCORD ACCOUNT:", "network_setup/discord_account",
     "Link a Discord account, which lets you join\nservers that block VPNs while you use one."),
]
SCREENS["network_setup/map_sharing"] = {
    "screen": "map_sharing_settings_screen", "header": _network["header"], "spacing": 30,
    "rows": [
        ("DOWNLOAD MAPS:", "network.map_download", ON_OFF,
         "Joining a game on a Custom Edition map you do not\nhave: download it from the players (if shared).",
         None),
        ("HELP SEND MAPS:", "network.map_seed", ON_OFF,
         "In another's game, send its map to players who\njoin without it, as a torrent's peers do.", None),
        ("UPLOAD LIMIT:", "network.map_seed_upload",
         [(f"{kb} KB/S" if kb < 1024 else f"{kb // 1024} MB/S", str(kb)) for kb in SEED_UPLOADS],
         "The most you send of maps to other players, so\nthat the game keeps the rest of your upload.", None),
    ],
}

# Gameplay, a screen of its own from Settings (a row of the profile menu)
SCREENS["gameplay"] = {
    "screen": "gameplay_settings_screen",
    "header": ("header_gameplay", f"{PE}/gameplay/header_gameplay"),
    "spacing": 30,
    "rows": [
        ("PLAYER RAGDOLLS:", "game.ragdoll_players",
         [("NONE", "none"), ("WOODY", "woody"), ("HALO 2", "halo2")],
         "How a player's body falls:\nthe animation, a loose ragdoll, or Halo 2's.",
         None),
        ("AI RAGDOLLS:", "game.ragdoll_ai",
         [("NONE", "none"), ("WOODY", "woody"), ("HALO 2", "halo2")],
         "How an AI's body falls:\nthe animation, a loose ragdoll, or Halo 2's.",
         None),
        ("TERMINAL OUTPUT:", "game.terminal_output",
         [("NONE", "none"), ("MENUS ONLY", "menus"), ("ALWAYS", "always")],
         "The game's messages at the bottom left: never,\nin the menus only, or in games too.",
         None),
    ],
}
# Network Setup's server browser (menu_functions.c's lobby_browser_update): where
# its games come from (network.listings, master_list.c), the Filters screen's
# settings and its order; the master servers that answered, shown, not set
# (a row with no values: its value's text, menu_functions.c's
# master_servers_show)
SCREENS["network_setup/server_browser"] = {
    "screen": "server_browser_settings_screen", "header": _network["header"], "spacing": 30,
    "rows": [
        ("LISTS:", "network.listings",
         [("ALL GAMES", "both"), ("DEDICATED", "masters"), ("P2P GAMES", "brokers")],
         "The master servers' dedicated servers, the games\nplayers host (through the brokers), or all of them.", None),
        ("SHOW EMPTY SERVERS:", "network.browser_show_empty", YES_NO, "List games no one is playing yet.", None),
        ("SHOW FULL SERVERS:", "network.browser_show_full", YES_NO, "List games with no room left.", None),
        ("SHOW MISSING MAPS:", "network.browser_maps",
         [("NO", "no"), ("YES", "yes"), ("IF DOWNLOADABLE", "downloadable")],
         "List games on maps you do not have: IF\nDOWNLOADABLE, those Map Sharing can download.", None),
        # (a column title chosen again writes the order reversed, "-players": the row shows its order's word,
        # menu_functions.c's setting_value_index, and writes it again only if another is chosen)
        ("SORT BY:", "network.browser_sort",
         [("DEDICATED FIRST", "servers"), ("PLAYERS", "players"), ("NAME", "name"), ("MAP", "map"),
          ("GAME TYPE", "type"), ("LOCKED FIRST", "locked")],
         "The list's order. Its column titles sort it too;\nchoosing one again reverses it.", None),
        ("MASTER SERVERS:", "network.masters_file", None,
         "The master servers (masters.txt) that answered\nwhen the Server Browser last asked.", None,
         "master_servers"),
    ],
}
# (the read-only row's words, its screen's words: menu_functions.c's
# master_servers_show)
SCREENS["network_setup/server_browser"]["words"] = ["OF", "NONE LISTED", "NOT ASKED YET", "OFF"]

# Network Setup's Discord account (port/linux/src/player_link.c): the account
# linked at the master servers (masters.txt's, up to 4), which a dedicated
# server that blocks VPNs lets in from one when it is listed by the master the
# account is linked with. A row a master: its state; LINK WITH MASTER n opens
# that master's page in the browser (on OK; the master a refusing server needs
# is chosen already), UNLINK ALL forgets the account (link.action is no
# setting of config.toml's: menu_functions.c's setting_write does it). The
# rows' text: menu_functions.c's discord_account_show, its words
SCREENS["network_setup/discord_account"] = {
    "screen": "discord_account_settings_screen", "header": _network["header"], "spacing": 30,
    "wide_values": ["link_master_1", "link_master_2", "link_master_3", "link_master_4"],
    "rows": [
        ("DISCORD ACCOUNT:", "network.link_share", None,
         "The Discord account linked to this game, and with\nhow many of the master servers.", None,
         "discord_account"),
        *[(f"MASTER {n}:", "network.masters_file", None,
           "A master server (masters.txt) and the account\nlinked with it: a server it lists lets it in.", None,
           f"link_master_{n}") for n in range(1, 5)],
        ("LINK:", "link.action",
         [("NO CHANGE", "none")] + [(f"WITH MASTER {n}", f"link_{n}") for n in range(1, 5)] +
         [("UNLINK ALL", "unlink")],
         "WITH MASTER n opens that master's page in the\nbrowser (on OK). UNLINK ALL forgets the account.", None,
         "link_action"),
        ("SHOW TO SERVERS:", "network.link_share",
         [("WHEN ASKED", "when_needed"), ("ALWAYS", "always"), ("NEVER", "never")],
         "Which servers see your Discord account: WHEN\nASKED, those that block VPNs, joined from one.", None),
    ],
    "words": ["NOT LINKED", "LINKED AS", "WAITING: CODE", "REVOKED", "EXPIRED:", "OF", "A SERVER NEEDS IT", "NONE"],
}

# Network Setup's multiplayer HUD: its rows, moved from Network Setup, and the
# text chat's (port/linux/game/network_chat.c)
SCREENS["network_setup/multiplayer_hud"] = {
    "screen": "multiplayer_hud_settings_screen", "header": _network["header"], "spacing": 30,
    "rows": _hud_rows + [
        ("TEXT CHAT:", "display.text_chat", [("SHOW", "true"), ("HIDE", "false")],
         "SHOW the text chat, or HIDE it: no lines, and\nits keys and D-pad Up open nothing.", None),
        ("CHAT TEXT:", "display.chat_style", [("COLORS", "colors"), ("WHITE", "white")],
         "The text chat's colors: COLORS, names in their\nteam's color; WHITE, as Custom Edition.", None),
        ("MATCH TIMER:", "display.match_timer", ON_OFF,
         "Draw a clock at the bottom right in multiplayer,\ncounting up from the start of the game.", None),
    ],
}

# Network Setup's streamer mode (port/linux/game/text_filter.c): words hidden
# in what other players, maps and mods show, each group from a severity of
# Surge AI's on (port/assets/text_filter)
STREAMER_LEVELS = [("OFF", "off"), ("SEVERE", "severe"), ("STRONG AND WORSE", "strong"), ("MILD AND WORSE", "mild")]
SCREENS["network_setup/streamer_mode"] = {
    "screen": "streamer_mode_settings_screen", "header": _network["header"], "spacing": 30,
    "rows": [
        ("STREAMER MODE:", "streamer.enabled", ON_OFF,
         "Hide offensive words in names and text from\nother players, maps and mods, as stars.", None),
        ("CENSOR SLURS:", "streamer.slurs", STREAMER_LEVELS,
         "Slurs about race, sexuality, gender and\ndisability.", None),
        ("CENSOR SWEARING:", "streamer.swearing", STREAMER_LEVELS,
         "Swearing and sexual words (STRONG AND WORSE\nleaves the mildest showing).", None),
        ("CENSOR INSULTS:", "streamer.insults", STREAMER_LEVELS,
         "Name-calling and insults that are not slurs.", None),
    ],
}

# Network Setup in a game (the pause menu's SETTINGS: port/linux/game/
# menu_tags.c's pause_network_patch): its pages alone, without the rows that
# would drop the game (internet play) or are for the main menu (the router,
# the clipboard, the updates)
SCREENS["network_setup/in_game"] = {
    "screen": "in_game_network_settings_screen", "header": _network["header"], "spacing": 30,
    "platform_places": True, "rows": [],
    "categories": [category for category in _network["categories"] if category[1] != "network_setup/server_browser"],
}

# Controls Setup: the keyboard and mouse's actions, in groups (the order of
# port/linux/game/menu_functions.c's table of them)
CONTROL_GROUPS = ["MOVEMENT", "WEAPONS", "ACTIONS", "COMMUNICATION"]
CONTROL_ROWS = 7

# the settings whose rows have the wider spinner (a device's name)
WIDE_SETTINGS = {"audio.output_device", "audio.input_device", "streamer.slurs", "streamer.swearing",
                 "streamer.insults", "network.listings", "network.browser_maps", "network.browser_sort",
                 "profile.button_preset"}

# Teamplay Options' rows of the port's (_teamplay_options_extras): the
# gametype's FORCE TEAM SPAWNS, then the host's voice chat and vote kicks;
# each row's key, its label, its choices' words (menu_functions.c's
# gametype_options give their values) and their helps
TEAMPLAY_EDIT = "main_menu/settings_select/multiplayer_setup/teamplay_options_edit"
VOICE_QUALITIES = (8, 12, 16, 24, 32, 48, 64)
# (the rows' step: closer than the PC's 30px, so that all of Server Setup's
# fit above the help text, which moves down to TEAMPLAY_HELP_TOP)
TEAMPLAY_STEP = 26
TEAMPLAY_HELP_TOP = 351
# (game_engine.c's game_engine_force_team_spawns: the gametype's own, so
# shown in Edit Gametypes' copy too, and not on Server Setup's VOICE OPTIONS)
TEAMPLAY_GAMETYPE_ROWS = [
    ("force_team_spawns", "FORCE TEAM SPAWNS:", ["OFF", "ON"], [
        "Players will spawn at any spawn point the\\ngametype allows.",
        "Players will only spawn at their own team's\\nCapture the Flag spawn points.",
    ]),
]
TEAMPLAY_ROWS = [
    ("voice_mode", "VOICE CHAT:", ["OFF", "TEAM NEAR", "ANYONE NEAR", "TEAM", "TEAM, ENEMIES NEAR"], [
        "No voice chat during the game.",
        "Players hear their teammates who are near them.",
        "Players hear everyone who is near them.",
        "Players hear all their teammates, wherever they\\nare.",
        "Players hear all their teammates, and the enemies\\nwho are near them.",
    ]),
    ("voice_lobby", "LOBBY VOICE CHAT:", ["ON", "OFF"], [
        "Everyone hears everyone in the lobby, before and\\nafter the game.",
        "No voice chat in the lobby.",
    ]),
    ("voice_kbps", "VOICE QUALITY:", [f"{kbps} KBPS" for kbps in VOICE_QUALITIES],
     [f"Voices at {kbps} kilobits a second, in the lobby\\nand in the game." for kbps in VOICE_QUALITIES]),
    ("voice_proximity", "VOICE NEAR DISTANCE:", ["15 M", "30 M", "45 M", "60 M", "90 M", "150 M"],
     [f"Players {metres} metres apart or less are near\\nfor voice chat." for metres in (15, 30, 45, 60, 90, 150)]),
    # (network_votekick.c)
    ("votekick", "VOTE KICK:", ["ON", "OFF"], [
        "Players can vote to kick a player, from the\\nscoreboard.",
        "Players cannot vote to kick anyone.",
    ]),
    # (network_chat.c: one row for the chat and its links, as Teamplay
    # Options has room for one more)
    ("text_chat", "TEXT CHAT:", ["ON", "WITH LINKS", "OFF"], [
        "Players can type to each other; lines with links\\nare refused. Off in a public internet game.",
        "Players can type to each other, links too. Off\\nin a public internet game.",
        "No text chat in the game.",
    ]),
]
# (Teamplay Options' rows of the port's, in order)
TEAMPLAY_EXTRA_ROWS = TEAMPLAY_GAMETYPE_ROWS + TEAMPLAY_ROWS

# the profile menu's words for what its items now open
STRING_OVERRIDES = {
    f"{PE}/profile_edit_descriptions": [
        "Rename this profile.\\n\\n\\nProfile:",
        "Choose the keys and mouse\\nbuttons for each action.\\n\\nProfile:",
        "Set this profile's controller\\nsensitivity and layouts.\\n\\nProfile:",
        "Adjust the mouse's sensitivity\\nand aiming.\\n\\nProfile:",
        "Adjust the volume of the music\\nand of everything else.\\n\\nProfile:",
        "Choose a window or the full\\nscreen, the frame rate and more.\\n\\nProfile:",
        "Internet play, updates and the\\nmultiplayer HUD.\\n\\nProfile:",
        "Change the current profile's\\nfree-for-all multiplayer color.\\n\\nProfile:",
        "How players and the AI fall:\\nthe animation, or a ragdoll.\\n\\nProfile:",
        "Halo: Combat Evolved, the Xbox\\ngame, on this computer.\\n\\nProfile:",
    ],
}


def attributes(pairs: list) -> str:
    return "".join(f" {key}={quoteattr(str(value))}" for key, value in pairs if value is not None)


def _widget(name: str, pairs: list, inner: list) -> list:
    return [f"\t<widget{attributes([('name', name)] + pairs)}>", *[f"\t\t{line}" for line in inner], "\t</widget>"]


def _strings(name: str, texts: list) -> list:
    return [f"\t<strings{attributes([('name', name)])}>",
            *[f"\t\t<string{attributes([('text', text)])}/>" for text in texts], "\t</strings>"]


def _button(name: str, caption: int, handlers: list) -> list:
    pairs = [("type", "text"), ("width", 128), ("height", 24), ("bitmap", "bitmaps/text_button_background"),
             ("string_list", "strings/common_button_captions"), ("string_index", caption), ("font", "ui\\small_ui"),
             ("color", "#FFFFFFFF"), ("align", "center"), ("text_y", 2)]
    return _widget(name, pairs, handlers + ['<on event="left_mouse" run="mouse emit accept event"/>'])


def _screen(folder: str, spec: dict, rows: list, list_inputs: list, list_handlers: list, extra: list) -> list:
    """a screen: its header, the list of rows (with the help line as its
    description), and the buttons"""
    base = folder if folder.startswith("main_menu/") else f"{PE}/{folder}"
    header, header_bitmap = spec["header"]
    lines = []
    # (save_on_leave: B and Cancel write the screen, as OK does, then go back)
    leave = ['<on event="b" run="port settings save" back="true"/>',
             '<on event="back" run="port settings save" back="true"/>'] if spec.get("save_on_leave") else \
        ['<on event="b" back="true"/>', '<on event="back" back="true"/>']
    lines += _widget(f"{base}/{spec['screen']}",
                     [("width", 640), ("height", 480), ("flags", "pass_unhandled_to_focused_child pause_game"),
                      ("bitmap", "bitmaps/gradient")],
                     [*leave,
                      f'<child{attributes([("widget", f"{base}/{header}")])}/>',
                      f'<child{attributes([("widget", f"{base}/options_menu")])}/>',
                      *[f'<child{attributes([("widget", f"{base}/note_{index}"), ("x", 54), ("y", 73 + place * spec["spacing"]), ("platform", platform)])}/>'
                        for index, (_, platform, place) in enumerate(spec.get("notes", []))],
                      *spec.get("screen_children", [])])
    lines += _widget(f"{base}/{header}", [("controller", 1), ("left", 35), ("top", 11), ("width", 605), ("height", 59),
                                          ("bitmap", header_bitmap)], [])
    # (a note is text alone, which the focus never lands on)
    for index, (text, platform, _) in enumerate(spec.get("notes", [])):
        lines += _widget(f"{base}/note_{index}", [("type", "text"), ("width", 512), ("height", 24), ("text", text),
                                                  ("font", "ui\\small_ui"), ("color", "#FF2896FF"), ("text_x", 13),
                                                  ("text_y", 5), ("text_flags", "no_focus_test"),
                                                  ("platform", platform)], [])
    lines += _widget(f"{base}/help", [("type", "text"), ("controller", 1), ("left", 68),
                                      ("top", spec.get("help_top", 350)), ("width", 482),
                                      ("height", spec.get("help_height", 60)), ("string_list", f"{base}/help_strings"),
                                      ("font", "ui\\large_ui"), ("color", "#FFFFFFFF")], [])
    children = [f'<data input="{name}"/>' for name in list_inputs] + list_handlers
    # (each row: its widget, platform, and the place it is in, else the next)
    for index, (row, platform, *place) in enumerate(rows):
        y = 73 + (place[0] if place else index) * spec["spacing"]
        children.append(f'<child{attributes([("widget", row), ("x", 54), ("y", y), ("platform", platform)])}/>')
    children.append(f'<child{attributes([("widget", f"{base}/button_bar"), ("y", 414)])}/>')
    lines += _widget(f"{base}/options_menu",
                     [("type", "column_list"), ("width", 640), ("height", 480),
                      ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                      ("description", f"{base}/help")], children)
    lines += _widget(f"{base}/button_bar",
                     [("type", "column_list"), ("width", 640), ("height", 28),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     [f'<child{attributes([("widget", f"{base}/button_defaults"), ("y", 1)])}/>',
                      *[f'<child{attributes([("widget", f"{base}/{key}"), ("x", x), ("y", 1), ("platform", platform)])}/>'
                        for key, _, _, platform, x in spec.get("bar_buttons", [])],
                      f'<child{attributes([("widget", f"{base}/button_ok"), ("x", 380), ("y", 1)])}/>',
                      '<child widget="common_button_cancel" x="510" y="1"/>'])
    for key, words, function, _, _ in spec.get("bar_buttons", []):
        lines += _text_button(f"{base}/{key}", words, [f'<on event="a" run="{function}"/>',
                                                       f'<on event="start" run="{function}"/>'])
    lines += extra
    return lines


def _placed(row: str, platform, members: list, places: dict) -> list:
    """a row's children (row, platform, place): one for its platforms where
    they have it in the same place, else one for each place, with the
    platforms whose place it is"""
    groups = {}
    for name in members:
        groups.setdefault(places[name], []).append(name)
    if len(groups) == 1:
        return [(row, platform, places[members[0]])]
    return [(row, " ".join(names) if len(names) < len(places) else None, place) for place, names in groups.items()]


def _setting_screen(folder: str, spec: dict) -> list:
    base = f"{PE}/{folder}"
    rows, extra = [], []
    place = -1
    # (platform_places: each platform's rows in places of their own, with
    # no gap where the others' are; a row in one place for all its
    # platforms, else a child for each place. The Switch's are its own: a row
    # for "android" is the Android app's, port/linux/src/menu_files.c)
    places = {"desktop": -1, "android": -1, "switch": -1, "ps5": -1, "web": -1}
    for index, (label, setting, choices, _, platform, *named) in enumerate(spec["rows"]):
        key = named[0] if named else setting.split(".", 1)[1]
        row = f"{base}/op_{key}"
        shares = setting in spec.get("same_place", ()) or key in spec.get("same_place", ())
        if spec.get("platform_places"):
            members = platform.split() if platform else list(places)
            for name in members:
                if not shares:
                    places[name] += 1
            rows += _placed(row, platform, members, places)
        else:
            if setting not in spec.get("same_place", ()) and key not in spec.get("same_place", ()):
                place += 1
            rows.append((row, platform, place))
        # (a row with no values shows its value, set by the game: its text; a
        # wide one, a short label and a long value in the small font)
        if choices is None:
            wide_value = key in spec.get("wide_values", ())
            label_width, value_x, value_width = (130, 130, 382) if wide_value else (300, 286, 212)
            extra += _widget(row, [("width", 512), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                                   ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF"), ("platform", platform)],
                             [f'<child{attributes([("widget", f"{base}/{key}_label")])}/>',
                              f'<child{attributes([("widget", f"{base}/{key}_value"), ("x", value_x), ("y", 1)])}/>'])
            extra += _widget(f"{base}/{key}_label",
                             [("type", "text"), ("controller", 1), ("width", label_width), ("height", 22),
                              ("string_list", f"{base}/labels"), ("string_index", index), ("font", "ui\\large_ui"),
                              ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
            extra += _widget(f"{base}/{key}_value",
                             [("type", "text"), ("controller", 1), ("width", value_width), ("height", 22),
                              ("font", "ui\\small_ui" if wide_value else "ui\\large_ui"), ("color", "#FF2896FF"),
                              ("align", "left" if wide_value else "center"), ("text_y", 5 if wide_value else 1),
                              ("text_flags", "no_focus_test")], [])
            continue
        # (a device's name wants the wider spinner Server Setup's co-op rows
        # have: WIDE_SETTINGS)
        wide = setting in WIDE_SETTINGS
        extra += _widget(row, [("width", 512), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                               ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF"), ("platform", platform)],
                         [f'<child{attributes([("widget", f"{base}/{key}_label")])}/>',
                          f'<child{attributes([("widget", f"{base}/{key}_spinner"), ("x", 286 if wide else 320), ("y", 1)])}/>'])
        extra += _widget(f"{base}/{key}_label",
                         [("type", "text"), ("controller", 1), ("width", 300), ("height", 22),
                          ("string_list", f"{base}/labels"), ("string_index", index), ("font", "ui\\large_ui"),
                          ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
        extra += _widget(f"{base}/{key}_spinner",
                         [("type", "spinner"), ("left", None if wide else 3), ("top", 2), ("width", 206 if wide else 147),
                          ("height", 20), ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
                          ("strings", "|".join(shown for shown, _ in choices)), ("setting", setting),
                          ("values", "|".join(value for _, value in choices)), ("font", "ui\\large_ui"),
                          ("color", "#FF2896FF"), ("align", "center"), ("text_y", 4 if wide else 1),
                          ("header_bitmap", "bitmaps/arrow_sm_left"), ("footer_bitmap", "bitmaps/arrow_sm_right"),
                          ("header_bounds", "7 -13 19 -7" if wide else "7 -6 19 0"),
                          ("footer_bounds", "7 208 19 214" if wide else "7 150 19 156")],
                         ['<on event="created" run="port setting load"/>'] +
                         (['<on event="deleted" run="port setting save"/>'] if spec.get("save_on_leave") else []))
    # (each category's row: its label, the screen it opens, its help, and the
    # platforms it is for, as a row's: every one's when it names none)
    for index, (label, category_folder, _, *category_platform) in enumerate(spec.get("categories", ())):
        key = category_folder.rsplit("/", 1)[-1]
        row = f"{base}/op_{key}"
        target = f"{PE}/{category_folder}/{SCREENS[category_folder]['screen']}"
        platform = category_platform[0] if category_platform else None
        if spec.get("platform_places"):
            members = platform.split() if platform else list(places)
            for name in members:
                places[name] += 1
            rows += _placed(row, platform, members, places)
        else:
            rows.append((row, platform, place + 1 + index))
        extra += _widget(row, [("width", 512), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                               ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF"), ("platform", platform)],
                         [f'<on event="a" open="{target}"/>', f'<on event="start" open="{target}"/>',
                          '<on event="left_mouse" run="mouse emit accept event"/>',
                          f'<child widget="{base}/{key}_label"/>'])
        extra += _widget(f"{base}/{key}_label",
                         [("type", "text"), ("controller", 1), ("width", 512), ("height", 22),
                          ("string_list", f"{base}/labels"), ("string_index", len(spec["rows"]) + index),
                          ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
    extra += _button(f"{base}/button_defaults", 3, ['<on event="a" run="port settings defaults"/>',
                                                    '<on event="start" run="port settings defaults"/>'])
    extra += _button(f"{base}/button_ok", 1, ['<on event="a" run="port settings save" back="true"/>',
                                              '<on event="start" run="port settings save" back="true"/>'])
    extra += _strings(f"{base}/labels", [label for label, *_ in spec["rows"]] +
                      [label for label, *_ in spec.get("categories", ())])
    if spec.get("words"):
        extra += _strings(f"{base}/words", spec["words"])
    # (the help of the row whose label is string n is n + 1: the buttons' is 0)
    extra += _strings(f"{base}/help_strings",
                      [""] + [help_text.replace("\n", "\\n") for _, _, _, help_text, *_ in spec["rows"]] +
                      [help_text.replace("\n", "\\n") for _, _, help_text, *_ in spec.get("categories", ())])
    # (a note's place None: the one after its platform's last row)
    if spec.get("notes"):
        spec = dict(spec, notes=[(text, platform, places[platform] + 1 if place is None else place)
                                 for text, platform, place in spec["notes"]])
    return _screen(folder, spec, rows, ["port settings help", *spec.get("inputs", ())], [], extra)


def _controls_screen() -> list:
    folder = "controls_setup"
    base = f"{PE}/{folder}"
    spec = {"screen": "controls_settings_screen", "spacing": 30,
            "header": ("header_profile_controls_settings", f"{base}/header_profile_controls_settings")}
    rows, extra = [(f"{base}/op_group", None)], []
    extra += _widget(f"{base}/op_group", [("width", 512), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                                          ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                     [f'<child{attributes([("widget", f"{base}/group_label")])}/>',
                      f'<child{attributes([("widget", f"{base}/group_spinner"), ("x", 200), ("y", 1)])}/>'])
    extra += _widget(f"{base}/group_label",
                     [("type", "text"), ("controller", 1), ("width", 200), ("height", 22), ("text", "SHOW:"),
                      ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
    extra += _widget(f"{base}/group_spinner",
                     [("type", "spinner"), ("left", 3), ("top", 2), ("width", 297), ("height", 20),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
                      ("strings", "|".join(CONTROL_GROUPS)), ("font", "ui\\large_ui"), ("color", "#FF2896FF"),
                      ("align", "center"), ("text_y", 1), ("header_bitmap", "bitmaps/arrow_sm_left"),
                      ("footer_bitmap", "bitmaps/arrow_sm_right"), ("header_bounds", "7 -6 19 0"),
                      ("footer_bounds", "7 300 19 306")], [])
    for index in range(CONTROL_ROWS):
        row = f"{base}/op_command_{index + 1}"
        rows.append((row, None))
        extra += _widget(row, [("width", 512), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                               ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                         ['<on event="a" run="controls begin binding"/>',
                          '<on event="start" run="controls begin binding"/>',
                          '<on event="left right" run="controls binding slot"/>',
                          f'<child{attributes([("widget", f"{base}/command_label")])}/>',
                          f'<child{attributes([("widget", f"{base}/command_binding"), ("x", 200)])}/>',
                          f'<child{attributes([("widget", f"{base}/command_binding"), ("x", 356)])}/>'])
    extra += _widget(f"{base}/command_label",
                     [("type", "text"), ("controller", 1), ("width", 200), ("height", 22), ("font", "ui\\large_ui"),
                      ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
    extra += _widget(f"{base}/command_binding",
                     [("type", "text"), ("controller", 1), ("width", 152), ("height", 22), ("font", "ui\\small_ui"),
                      ("color", "#FF2896FF"), ("align", "center"), ("text_y", 5), ("text_flags", "no_focus_test")],
                     [])
    extra += _button(f"{base}/button_defaults", 3, ['<on event="a" run="controls screen defaults"/>',
                                                    '<on event="start" run="controls screen defaults"/>'])
    extra += _button(f"{base}/button_ok", 1, ['<on event="a" run="controls screen change set" back="true"/>',
                                              '<on event="start" run="controls screen change set" back="true"/>'])
    extra += _strings(f"{base}/help_strings", [
        "Choose what to show, with left and right.",
        "Enter sets the key chosen; left and right choose\\nwhich of two. Then press the new key or button.",
        "Press a key, a mouse button or turn the wheel...\\nEscape or a controller's B cancels, Delete clears it.",
    ])
    lines = _screen(folder, spec, rows, ["controls update menu"], ['<on event="created" run="controls screen init"/>'],
                    extra)
    # (B cancels a key's setting first)
    return [line.replace('<on event="b" back="true"/>', '<on event="b" run="controls back handler"/>')
            .replace('<on event="back" back="true"/>', '<on event="back" run="controls back handler"/>')
            for line in lines]


def _gameplay_profile_item() -> list:
    """Settings' Gameplay row, before About: the screen is this folder's"""
    name = f"{PE}/gameplay_profile_item"
    return _widget(name, [("type", "text"), ("left", 51), ("top", profile_row_top("gameplay_profile_item")), ("width", 232), ("height", 32),
                          ("bitmap", "bitmaps/list_item_bkd"), ("text", "GAMEPLAY"), ("font", "ui\\large_ui"),
                          ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 5)],
                   [f'<on event="a" open="{PE}/gameplay/gameplay_settings_screen"/>',
                    f'<on event="start" open="{PE}/gameplay/gameplay_settings_screen"/>',
                    '<on event="left_mouse" run="mouse emit accept event"/>'])


def layout_overlay_places(controller: str, size: str) -> tuple:
    """where an overlay's picture of the controller is (left, top, width,
    height), its picture (the controller and the labels' lines: the same
    for every layout) and each label's line's middle: {button: (x, y)},
    all in the menu's units"""
    spec, overlay = CONTROLLERS[controller], LAYOUT_OVERLAYS[size]
    left, top, width, height = overlay["area"]
    picture_width = overlay["width"]
    picture_height = picture_width / spec["aspect"]
    x0 = left + (width - picture_width) / 2 + overlay.get("shift", 0)
    y0 = top + (height - picture_height) / 2
    anchors = {button: (x0 + x * picture_width, y0 + y * picture_height) for button, (x, y) in spec["buttons"].items()}
    # (the controller alone: no labels, so no lines)
    if not overlay.get("labels", True):
        return (x0 - 8, top, picture_width + 16, height), anchors, {}
    line = overlay["line"]
    # (those above it, just above it: Back's ending left of its middle,
    # Start's from right of it)
    middle = x0 + picture_width / 2
    tops = spec.get("top", ())
    middles = {button: (middle - 4 if index == 0 else middle + 4, max(top + line / 2, y0 - line))
               for index, button in enumerate(tops)}
    for side, labels in LAYOUT_LABELS.items():
        labels = {button: count for button, count in labels.items() if button not in tops}
        order = sorted(labels, key=lambda button: anchors[button][1])
        # (each as near its button's height as the ones above leave it,
        # then those that would go past the bottom moved up)
        ys = []
        for index, button in enumerate(order):
            least = top + line / 2 if not index else ys[-1] + labels[order[index - 1]] * line + 2
            ys.append(max(anchors[button][1], least))
        for index in reversed(range(len(order))):
            most = top + height - (labels[order[index]] - 0.5) * line if index == len(order) - 1 else \
                ys[index + 1] - labels[order[index]] * line - 2
            ys[index] = min(ys[index], most)
        edge = x0 - 6 if side == "left" else x0 + picture_width + 6
        middles.update({button: (edge, y) for button, y in zip(order, ys)})
    return (x0 - 8, top, picture_width + 16, height), anchors, middles


def layout_overlay_svg(controller: str, size: str) -> str:
    """an overlay's picture: the controller (port_svg/controllers' copy),
    tinted, and from each label's place a line to its button"""
    (left, top, width, height), anchors, middles = layout_overlay_places(controller, size)
    overlay = LAYOUT_OVERLAYS[size]
    picture_width = overlay["width"]
    picture_height = picture_width / CONTROLLERS[controller]["aspect"]
    dark, light = LAYOUT_TINT
    matrix = " ".join(" ".join(f"{(high - low) * weight:.4f}" for weight in (0.2126, 0.7152, 0.0722)) + f" 0 {low:.4f}"
                      for low, high in zip(dark, light)) + " 0 0 0 1 0"
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
             f'width="{width:g}" height="{height:g}" viewBox="{left:g} {top:g} {width:g} {height:g}">',
             '  <defs><filter id="tint" color-interpolation-filters="sRGB">'
             f'<feColorMatrix type="matrix" values="{matrix}"/></filter></defs>',
             f'  <image xlink:href="{controller}.svg" href="{controller}.svg" x="{left + 8:.2f}" '
             f'y="{top + (height - picture_height) / 2:.2f}" width="{picture_width:g}" height="{picture_height:.2f}" '
             'preserveAspectRatio="none" filter="url(#tint)"/>']
    for button, (x, y) in sorted(middles.items()):
        ax, ay = anchors[button]
        # (from below a label above the controller)
        if button in CONTROLLERS[controller].get("top", ()):
            y += overlay["line"] / 2
        lines.append(f'  <line x1="{x:.2f}" y1="{y:.2f}" x2="{ax:.2f}" y2="{ay:.2f}" stroke="{LAYOUT_LINE_COLOR}" '
                     'stroke-width="0.75" stroke-linecap="round"/>')
        lines.append(f'  <circle cx="{ax:.2f}" cy="{ay:.2f}" r="1.75" fill="#FFFFFF" stroke="{LAYOUT_LINE_COLOR}" '
                     'stroke-width="0.75"/>')
    lines.append("</svg>")
    return "\n".join(lines) + "\n"


def browser_hint_pictures() -> list:
    """the server browser's shoulder pictures (BROWSER_HINT_PICTURES):
    Kenney's, as large as a hint's: (name, frames), as layout_overlay_pictures"""
    sources = {"hints/switch_l": "switch/kenney/switch_button_l", "hints/switch_r": "switch/kenney/switch_button_r",
               "hints/ps5_l1": "ps5/kenney/playstation_trigger_l1", "hints/ps5_r1": "ps5/kenney/playstation_trigger_r1",
               "hints/switch_dpad": "switch/kenney/switch_dpad_all", "hints/ps5_dpad": "ps5/kenney/playstation_dpad_all"}
    pictures = []
    for name, source in sources.items():
        text = (Path(__file__).resolve().parents[1] / "port/assets/buttons/platforms" / f"{source}.svg").read_text()
        text = text.replace('<svg width="64" height="64"', '<svg width="14" height="14" viewBox="0 0 64 64"', 1)
        pictures.append((name, [(text, 14, 14)]))
    return pictures


def layout_overlay_pictures() -> list:
    """the overlays' bitmaps (tools/ce_menus.py draws them): (name, frames)"""
    pictures = []
    for controller in CONTROLLERS:
        for size in LAYOUT_OVERLAYS:
            (_, _, width, height), _, _ = layout_overlay_places(controller, size)
            pictures.append((f"controllers/{controller}_{size}",
                             [(layout_overlay_svg(controller, size), round(width), round(height))]))
    return pictures


def _layout_overlays() -> list:
    """the overlays, a picture of each platform's controller and its labels,
    which menu_functions.c's gamepad_layout_overlay fills in: BUTTON
    LAYOUT's, and the profile settings' controller alone (Edit Profile
    Settings' picture's children: WIDGET_PATCHES)"""
    lines = []
    for controller, spec in CONTROLLERS.items():
        for size, overlay in LAYOUT_OVERLAYS.items():
            name = f"{LAYOUT}/{controller}/{size}"
            (left, top, width, height), _, middles = layout_overlay_places(controller, size)
            labels = {button: count for side in LAYOUT_LABELS.values() for button, count in side.items()}
            # (the controller alone needs nothing filled in)
            children = ['<data input="port gamepad layout overlay"/>'] if middles else []
            for button, (x, y) in sorted(middles.items()):
                right = x < left + width / 2
                label_width = overlay["label_width"]["left" if right else "right"]
                label_x = x - 2 - label_width if right else x + 2
                label_y = y - overlay["line"] / 2 - overlay["text_y"]
                children.append(f'<child{attributes([("widget", f"{name}/label_{button}"), ("x", round(label_x)), ("y", round(label_y))])}/>')
                lines += _widget(f"{name}/label_{button}",
                                 [("type", "text"), ("width", label_width),
                                  # (a label of two lines a line taller: the
                                  # font's lines are further apart than the
                                  # labels' places, and the second was cut)
                                  ("height", (2 * labels[button] - 1) * overlay["line"] + 2 * overlay["text_y"]),
                                  ("font", overlay["font"]), ("color", "#FF2896FF"),
                                  ("align", "right" if right else "left"), ("text_y", overlay["text_y"]),
                                  ("text_flags", "no_focus_test")], [])
            lines += _widget(name, [("left", round(left)), ("top", round(top)), ("width", round(width)),
                                    ("height", round(height)), ("bitmap", f"controllers/{controller}_{size}"),
                                    ("platform", spec["platform"])], children)
    lines += _strings(f"{LAYOUT}/words", LAYOUT_WORDS)
    return lines


def settings_files() -> dict:
    """the files of the port's screens, by their names in ce/"""
    files = {}
    for folder, spec in SCREENS.items():
        lines = _setting_screen(folder, spec)
        if folder == "gameplay":
            lines += _gameplay_profile_item()
        if folder == "gamepad_setup/layout":
            lines += _layout_overlays()
        files[f"{PE}/{folder}".replace("/", ".") + ".xml"] = lines
    files[f"{PE}/controls_setup".replace("/", ".") + ".xml"] = _controls_screen()
    return {name: ['<?xml version="1.0" encoding="UTF-8"?>',
                   "<!-- The port's settings screen, in the PC version's style (tools/port_settings.py) -->",
                   "<menus>", *lines, "</menus>", ""] for name, lines in files.items()}


def replaced(folder: str) -> bool:
    """whether the PC version's folder of widgets is replaced by ours (and
    those below it: the controls' advanced screen, the video test)"""
    return any(folder == path or folder.startswith(f"{path}/")
               for path in [*(f"{PE}/{name}" for name in [*SCREENS, "controls_setup"]), *REPLACED_FOLDERS])


# ---------- multiplayer (PLAN.md section 11)

MT = "main_menu/multiplayer_type_select"

# strings added to the PC version's lists: (at, strings) of each list. Slayer's
# kills to win goes up to 500, for big games (the Xbox editor's are
# source/interface/ui_widget.c's kills_to_win_extra_strings): the values,
# and their helps after the five of its own (its helps are the rows' values'
# in turn: menu_functions.c's gametype_option_help)
SLAYER_EDIT = "main_menu/settings_select/multiplayer_setup/playlist_edit/slayer_edit"
PLAYER_EDIT = "main_menu/settings_select/multiplayer_setup/player_options_edit"
# (its rows, the PC version's, in order: ce_menus.py moves them 27 apart)
PLAYER_OPTIONS_ROWS = ['number_of_lives', 'maximum_health', 'shields', 'respawn_time', 'respawn_time_growth', 'odd_man_out', 'invisible_players', 'suicide_penalty']
# the vehicle sets' PC set (game_engine.h's VARIANT_VEHICLE_SET_PC), between
# GUN TURRETS and CUSTOM (menu_functions.c's VEHICLE_PRESET_PC), and its help
# among the sets' (its rows' values' in turn: gametype_option_help)
VEHICLE_EDIT = "main_menu/settings_select/multiplayer_setup/vehicle_options_edit"
AIM_ASSIST_HELP = [
    "Players keep their current mouse aim assist setting.",
    "Disable mouse aim assist for mouse/keyboard players.\\nController and touch aim assist are unchanged.",
]
# the host's shield effects, for every player of a network game
# (network_shield_effects.c): their words, then their helps
SHIELD_EFFECTS = ['STOCK', 'GLOW', 'LIGHT SHOW', 'OVERSHIELD', 'GLOW+OVERSHIELD', 'SHOW+OVERSHIELD',
                  "PLAYERS' OWN"]
SHIELD_EFFECTS_HELP = [
    "Everyone sees the stock game's shields and\\novershield, whatever their own settings.",
    "Everyone's shields light their surroundings as\\nthey flare.",
    "Everyone's shields light their surroundings as\\nthey flare, three times as far (Light Show).",
    "For everyone, the overshield rises up the body\\nand stays lit, in its own color.",
    "For everyone, shields light their surroundings,\\nand the overshield rises and stays lit.",
    "For everyone, Light Show's shield glow, and the\\novershield rising and staying lit.",
    "Not synced: each player sees the shield effects\\nof their own Video Setup settings.",
]

# OpenCE's gametypes' options (port/linux/game/game_modes.c), a screen of the
# gametype editor's (menu_functions.c's "mp profile init opence options"): the
# mode and Hardcore, then the mode's own rows, which menu_functions.c's
# gametype_option_help shows (op_inf_ for Infection, op_gg_ for Gun Game)
# below them; a row's values are menu_functions.c's gametype_options'
PLAYLIST_EDIT = "main_menu/settings_select/multiplayer_setup/playlist_edit"
OPENCE_EDIT = "main_menu/settings_select/multiplayer_setup/opence_options_edit"
OPENCE_ROWS = [
    ("oce_mode", "MODE:", ["NONE", "SWAT", "INFECTION", "GUN GAME"], [
        "The game type's own rules.",
        "No shields, and a shot to the head kills. Slayer only.",
        "Humans killed join the zombies, who win when\\nno human is left. Team Slayer only.",
        "Each kill gives the next weapon; a kill with the\\nlast wins. Free for all Slayer only."]),
    ("oce_hardcore", "HARDCORE:", ["OFF", "ON"], [
        "The motion sensor and aim assist as the game\\ntype has them.",
        "No motion sensor, no aim assist, and a kill\\nrestores the killer's health and shields."]),
    ("inf_zombies", "FIRST ZOMBIES:", ["1", "2", "3", "4", "8", "16"],
     ["The zombies chosen when the round starts\\n(a human is always left)."] * 6),
    ("inf_speed", "ZOMBIE SPEED:", ["100%", "110%", "125%", "150%", "175%", "200%"],
     ["How fast zombies move, of a human's speed."] * 6),
    ("inf_damage", "SKULL DAMAGE:", ["100%", "150%", "200%", "250%"],
     ["The damage of a zombie's skull's melee."] * 4),
    ("inf_respawn", "ZOMBIE RESPAWN:", ["1 SECOND", "3 SECONDS", "5 SECONDS", "10 SECONDS", "15 SECONDS",
                                        "30 SECONDS"],
     ["How long a dead zombie waits to respawn."] * 6),
    ("inf_countdown", "COUNTDOWN:", ["NONE", "5 SECONDS", "10 SECONDS", "15 SECONDS", "30 SECONDS"],
     ["How long the round waits, once two players are\\nalive, before the first zombies are chosen."] * 5),
    ("inf_shields", "ZOMBIE SHIELDS:", ["OFF", "ON"],
     ["Zombies have no shields.", "Zombies have shields, as humans do."]),
    ("gg_order", "WEAPON ORDER:", ["STRONGEST FIRST", "WEAKEST FIRST", "RANDOM"], [
        "From the rocket launcher to the pistol.", "From the pistol to the rocket launcher.",
        "In an order the host draws for each game."]),
    ("gg_melee", "MELEE DEMOTES:", ["OFF", "ON"], [
        "A melee kill takes nothing from the victim.",
        "A melee kill takes the victim back a weapon."]),
]
OPENCE_ITEM = f"{PLAYLIST_EDIT}/playlist_edit_opence_item"

STRING_INSERTS = {
    # (Filters' SHOW MISSING MAPS' third value's help, after the PC version's:
    # menu_functions.c's filters_help)
    f"{MT}/join_game/filters/cap_filter_options": [(11, [
        "Show servers on maps you do not have if their\\nhost shares them: they download as you join."])],
    # (the gametype editor's OPENCE OPTIONS' words, after the PC version's:
    # menu_functions.c's playlist_edit_description)
    f"{PLAYLIST_EDIT}/playlist_edit_list_extended_dsc_text": [(8, [
        "Choose the rules of OpenCE's\\ngametypes: SWAT, Infection,\\nGun Game and Hardcore.\\nThis gametype:"])],
    f"{PLAYER_EDIT}/player_options_labels": [(8, ["AIM ASSIST:", "SHIELD EFFECTS:"])],
    f"{PLAYER_EDIT}/cap_player_options": [(28, AIM_ASSIST_HELP + SHIELD_EFFECTS_HELP)],
    # (Teamplay Options' voice chat and vote kick rows, after its own:
    # TEAMPLAY_ROWS)
    f"{TEAMPLAY_EDIT}/teamplay_options_labels": [(3, [label for _, label, *_ in TEAMPLAY_EXTRA_ROWS])],
    f"{TEAMPLAY_EDIT}/cap_teamplay_options": [(10, [text for *_, helps in TEAMPLAY_EXTRA_ROWS for text in helps])],
    f"{SLAYER_EDIT}/var_kills_to_win": [(5, ["75", "100", "150", "200", "250", "500"])],
    f"{VEHICLE_EDIT}/var_vehicle_set": [(8, ["PC"])],
    f"{VEHICLE_EDIT}/cap_vehicle_options": [(17, [
        "Every vehicle the map places will spawn, as in\\nHalo PC.",
    ])],
    f"{SLAYER_EDIT}/cap_slayer": [(11, [
        "Seventy-five kills to win. Settle in for a long\\nfight.",
        "A hundred kills to win. Made for big games.",
        "A hundred and fifty kills to win. Only a crowded\\nserver gets there.",
        "Two hundred kills to win. Bring friends. Lots of\\nthem.",
        "Two hundred and fifty kills to win.",
        "Five hundred kills to win. You'll be here a while.",
    ])],
}


# a custom loadout's weapons (game_engine.h's _loadout_weapon_*)
LOADOUT_WEAPONS = ["NONE", "RANDOM", "ASSAULT RIFLE", "PISTOL", "SHOTGUN", "SNIPER RIFLE", "ROCKET LAUNCHER",
                   "PLASMA PISTOL", "PLASMA RIFLE", "NEEDLER"]


def LOADOUT_HELPS(slot: str) -> list:
    """the helps of a custom loadout's weapon's values"""
    helps = [f"No {slot} weapon.", f"A random {slot} weapon, of those the map has,\\neach time you spawn."]
    for weapon in LOADOUT_WEAPONS[2:]:
        helps.append(f"Everyone spawns with a {weapon.lower()} as their {slot}\\nweapon (where the map has one).")
    return helps


STRING_OVERRIDES.update({
    f"{MT}/multiplayer_options": ["JOIN GAME", "CREATE GAME", "INTERNET", "LAN", "DIRECT LINK", "EDIT GAMETYPES",
                                  "SERVER BROWSER", "CO-OP CAMPAIGN"],
    f"{MT}/multiplayer_option_descriptions": [
        "Browse the games on the\\nInternet.",
        "Join a multiplayer game on\\nyour LAN.",
        "Join a game by its invite link,\\nor one a Discord invite\\nreached.",
        "Host a game on the Internet:\\nplayers join by its invite\\nlink or Discord.",
        "Host a game on your LAN\\nonly.",
        "Play the campaign with a\\nfriend in split screen:\\nplayer 2 on a gamepad.",
        "Set all the attributes for your \\nmultiplayer gametypes and\\nkeep them for future use.",
    ],
    # (and the port's OPENCE: OpenCE's gametypes, port/linux/game/game_modes.c)
    "main_menu/gametype_select/var_gametype_banks": ["STANDARD", "OPENCE", "CUSTOM"],
    # (the PC's weapon sets, then the Xbox's NO GRENADES: menu_functions.c's
    # gametype_options maps them to the engine's)
    "main_menu/settings_select/multiplayer_setup/item_options_edit/item_options_labels": ["INFINITE GRENADES:", "WEAPON SET:", "VEHICLE SET:", "STARTING EQUIPMENT:",
                                  "VEHICLE RESPAWN TIME:", "LOADOUT:", "PRIMARY WEAPON:", "SECONDARY WEAPON:",
                                  "MAP WEAPONS:"],
    # (the helps of each option's values in turn: menu_functions.c's
    # gametype_option_help)
    "main_menu/settings_select/multiplayer_setup/item_options_edit/cap_item_options": [
        "Everyone has a limitless supply of grenades, which\\nmakes for loud, explosive and occasionally messy fun.",
        "Each player can only carry up to 4 grenades of each\\ntype at any given time.",
        "The map will contain whatever weapons the designers\\nplaced on it.",
        "All the weapons on the map will be replaced by\\npistols.",
        "All the weapons on the map will be replaced by\\nassault rifles and plasma rifles.",
        "All the weapons on the map will be replaced by\\nplasma weapons.",
        "The guns on the map will be replaced by weapons\\nwith sniper scopes.",
        "Sniper rifles and pistols will not appear on the map.",
        "All the weapons on the map will be replaced by\\nrocket launchers.",
        "All the weapons on the map will be replaced by\\nshotguns.",
        "All the weapons on the map are only effective at\\nshort ranges.",
        "There are no weapons on the map that were made in\\nCovenant sweat shops.",
        "Only weapons made by the Covenant will appear\\nin the map.",
        "The flamethrower and fuel rod gun will not appear\\nin the map.",
        "Only the most devastating and dangerous of weapons\\nwill appear in the map.",
        "The map's weapons, without any grenades.",
        "You will start the game with the weapon set that the \\nmap designers custom tailored for it.",
        "Use the generic starting weapons across all maps.",
        "The map's weapons appear where its designers\\nplaced them.",
        "No weapons appear on the map: with a loadout of\\nNONE and NONE, melee and grenades only.",
        "The weapons follow the weapon set above.",
        "Everyone starts with the primary and secondary\\nweapons below; the map's weapons are its own.",
        *LOADOUT_HELPS("primary"),
        *LOADOUT_HELPS("secondary"),
    ],
    "main_menu/settings_select/multiplayer_setup/item_options_edit/var_weapon_set": [
        "NORMAL", "PISTOLS", "RIFLES", "PLASMA WEAPONS", "SNIPER", "NO SNIPING", "ROCKET LAUNCHERS", "SHOTGUNS",
        "SHORT RANGE", "HUMAN", "COVENANT", "CLASSIC", "HEAVY WEAPONS", "NO GRENADES"],
})

# which of STRING_OVERRIDES' strings say what one of the PC version's own
# says: its index in the PC version's list, or None (the port's own). A map
# in another language than English has those in its language (map=,
# menu_tags.c's map_strings); the inserted strings (STRING_INSERTS) are the
# port's own, the rest of their lists the PC version's
OVERRIDE_ORIGINALS = {
    f"{PE}/profile_edit_descriptions": [0, None, None, None, None, None, None, 7, None],
    f"{MT}/multiplayer_options": [0, 1, 2, 3, None, 5, None, None],
    f"{MT}/multiplayer_option_descriptions": [0, 1, None, None, 4, None, 5],
    "main_menu/gametype_select/var_gametype_banks": [0, None, 2],
    "main_menu/settings_select/multiplayer_setup/item_options_edit/item_options_labels": [0, 1, 2, 3, 4] + [None] * 4,
    # (the first's note that infinite grenades work only with 4 players or
    # fewer is left out: not so here)
    "main_menu/settings_select/multiplayer_setup/item_options_edit/cap_item_options":
        [None] + list(range(1, 15)) + [None, 15, 16] + [None] * 24,
    "main_menu/settings_select/multiplayer_setup/item_options_edit/var_weapon_set": list(range(13)) + [None],
}

# the map lists' first row: SINGLEPLAYER, MULTIPLAYER, CUSTOM SINGLEPLAYER or
# CUSTOM MULTIPLAYER maps (_map_kind)
MAP_KIND_CHOOSER = "main_menu/new_select/list_item_0_map_kind"
# the map lists' IMPORT (Android's: New Game's and the Map screen's button
# bars, left of OK): the app's file picker, which copies .map files, and the
# maps in .zip files, into custom_maps (menu_functions.c's map_import_start,
# HaloActivity.importMaps)
MAP_IMPORT = "main_menu/new_select/button_map_import"

# the main menu's news: the latest release's notes (tools/embed_assets.py's
# news_text) in a window the main menu's X opens (WIDGET_PATCHES), its lines
# filled and scrolled by menu_functions.c's news screen ("port news ...")
NEWS = "main_menu/news"
NEWS_ROWS = 12
# the window's panel (bitmaps/controller_border, drawn a texel a unit from
# its widget's corner: the panel is its texels 49 to 556 across and 104 to
# 349 down), placed in the middle of the screen
NEWS_PANEL = (17, 14)


# changes to the PC version's widgets (by our names): attributes set, all
# their handlers replaced, children added, game data inputs added
# the Switch's and the PS5's server browser buttons as hints, a widget each
# (_join_game_extras), and where they are in the button bar's place; the
# mode's own shown (menu_functions.c's browser_list_show: LAN's and Direct
# Link's join and go back alone)
BROWSER_HINTS = [("browser_hint_join", "%a-button JOIN GAME"), ("browser_hint_refresh", "%y-button REFRESH"),
                 ("browser_hint_filters", "%x-button FILTERS"), ("browser_hint_mode", "MODE"),
                 ("browser_hint_sort", "SORT"), ("browser_hint_back", "%b-button BACK")]
# (the shoulders' pictures before MODE, Kenney's: the menus' text has the
# Xbox's black and white alone; port/assets/menus/ce/port/hints)
BROWSER_HINT_PICTURES = [("browser_hint_l_switch", "hints/switch_l"), ("browser_hint_r_switch", "hints/switch_r"),
                         ("browser_hint_l_ps5", "hints/ps5_l1"), ("browser_hint_r_ps5", "hints/ps5_r1"),
                         # (the d-pad: left and right the column, up and
                         # down its order)
                         ("browser_hint_dpad_switch", "hints/switch_dpad"),
                         ("browser_hint_dpad_ps5", "hints/ps5_dpad")]
# (the first four together at the left, BACK at the right, as the buttons were)
BROWSER_HINT_PLACES = [("browser_hint_join", 24, 452, "switch ps5"), ("browser_hint_refresh", 135, 452, "switch ps5"),
                       ("browser_hint_filters", 229, 452, "switch ps5"),
                       ("browser_hint_l_switch", 327, 455, "switch"), ("browser_hint_r_switch", 343, 455, "switch"),
                       ("browser_hint_l_ps5", 327, 455, "ps5"), ("browser_hint_r_ps5", 343, 455, "ps5"),
                       ("browser_hint_mode", 361, 452, "switch ps5"),
                       ("browser_hint_dpad_switch", 417, 455, "switch"), ("browser_hint_dpad_ps5", 417, 455, "ps5"),
                       ("browser_hint_sort", 435, 452, "switch ps5"), ("browser_hint_back", 540, 452, "switch ps5")]

# the Switch's and the PS5's Mods buttons as hints (_mods): A saves, the
# north turns the mod on or off, the shoulders move it, B cancels
MOD_HINTS = [("mod_hint_save", "%a-button SAVE"), ("mod_hint_toggle", "%y-button ON / OFF"),
             ("mod_hint_move", "MOVE"), ("mod_hint_cancel", "%b-button CANCEL")]
MOD_HINT_PICTURES = [("mod_hint_l_switch", "hints/switch_l"), ("mod_hint_r_switch", "hints/switch_r"),
                     ("mod_hint_l_ps5", "hints/ps5_l1"), ("mod_hint_r_ps5", "hints/ps5_r1")]
MOD_HINT_PLACES = [("mod_hint_save", 8, 415, "switch ps5"), ("mod_hint_toggle", 80, 415, "switch ps5"),
                   ("mod_hint_l_switch", 184, 418, "switch"), ("mod_hint_r_switch", 200, 418, "switch"),
                   ("mod_hint_l_ps5", 184, 418, "ps5"), ("mod_hint_r_ps5", 200, 418, "ps5"),
                   ("mod_hint_move", 218, 415, "switch ps5"), ("mod_hint_cancel", 510, 415, "switch ps5")]
# ... and Select Profile's (main_menu.profile_manager.xml: A chooses the
# profile, its own Y and X create one and delete it, B goes back)
PROFILE = "main_menu/profile_manager"
PROFILE_HINTS = [("profile_hint_select", "%a-button SELECT"), ("profile_hint_new", "%y-button CREATE NEW"),
                 ("profile_hint_delete", "%x-button DELETE"), ("profile_hint_back", "%b-button BACK")]
PROFILE_HINT_PLACES = [("profile_hint_select", 8, 415, "switch ps5"), ("profile_hint_new", 92, 415, "switch ps5"),
                       ("profile_hint_delete", 214, 415, "switch ps5"), ("profile_hint_back", 510, 415, "switch ps5")]


def _hint_widgets(base: str, hints: list, pictures: list = ()) -> list:
    """the consoles' hints of a screen's buttons: each a text, its button's
    picture first, and the shoulders' pictures (Kenney's: hints/)"""
    lines = []
    for key, text in hints:
        lines += _widget(f"{base}/{key}", [("type", "text"), ("controller", 1), ("width", 160), ("height", 24),
                                          ("text", text), ("font", "ui\\small_ui"), ("color", "#FFFFFFFF"),
                                          ("text_y", 2), ("text_flags", "no_focus_test")], [])
    for key, bitmap in pictures:
        lines += _widget(f"{base}/{key}", [("width", 14), ("height", 14), ("bitmap", bitmap)], [])
    return lines


WIDGET_PATCHES = {
    # The host returns here after a round with no menu history. Back must
    # reopen the main menu when it closes the last screen, as the lobby does.
    f"{MT}/connected/connected_map_select_wrapper": {"set": {
        "flags": "pass_unhandled_to_focused_child pause_game main_menu_if_no_history tag_controller_index force_handle_mouse",
    }},
    # (the main menu's THEATER, after Multiplayer, and MODS, before Quit:
    # _theater, _mods)
    "main_menu/main_menu_select_list": {"insert_before": {
        "main_menu/main_menu_item_profile": [
            f'<child widget="main_menu/main_menu_item_theater" x="{THEATER_ITEM_PLACE[0]}" y="{THEATER_ITEM_PLACE[1]}"/>'],
        "main_menu/main_menu_item_quit_game": [
            f'<child widget="main_menu/main_menu_item_mods" x="{MODS_ITEM_PLACE[0]}" y="{MODS_ITEM_PLACE[1]}"/>']},
        # (the main menu's X - the west face button: Y on a Switch, Square on
        # a PS5 - opens the news, the latest release's notes: _news_screen;
        # on the main menu alone)
        "handlers": ['<on event="down"/>', f'<on event="x" open="{NEWS}/news_screen"/>']},
    # (and the hint that X opens it, over the build number: a tap or a click
    # on it presses X, its picture a key's icon to the pointer, x_butn_news)
    "main_menu/main_menu": {"insert_before": {"main_menu/build_number": [
        f'<child widget="{NEWS}/x_butn_news" x="450" y="380"/>',
        f'<child widget="{NEWS}/news_hint" x="450" y="380"/>']}},
    f"{PLAYER_EDIT}/player_options_menu": {"insert_before": {
        f"{PLAYER_EDIT}/player_button_bar": [
            f'<child widget="{PLAYER_EDIT}/op_aim_assist" x="54" y="289"/>',
            f'<child widget="{PLAYER_EDIT}/op_shield_effects" x="54" y="316"/>']}},
    # (its rows 27 apart, not 30, so that ten fit above the help, which keeps
    # the three lines some have: ce_menus.py's CHILD_OFFSETS)
    f"{PLAYER_EDIT}/player_options_help": {"set": {"top": 345, "height": 60}},
    # (the gametype editor's OPENCE OPTIONS, after its own rows: _opence_options)
    f"{PLAYLIST_EDIT}/edit_playlist_select_list": {
        "insert_before": {f"{PLAYLIST_EDIT}/playlist_edit_button_bar": [f'<child widget="{OPENCE_ITEM}"/>']},
        "inputs": ["port playlist edit desc"]},
    # (the profile settings' picture: on Gamepad Setup's row, the overlay's
    # controller alone in its place, _layout_overlays;
    # menu_functions.c's profile_gamepad_layout)
    f"{PE}/profile_edit_extended_desc_pic": {"inputs": ["port gamepad layout preview"]},
    f"{PE}/profile_edit_extended_description": {"children": [
        f'<child widget="{LAYOUT}/{controller}/overlay_small" x="4" platform="{spec["platform"]}"/>'
        for controller, spec in CONTROLLERS.items()]},
    # (Gameplay, before About: its screen is the gameplay file's; the rows'
    # places, PROFILE_ROWS)
    f"{PE}/profile_edit_select_list": {"insert_before": {
        f"{PE}/about_item": [f'<child widget="{PE}/gameplay_profile_item"/>'],
    }},
    # (straight to their screens: no "checking for updates" dialog, which
    # asked the PC version's servers)
    f"{MT}/multiplayer_type_join_internet_item": {"set": {"string_index": 6}, "handlers": [
        f'<on event="a" run="mp type set mode" open="{MT}/join_game/join_game_screen"/>',
        f'<on event="start" run="mp type set mode" open="{MT}/join_game/join_game_screen"/>',
        '<on event="left_mouse" run="mouse emit accept event"/>',
    ]},
    f"{MT}/multiplayer_type_create_internet_item": {"handlers": [
        '<on event="a" run="join controller to mp game"/>',
        f'<on event="a" run="mp type set mode" open="{MT}/connected/connected_map_select_wrapper"/>',
        '<on event="left_mouse" run="mouse emit accept event"/>',
    ]},
    f"{MT}/multiplayer_type_join_direct_item": {"handlers": [
        f'<on event="a" run="mp type set mode" open="{MT}/join_game/join_game_screen"/>',
        f'<on event="start" run="mp type set mode" open="{MT}/join_game/join_game_screen"/>',
        '<on event="left_mouse" run="mouse emit accept event"/>',
    ]},
    # (Co-op, after Create Game's: _coop)
    f"{MT}/multiplayer_type_select_list": {"insert_before": {
        f"{MT}/multiplayer_type_gametypes_item": [f'<child widget="{MT}/multiplayer_type_coop_item" y="309"/>'],
    }},
    # (the Map screen's and New Game's lists: SINGLEPLAYER or MULTIPLAYER
    # maps, the first row's chooser's (_map_kind); B on the Map screen goes
    # from a co-op level's difficulties back to the level, else out of the
    # screen: menu_functions.c's map list)
    f"{MT}/mp_map_select/mp_map_select_list_2": {"swap": {"main_menu/new_select/list_item_0": MAP_KIND_CHOOSER},
                                                 "handlers": [
        '<on event="created" run="mp level list initialize"/>',
        '<on event="deleted" run="mp level list dispose"/>',
        f'<on event="custom_activation" run="mp level select" open="{MT}/connected/gametype_select_screen_wrapper"/>',
        '<on event="b" run="port map list back"/>',
        '<on event="back" run="port map list back"/>',
    ]},
    "main_menu/solo_level_select/solo_level_select_list": {"swap": {"main_menu/new_select/list_item_0": MAP_KIND_CHOOSER}},
    # (their IMPORT, Android's: MAP_IMPORT)
    f"{MT}/mp_map_select/mp_map_select_button_bar": {"insert_before": {
        f"{MT}/mp_map_select/mp_map_select_button_ok": [f'<child widget="{MAP_IMPORT}" x="250" y="1" platform="android"/>']}},
    "main_menu/solo_level_select/solo_level_select_button_bar": {"insert_before": {
        "main_menu/solo_level_select/solo_level_select_button_ok": [
            f'<child widget="{MAP_IMPORT}" x="250" y="1" platform="android"/>']}},
    # (each description shows the other kind's maps too: a campaign level's
    # picture, name and words on the Map screen, a multiplayer map's on New
    # Game's)
    f"{MT}/mp_map_select/mp_map_right_item": {"children": [
        '<child widget="main_menu/solo_level_select/replay_level_right_name"/>',
        '<child widget="main_menu/solo_level_select/replay_level_right_pic"/>',
        '<child widget="main_menu/solo_level_select/replay_level_right_data"/>',
    ]},
    "main_menu/solo_level_select/replay_level_right_item": {"children": [
        f'<child widget="{MT}/mp_map_select/mp_map_right_name"/>',
        f'<child widget="{MT}/mp_map_select/mp_map_right_pic"/>',
        f'<child widget="{MT}/mp_map_select/mp_map_right_data"/>',
    ]},
    f"{MT}/join_game/header_join_game": {"children": [
        f'<child widget="{MT}/join_game/header_server_browser"/>',
        f'<child widget="{MT}/join_game/header_direct_link"/>',
    ]},
    # (the small font's line does not fit below their 4 units of offset)
    f"{MT}/join_game/ticker_player_info": {"set": {"text_y": 1}},
    # (the server browser's top-left tab: which games it lists, not Internet
    # or LAN, which are menu items of their own here; wider for its words, in
    # the small font of the counts beside it, and moved left to stay centred
    # in its tab: ce_menus.py's CHILD_OFFSETS)
    # (the tab behind it without CE's rounded box: the screen's border
    # picture already outlines the tab, and the two outlines clashed)
    f"{MT}/join_game/op_browser_mode": {"set": {"bitmap": None}},
    f"{MT}/join_game/browser_mode_spinner": {"set": {
        "string_list": None, "strings": "ALL GAMES|DEDICATED|P2P GAMES", "setting": "network.browser_source",
        "values": "all|dedicated|players", "width": 117, "footer_bounds": "7 120 19 126", "font": "ui\\small_ui",
        "text_y": 3}},
    # (the server browser's error panel: its words in a box of their own in
    # the error box's place, under its title and button; menu_functions.c's
    # lobby_browser_error hides the error box)
    f"{MT}/join_game/gamespy_error_fullscreen": {"insert_before": {"error/error_modal": [
        f'<child widget="{MT}/join_game/error_words" x="192" y="176"/>']}},
    # (wider to the left, for "SERVERS: 8 (5 dedicated)": ce_menus.py's
    # CHILD_OFFSETS keeps its right edge)
    f"{MT}/join_game/server_count_label": {"set": {"width": 210}},
    # (the page, when there is more than one: centred in the counts' panel,
    # between the players and the servers, whatever their numbers' widths;
    # menu_functions.c's lobby_browser_update places it)
    f"{MT}/join_game/page_count_label": {"set": {"align": "center"}},
    # (Filters' rows set the network.browser_* settings, as Network Setup's
    # SERVER BROWSER does: menu_functions.c's filters_show and
    # "gamespy update filter settings". SHOW MISSING MAPS has a third value,
    # those a host shares: Map Sharing downloads them)
    f"{MT}/join_game/filters/empty_servers_spinner": {"set": {
        "string_list": None, "strings": "NO|YES", "setting": "network.browser_show_empty", "values": "false|true"}},
    f"{MT}/join_game/filters/full_servers_spinner": {"set": {
        "string_list": None, "strings": "NO|YES", "setting": "network.browser_show_full", "values": "false|true"}},
    f"{MT}/join_game/filters/game_type_spinner": {"set": {
        "string_list": None, "strings": "ANY|CTF|SLAYER|ODDBALL|KING|RACE", "setting": "network.browser_gametype",
        "values": "any|ctf|slayer|oddball|king|race"}},
    f"{MT}/join_game/filters/installed_maps_spinner": {"set": {
        "string_list": None, "strings": "NO|YES|IF DOWNLOADABLE", "setting": "network.browser_maps",
        "values": "no|yes|downloadable"}},
    f"{MT}/join_game/ticker_rules_info": {"set": {"text_y": 1}},
    # (Item Options' loadout rows, over its buttons)
    "main_menu/settings_select/multiplayer_setup/item_options_edit/item_options_menu": {"insert_before": {
        "main_menu/settings_select/multiplayer_setup/item_options_edit/item_button_bar": [
            f'<child widget="main_menu/settings_select/multiplayer_setup/item_options_edit/op_map_weapons" x="54" y="163"/>',
            f'<child widget="main_menu/settings_select/multiplayer_setup/item_options_edit/op_loadout" x="54" y="193"/>',
            f'<child widget="main_menu/settings_select/multiplayer_setup/item_options_edit/op_primary_weapon" x="54" y="223"/>',
            f'<child widget="main_menu/settings_select/multiplayer_setup/item_options_edit/op_secondary_weapon" x="54" y="253"/>',
        ]}},
    # (Teamplay Options' voice chat and vote kick rows, over its buttons:
    # Server Setup's only, _teamplay_options_extras)
    f"{TEAMPLAY_EDIT}/teamplay_options_menu": {"insert_before": {
        f"{TEAMPLAY_EDIT}/teamplay_button_bar": [
            f'<child widget="{TEAMPLAY_EDIT}/op_{key}" x="54" y="{163 + TEAMPLAY_STEP * index}"/>'
            for index, (key, *_) in enumerate(TEAMPLAY_EXTRA_ROWS)]}},
    f"{TEAMPLAY_EDIT}/teamplay_options_help": {"set": {"top": TEAMPLAY_HELP_TOP,
                                                       "height": 414 - TEAMPLAY_HELP_TOP}},
    # (the PC's Vehicles row's Start opened Item Options)
    "main_menu/settings_select/multiplayer_setup/playlist_edit/playlist_edit_vehicles_list_item": {"handlers": [
        '<on event="a" open="main_menu/settings_select/multiplayer_setup/vehicle_options_edit/vehicle_options_screen"/>',
        '<on event="start" open="main_menu/settings_select/multiplayer_setup/vehicle_options_edit/vehicle_options_screen"/>',
        '<on event="left_mouse" run="mouse emit accept event"/>',
    ]},
    # (Direct Link's clipboard button in the place of the PC version's
    # Refresh, and Refresh in Get List's: the Server Browser's only, which
    # asks the public games' hosts again; "swap" puts another widget in a
    # child's place)
    f"{MT}/join_game/join_game_button_bar": {"swap": {
        f"{MT}/join_game/join_game_button_refresh": f"{MT}/join_game/button_clipboard",
        f"{MT}/join_game/join_game_button_update": f"{MT}/join_game/join_game_button_refresh",
    },
        # (the Switch's and the PS5's: hints of the buttons in their place,
        # browser_hints; a touchscreen keeps the buttons)
        "child_platforms": {f"{MT}/join_game/{button}": "desktop android web"
                            for button in ("join_game_button_refresh", "join_game_button_filters",
                                           "join_game_button_join")} | {"common_button_back": "desktop android web"}},
    # (the server browser's buttons, whatever row has the focus: the north
    # face button - Y, a Switch's X, a PS5's Triangle - refreshes, the west -
    # X, a Switch's Y, a PS5's Square - opens the filters, the shoulders
    # change the source tab: menu_functions.c's browser_shortcut)
    f"{MT}/join_game/join_game_items_list": {"handlers": [
        '<on event="y" run="port browser refresh"/>', '<on event="x" run="port browser filters"/>',
        '<on event="white" run="port browser mode previous"/>', '<on event="black" run="port browser mode next"/>',
        # (the d-pad: left and right highlight a column, up sorts by it the
        # fewest or A first, down the most or Z first, as the column's arrow
        # points; the stick goes through the rows: ui_widget.c's
        # ui_widget_port_dpad_updown_taken)
        '<on event="left" run="port browser column previous"/>', '<on event="right" run="port browser column next"/>',
        '<on event="up" run="port browser sort increasing"/>', '<on event="down" run="port browser sort decreasing"/>']},
    # (Select Profile's buttons as hints on the Switch and the PS5,
    # PROFILE_HINTS; a touchscreen keeps them)
    f"{PROFILE}/profile_manager_button_bar": {"child_platforms": {
        f"{PROFILE}/{button}": "desktop android web"
        for button in ("profile_manager_button_new", "profile_manager_button_delete", "profile_manager_button_ok")}
        | {"common_button_back": "desktop android web"}},
    f"{PROFILE}/player_profile_manager_screen": {"children": [
        f'<child widget="{PROFILE}/{key}" x="{x}" y="{y}" platform="{platform}"/>'
        for key, x, y, platform in PROFILE_HINT_PLACES]},
    f"{MT}/join_game/join_game_screen": {"children": [
        f'<child widget="{MT}/join_game/{key}" x="{x}" y="{y}" platform="{platform}"/>'
        for key, x, y, platform in BROWSER_HINT_PLACES]},
}

# the titles this port has that the PC version has not, set as its headers
# are (port/assets/menus/port_svg): bitmap name, text
TITLES = {
    "main_menu/mods/header_mods": "MODS",
    "main_menu/theater/header_theater": "THEATER",
    f"{MT}/join_game/header_server_browser": "SERVER BROWSER",
    f"{MT}/join_game/header_direct_link": "DIRECT LINK",
    f"{MT}/join_game/header_password": "PASSWORD",
    f"{MT}/lobby/header_lobby": "GAME LOBBY",
    f"{MT}/coop/header_player_2": "PLAYER 2 PROFILE",
    f"{MT}/lobby/header_add_player": "ADD PLAYER",
    f"{PE}/gameplay/header_gameplay": "GAMEPLAY",
}


def title_backdrop(width: float) -> str:
    """a header's dark backdrop, as the PC headers' redraws have it, for text
    width units wide (the text is set over it in OpenCE: ce_menus.py)"""
    return "\n".join([
        '<svg xmlns="http://www.w3.org/2000/svg" width="512" height="64" viewBox="0 0 512 64">',
        "  <defs>",
        '    <filter id="soft" filterUnits="userSpaceOnUse" x="-60" y="-60" width="632" height="184">',
        '      <feGaussianBlur stdDeviation="14.23 13.06"/>', "    </filter>",
        '    <mask id="cut" maskUnits="userSpaceOnUse" x="0" y="0" width="512" height="64">',
        '      <rect width="512" height="59" fill="#fff"/>', "    </mask>", "  </defs>",
        '  <g mask="url(#cut)">',
        f'    <rect x="29.08" y="28.05" width="{width + 28:.2f}" height="40.95" fill="#021931" fill-opacity="0.893" '
        'filter="url(#soft)"/>',
        "  </g>", "</svg>", ""])


# the in-game pause menu's box, taller for the buttons this port adds
# (menu_tags.c's pause_patch: SETTINGS, and the host's END GAME): the
# pausebox5 redraw's rounded box (ui-svg-handmade/shell/bitmaps), drawn to
# hold each number of buttons, one frame each (the Xbox's 2-button box is
# 159 units high, and each button 35 more)
PAUSE_BOX_BUTTONS = [3, 4]
# the campaign pause box, tall enough for five rows (SETTINGS added) or six
# (network co-op's FRIENDLY FIRE too) at the PC version's pause menu's pitch
# (30) with the prompts still under the last row: the stock box is 159, and
# those rows need 36 and 66 more (menu_tags.c's pause_campaign_patch); the
# mission objectives' box beside it grows with it
PAUSE_BOX_EXTRA_HEIGHTS = [195, 225]


def pause_box_height(buttons: int) -> int:
    return 159 + (buttons - 2) * 35


def pause_box_svg(piece: str, height: int, header: bool = False) -> str:
    """a piece of the pause box (left cap, centre strip, right cap; 9-slice:
    the strip tiles across), height units high in its 256-high texture"""
    width = 4 if piece == "center" else 16
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="256" viewBox="0 0 {width} 256">']
    fill = "#0a2649"
    body_clip = ""
    if not header:
        # Keep the translucent footer clear of the body: blending two
        # 75%-opaque fills would make it nearly opaque and tint it blue.
        lines += [f'  <defs><clipPath id="body-area"><rect x="-16" y="0" width="48" height="{height - 28}"/></clipPath></defs>']
        body_clip = ' clip-path="url(#body-area)"'
    if height in PAUSE_BOX_EXTRA_HEIGHTS or height in map(pause_box_height, PAUSE_BOX_BUTTONS):
        # The stock campaign body fades from blue to dark navy at 75% opacity.
        # Scale it within the body, excluding the objective header or key footer.
        top, bottom = (29, height - 2) if header else (2, height - 28)
        lines += [f'  <defs><linearGradient id="body" gradientUnits="userSpaceOnUse" x1="0" y1="{top}" x2="0" y2="{bottom}">',
                  '    <stop offset="0" stop-color="#0a2649"/>',
                  '    <stop offset="1" stop-color="#050d1c"/>',
                  '  </linearGradient></defs>']
        fill = "url(#body)"
    if piece == "center":
        lines += [f'  <rect x="0" y="2" width="4" height="{height - 4}" fill="{fill}" fill-opacity="0.75"{body_clip}/>',
                  '  <rect x="0" y="0" width="4" height="2" fill="#2896ff"/>',
                  f'  <rect x="0" y="{height - 2}" width="4" height="2" fill="#2896ff"/>']
    else:
        bottom, corner = height - 1, height - 9.5
        cap = [f'    <path d="M9.5,1 H17 V{bottom} H9.5 A8.5,8.5 0 0 1 1,{corner} V9.5 A8.5,8.5 0 0 1 9.5,1 Z" '
               f'fill="{fill}" fill-opacity="0.75" fill-rule="evenodd"{body_clip}/>',
               f'    <path d="M17,1 H9.5 A8.5,8.5 0 0 0 1,9.5 V{corner} A8.5,8.5 0 0 0 9.5,{bottom} H17" fill="none" '
               'stroke="#2896ff" stroke-width="2"/>']
        lines += (['  <g transform="matrix(-1 0 0 1 16 0)">'] + cap + ['  </g>']) if piece == "right" else \
            [line[2:] for line in cap]
    # The stock objectives have a 28-unit header; the button key has a
    # 28-unit footer. Clip the black fill to the rounded panel interior.
    top = 2 if header else height - 28
    divider = 28 if header else height - 28
    if piece == "left":
        lines += [f'  <defs><clipPath id="strip"><rect x="2" y="2" width="{width + 16}" height="{height - 4}" rx="7.5"/></clipPath></defs>']
    if piece == "right":
        lines += [f'  <defs><clipPath id="strip"><rect x="-16" y="2" width="{width + 14}" height="{height - 4}" rx="7.5"/></clipPath></defs>']
    clip = ' clip-path="url(#strip)"' if piece != "center" else ""
    opacity = ' fill-opacity="0.75"' if not header else ""
    lines += [f'  <g{clip}>',
              f'    <rect x="0" y="{top}" width="{width}" height="26" fill="black"{opacity}/>',
              f'    <rect x="0" y="{divider}" width="{width}" height="0.75" fill="#2896ff"/>',
              '  </g>']
    lines += ["</svg>", ""]
    return "\n".join(lines)


# where the PC headers' text is (units of their 512x64), and its colour
TITLE_LEFT, TITLE_TOP, TITLE_CAP = 30.68, 33.5, 21.5
TITLE_COLOR = (0x29, 0x95, 0xFD, 255)


def _header(name: str, bitmap: str, platform=None) -> list:
    return _widget(name, [("controller", 1), ("left", 35), ("top", 11), ("width", 363), ("height", 59),
                          ("bitmap", bitmap), ("platform", platform)], [])


# a text field's B and Back do nothing of their own (the engine would go
# back from the field): its screen's (SCREEN_BACK) cancel the edit, else
# leave
FIELD_BACK = ['<on event="b"/>', '<on event="back"/>']
SCREEN_BACK = ['<on event="b" run="gamespy back handler"/>', '<on event="back" run="gamespy back handler"/>']


def _value_row(base: str, key: str, label_index: int, run: str) -> list:
    """a row of a label and a value the player sets (A edits it)"""
    lines = _widget(f"{base}/op_{key}", [("width", 512), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                                         ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                    [f'<on event="a" run="{run}"/>', f'<on event="start" run="{run}"/>', *FIELD_BACK,
                     '<on event="left_mouse" run="mouse emit accept event"/>',
                     f'<child widget="{base}/{key}_label"/>', f'<child widget="{base}/{key}_value" x="230" y="1"/>'])
    lines += _widget(f"{base}/{key}_label", [("type", "text"), ("controller", 1), ("width", 230), ("height", 22),
                                             ("string_list", f"{base}/labels"), ("string_index", label_index or None),
                                             ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("text_x", 13),
                                             ("text_y", 4)], [])
    lines += _widget(f"{base}/{key}_value", [("type", "text"), ("controller", 1), ("width", 280), ("height", 22),
                                             ("font", "ui\\small_ui"), ("color", "#FF2896FF"), ("align", "center"),
                                             ("text_y", 5), ("text_flags", "no_focus_test")], [])
    return lines


# the server browser's words (menu_functions.c's lobby_browser_update, in
# this order: its _browser_word_*): the line below the rows, the rows' status,
# the counts, and the error panel's
BROWSER_WORDS = [
    # (the chosen game's line: "12 of 16 players", and its state)
    "of", "players", "player", "under way", "full or starting",
    # (where it was listed, and what it needs)
    "Dedicated server by", "listed on", "master server", "master servers", "Dedicated server",
    "Hosted by a player (anonymous listing)", "Password required",
    "Needs the newer build: Settings > Network Setup > Check for updates",
    "Runs an older build: it can be joined once it updates",
    # (a join, as it goes: p2p_join_progress)
    "Asking the brokers for the host...", "The host answered: connecting (UDP hole punching)...",
    "Connected: joining the game...",
    "The host did not answer (offline, or its listing is stale)",
    "No route to the host: a forwarded port on either side helps",
    "The host refused: full", "The host refused: closed", "The host refused: banned",
    "The host refused: wrong password", "The host refused: another version", "The host refused: its invite changed",
    "Strict NAT: turn on UPnP, or forward a port",
    # (the list's state)
    "Internet play is off (Settings)", "The server browser is off (network.public_lobby)",
    "Looking for public games...", "Games on a newer version (update the game to join them):",
    "No public games found", "No games match the filters",
    "No master server answered: showing player-hosted games only",
    # (the counts: "SERVERS: 8 (3 dedicated)"; the rows' status)
    "dedicated", "JOINING", "NEWER", "OLDER", "FAILED", "LIVE", "CLOSED",
    # (the error panel: nothing can be listed)
    "Internet play is off:\\nSettings > Network Setup",
    "Neither the master servers\\nnor the brokers answer:\\ncheck the connection",
    # (a refusal added later, last so that the others keep their places)
    "The host refused: your player name",
    # (and a dedicated server's VPN policy's: port/linux/game/network_link.c)
    "The host blocks VPNs: link Discord in Settings > Network Setup",
    "The host refused: it allows no VPNs",
    "The host refused your Discord link: link it again",
    "The host refused: your Discord account is too new",
    "The host refused: your Discord account is in the game",
    "The host blocks VPNs: link Discord with its master (Settings > Network Setup)",
    "The host refused: too many players from your address",
]


def _join_game_extras() -> list:
    """the browser's added titles, Direct Link's clipboard button, the
    password screen, and the browser's words"""
    base = f"{MT}/join_game"
    lines = _password_screen()
    lines += _strings(f"{base}/browser_words", BROWSER_WORDS)
    # (the error panel's words, in a box as the error box: its own text is
    # the game's error messages')
    lines += _widget(f"{base}/error_words", [("type", "text"), ("controller", 1), ("width", 280), ("height", 159),
                                             ("bitmap", "bitmaps/alert_bkd"), ("font", "ui\\large_ui"),
                                             ("color", "#FF2896FF"), ("text_x", 9),
                                             ("text_y", 35), ("text_flags", "no_focus_test")], [])
    # (the Switch's and the PS5's button bar: what each button does, the
    # server browser's or LAN's and Direct Link's, browser_initialize showing
    # the mode's)
    # (a hint a widget, its picture first: the menus' text draws a picture
    # amid centred words out of its place. The shoulders by name, the
    # menus' pictures of them the Xbox's black and white)
    for key, text in BROWSER_HINTS:
        lines += _widget(f"{base}/{key}", [("type", "text"), ("controller", 1), ("width", 128), ("height", 22),
                                          ("text", text), ("font", "ui\\small_ui"), ("color", "#FFFFFFFF"),
                                          ("text_flags", "no_focus_test")], [])
    for key, bitmap in BROWSER_HINT_PICTURES:
        lines += _widget(f"{base}/{key}", [("width", 14), ("height", 14), ("bitmap", bitmap)], [])
    lines += _header(f"{base}/header_server_browser", f"{base}/header_server_browser")
    lines += _header(f"{base}/header_direct_link", f"{base}/header_direct_link")
    lines += _widget(f"{base}/button_clipboard", [("type", "text"), ("width", 128), ("height", 24),
                                                  ("bitmap", "bitmaps/text_button_background"),
                                                  ("text", "PASTE LINK"), ("font", "ui\\small_ui"),
                                                  ("color", "#FFFFFFFF"), ("align", "center"), ("text_y", 2)],
                     ['<on event="a" run="direct ip connect go"/>', '<on event="start" run="direct ip connect go"/>',
                      '<on event="left_mouse" run="mouse emit accept event"/>'])
    return lines


def _password_screen() -> list:
    """the server browser's password screen: a game with a password chosen
    (menu_functions.c's lobby_browser_select), its password typed (shown as
    stars), JOIN GAME joins it if it is the game's; else its help says so"""
    base = f"{MT}/join_game/password"
    spec = {"screen": "password_screen", "spacing": 28, "help_top": 120,
            "header": ("header_password", f"{MT}/join_game/header_password")}
    extra = _value_row(base, "password", 0, "port password edit")
    extra += _button(f"{base}/button_defaults", 3, [])
    extra += _widget(f"{base}/button_ok", [("type", "text"), ("width", 128), ("height", 24),
                                           ("bitmap", "bitmaps/text_button_background"), ("text", "JOIN GAME"),
                                           ("font", "ui\\small_ui"), ("color", "#FFFFFFFF"), ("align", "center"),
                                           ("text_y", 2)],
                     ['<on event="a" run="port password join"/>', '<on event="start" run="port password join"/>',
                      '<on event="left_mouse" run="mouse emit accept event"/>'])
    extra += _strings(f"{base}/labels", ["PASSWORD:"])
    # (by its state: menu_functions.c's password_screen_update)
    extra += _strings(f"{base}/help_strings", [
        "",
        "This game has a password. Type it, then press\\nEnter to join.",
        "That is not the game's password. Try again.",
        "Could not join the game.",
    ])
    lines = _screen(base, spec, [(f"{base}/op_password", None)], ["port password update"],
                    ['<on event="created" run="port password init"/>'], extra)
    # (no Defaults: the bar is JOIN GAME and CANCEL; B goes back at once,
    # even while typing, which the screen starts with)
    lines = [line.replace(f'<child widget="{base}/button_defaults" y="1"/>', "") for line in lines]
    lines = [line.replace('<on event="b" back="true"/>', '<on event="b" run="port password back"/>')
             .replace('<on event="back" back="true"/>', '<on event="back" run="port password back"/>')
             for line in lines]
    return lines


def _server_settings() -> list:
    """Create Game's server settings (in the PC version's place): the game's
    name, the most players (up to the port's 128), its invite link, and
    whether the server browser lists it (PUBLIC or PRIVATE: an internet
    game's)"""
    base = f"{MT}/server_settings"
    # (twelve rows: closer together, the help lower)
    spec = {"screen": "server_settings_screen", "spacing": 24, "help_top": 366,
            "header": ("header_server_settings", f"{base}/header_server_settings")}
    rows, extra = [], []
    extra += _value_row(base, "server_name", 0, "ss edit server name")
    rows.append((f"{base}/op_server_name", None))
    extra += _widget(f"{base}/op_max_players", [("width", 512), ("height", 28),
                                                ("flags", "pass_unhandled_to_focused_child"),
                                                ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                     [f'<child widget="{base}/max_players_label"/>',
                      f'<child widget="{base}/max_players_spinner" x="320" y="1"/>'])
    extra += _widget(f"{base}/max_players_label", [("type", "text"), ("controller", 1), ("width", 300),
                                                   ("height", 22), ("string_list", f"{base}/labels"),
                                                   ("string_index", 1), ("font", "ui\\large_ui"),
                                                   ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
    extra += _widget(f"{base}/max_players_spinner",
                     [("type", "spinner"), ("left", 3), ("top", 2), ("width", 147), ("height", 20),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
                      ("strings", "|".join(str(count) for count in MAXIMUM_PLAYERS)), ("font", "ui\\large_ui"),
                      ("color", "#FF2896FF"), ("align", "center"), ("text_y", 1),
                      ("header_bitmap", "bitmaps/arrow_sm_left"), ("footer_bitmap", "bitmaps/arrow_sm_right"),
                      ("header_bounds", "7 -6 19 0"), ("footer_bounds", "7 150 19 156")], [])
    rows.append((f"{base}/op_max_players", None))
    extra += _value_row(base, "invite", 2, "ss copy invite")
    rows.append((f"{base}/op_invite", None))
    # (PUBLIC or PRIVATE: menu_functions.c's server_settings_update)
    extra += _widget(f"{base}/op_listing", [("width", 512), ("height", 28),
                                            ("flags", "pass_unhandled_to_focused_child"),
                                            ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                     [f'<child widget="{base}/listing_label"/>',
                      f'<child widget="{base}/listing_spinner" x="320" y="1"/>'])
    extra += _widget(f"{base}/listing_label", [("type", "text"), ("controller", 1), ("width", 300),
                                               ("height", 22), ("string_list", f"{base}/labels"),
                                               ("string_index", 9), ("font", "ui\\large_ui"),
                                               ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
    extra += _widget(f"{base}/listing_spinner",
                     [("type", "spinner"), ("left", 3), ("top", 2), ("width", 147), ("height", 20),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
                      ("strings", "PUBLIC|PRIVATE"), ("font", "ui\\large_ui"),
                      ("color", "#FF2896FF"), ("align", "center"), ("text_y", 1),
                      ("header_bitmap", "bitmaps/arrow_sm_left"), ("footer_bitmap", "bitmaps/arrow_sm_right"),
                      ("header_bounds", "7 -6 19 0"), ("footer_bounds", "7 150 19 156")],
                     ['<on event="left_mouse" run="mouse spinner 1wide click"/>'])
    rows.append((f"{base}/op_listing", None))
    # PASSWORD (a public internet game's: menu_functions.c's server_settings_update),
    # which players from the server browser must type
    extra += _value_row(base, "password", 10 + len(COOP_SETUP_SCREENS), "ss edit server password")
    rows.append((f"{base}/op_password", None))
    # MAP SHARING (map_share.c), a screen of its own as co-op's options are,
    # for any game: SETUP_OPTIONS_PLACE below it
    key, _, _, _ = MAP_SHARE_SETUP
    target = f"{base}/{key}/{key}_screen"
    extra += _widget(f"{base}/op_{key}", [("width", 512), ("height", 28),
                                         ("flags", "pass_unhandled_to_focused_child"),
                                         ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                     [f'<on event="a" open="{target}"/>', f'<on event="start" open="{target}"/>',
                      '<on event="left_mouse" run="mouse emit accept event"/>',
                      f'<child widget="{base}/{key}_label"/>'])
    extra += _widget(f"{base}/{key}_label", [("type", "text"), ("controller", 1), ("width", 300), ("height", 22),
                                             ("string_list", f"{base}/labels"),
                                             ("string_index", 11 + len(COOP_SETUP_SCREENS)),
                                             ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("text_x", 13),
                                             ("text_y", 4)], [])
    rows.append((f"{base}/op_{key}", None, SETUP_OPTIONS_PLACE - 1))
    # co-op's options, in the place of the gametype's rows, which co-op hides
    # as multiplayer hides co-op's (menu_functions.c's server_settings_update):
    # rows opening their screens, as the gametype's options' do
    # (_setup_option_screen; COOP_SETUP_SCREENS)
    for index, (key, title, _, _) in enumerate(COOP_SETUP_SCREENS):
        target = f"{base}/{key}/{key}_screen"
        extra += _widget(f"{base}/op_{key}", [("width", 512), ("height", 28),
                                             ("flags", "pass_unhandled_to_focused_child"),
                                             ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                         [f'<on event="a" open="{target}"/>', f'<on event="start" open="{target}"/>',
                          '<on event="left_mouse" run="mouse emit accept event"/>',
                          f'<child widget="{base}/{key}_label"/>'])
        extra += _widget(f"{base}/{key}_label", [("type", "text"), ("controller", 1), ("width", 300), ("height", 22),
                                                 ("string_list", f"{base}/labels"), ("string_index", 10 + index),
                                                 ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("text_x", 13),
                                                 ("text_y", 4)], [])
        rows.append((f"{base}/op_{key}", None, SETUP_OPTIONS_PLACE + index))
    # the gametype's options for this game (the gametype editor's screens,
    # editing a copy of the gametype chosen: "port setup edit")
    for index, (key, screen) in enumerate(SETUP_OPTION_SCREENS):
        target = f"main_menu/settings_select/multiplayer_setup/{screen}"
        extra += _widget(f"{base}/op_{key}", [("width", 512), ("height", 28),
                                             ("flags", "pass_unhandled_to_focused_child"),
                                             ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                         [f'<on event="a" run="port setup edit" open="{target}"/>',
                          f'<on event="start" run="port setup edit" open="{target}"/>',
                          '<on event="left_mouse" run="mouse emit accept event"/>',
                          f'<child widget="{base}/{key}_label"/>', f'<child widget="{base}/{key}_value" x="230" y="1"/>'])
        extra += _widget(f"{base}/{key}_label", [("type", "text"), ("controller", 1), ("width", 230), ("height", 22),
                                                 ("string_list", f"{base}/labels"), ("string_index", 3 + index),
                                                 ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("text_x", 13),
                                                 ("text_y", 4)], [])
        extra += _widget(f"{base}/{key}_value", [("type", "text"), ("controller", 1), ("width", 280), ("height", 22),
                                                 ("font", "ui\\small_ui"), ("color", "#FF2896FF"), ("align", "center"),
                                                 ("text_y", 5), ("text_flags", "no_focus_test")], [])
        rows.append((f"{base}/op_{key}", None, SETUP_OPTIONS_PLACE + index))
    extra += _button(f"{base}/button_defaults", 3, [])
    extra += _widget(f"{base}/button_ok", [("type", "text"), ("width", 128), ("height", 24),
                                           ("bitmap", "bitmaps/text_button_background"), ("text", "START GAME"),
                                           ("font", "ui\\small_ui"), ("color", "#FFFFFFFF"), ("align", "center"),
                                           ("text_y", 2)],
                     [f'<on event="a" run="ss start game" open="{MT}/lobby/lobby_screen"/>',
                      f'<on event="start" run="ss start game" open="{MT}/lobby/lobby_screen"/>',
                      '<on event="left_mouse" run="mouse emit accept event"/>'])
    extra += _strings(f"{base}/labels", ["GAME NAME:", "MAXIMUM PLAYERS:", "INVITE LINK:", "GAME TYPE:",
                                         "PLAYER OPTIONS:", "ITEM OPTIONS:", "VEHICLE OPTIONS:", "INDICATOR OPTIONS:",
                                         "TEAMPLAY OPTIONS:", "LISTING:",
                                         *(f"{title}:" for _, title, _, _ in COOP_SETUP_SCREENS), "PASSWORD:",
                                         f"{MAP_SHARE_SETUP[1]}:"])
    extra += _strings(f"{base}/help_strings", [
        "",
        "The name the game shows in the lists of games.\\nEnter changes it.",
        "The most players the game takes (up to 128).",
        "Players join this game by its invite link, or your\\nDiscord invite. Enter copies the link again.",
        "The gametype and its rules, for this game.",
        "Lives, health, shields and respawning, for this game.",
        "Weapons, grenades and starting equipment, for\\nthis game.",
        "Each team's vehicles and their respawn time, for\\nthis game.",
        "The motion tracker and nav points, for this game.",
        "Friendly fire and team balance, for this game.",
        # (LISTING's, by its choice; PUBLIC's with a publisher key is last)
        "Anyone can see and join your game: it is listed\\nfor everyone through the brokers.",
        "Only players with your invite link can join.",
        # (co-op's options' rows: COOP_SETUP_SCREENS)
        *(help_text for _, _, help_text, _ in COOP_SETUP_SCREENS),
        # (PASSWORD's)
        "Players from the Server Browser must type it to\\njoin (an invite link needs none). Enter sets it.",
        # (MAP SHARING's)
        MAP_SHARE_SETUP[2],
        # (LISTING's PUBLIC, on a machine with a publisher key: menu_functions.c's
        # server_settings_update)
        "Listed for everyone through the brokers, and on\\nthe master servers with your publisher key.",
    ])
    lines = _screen(base, spec, rows, ["server settings update"],
                    ['<on event="created" run="server settings init"/>'], extra)
    # (co-op's options' screens, and map sharing's)
    for key, title, _, screen_rows in [*COOP_SETUP_SCREENS, MAP_SHARE_SETUP]:
        lines += _setup_option_screen(f"{base}/{key}", key, screen_rows, fit=True)
    # (no Defaults: the bar is START and CANCEL; B cancels the name's edit
    # first)
    lines = [line.replace(f'<child widget="{base}/button_defaults" y="1"/>', "") for line in lines]
    lines = [line.replace('<on event="b" back="true"/>', SCREEN_BACK[0])
             .replace('<on event="back" back="true"/>', SCREEN_BACK[1]) for line in lines]
    return lines


MAXIMUM_PLAYERS = [2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128]
# CO-OP OPTIONS' amounts of PER PLAYER (network.coop_enemies, percentages)
# and STATIC MULTIPLIER (network.coop_enemies_multiplier), as menu_functions.c's
# gametype_options have their values
COOP_ENEMIES_PERCENTAGES = [25, 50, 100, 150, 200]
COOP_ENEMIES_MULTIPLIERS = [2, 4, 8, 16, 32]
# co-op's Server Setup options, in screens of their own as the gametype's
# are (its rows: _server_settings), each its key, its title, its row's help
# and its rows (as TEAMPLAY_ROWS: each spinner's key, label, words and the
# help of each of them; menu_functions.c's gametype_options give their
# settings' values)
COOP_SETUP_SCREENS = [
    ("coop_options", "CO-OP OPTIONS", "Team wipes, friendly fire, enemies and collisions,\\nfor this game.", [
        ("coop_friendly_fire", "FRIENDLY FIRE:", ["OFF", "ON", "SHIELD ONLY", "EXPLOSIVES ONLY"], [
            "Players can not be hurt by weapons and explosives\\nfired by the other players.",
            "Players can be hurt by weapons or explosives\\nfired by the other players.",
            "Damage from the other players will only reduce\\nshields. Health will be unaffected.",
            "Players can be hurt by damage from explosives\\nfired by the other players.",
        ]),
        ("coop_extra_enemies", "EXTRA ENEMIES:", ["PER PLAYER", "STATIC MULTIPLIER", "NONE"], [
            "Enemy squads grow with the players: by the value of\\n'Per Player' for each player past the first.",
            "Enemy squads use 'Multiplier', however many players\\nthere are.",
            "Enemy squads are as the campaign has them.",
        ]),
        ("coop_extra_pickups", "EXTRA PICKUPS:", ["PER PLAYER", "STATIC MULTIPLIER", "NONE"], [
            "Pickups grow with the players: by the value of\\n'Per Player' for each player past the first.",
            "Pickups use 'Multiplier', up to eight times as many,\\nhowever many players there are.",
            "Pickups are as the campaign has them.",
        ]),
        ("coop_extra_vehicles", "EXTRA VEHICLES:", ["PER PLAYER", "STATIC MULTIPLIER", "NONE"], [
            "Vehicles respawn after being driven a short distance\\nbased on the number of players: by two thirds the value of\\n'Per Player' for each player past the first.",
            "Vehicles respawn after being driven a short distance,\\nwith extra copies equal to ('Multiplier' - 1) * 2/3,\\nrounded down, however many players there are.",
            "Vehicles are as the campaign has them.",
        ]),
        ("coop_enemies_multiplier", "MULTIPLIER:", [f"{value}X" for value in COOP_ENEMIES_MULTIPLIERS],
         ["Static amounts for the options set to this mode.\\nPickups are capped at eight times as many."] * len(COOP_ENEMIES_MULTIPLIERS)),
        ("coop_enemies_per_player", "PER PLAYER:", [f"{value}%" for value in COOP_ENEMIES_PERCENTAGES],
         ["For each player past the first, options set to Per\\nPlayer get this much more (100%: as many again)."] *
         len(COOP_ENEMIES_PERCENTAGES)),
        ("coop_player_collisions", "PLAYER COLLISIONS:", ["ON", "OFF"], [
            "Players bump into each other, as in the campaign.",
            "Players walk through each other, so that no one\\nblocks a doorway. The AI's characters still block.",
        ]),
        # (network.coop_ally_friendly_fire: game_engine.c's
        # game_engine_friendly_damage)
        ("coop_ally_friendly_fire", "ALLY FRIENDLY FIRE:", ["ON", "OFF"], [
            "Players can hurt and kill their AI allies, as in\\nthe campaign.",
            "Players can not hurt their AI allies, so that no\\none can kill Keyes or the Marines a level needs.",
        ]),
        ("coop_team_wipe", "TEAM WIPE:", ["AUTO", "REVERT CHECKPOINT", "RESPAWN ONLY"], [
            "Private games revert the checkpoint; public games\\nonly respawn. A changed team or missing save\\nalways uses respawn only.",
            "Revert the checkpoint after everyone dies. A changed\\nteam or missing save uses respawn only.",
            "Respawn at checkpoint positions after everyone dies.\\nThe world keeps its progress.",
        ]),
    ]),
    ("voice_options", "VOICE AND VOTING", "Voice chat, text chat and votes to kick a player,\\nfor this game.",
     TEAMPLAY_ROWS),
]
# Server Setup's map sharing (port/linux/game/map_share.c), for any game,
# laid out as co-op's options are (its key, its title, its row's help and
# its rows: menu_functions.c's gametype_options give their settings' values)
MAP_SHARE_UPLOADS = (256, 512, 1024, 2048, 4096, 8192, 16384, 20480)
MAP_SHARE_SETUP = ("map_share_options", "MAP SHARING",
                   "Sending players who join without the game's\\nCustom Edition map the map, and how fast.", [
    ("map_share", "SHARE MAPS:", ["ON", "OFF"], [
        "Players who join without the map download it\\nfrom you and the other players.",
        "Players who join without the map cannot play.",
    ]),
    ("map_share_upload", "UPLOAD LIMIT:", [f"{kb} KB/S" if kb < 1024 else f"{kb // 1024} MB/S"
                                           for kb in MAP_SHARE_UPLOADS],
     ["The most you send of maps, so that the game\\nkeeps the rest of your upload."] * len(MAP_SHARE_UPLOADS)),
])
# (Server Setup's place of its options' rows, co-op's or the gametype's: after
# MAP SHARING's)
SETUP_OPTIONS_PLACE = 6
# (their screens' titles)
TITLES.update({f"{MT}/server_settings/{key}/header_{key}": title
               for key, title, _, _ in [*COOP_SETUP_SCREENS, MAP_SHARE_SETUP]})
# (the rows of a co-op options screen in one place, the one shown: PER
# PLAYER's and MULTIPLIER's, by EXTRA ENEMIES')
SETUP_OPTION_SAME_PLACE = set()


def _setup_option_screen(base: str, key: str, rows: list, fit: bool = False) -> list:
    """a screen of options laid out as the gametype's are (Teamplay
    Options'), its spinners setting config.toml's settings (menu_functions.c's
    "port setup options init" and "port setup options save", OK), each
    choice's help shown as the gametype's are ("game settings lists text
    update": the help strings, each spinner's choices' in turn)"""
    lines = _widget(f"{base}/{key}_screen", [("width", 640), ("height", 480),
                                            ("flags", "pass_unhandled_to_focused_child"),
                                            ("bitmap", "bitmaps/gradient")],
                    [f'<child widget="{base}/header_{key}"/>', f'<child widget="{base}/{key}_menu"/>'])
    lines += _header(f"{base}/header_{key}", f"{base}/header_{key}")
    lines += _widget(f"{base}/{key}_help", [("type", "text"), ("controller", 1), ("left", 68), ("top", 341),
                                           ("width", 482), ("height", 79), ("string_list", f"{base}/cap_{key}"),
                                           ("font", "ui\\large_ui"), ("color", "#FFFFFFFF")], [])
    children = ['<data input="game settings lists text update"/>',
                '<on event="created" run="port setup options init"/>']
    # (30 apart; with fit, closer for more rows than eight, the last 28 tall
    # clear of the help at 341: Server Setup's, whose rows are all shown, as
    # OpenCE Options' are not)
    places = len([row_key for row_key, *_ in rows if row_key not in SETUP_OPTION_SAME_PLACE])
    spacing = min(30, (341 - 1 - 73 - 28) // max(1, places - 1)) if fit else 30
    place = -1
    for row_key, *_ in rows:
        if row_key not in SETUP_OPTION_SAME_PLACE:
            place += 1
        children.append(f'<child widget="{base}/op_{row_key}" x="54" y="{73 + spacing * place}"/>')
    children.append(f'<child widget="{base}/{key}_button_bar" y="414"/>')
    lines += _widget(f"{base}/{key}_menu", [("type", "column_list"), ("width", 640), ("height", 480),
                                           ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                                           ("description", f"{base}/{key}_help")], children)
    lines += _widget(f"{base}/{key}_button_bar", [("type", "column_list"), ("width", 640), ("height", 28),
                                                 ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     ['<data input="common button bar update"/>', f'<child widget="{base}/{key}_button_ok" x="380" y="1"/>',
                      '<child widget="common_button_cancel" x="510" y="1"/>'])
    lines += _button(f"{base}/{key}_button_ok", 1, ['<on event="a" run="port setup options save" back="true"/>',
                                                     '<on event="start" run="port setup options save" back="true"/>'])
    for index, (row_key, label, strings, _) in enumerate(rows):
        lines += _widget(f"{base}/op_{row_key}", [("width", 512), ("height", 28),
                                                 ("flags", "pass_unhandled_to_focused_child"),
                                                 ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                         [f'<child widget="{base}/{row_key}_label"/>',
                          f'<child widget="{base}/{row_key}_spinner" x="286" y="1"/>'])
        lines += _widget(f"{base}/{row_key}_label", [("type", "text"), ("controller", 1), ("width", 300),
                                                     ("height", 22), ("string_list", f"{base}/{key}_labels"),
                                                     ("string_index", index), ("font", "ui\\large_ui"),
                                                     ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
        lines += _widget(f"{base}/{row_key}_spinner",
                         [("type", "spinner"), ("top", 2), ("width", 206), ("height", 20),
                          ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
                          ("string_list", f"{base}/var_{row_key}"), ("font", "ui\\large_ui"),
                          ("color", "#FF2896FF"), ("align", "center"), ("text_y", 4),
                          ("list_flags", "items_from_strings"),
                          ("header_bitmap", "bitmaps/arrow_sm_left"), ("footer_bitmap", "bitmaps/arrow_sm_right"),
                          ("header_bounds", "7 -13 19 -7"), ("footer_bounds", "7 208 19 214")],
                         ['<on event="left_mouse" run="mouse spinner 1wide click"/>'])
        lines += _strings(f"{base}/var_{row_key}", strings)
    lines += _strings(f"{base}/{key}_labels", [label for _, label, *_ in rows])
    lines += _strings(f"{base}/cap_{key}", [text for *_, helps in rows for text in helps])
    return lines
# Server Setup's rows of the gametype's options: the gametype editor's
# screens
SETUP_OPTION_SCREENS = [
    ("game_type", "playlist_edit/game_type_select/gametype_select_screen"),
    ("player_options", "player_options_edit/player_options_screen"),
    ("item_options", "item_options_edit/item_options_screen"),
    ("vehicle_options", "vehicle_options_edit/vehicle_options_screen"),
    ("indicator_options", "indicator_options_edit/indicator_options_screen"),
    ("team_options", "teamplay_options_edit/teamplay_options_screen"),
]


def _lobby() -> list:
    """the lobby the host's and the joining players wait in (the Xbox's
    pregame's functions, which need no widget of theirs): up to the port's
    128 players, scrolling; the game's map and gametype; the countdown"""
    base = f"{MT}/lobby"
    # (split screen: another controller's START joins it, its B leaves alone:
    # menu_functions.c's lobby_join. After a game it is the only screen,
    # network_game_reset_to_pregame_ui opening it in place of the Xbox's
    # pregame: leaving it goes to the main menu, as the PC version's postgame
    # screens do)
    lines = _widget(f"{base}/lobby_screen", [("width", 640), ("height", 480),
                                             ("flags", "pass_unhandled_to_focused_child main_menu_if_no_history"),
                                             ("bitmap", "bitmaps/gradient")],
                    ['<on event="created" run="port lobby open"/>',
                     '<on event="created" run="net server accept conx"/>',
                     '<on event="created" run="net server allow start"/>',
                     '<on event="b" run="port lobby leave" back="true"/>',
                     '<on event="back" run="port lobby leave" back="true"/>',
                     # (another controller's START joins, its ADD PLAYER too: both
                     # ask player 1's Y, which opens the 4-way screen as player 1's)
                     '<on event="start" run="port lobby join"/>',
                     f'<on event="y" run="port lobby add player" open="{base}/join4_screen"/>',
                     # (a dedicated server's map vote: its screen, opened as
                     # the vote begins by a posted X, and again with X while
                     # it runs: menu_functions.c's map_vote_lobby_check. Y is
                     # the 4-way join screen's)
                     f'<on event="x" run="port map vote open" open="{base}/map_vote/map_vote_screen" branch="true"/>',
                     '<child widget="main_menu/new_select/sel_list_desc_bkd"/>',
                     f'<child widget="{base}/lobby_list"/>',
                     f'<child widget="{base}/header_lobby"/>',
                     f'<child widget="{base}/lobby_join_help"/>'])
    lines += _header(f"{base}/header_lobby", f"{base}/header_lobby")
    lines += _widget(f"{base}/lobby_join_help", [("type", "text"), ("controller", 1), ("left", 355), ("top", 446),
                                                 ("width", 275), ("height", 24), ("font", "ui\\small_ui"),
                                                 ("color", "#FF2896FF"), ("align", "right"), ("text_y", 5),
                                                 ("text_flags", "no_focus_test")], [])
    # (its rows: any controller's left and right switch its own player's team,
    # on the rows only, so that they move along the buttons)
    rows = [f'<child widget="{base}/list_item_{index}" x="20" y="{73 + 30 * index}"/>' for index in range(11)]
    lines += _widget(f"{base}/lobby_list", [("type", "column_list"), ("width", 640), ("height", 480),
                                            ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                                            ("description", f"{base}/lobby_desc")],
                     ['<data input="net splitscreen prejoin players"/>', '<data input="port lobby update"/>',
                      *rows,
                      f'<child widget="{base}/lobby_button_bar" y="414"/>'])
    for index in range(11):
        lines += _widget(f"{base}/list_item_{index}",
                         [("width", 390), ("height", 28),
                          ("bitmap", "bitmaps/sel_list_item_bkd_top" if index == 0 else "bitmaps/sel_list_item_bkd"),
                          ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("align", "center"), ("text_y", 3)],
                         ['<on event="left right" run="swap player team"/>',
                          '<child widget="main_menu/new_select/list_item_text" x="25"/>',
                          '<child widget="main_menu/new_select/list_item_arrows"/>'])
    lines += _widget(f"{base}/lobby_desc", [("width", 640), ("height", 480)],
                     ['<child widget="main_menu/current_profile_name"/>',
                      f'<child widget="{base}/lobby_right_item" x="22" y="2"/>'])
    lines += _widget(f"{base}/lobby_right_item", [("controller", 1), ("left", 406), ("top", 75), ("width", 162),
                                                  ("height", 326),
                                                  ("bitmap", "bitmaps/spinner_list_3_wide_item_background")],
                     # (a campaign level's picture and name in place of the
                     # map's, for network co-op: menu_functions.c's
                     # lobby_map_show)
                     [f'<child widget="{base}/lobby_map_pic"/>', f'<child widget="{base}/lobby_map_name"/>',
                      '<child widget="main_menu/solo_level_select/replay_level_right_pic"/>',
                      '<child widget="main_menu/solo_level_select/replay_level_right_name"/>',
                      f'<child widget="{base}/lobby_game_data"/>'])
    lines += _widget(f"{base}/lobby_map_pic", [("controller", 1), ("left", 419), ("top", 87), ("width", 140),
                                               ("height", 114), ("bitmap", "ui\\shell\\bitmaps\\mp_map_grafix")], [])
    lines += _widget(f"{base}/lobby_map_name", [("type", "text"), ("controller", 1), ("left", 417), ("top", 204),
                                                ("width", 146), ("height", 43), ("string_list", "main_menu/mp_map_list"),
                                                ("font", "ui\\large_ui"), ("color", "#FF2896FF")], [])
    lines += _widget(f"{base}/lobby_game_data", [("type", "text"), ("controller", 1), ("left", 417), ("top", 250),
                                                 ("width", 146), ("height", 144), ("font", "ui\\small_ui"),
                                                 ("color", "#FF2896FF")], [])
    # (its words: menu_functions.c's lobby_update. "<name> - dedicated": the
    # game's host is a dedicated server)
    lines += _strings(f"{base}/lobby_words", ["dedicated"])
    lines += _widget(f"{base}/lobby_button_bar", [("type", "column_list"), ("width", 640), ("height", 28),
                                                  ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     # (SWITCH TEAM first: hidden in a game without teams, which
                     # moves the focus to the first shown, START NOW; at the end
                     # it traps none)
                     [f'<child widget="{base}/lobby_button_team" x="120" y="1"/>',
                      f'<child widget="{base}/lobby_button_start" x="250" y="1"/>',
                      f'<child widget="{base}/lobby_button_add" x="380" y="1"/>',
                      f'<child widget="{base}/lobby_button_leave" x="510" y="1"/>'])
    for key, caption, handlers in (
        ("add", "ADD PLAYER", [f'<on event="a" run="port lobby add player" open="{base}/join4_screen"/>',
                               f'<on event="start" run="port lobby add player" open="{base}/join4_screen"/>']),
        ("team", "SWITCH TEAM", ['<on event="a" run="swap player team"/>', '<on event="start" run="swap player team"/>']),
        ("start", "START NOW", ['<on event="a" run="net game speed start"/>',
                                '<on event="start" run="net game speed start"/>']),
        ("leave", "LEAVE", ['<on event="a" run="mouse emit back event"/>',
                            '<on event="start" run="mouse emit back event"/>'])):
        lines += _widget(f"{base}/lobby_button_{key}", [("type", "text"), ("width", 128), ("height", 24),
                                                       ("bitmap", "bitmaps/text_button_background"),
                                                       ("text", caption), ("font", "ui\\small_ui"),
                                                       ("color", "#FFFFFFFF"), ("align", "center"), ("text_y", 2)],
                         handlers + ['<on event="left_mouse" run="mouse emit accept event"/>'])
    # a dedicated server's map vote (network_map_vote.c), as the lobby: its
    # choices a row each (A votes), the focused one's map at the right with the
    # time left; B (or the vote's end) goes back to the lobby
    vote = f"{base}/map_vote"
    lines += _widget(f"{vote}/map_vote_screen", [("width", 640), ("height", 480),
                                                 ("flags", "pass_unhandled_to_focused_child"),
                                                 ("bitmap", "bitmaps/gradient")],
                     ['<on event="b" back="true"/>', '<on event="back" back="true"/>',
                      '<child widget="main_menu/new_select/sel_list_desc_bkd"/>',
                      f'<child widget="{vote}/map_vote_list"/>',
                      f'<child widget="{vote}/map_vote_title"/>'])
    lines += _widget(f"{vote}/map_vote_title", [("type", "text"), ("controller", 1), ("left", 30), ("top", 30),
                                                ("width", 580), ("height", 36), ("text", "VOTE FOR THE NEXT GAME"),
                                                ("font", "ui\\large_ui"), ("color", "#FFFFFFFF"),
                                                ("text_flags", "no_focus_test")], [])
    vote_rows = [f'<child widget="{vote}/list_item_{index}" x="20" y="{73 + 30 * index}"/>' for index in range(8)]
    lines += _widget(f"{vote}/map_vote_list", [("type", "column_list"), ("width", 640), ("height", 480),
                                               ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                                               ("description", f"{vote}/map_vote_desc")],
                     ['<data input="port map vote update"/>', *vote_rows,
                      f'<child widget="{vote}/map_vote_button_bar" y="414"/>'])
    for index in range(8):
        lines += _widget(f"{vote}/list_item_{index}",
                         [("width", 390), ("height", 28),
                          ("bitmap", "bitmaps/sel_list_item_bkd_top" if index == 0 else "bitmaps/sel_list_item_bkd"),
                          ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("align", "center"), ("text_y", 3)],
                         ['<on event="a" run="port map vote choose"/>',
                          '<on event="start" run="port map vote choose"/>',
                          '<on event="left_mouse" run="mouse emit accept event"/>',
                          '<child widget="main_menu/new_select/list_item_text" x="25"/>',
                          '<child widget="main_menu/new_select/list_item_arrows"/>'])
    lines += _widget(f"{vote}/map_vote_desc", [("width", 640), ("height", 480)],
                     ['<child widget="main_menu/current_profile_name"/>',
                      f'<child widget="{base}/lobby_right_item" x="22" y="2"/>'])
    lines += _widget(f"{vote}/map_vote_button_bar", [("type", "column_list"), ("width", 640), ("height", 28),
                                                     ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     [f'<child widget="{vote}/map_vote_button_lobby" x="510" y="1"/>'])
    lines += _widget(f"{vote}/map_vote_button_lobby", [("type", "text"), ("width", 128), ("height", 24),
                                                       ("bitmap", "bitmaps/text_button_background"),
                                                       ("text", "LOBBY"), ("font", "ui\\small_ui"),
                                                       ("color", "#FFFFFFFF"), ("align", "center"), ("text_y", 2)],
                     ['<on event="a" run="mouse emit back event"/>', '<on event="start" run="mouse emit back event"/>',
                      '<on event="left_mouse" run="mouse emit accept event"/>'])
    # a split screen player's profile, chosen as they join the lobby (any
    # controller's presses: Co-op's rows; the host's countdown waits)
    lines += _widget(f"{base}/player_profile_screen", [("width", 640), ("height", 480),
                                                       ("flags", "pass_unhandled_to_focused_child"),
                                                       ("bitmap", "bitmaps/gradient")],
                     ['<on event="created" run="net server defer start"/>',
                      '<on event="deleted" run="net server allow start"/>',
                      '<child widget="main_menu/new_select/sel_list_desc_bkd"/>',
                      f'<child widget="{base}/player_profile_list"/>',
                      f'<child widget="{base}/header_add_player"/>',
                      f'<child widget="{base}/player_profile_help" x="20" y="416"/>'])
    lines += _header(f"{base}/header_add_player", f"{base}/header_add_player")
    lines += _widget(f"{base}/player_profile_list",
                     [("type", "column_list"), ("width", 640), ("height", 480),
                      ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                      ("description", "main_menu/profile_manager/player_profile_extended_desc")],
                     ['<data input="3wide player profile list update"/>',
                      '<on event="created" run="port lobby player list initialize"/>',
                      '<on event="deleted" run="player profile list dispose"/>',
                      '<on event="custom_activation" run="port lobby player choose" back="true" branch="true"/>',
                      *[f'<child widget="{MT}/coop/list_item_{index}" x="20" y="{73 + 30 * index}"/>'
                        for index in range(11)],
                      f'<child widget="{MT}/coop/player_2_button_bar" y="414"/>'])
    lines += _widget(f"{base}/player_profile_help", [("type", "text"), ("width", 350), ("height", 24),
                                                     ("text", "The new player's profile."),
                                                     ("font", "ui\\small_ui"), ("color", "#FF2896FF"), ("text_y", 5),
                                                     ("text_flags", "no_focus_test")], [])
    # a game under way's lobby, before joining it (the browser's rows of
    # games in progress): what its advertisement tells, JOIN GAME. Split
    # screen players join here, as in the lobby, before JOIN GAME: the game
    # starts at once for the machine, with the players it brings
    lines += _widget(f"{base}/preview_screen", [("width", 640), ("height", 480),
                                                ("flags", "pass_unhandled_to_focused_child"),
                                                ("bitmap", "bitmaps/gradient")],
                     ['<on event="created" run="port lobby open"/>',
                      '<on event="b" run="port lobby preview leave" back="true"/>',
                      '<on event="back" run="port lobby preview leave" back="true"/>',
                      '<on event="start" run="port lobby preview add"/>',
                      f'<on event="y" run="port lobby add player" open="{base}/join4_screen"/>',
                      '<child widget="main_menu/new_select/sel_list_desc_bkd"/>',
                      f'<child widget="{base}/preview_list"/>',
                      f'<child widget="{base}/header_lobby"/>',
                      f'<child widget="{base}/lobby_join_help"/>'])
    lines += _widget(f"{base}/preview_list", [("type", "column_list"), ("width", 640), ("height", 480),
                                              ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                                              ("description", f"{base}/lobby_desc")],
                     ['<data input="port lobby preview update"/>',
                      f'<child widget="{base}/preview_status" x="30" y="75"/>',
                      f'<child widget="{base}/preview_button_bar" y="414"/>'])
    lines += _widget(f"{base}/preview_status", [("type", "text"), ("controller", 1), ("width", 360), ("height", 300),
                                                ("font", "ui\\large_ui"), ("color", "#FF2896FF")], [])
    lines += _widget(f"{base}/preview_button_bar", [("type", "column_list"), ("width", 640), ("height", 28),
                                                    ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     [f'<child widget="{base}/preview_button_join" x="250" y="1"/>',
                      f'<child widget="{base}/preview_button_add" x="380" y="1"/>',
                      f'<child widget="{base}/preview_button_back" x="510" y="1"/>'])
    for key, caption, run in (("join", "JOIN GAME", "port lobby preview join"),
                              ("add", "ADD PLAYER", "port lobby add player"),
                              ("back", "BACK", "mouse emit back event")):
        opened = f' open="{base}/join4_screen"' if key == "add" else ""
        lines += _widget(f"{base}/preview_button_{key}", [("type", "text"), ("width", 128), ("height", 24),
                                                         ("bitmap", "bitmaps/text_button_background"),
                                                         ("text", caption), ("font", "ui\\small_ui"),
                                                         ("color", "#FFFFFFFF"), ("align", "center"), ("text_y", 2)],
                         [f'<on event="a" run="{run}"{opened}/>', f'<on event="start" run="{run}"{opened}/>',
                          '<on event="left_mouse" run="mouse emit accept event"/>'])
    lines += _join4(base)
    return lines


def _join4(base: str) -> list:
    """ADD PLAYER's screen: the Xbox's split screen profile select (its own
    widgets and bitmaps, ui\\shell\\...\\4way_profile_select), a quarter for
    each controller. A controller not playing presses A to join, chooses its
    profile on the Xbox's spinner ("player profile list initialize": one no
    other player has) and presses A to select it; B goes back a step. A
    selected or playing quarter's A (or Enter) brings the players who
    selected theirs into the lobby, player 1's B goes back without them. The
    keyboard's SPACE makes the keyboard player 1 alone, freeing the gamepad
    it shared to join (menu_functions.c's join4). Each quarter takes its own
    controller's presses (pc_menu_screen_routes_by_controller)"""
    xbox = "ui\\shell\\main_menu\\multiplayer_type_select\\split_screen\\4way_profile_select\\"
    connected = "ui\\shell\\main_menu\\multiplayer_type_select\\connected\\4way_profile_select\\"
    quarters = [(70, 76), (328, 76), (70, 242), (328, 242)]
    lines = _widget(f"{base}/join4_screen",
                    [("width", 640), ("height", 480), ("bitmap", "ui\\shell\\bitmaps\\gradient"),
                     ("flags", "pass_unhandled_to_focused_child no_focused_child pass_unhandled_to_all_children "
                               "pass_handled_to_all_children")],
                    ['<on event="created" run="port join4 open"/>',
                     '<on event="created" run="net server defer start"/>',
                     # (player 1's B, Escape: back without them; a screen
                     # with no B of its own goes back at any controller's,
                     # before its quarters see it)
                     '<on event="b" run="port join4 back" back="true"/>',
                     '<on event="back" run="port join4 back" back="true"/>',
                     '<data input="port join4 update"/>',
                     f'<child widget="{xbox}header_select_profile"/>',
                     *[f'<child widget="{base}/join4_qtr_join" controller="{index + 1}" x="{x}" y="{y}"/>'
                       for index, (x, y) in enumerate(quarters)],
                     '<child widget="ui\\shell\\main_menu\\button_key"/>',
                     f'<child widget="{base}/join4_hint" x="30" y="419"/>'])
    # (a quarter for a controller not playing: "Press A to Join"; the screen
    # sends a player's quarter "black" (their profile) and the quarter of the
    # controller whose START opened it "white" (its spinner): join4_open)
    lines += _widget(f"{base}/join4_qtr_join",
                     [("width", 255), ("height", 165), ("bitmap", "ui\\shell\\bitmaps\\split_screen_bkd"),
                      ("flags", "pass_unhandled_to_focused_child")],
                     [f'<on event="black" run="port join4 playing" replace="{base}/join4_qtr_playing"/>',
                      f'<on event="white" run="port join4 opener" replace="{base}/join4_qtr_select"/>',
                      f'<on event="a" run="port join4 join" replace="{base}/join4_qtr_select"/>',
                      f'<on event="start" run="port join4 join" replace="{base}/join4_qtr_select"/>',
                      # (a widget with no B of its own goes back at it)
                      '<on event="b"/>', '<on event="back"/>',
                      f'<child widget="{connected}press_start_to_join" x="54" y="20"/>',
                      f'<child widget="{connected}press_start_to_join_controller"/>'])
    lines += _widget(f"{base}/join4_qtr_select",
                     [("width", 254), ("height", 164), ("bitmap", f"{xbox}4way_profile_border"),
                      ("flags", "pass_unhandled_to_focused_child")],
                     ['<on event="created" run="port join4 choosing"/>',
                      f'<on event="a" run="port join4 select" replace="{base}/join4_qtr_ready"/>',
                      f'<on event="start" run="port join4 select" replace="{base}/join4_qtr_ready"/>',
                      f'<on event="b" run="port join4 leave" replace="{base}/join4_qtr_join"/>',
                      '<on event="back"/>',
                      f'<child widget="{xbox}qtr_screen_profile_color_pic"/>',
                      f'<child widget="{xbox}qtr_screen_profile_desc"/>',
                      f'<child widget="{xbox}qtr_screen_profile_spinner"/>'])
    selected = [f'<child widget="{xbox}qtr_screen_profile_color_pic_selected"/>',
                f'<child widget="{xbox}qtr_screen_profile_selected_text"/>',
                f'<child widget="{xbox}qtr_screen_profile_selected_name"/>']
    lines += _widget(f"{base}/join4_qtr_ready",
                     [("width", 254), ("height", 164), ("bitmap", f"{xbox}4way_profile_border")],
                     ['<on event="created" run="port join4 ready"/>',
                      '<on event="a" run="port join4 done" back="true"/>',
                      '<on event="start" run="port join4 done" back="true"/>',
                      f'<on event="b" run="port join4 unready" replace="{base}/join4_qtr_select"/>',
                      '<on event="back"/>',
                      *selected])
    # (a player in the game already)
    lines += _widget(f"{base}/join4_qtr_playing",
                     [("width", 254), ("height", 164), ("bitmap", f"{xbox}4way_profile_border")],
                     ['<on event="a" run="port join4 done" back="true"/>',
                      '<on event="start" run="port join4 done" back="true"/>',
                      '<on event="b"/>', '<on event="back"/>',
                      *selected])
    lines += _widget(f"{base}/join4_hint", [("type", "text"), ("width", 345), ("height", 24),
                                            ("font", "ui\\small_ui"), ("color", "#FF2896FF"),
                                            ("text_flags", "no_focus_test")], [])
    return lines


def _coop() -> list:
    """Co-op, the campaign for two players on this machine in split screen
    (the Xbox's Cooperative Play): Multiplayer's CO-OP CAMPAIGN, then player
    2's profile, chosen with player 2's controller (its rows take any
    controller's presses, as the shared rows do only controller 1's), then
    New Game's levels and difficulty, which either player's controller uses
    (ui_widget.c's widget_takes_events_of_controller; menu_functions.c's
    coop_begin)"""
    base = f"{MT}/coop"
    lines = _widget(f"{MT}/multiplayer_type_coop_item",
                    [("type", "text"), ("left", 51), ("width", 232), ("height", 32), ("bitmap", "bitmaps/list_item_bkd"),
                     ("string_list", f"{MT}/multiplayer_options"), ("string_index", 7), ("font", "ui\\large_ui"),
                     ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 5)],
                    [f'<on event="a" run="port coop begin" open="{base}/player_2_profile_screen" branch="true"/>',
                     f'<on event="start" run="port coop begin" open="{base}/player_2_profile_screen" branch="true"/>',
                     '<on event="left_mouse" run="mouse emit accept event"/>'])
    lines += _widget(f"{base}/player_2_profile_screen", [("width", 640), ("height", 480),
                                                         ("flags", "pass_unhandled_to_focused_child"),
                                                         ("bitmap", "bitmaps/gradient")],
                     ['<child widget="main_menu/new_select/sel_list_desc_bkd"/>',
                      f'<child widget="{base}/player_2_profile_list"/>',
                      f'<child widget="{base}/header_player_2"/>',
                      f'<child widget="{base}/player_2_help" x="20" y="416"/>'])
    lines += _header(f"{base}/header_player_2", f"{base}/header_player_2")
    rows = [f'<child widget="{base}/list_item_{index}" x="20" y="{73 + 30 * index}"/>' for index in range(11)]
    lines += _widget(f"{base}/player_2_profile_list",
                     [("type", "column_list"), ("width", 640), ("height", 480),
                      ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                      ("description", "main_menu/profile_manager/player_profile_extended_desc")],
                     ['<data input="3wide player profile list update"/>',
                      '<on event="created" run="port coop player 2 list initialize"/>',
                      '<on event="deleted" run="player profile list dispose"/>',
                      '<on event="custom_activation" run="port coop player 2" '
                      'open="main_menu/solo_level_select/solo_level_select_screen" branch="true"/>',
                      *rows, f'<child widget="{base}/player_2_button_bar" y="414"/>'])
    for index in range(11):
        lines += _widget(f"{base}/list_item_{index}",
                         [("width", 390), ("height", 28),
                          ("bitmap", "bitmaps/sel_list_item_bkd_top" if index == 0 else "bitmaps/sel_list_item_bkd"),
                          ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("align", "center"), ("text_y", 3)],
                         ['<on event="a" run="single prev cl item activated"/>',
                          '<on event="start" run="single prev cl item activated"/>',
                          '<on event="left_mouse" run="mouse emit accept event"/>',
                          '<child widget="main_menu/new_select/list_item_text" x="25"/>',
                          '<child widget="main_menu/new_select/list_item_arrows"/>'])
    lines += _widget(f"{base}/player_2_help", [("type", "text"), ("width", 350), ("height", 24),
                                               ("text", "Player 2: choose with your controller."),
                                               ("font", "ui\\small_ui"), ("color", "#FF2896FF"), ("text_y", 5),
                                               ("text_flags", "no_focus_test")], [])
    lines += _widget(f"{base}/player_2_button_bar", [("type", "column_list"), ("width", 640), ("height", 28),
                                                     ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     ['<data input="common button bar update"/>',
                      '<child widget="main_menu/profile_manager/profile_manager_button_ok" x="380" y="1"/>',
                      '<child widget="common_button_back" x="510" y="1"/>'])
    return lines


def _item_options_extras() -> list:
    """Item Options' rows of the port's: the map's weapons (YES or NO), and
    the loadout, CATEGORY (the weapon set) or CUSTOM (each player's primary
    and secondary weapons)"""
    base = "main_menu/settings_select/multiplayer_setup/item_options_edit"
    lines = []
    for key, strings, label in (("map_weapons", "var_map_weapons", 8), ("loadout", "var_loadout", 5),
                                ("primary_weapon", "var_loadout_weapon", 6),
                                ("secondary_weapon", "var_loadout_weapon", 7)):
        lines += _widget(f"{base}/op_{key}", [("width", 512), ("height", 28),
                                               ("flags", "pass_unhandled_to_focused_child"),
                                               ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                         [f'<child widget="{base}/{key}_label"/>',
                          f'<child widget="{base}/{key}_spinner" x="286" y="1"/>'])
        lines += _widget(f"{base}/{key}_label", [("type", "text"), ("controller", 1), ("width", 300), ("height", 22),
                                                   ("string_list", f"{base}/item_options_labels"),
                                                   ("string_index", label), ("font", "ui\\large_ui"),
                                                   ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
        lines += _widget(f"{base}/{key}_spinner",
                         [("type", "spinner"), ("top", 2), ("width", 206), ("height", 20),
                          ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
                          ("string_list", f"{base}/{strings}"), ("font", "ui\\large_ui"), ("color", "#FF2896FF"),
                          ("align", "center"), ("text_y", 4), ("list_flags", "items_from_strings"),
                          ("header_bitmap", "bitmaps/arrow_sm_left"), ("footer_bitmap", "bitmaps/arrow_sm_right"),
                          ("header_bounds", "7 -13 19 -7"), ("footer_bounds", "7 208 19 214")],
                         ['<on event="left_mouse" run="mouse spinner 1wide click"/>'])
    lines += _strings(f"{base}/var_map_weapons", ["YES", "NO"])
    lines += _strings(f"{base}/var_loadout", ["CATEGORY", "CUSTOM"])
    lines += _strings(f"{base}/var_loadout_weapon", LOADOUT_WEAPONS)
    return lines


def _player_options_extras() -> list:
    """The host's aim-assist policy and shield effects, hidden while editing
    saved gametypes."""
    base = PLAYER_EDIT
    lines = []
    for key, label_index, words in (("aim_assist", 8, ["ALLOWED", "BLOCK M+KB"]),
                                    ("shield_effects", 9, SHIELD_EFFECTS)):
        lines += _widget(f"{base}/op_{key}", [("width", 512), ("height", 28),
            ("flags", "pass_unhandled_to_focused_child"), ("bitmap", "bitmaps/option_bkds"),
            ("color", "#FF2896FF")], [f'<child widget="{base}/{key}_label"/>',
            f'<child widget="{base}/{key}_spinner" x="286" y="1"/>'])
        lines += _widget(f"{base}/{key}_label", [("type", "text"), ("controller", 1),
            ("width", 280), ("height", 22), ("string_list", f"{base}/player_options_labels"),
            ("string_index", label_index), ("font", "ui\\large_ui"), ("color", "#FF2896FF"),
            ("text_x", 13), ("text_y", 4)], [])
        lines += _widget(f"{base}/{key}_spinner", [("type", "spinner"), ("top", 2),
            ("width", 206), ("height", 20), ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
            ("string_list", f"{base}/var_{key}"), ("font", "ui\\large_ui"),
            ("color", "#FF2896FF"), ("align", "center"), ("text_y", 4),
            ("list_flags", "items_from_strings"), ("header_bitmap", "bitmaps/arrow_sm_left"),
            ("footer_bitmap", "bitmaps/arrow_sm_right"), ("header_bounds", "7 -13 19 -7"),
            ("footer_bounds", "7 208 19 214")], ['<on event="left_mouse" run="mouse spinner 1wide click"/>'])
    return lines + _strings(f"{base}/var_aim_assist", ["ALLOWED", "BLOCK M+KB"]) + \
        _strings(f"{base}/var_shield_effects", SHIELD_EFFECTS)


def _opence_options() -> list:
    """the gametype editor's OPENCE OPTIONS: its row, after the editor's own
    (as they are laid out), and its screen, as the gametype's options' are
    (_setup_option_screen), its spinners the gametype's (menu_functions.c's
    gametype_options), its header its words (no picture of its own)"""
    lines = _setup_option_screen(OPENCE_EDIT, "opence", OPENCE_ROWS)
    lines = [line.replace("port setup options init", "mp profile init opence options")
             .replace("port setup options save", "mp profile set opence options")
             .replace(f'bitmap="{OPENCE_EDIT}/header_opence"',
                      'type="text" text="OPENCE OPTIONS" font="ui\\large_ui" color="#FFFFFFFF" text_x="13" text_y="16"')
             for line in lines]
    lines += _widget(OPENCE_ITEM, [("type", "text"), ("left", 51), ("top", 309), ("width", 232), ("height", 32),
                                   ("bitmap", "bitmaps/list_item_bkd"), ("text", "OPENCE OPTIONS"),
                                   ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 5)],
                     [f'<on event="a" open="{OPENCE_EDIT}/opence_screen"/>',
                      f'<on event="start" open="{OPENCE_EDIT}/opence_screen"/>',
                      '<on event="left_mouse" run="mouse emit accept event"/>'])
    return lines


def _teamplay_options_extras() -> list:
    """Teamplay Options' rows of the port's: the gametype's FORCE TEAM
    SPAWNS (game_engine.c), then the host's voice chat
    (port/linux/game/network_voice.c) and vote kicks (network_votekick.c),
    in Server Setup's copy only (menu_functions.c's gametype_options_init),
    kept in config.toml"""
    lines = []
    for index, (key, label, strings, _) in enumerate(TEAMPLAY_EXTRA_ROWS):
        lines += _widget(f"{TEAMPLAY_EDIT}/op_{key}", [("width", 512), ("height", 28),
                                                       ("flags", "pass_unhandled_to_focused_child"),
                                                       ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                         [f'<child widget="{TEAMPLAY_EDIT}/{key}_label"/>',
                          f'<child widget="{TEAMPLAY_EDIT}/{key}_spinner" x="286" y="1"/>'])
        lines += _widget(f"{TEAMPLAY_EDIT}/{key}_label", [("type", "text"), ("controller", 1), ("width", 300),
                                                          ("height", 22),
                                                          ("string_list", f"{TEAMPLAY_EDIT}/teamplay_options_labels"),
                                                          ("string_index", 3 + index), ("font", "ui\\large_ui"),
                                                          ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
        lines += _widget(f"{TEAMPLAY_EDIT}/{key}_spinner",
                         [("type", "spinner"), ("top", 2), ("width", 206), ("height", 20),
                          ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
                          ("string_list", f"{TEAMPLAY_EDIT}/var_{key}"), ("font", "ui\\large_ui"),
                          ("color", "#FF2896FF"), ("align", "center"), ("text_y", 4),
                          ("list_flags", "items_from_strings"),
                          ("header_bitmap", "bitmaps/arrow_sm_left"), ("footer_bitmap", "bitmaps/arrow_sm_right"),
                          ("header_bounds", "7 -13 19 -7"), ("footer_bounds", "7 208 19 214")],
                         ['<on event="left_mouse" run="mouse spinner 1wide click"/>'])
        lines += _strings(f"{TEAMPLAY_EDIT}/var_{key}", strings)
    return lines


def _map_kind() -> list:
    """the map lists' first row (New Game's and the Map screen's), as the
    gametype list's chooser: a spinner of SINGLEPLAYER or MULTIPLAYER maps,
    or the Custom Edition maps of the custom_maps folder of either kind
    (menu_functions.c's map lists), for either controller (split screen
    co-op's New Game takes both)"""
    lines = _widget(MAP_KIND_CHOOSER, [("width", 256), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                                       ("bitmap", "bitmaps/option_bkds_sm"), ("font", "ui\\large_ui"),
                                       ("color", "#FF2896FF"), ("align", "center"), ("text_y", 3)],
                    [f'<child widget="{MAP_KIND_CHOOSER}_spinner" x="9" y="2"/>'])
    # (wider than the gametype chooser's, for CUSTOM SINGLEPLAYER, its
    # arrows at the row's ends)
    lines += _widget(f"{MAP_KIND_CHOOSER}_spinner",
                     [("type", "spinner"), ("width", 238), ("height", 22), ("flags", "left_right_tabs_items"),
                      ("string_list", "main_menu/new_select/var_map_kinds"), ("font", "ui\\large_ui"),
                      ("color", "#FF2896FF"), ("align", "center"), ("text_y", 1), ("list_flags", "items_from_strings"),
                      ("header_bitmap", "bitmaps/arrow_sm_left"), ("footer_bitmap", "bitmaps/arrow_sm_right"),
                      ("header_bounds", "7 -6 19 0"), ("footer_bounds", "7 238 19 244")],
                     ['<on event="left_mouse" run="mouse spinner 1wide click"/>'])
    lines += _strings("main_menu/new_select/var_map_kinds",
                      ["SINGLEPLAYER", "MULTIPLAYER", "CUSTOM SINGLEPLAYER", "CUSTOM MULTIPLAYER"])
    lines += _text_button(MAP_IMPORT, "IMPORT", ['<on event="a" run="port map import"/>'])
    return lines


def _news_screen() -> list:
    """the news window: its title over a panel (bitmaps/controller_border)
    of NEWS_ROWS lines of the notes, and under it the buttons that scroll
    (the menus' scroll arrows, shown while there is more than the panel
    holds) and close it (B as the console's picture of it: the menus'
    %-codes); the title's and the lines' texts menu_functions.c's alone,
    which makes them as long as it needs.
    Up and down (the D-pad, the sticks, the mouse wheel) scroll a line, left
    and right a page, B, Back or X again close it. And the main menu's hint
    that X opens it (WIDGET_PATCHES)"""
    base = NEWS
    left, top = NEWS_PANEL[0] + 49, NEWS_PANEL[1] + 104
    close = ['<on event="b" run="port news close"/>', '<on event="back" run="port news close"/>',
             '<on event="x" run="port news close"/>']
    scroll = [f'<on event="{event}" run="port news up"/>' for event in ("up", "stick_up", "right_stick_up")]
    scroll += [f'<on event="{event}" run="port news down"/>' for event in ("down", "stick_down", "right_stick_down")]
    scroll += ['<on event="left" run="port news page up"/>', '<on event="right" run="port news page down"/>']
    lines = _widget(f"{base}/news_screen", [("width", 640), ("height", 480), ("bitmap", "bitmaps/gradient_big")],
                    ['<data input="port news update"/>', '<on event="created" run="port news init"/>', *close,
                     *scroll, f'<child widget="{base}/news_panel" x="{NEWS_PANEL[0]}" y="{NEWS_PANEL[1]}"/>',
                     f'<child widget="{base}/news_title" x="{left}" y="{top - 34}"/>',
                     *[f'<child widget="{base}/news_line" x="{left + 16}" y="{top + 12 + 18 * row}"/>'
                       for row in range(NEWS_ROWS)],
                     # (under the panel, the buttons left to right from a little
                     # left of its middle: the up arrow, "Scroll", the down
                     # arrow, B's picture and "Back"; widgets of their own,
                     # which the menus' centring of a text with pictures in it
                     # put apart; no picture of the D-pad is drawn in text, so
                     # the menus' own scroll arrows. The arrows are the d-pad's
                     # up and down to a tap or a click (a key's icon by their
                     # names: ui_widget.c's ui_mouse_key_button), each further
                     # than a key's label from "Scroll", so that it is neither's.
                     # Their pictures sit high in their 16 texels: 7 down puts
                     # their middles at the text's)
                     f'<child widget="{base}/dpad_up_butn_news" x="{left + 164}" y="{top + 253 + 7}"/>',
                     f'<child widget="{base}/news_scroll_text" x="{left + 198}" y="{top + 253}"/>',
                     f'<child widget="{base}/dpad_down_butn_news" x="{left + 254}" y="{top + 253 + 7}"/>',
                     # (Back's tap and click: B's, as the hint's is X's)
                     f'<child widget="{base}/b_butn_news_back" x="{left + 300}" y="{top + 253}"/>',
                     f'<child widget="{base}/news_back_text" x="{left + 300}" y="{top + 253}"/>'])
    lines += _widget(f"{base}/news_panel", [("width", 640), ("height", 480), ("bitmap", "bitmaps/controller_border")],
                     [])
    lines += _widget(f"{base}/news_title", [("type", "text"), ("controller", 1), ("width", 507), ("height", 30),
                                           ("font", "ui\\large_ui"), ("color", "#FF2896FF")], [])
    lines += _widget(f"{base}/news_line", [("type", "text"), ("controller", 1), ("width", 475), ("height", 18),
                                          ("font", "ui\\small_ui"), ("color", "#FFFFFFFF"),
                                          ("text_flags", "no_focus_test")], [])
    for name in ("up", "down"):
        lines += _widget(f"{base}/dpad_{name}_butn_news", [("width", 16), ("height", 16),
                                                           ("bitmap", f"bitmaps/arrow_sm_{name}")], [])
    for name, text, width in (("scroll", "Scroll", 48), ("back", "%b-button Back", 120)):
        lines += _widget(f"{base}/news_{name}_text", [("type", "text"), ("controller", 1), ("width", width),
                                                      ("height", 20), ("text", text), ("font", "ui\\small_ui"),
                                                      ("color", "#FFFFFFFF"), ("text_flags", "no_focus_test")], [])
    # (the hint's tap and click: a widget named as a screen key's icon is X's
    # to the pointer, and the text beside it its label, ui_widget.c's
    # ui_mouse_key_button; nothing of it drawn, the hint's own picture is)
    lines += _widget(f"{base}/x_butn_news", [("width", 16), ("height", 20)], [])
    lines += _widget(f"{base}/b_butn_news_back", [("width", 16), ("height", 20)], [])
    lines += _widget(f"{base}/news_hint", [("type", "text"), ("controller", 1), ("width", 150), ("height", 20),
                                          ("text", "%x-button NEWS"), ("font", "ui\\small_ui"),
                                          ("color", "#FFCCCCCC"), ("text_flags", "no_focus_test")], [])
    return lines


def _invite_qr_screen() -> list:
    """(the Switch's) Server Setup's invite link as a QR code, for a phone
    to read: its own screen, opened by the invite row's A (menu_functions.c's
    invite_copy), which only B closes; the code is drawn above its line of
    text (menu_functions.c's link_qr_draw)"""
    base = f"{MT}/server_settings/invite_qr"
    lines = _widget(f"{base}/invite_qr_screen", [("width", 640), ("height", 480), ("flags", "pause_game"),
                                                 ("bitmap", "bitmaps/gradient")],
                    ['<on event="b" run="ss invite qr back"/>',
                     f'<child widget="{base}/invite_qr_title"/>',
                     f'<child widget="{base}/invite_qr_text"/>'])
    lines += _widget(f"{base}/invite_qr_title", [("type", "text"), ("controller", 1), ("left", 35), ("top", 24),
                                                 ("width", 570), ("height", 30), ("text", "INVITE LINK"),
                                                 ("font", "ui\\large_ui"), ("color", "#FF2896FF")], [])
    lines += _widget(f"{base}/invite_qr_text", [("type", "text"), ("controller", 1), ("left", 40), ("top", 410),
                                                ("width", 560), ("height", 18),
                                                ("text", "Scan it with your phone for the invite link. B: back"),
                                                ("font", "ui\\small_ui"), ("color", "#FFFFFFFF"), ("align", "center"),
                                                ("text_flags", "no_focus_test")], [])
    return lines


def multiplayer_files() -> dict:
    """the port's multiplayer widgets: the browser's additions, the server
    settings, the lobby"""
    head = ['<?xml version="1.0" encoding="UTF-8"?>',
            "<!-- The port's multiplayer screens, in the PC version's style (tools/port_settings.py) -->", "<menus>"]
    return {
        f"{MT}/join_game".replace("/", ".") + ".port.xml": head + _join_game_extras() + ["</menus>", ""],
        f"{MT}/server_settings".replace("/", ".") + ".xml": head + _server_settings() + _invite_qr_screen() +
        ["</menus>", ""],
        f"{MT}/lobby".replace("/", ".") + ".xml": head + _lobby() + ["</menus>", ""],
        f"{MT}/coop".replace("/", ".") + ".xml": head + _coop() + ["</menus>", ""],
        "main_menu/new_select".replace("/", ".") + ".port.xml": head + _map_kind() + ["</menus>", ""],
        PROFILE.replace("/", ".") + ".port.xml": head + _hint_widgets(PROFILE, PROFILE_HINTS) + ["</menus>", ""],
        # (and the main menu's news)
        NEWS.replace("/", ".") + ".xml": head + _news_screen() + ["</menus>", ""],
        "main_menu/settings_select/multiplayer_setup/item_options_edit".replace("/", ".") + ".port.xml": head + _item_options_extras() + ["</menus>", ""],
        PLAYER_EDIT.replace("/", ".") + ".port.xml": head + _player_options_extras() + ["</menus>", ""],
        TEAMPLAY_EDIT.replace("/", ".") + ".port.xml": head + _teamplay_options_extras() + ["</menus>", ""],
        OPENCE_EDIT.replace("/", ".") + ".port.xml": head + _opence_options() + ["</menus>", ""],
        MODS.replace("/", ".") + ".xml": [head[0], "<!-- The port's Mods screen, in the PC version's style "
                                          "(tools/port_settings.py) -->", head[2]] + _mods() + ["</menus>", ""],
        THEATER.replace("/", ".") + ".xml": [head[0], "<!-- The port's Theater screen, in the PC version's style "
                                             "(tools/port_settings.py) -->", head[2]] + _theater() + ["</menus>", ""],
    }


REPLACED_FOLDERS = [f"{MT}/server_settings"]


# ---------- the mods (port/linux/game/mods.c, docs/mods.md): the main menu's
# MODS, which opens the list of the mods folder's mods, in their load order,
# each turned on or off and moved up or down (menu_functions.c's "port mods"
# functions)

# the redraws' main menu items (svg/shell/main_menu/menu_*.svg): their
# letters' top and scale, and where their middle is
MENU_ITEM_TOP, MENU_ITEM_SCALE, MENU_ITEM_MIDDLE = 6.57, 0.9037, 130.0
# the space between two of the redraws' letters, in their units (their
# capitals are 21.5 high), for the pairs the port's items have: no redraw has
# these pairs, so each is the sum of a space after the first letter and one
# before the second, fitted (least squares) to the 53 pairs the redraws' items
# and headers have (D-I, of the main menu's Credits, gives D's right side)
MENU_ITEM_SPACES = {"MO": 3.17, "OD": 2.8, "DS": 2.3,
                    # (THEATER's: ER as the redraws' PLAYER has it; the
                    # others set by eye beside the PC version's items)
                    "TH": 2.5, "HE": 2.6, "EA": 1.5, "AT": 0.5, "TE": 1.5, "ER": 3.24}
# the mods' status icons (port/assets/icons/lucide): a mod out of the order it
# asks for, and one that cannot be played (another mod says it cannot be
# played with it, or this build cannot play it): menu_functions.c's
# mods_update shows frame 1 or 2
MOD_STATUS_ICONS = [("triangle-alert", "#FFB020"), ("circle-x", "#FF4040")]


def redraw_letters(redraws: Path) -> dict:
    """the redraws' letters (their <path id="gA">s, one drawing of each
    letter in every item and header), by letter"""
    letters = {}
    for svg in sorted(redraws.rglob("*.svg")):
        if any(part.startswith("_") for part in svg.relative_to(redraws).parts):
            continue
        for letter, path in re.findall(r'<path id="g([A-Z])" d="([^"]+)"', svg.read_text()):
            if letters.setdefault(letter, path) != path:
                sys.exit(f"{svg}: the redraws draw {letter} two ways")
    return letters


def menu_item_frames(text: str, redraws: Path) -> list:
    """the frames (svg, width, height) of a main menu item of the port's: its
    text in the redraws' own letters (redraw_letters), spaced, placed and
    coloured as their items are, dim, then white with their blue glow"""
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.svgLib.path import parse_path

    letters = redraw_letters(redraws)
    widths = []
    for letter in text:
        pen = BoundsPen(None)
        parse_path(letters[letter], pen)
        widths.append(pen.bounds[2])
    spaces = [MENU_ITEM_SPACES[first + second] for first, second in zip(text, text[1:])]
    x = MENU_ITEM_MIDDLE - (sum(widths) + sum(spaces)) * MENU_ITEM_SCALE / 2
    uses = []
    for index, letter in enumerate(text):
        uses.append(f'<use href="#g{letter}" transform="translate({x:.2f},{MENU_ITEM_TOP}) scale({MENU_ITEM_SCALE})"/>')
        x += (widths[index] + (spaces[index] if index < len(spaces) else 0)) * MENU_ITEM_SCALE
    paths = [f'    <path id="g{letter}" d="{letters[letter]}"/>' for letter in sorted(set(text))]
    head = ['<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="256" '
            'height="64" viewBox="0 0 256 64">', "  <defs>", *paths]
    dim = [*head, "  </defs>", f'  <!-- "{text}" (normal state: dim blue) -->',
           '  <g id="text" fill="#2895ff" fill-opacity="0.467">', *[f"    {use}" for use in uses], "  </g>", "</svg>", ""]
    glow = [*head, '    <filter id="glow" filterUnits="userSpaceOnUse" x="0" y="0" width="256" height="64">',
            '      <feGaussianBlur stdDeviation="2.19"/>', "    </filter>", "  </defs>",
            f'  <!-- "{text}" (highlighted: white with blue glow) -->',
            '  <g fill="#2895ff" stroke="#2895ff" stroke-width="6.3" stroke-linejoin="round" opacity="0.650" '
            'filter="url(#glow)">', *[f"    {use}" for use in uses], "  </g>",
            '  <g id="text" fill="#fff">', *[f"    {use}" for use in uses], "  </g>", "</svg>", ""]
    return [("\n".join(dim), 256, 64), ("\n".join(glow), 256, 64)]


def mod_status_frames(icons: Path, size: int = 24) -> list:
    """the frames of the mods' status icon, `size` pixels square (the menus
    draw a bitmap at its size): none, out of order, incompatible"""
    blank = f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 24 24"/>\n'
    return [(blank, size, size)] + [
        ((icons / f"{icon}.svg").read_text().replace('stroke="currentColor"', f'stroke="{color}"')
         .replace('width="24"', f'width="{size}"').replace('height="24"', f'height="{size}"'), size, size)
        for icon, color in MOD_STATUS_ICONS]


def _text_button(name: str, text: str, handlers: list) -> list:
    """a button of the button bar with its own words"""
    pairs = [("type", "text"), ("width", 128), ("height", 24), ("bitmap", "bitmaps/text_button_background"),
             ("text", text), ("font", "ui\\small_ui"), ("color", "#FFFFFFFF"), ("align", "center"), ("text_y", 2)]
    return _widget(name, pairs, handlers + ['<on event="left_mouse" run="mouse emit accept event"/>'])


def _mods() -> list:
    """the main menu's MODS and its screen, laid out as the lobby is: on the
    left two thirds the mods as a resource pack list has them (the one at the
    top playing over those below it: the load order upside down), a row each
    with its status icon, name and ON or OFF; on the right third, in the
    lobby's frame, the mod chosen: its name, what is wrong with it, whether it
    is on and its priority, and what its mod.toml says. A turns the mod chosen
    on or off, left and right (and MOVE UP and MOVE DOWN, for the mouse) move
    it; OK keeps it all (load_order.toml) and plays them, Cancel forgets it
    (menu_functions.c). Android has a row fewer, and in its place a second
    bar: IMPORT (a mod's .zip file, the app's file picker,
    HaloActivity.importMods) and DELETE (the mod chosen, at a second press)"""
    base = MODS
    lines = _widget("main_menu/main_menu_item_mods",
                    [("type", "text"), ("width", 256), ("height", 33), ("bitmap", "main_menu/menu_mods")],
                    [f'<on event="a" open="{base}/mods_screen"/>', f'<on event="start" open="{base}/mods_screen"/>',
                     '<on event="left_mouse" run="mouse emit accept event"/>'])
    lines += _widget(f"{base}/mods_screen", [("width", 640), ("height", 480),
                                             ("flags", "pass_unhandled_to_focused_child pause_game"),
                                             ("bitmap", "bitmaps/gradient")],
                     ['<on event="b" back="true"/>', '<on event="back" back="true"/>',
                      '<child widget="main_menu/new_select/sel_list_desc_bkd"/>',
                      f'<child widget="{base}/mods_list"/>',
                      f'<child widget="{base}/header_mods"/>',
                      # (the Switch's and the PS5's buttons as hints where the
                      # button bar's are, which a touchscreen keeps)
                      *[f'<child widget="{base}/{key}" x="{x}" y="{y}" platform="{platform}"/>'
                        for key, x, y, platform in MOD_HINT_PLACES]])
    lines += _header(f"{base}/header_mods", f"{base}/header_mods")
    # (the lobby's rows, MOD_ROWS of them, scrolling over more mods; the
    # last not on Android, where IMPORT and DELETE are)
    rows = [f'<child{attributes([("widget", f"{base}/op_mod_{index + 1}"), ("x", 20), ("y", 73 + 30 * index), ("platform", "desktop switch ps5 web" if index == MOD_ROWS - 1 else None)])}/>'
            for index in range(MOD_ROWS)]
    lines += _widget(f"{base}/mods_list", [("type", "column_list"), ("width", 640), ("height", 480),
                                           ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                                           ("description", f"{base}/mod_details")],
                     ['<data input="port mods update"/>', '<on event="created" run="port mods init"/>',
                      '<on event="deleted" run="port mods close"/>',
                      # (the north face button - Y, a Switch's X, a PS5's
                      # Triangle - turns the mod chosen on or off, the
                      # shoulders move it: the list's, whatever row has the
                      # focus)
                      '<on event="y" run="port mods toggle"/>', '<on event="white" run="port mods move up"/>',
                      '<on event="black" run="port mods move down"/>', *rows,
                      f'<child widget="{base}/button_bar_files" y="384" platform="android"/>',
                      f'<child widget="{base}/button_bar" y="414"/>'])
    for index in range(MOD_ROWS):
        lines += _widget(f"{base}/op_mod_{index + 1}",
                         [("width", 390), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                          ("bitmap", "bitmaps/sel_list_item_bkd_top" if index == 0 else "bitmaps/sel_list_item_bkd"),
                          ("color", "#FF2896FF")],
                         # (A: on the Switch and the PS5 OK, elsewhere, and a
                         # tap, on or off: menu_functions.c's mods_accept)
                         ['<on event="a" run="port mods accept"/>', '<on event="start" run="port mods accept"/>',
                          '<on event="left" run="port mods move up"/>', '<on event="right" run="port mods move down"/>',
                          '<on event="left_mouse" run="mouse emit accept event"/>',
                          f'<child widget="{base}/mod_status" x="8" y="2"/>',
                          f'<child widget="{base}/mod_name" x="34"/>',
                          f'<child widget="{base}/mod_state" x="318"/>'])
    lines += _widget(f"{base}/mod_status", [("controller", 1), ("width", 24), ("height", 24),
                                            ("bitmap", "mods/status")], [])
    lines += _widget(f"{base}/mod_name",
                     [("type", "text"), ("controller", 1), ("width", 284), ("height", 22), ("font", "ui\\large_ui"),
                      ("color", "#FF2896FF"), ("text_y", 4)], [])
    lines += _widget(f"{base}/mod_state",
                     [("type", "text"), ("controller", 1), ("width", 66), ("height", 22), ("font", "ui\\large_ui"),
                      ("color", "#FF2896FF"), ("align", "center"), ("text_y", 4), ("text_flags", "no_focus_test")], [])
    # (in the lobby's frame, sel_list_desc_bkd's: the mod chosen's name, the
    # icon of what is wrong with it, and the rest)
    lines += _widget(f"{base}/mod_details", [("width", 640), ("height", 480)],
                     [f'<child widget="{base}/mod_details_name"/>', f'<child widget="{base}/mod_details_status"/>',
                      f'<child widget="{base}/mod_details_text"/>'])
    lines += _widget(f"{base}/mod_details_name",
                     [("type", "text"), ("controller", 1), ("left", 412), ("top", 84), ("width", 200), ("height", 44),
                      ("font", "ui\\large_ui"), ("color", "#FFFFFFFF")], [])
    lines += _widget(f"{base}/mod_details_status", [("controller", 1), ("left", 411), ("top", 132), ("width", 16),
                                                    ("height", 16), ("bitmap", "mods/status_small")], [])
    lines += _widget(f"{base}/mod_details_text",
                     [("type", "text"), ("controller", 1), ("left", 412), ("top", 130), ("width", 200),
                      ("height", 272), ("font", "ui\\small_ui"), ("color", "#FF2896FF")], [])
    lines += _widget(f"{base}/button_bar",
                     [("type", "column_list"), ("width", 640), ("height", 28),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     [f'<child widget="{base}/button_up" y="1" platform="desktop android web"/>',
                      f'<child widget="{base}/button_down" x="130" y="1" platform="desktop android web"/>',
                      f'<child widget="{base}/button_ok" x="380" y="1" platform="desktop android web"/>',
                      '<child widget="common_button_cancel" x="510" y="1" platform="desktop android web"/>'])
    # (Android's, under MOVE UP and MOVE DOWN)
    lines += _widget(f"{base}/button_bar_files",
                     [("type", "column_list"), ("width", 640), ("height", 28),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     [f'<child widget="{base}/button_import" y="1"/>',
                      f'<child widget="{base}/button_delete" x="130" y="1"/>'])
    lines += _text_button(f"{base}/button_import", "IMPORT", ['<on event="a" run="port mods import"/>',
                                                             '<on event="start" run="port mods import"/>'])
    lines += _text_button(f"{base}/button_delete", "DELETE", ['<on event="a" run="port mods delete"/>',
                                                             '<on event="start" run="port mods delete"/>'])
    lines += _text_button(f"{base}/button_up", "MOVE UP", ['<on event="a" run="port mods move up"/>',
                                                          '<on event="start" run="port mods move up"/>'])
    lines += _text_button(f"{base}/button_down", "MOVE DOWN", ['<on event="a" run="port mods move down"/>',
                                                              '<on event="start" run="port mods move down"/>'])
    lines += _button(f"{base}/button_ok", 1, ['<on event="a" run="port mods save" back="true"/>',
                                              '<on event="start" run="port mods save" back="true"/>'])
    lines += _hint_widgets(base, MOD_HINTS, MOD_HINT_PICTURES)
    return lines


# ---------- the Theater (port/linux/REPLAY.md): the main menu's THEATER,
# the replays a host or a client records (port/linux/game/network_replay.c),
# laid out as the Mods screen is: on the left the replays, a row each, the
# newest first (its map and when, and its length), scrolling over more, A
# playing the one chosen (network_replay_playback.c); on the right, in the
# lobby's frame, what the one chosen is; and the buttons: RECORD (ON or OFF,
# network.record_replays), DELETE (the one chosen, at a second press), IMPORT
# and EXPORT (Android's: the app's file and folder pickers,
# HaloActivity.importReplays and exportReplays) and BACK (menu_functions.c's
# "port theater" functions). The Switch and the PS5 have
# fewer rows: under them a QR code of their replay page
# (network_replay_page.c)

THEATER_ROWS = 10
THEATER_QR_ROWS = 6
THEATER_ROW_TOP = 73


def _theater() -> list:
    base = THEATER
    lines = _widget("main_menu/main_menu_item_theater",
                    [("type", "text"), ("width", 256), ("height", 33), ("bitmap", "main_menu/menu_theater")],
                    [f'<on event="a" open="{base}/theater_screen"/>', f'<on event="start" open="{base}/theater_screen"/>',
                     '<on event="left_mouse" run="mouse emit accept event"/>'])
    lines += _widget(f"{base}/theater_screen", [("width", 640), ("height", 480),
                                                ("flags", "pass_unhandled_to_focused_child pause_game"),
                                                ("bitmap", "bitmaps/gradient")],
                     ['<on event="b" back="true"/>', '<on event="back" back="true"/>',
                      '<child widget="main_menu/new_select/sel_list_desc_bkd"/>',
                      f'<child widget="{base}/theater_list"/>',
                      f'<child widget="{base}/header_theater"/>',
                      f'<child widget="{base}/theater_qr_text" platform="switch ps5"/>',
                      # (the consoles' RECORD and DELETE: hints of the north
                      # and west buttons where the button bar's would be,
                      # which a touchscreen keeps; theater_list's bar at 414)
                      f'<child widget="{base}/theater_record_hint" x="8" y="415" platform="switch ps5"/>',
                      f'<child widget="{base}/theater_delete_hint" x="128" y="415" platform="switch ps5"/>',
                      f'<child widget="{base}/theater_back_hint" x="510" y="415" platform="switch ps5"/>'])
    lines += _header(f"{base}/header_theater", f"{base}/header_theater")
    rows = []
    for index in range(THEATER_ROWS):
        rows.append(f'<child{attributes([("widget", f"{base}/op_replay_{index + 1}"), ("x", 20), ("y", THEATER_ROW_TOP + 30 * index), ("platform", "desktop android web" if index >= THEATER_QR_ROWS else None)])}/>')
    lines += _widget(f"{base}/theater_list", [("type", "column_list"), ("width", 640), ("height", 480),
                                              ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                                              ("description", f"{base}/theater_details")],
                     ['<data input="port theater update"/>',
                      # (the west face button - X, a Switch's Y, a PS5's
                      # Square - deletes the replay chosen, asking first; the
                      # north - Y, a Switch's X, a PS5's Triangle - turns
                      # RECORD on or off: the list's, as the main menu's X is,
                      # whatever row has the focus)
                      '<on event="x" run="port theater delete"/>',
                      '<on event="y" run="port theater record"/>',
                      *rows, f'<child widget="{base}/button_bar" y="414"/>'])
    for index in range(THEATER_ROWS):
        lines += _widget(f"{base}/op_replay_{index + 1}",
                         [("width", 390), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                          ("bitmap", "bitmaps/sel_list_item_bkd_top" if index == 0 else "bitmaps/sel_list_item_bkd"),
                          ("color", "#FF2896FF")],
                         ['<on event="a" run="port theater watch"/>', '<on event="start" run="port theater watch"/>',
                          '<on event="left_mouse" run="mouse emit accept event"/>',
                          f'<child widget="{base}/theater_replay_name" x="12"/>',
                          f'<child widget="{base}/theater_replay_length" x="290"/>'])
    lines += _widget(f"{base}/theater_replay_name",
                     [("type", "text"), ("controller", 1), ("width", 276), ("height", 22), ("font", "ui\\large_ui"),
                      ("color", "#FF2896FF"), ("text_y", 4)], [])
    lines += _widget(f"{base}/theater_replay_length",
                     [("type", "text"), ("controller", 1), ("width", 90), ("height", 22), ("font", "ui\\large_ui"),
                      ("color", "#FF2896FF"), ("align", "right"), ("text_y", 4), ("text_flags", "no_focus_test")], [])
    # (the Switch's and the PS5's: the replay page's QR code, drawn over this
    # line by menu_functions.c, network_replay_page.c)
    lines += _widget(f"{base}/theater_qr_text",
                     [("type", "text"), ("controller", 1), ("left", 20), ("top", 392), ("width", 390), ("height", 18),
                      ("text", "Scan it to download or upload replays."), ("font", "ui\\small_ui"),
                      ("color", "#FFFFFFFF"), ("align", "center"), ("text_flags", "no_focus_test")], [])
    # (in the lobby's frame, sel_list_desc_bkd's, as the mods' details)
    lines += _widget(f"{base}/theater_details", [("width", 640), ("height", 480)],
                     [f'<child widget="{base}/theater_details_text"/>'])
    lines += _widget(f"{base}/theater_details_text",
                     [("type", "text"), ("controller", 1), ("left", 412), ("top", 84), ("width", 200),
                      ("height", 318), ("font", "ui\\small_ui"), ("color", "#FF2896FF")], [])
    lines += _widget(f"{base}/button_bar",
                     [("type", "column_list"), ("width", 640), ("height", 28),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     # (five buttons of 128 across the 640: Android's IMPORT
                     # and EXPORT between DELETE and BACK)
                     [f'<child widget="{base}/button_record" y="1" platform="desktop android web"/>',
                      f'<child widget="{base}/button_delete" x="128" y="1" platform="desktop android web"/>',
                      f'<child widget="{base}/button_import" x="256" y="1" platform="android"/>',
                      f'<child widget="{base}/button_export" x="384" y="1" platform="android"/>',
                      f'<child widget="{base}/button_back" x="512" y="1" platform="desktop android web"/>'])
    # (its words a string list's, OFF and ON, which "port theater update"
    # chooses between: a text widget's own words are its string list's again
    # each time it is drawn, so words set on it would not show)
    lines += _widget(f"{base}/button_record",
                     [("type", "text"), ("width", 128), ("height", 24), ("bitmap", "bitmaps/text_button_background"),
                      ("string_list", f"{base}/record_strings"), ("string_index", 0), ("font", "ui\\small_ui"),
                      ("color", "#FFFFFFFF"), ("align", "center"), ("text_y", 2)],
                     ['<on event="a" run="port theater record"/>', '<on event="start" run="port theater record"/>',
                      '<on event="left_mouse" run="mouse emit accept event"/>'])
    lines += _strings(f"{base}/record_strings", ["RECORD: OFF", "RECORD: ON"])
    lines += _text_button(f"{base}/button_delete", "%x-button DELETE", ['<on event="a" run="port theater delete"/>',
                                                                      '<on event="start" run="port theater delete"/>'])
    # (its words a string list's, as button_record's: "port theater update"
    # chooses OFF or ON)
    lines += _widget(f"{base}/theater_record_hint", [("type", "text"), ("controller", 1), ("width", 150),
                                                     ("height", 24), ("string_list", f"{base}/record_hint_strings"),
                                                     ("string_index", 0), ("font", "ui\\small_ui"),
                                                     ("color", "#FFFFFFFF"), ("text_y", 2),
                                                     ("text_flags", "no_focus_test")], [])
    lines += _strings(f"{base}/record_hint_strings", ["%y-button RECORD: OFF", "%y-button RECORD: ON"])
    lines += _widget(f"{base}/theater_back_hint", [("type", "text"), ("controller", 1), ("width", 128),
                                                   ("height", 24), ("text", "%b-button BACK"),
                                                   ("font", "ui\\small_ui"), ("color", "#FFFFFFFF"),
                                                   ("align", "center"), ("text_y", 2),
                                                   ("text_flags", "no_focus_test")], [])
    lines += _widget(f"{base}/theater_delete_hint", [("type", "text"), ("controller", 1), ("width", 200),
                                                     ("height", 24), ("text", "%x-button DELETE"),
                                                     ("font", "ui\\small_ui"), ("color", "#FFFFFFFF"),
                                                     ("text_y", 2),
                                                     ("text_flags", "no_focus_test")], [])
    lines += _text_button(f"{base}/button_import", "IMPORT", ['<on event="a" run="port theater import"/>',
                                                             '<on event="start" run="port theater import"/>'])
    lines += _text_button(f"{base}/button_export", "EXPORT", ['<on event="a" run="port theater export"/>',
                                                             '<on event="start" run="port theater export"/>'])
    lines += _text_button(f"{base}/button_back", "BACK", ['<on event="a" back="true"/>',
                                                         '<on event="start" back="true"/>'])
    return lines
