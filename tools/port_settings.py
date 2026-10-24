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
from typing import NamedTuple
from xml.sax.saxutils import quoteattr

PE = "main_menu/settings_select/player_setup/player_profile_edit"
# the mods' screen, and its rows (_mods); the main menu's items
# (ce/main_menu.xml): MODS in Credits' place (Credits is left out:
# tools/ce_menus.py's LEFT_OUT), Quit a row below it (its CHILD_OFFSETS)
MODS = "main_menu/mods"
MOD_ROWS = 11
MODS_ITEM_PLACE = (192, 391)
QUIT_ITEM_PLACE = (192, 427)
# the update question (the Steam Frame's, gamescope's), over the main menu (_update)
UPDATE = "main_menu/update"
YES_NO = [("YES", "true"), ("NO", "false")]
ON_OFF = [("ON", "true"), ("OFF", "false")]
# the multiplayer armor colors, in the profile's order (player_profile.c)
SHIELD_COLORS = ["white", "black", "red", "blue", "gray", "yellow", "green", "pink", "purple", "cyan", "cobalt",
                 "orange", "teal", "sage", "brown", "tan", "maroon", "salmon"]
# input.mouse_vertical_sensitivity's: a multiplier of the horizontal
VERTICAL_SENSITIVITIES = [(f"{value:g}X", f"{value:g}") for value in (0.25, 0.5, 0.6, 0.7, 0.75, 0.8, 0.9, 1, 1.1, 1.25,
                                                                       1.5, 2)]
VOLUMES = [(str(step), f"{step / 10:g}") for step in range(11)]
# the platforms a menu element's platform attribute names (menu_files.c's
# MENU_PLATFORM, "vr": the VR build, playing in the headset, and "hdr": a
# renderer that draws display.hdr)
MENU_PLATFORMS = ("desktop", "android", "web", "vr", "hdr", "touch", "pointer", "computer")
# display.hdr_paper_white's values, percent of the system's SDR white
PAPER_WHITES = (50, 60, 70, 80, 90, 100, 110, 125, 150, 175, 200, 250)

# the old settings screens' layout (rows of a label and a spinner over their
# row backgrounds, a line of help for the row chosen, Defaults, OK and Cancel),
# which VR Setup keeps: each its folder below PE, its screen's widget, its
# header (widget, bitmap), the row spacing, and its rows (label, setting,
# [(shown, value)], help, platform[, key])
SCREENS = {}

# ---------- VR Setup (the VR build's, HALO_VR: port/linux/src/vr.c), its
# item in the profile menu only while the game runs in VR (platform "vr").
# Two screens: what the hands do, then where things are. Its settings are
# read again as config.toml changes (vr.c, settings_read), but those marked
# RESTART, which open the headset's session. A row with a screen in place
# of a setting opens it (VR_LINK), after saving this screen's.
# (A value added to a setting is one more (shown, value) pair here, then
# python3 tools/port_settings.py writes the screens' files.)

RESTART = "(RESTART)"
VR_HANDS = [("ARMS", "arms"), ("FLOATING HANDS", "floating"), ("GAME ARMS", "game")]
VR_AIM = [("CONTROLLER", "controller"), ("HEAD", "head"), ("GAMEPAD", "gamepad")]
VR_DISPLAY = f"{PE}/vr_display/vr_display_screen"
VR_VISOR = f"{PE}/vr_visor/vr_visor_screen"
VR_SCREENS = {
    "vr_controls": {
        "screen": "vr_controls_screen",
        "header": ("header_vr_controls", f"{PE}/vr_controls/header_vr_controls"),
        # (sixteen rows: closer than the others' 28px, the help line lower)
        "spacing": 18,
        "help_top": 366,
        "rows": [
            ("HANDS:", "vr.hands", VR_HANDS,
             "Aiming with the controller, your hands: with arms\nfrom the shoulders, floating, or the game's arms.",
             None),
            ("AIM:", "vr.aim", VR_AIM,
             "What aims: the right controller, which holds the\nweapon, your head, or a gamepad's right stick.", None),
            ("GAMEPAD VIEW:", "vr.gamepad_view", [("CAMERA", "camera"), ("LEVEL", "level"), ("SNAP", "snap")],
             "With a gamepad, the right stick turns and\npitches the view, only turns it, or in steps.", None),
            ("TWO-HANDED AIM:", "vr.two_handed", ON_OFF,
             "The left grip holds a long gun by its foregrip:\nit aims along the line between your hands.", None),
            ("AIM ASSIST:", "vr.aim_assist", [("OFF", "0"), ("WEAK", "0.5"), ("ON", "1"), ("STRONG", "1.5")],
             "Near an enemy your aim is slow to leave it, and\nzoomed, it is drawn gently toward it.", None),
            ("SWING TO MELEE:", "vr.melee_gesture", ON_OFF,
             "A hard swing of the right controller melees, as\nB does; with its grip held, a softer one will do.", None),
            ("HOLSTERS:", "vr.holsters", ON_OFF,
             "Reach over your right shoulder for a long gun, or\nto your right hip for a pistol, and grip to draw it.",
             None),
            ("PHYSICAL CROUCH:", "vr.physical_crouch", ON_OFF,
             "Crouch in the room and you crouch in the game;\nstand up and you stand.", None),
            ("SCOPES:", "vr.scope_view", [("OFF", "off"), ("SMART-LINK", "smartlink"), ("AT THE EYE", "eye")],
             "Smart-link: the right stick's click zooms on your\nvisor. At the eye: raise the scope to your eye.", None),
            ("RELOAD BY HAND:", "vr.manual_reload", ON_OFF,
             "Take a magazine from your left hip and push it\nin: loaded at once. X still reloads.", None),
            ("TURNING:", "vr.turn", [("SNAP", "snap"), ("SMOOTH", "smooth")],
             "How the right stick turns you: in steps, or\ncontinuously.", None),
            ("SNAP TURN ANGLE:", "vr.snap_turn_angle",
             [(f"{angle} DEGREES", str(angle)) for angle in (15, 22.5, 30, 45, 60, 90)],
             "How far each snap turn turns you.", None),
            ("SMOOTH TURN SPEED:", "vr.smooth_turn_speed",
             [(f"{speed} DEG/S", str(speed)) for speed in (60, 90, 120, 150, 180, 240, 300, 360)],
             "How fast smooth turning turns you, the stick\npushed all the way: degrees a second.", None),
            ("VEHICLE VIEW:", "vr.vehicle_view", [("FIRST PERSON", "first_person"), ("THIRD PERSON", "third_person")],
             "In a vehicle's seat, the view from your head, or\nthe game's camera following the vehicle.", None),
            ("DEATH VIEW:", "vr.death_view", [("FACE YOUR BODY", "face"), ("STILL", "still"), ("GAME'S CAMERA", "game")],
             "As you die, the view stays where your head was\n(turned to your body in a blink), or the game's.",
             None),
            ("HAND, HEIGHT, HUD...", None, VR_DISPLAY,
             "The hand that holds the weapon, your height, the\nworld's scale, the HUD, the menus, the resolution.",
             None),
        ],
    },
    "vr_display": {
        "screen": "vr_display_screen",
        "header": ("header_vr_display", f"{PE}/vr_display/header_vr_display"),
        # (twelve rows: closer than the others' 28px, the help line lower)
        "spacing": 24,
        "help_top": 366,
        "rows": [
            ("DOMINANT HAND:", "vr.dominant_hand", [("RIGHT", "right"), ("LEFT", "left")],
             "The hand that holds the weapon. Left: mirrored,\nthe holsters at your left, the magazines at your right.",
             None),
            ("SWAP STICKS:", "vr.swap_sticks", [("AS THE HAND", "auto"), ("NO", "no"), ("YES", "yes")],
             "Move with the right stick and turn with the left:\nby default, with the left hand dominant.", None),
            (f"POSITION {RESTART}:", "vr.height", [("SEATED", "seated"), ("STANDING", "standing")],
             "Seated: your eyes where they are when you\nrecentre. From the next time the game starts.", None),
            ("EYE HEIGHT:", "vr.player_height",
             [(f"{height / 100:.2f} M", f"{height / 100:g}") for height in range(140, 205, 5)],
             "Standing, the height of your eyes above the\nfloor.", None),
            ("WORLD SCALE:", "vr.world_scale",
             [(f"{scale:g}X", f"{scale:g}") for scale in (0.75, 0.8, 0.9, 1, 1.1, 1.2, 1.25, 1.5)],
             "How large the world looks: more than 1 makes it\nlarger, and you smaller in it.", None),
            ("HUD DISTANCE:", "vr.hud_distance",
             [(f"{distance:.1f} M", f"{distance:g}") for distance in (1, 1.5, 2, 2.5, 3, 4, 5)],
             "How far in front of your eyes the HUD is.", None),
            ("HUD SIZE:", "vr.hud_size",
             [(f"{size} DEGREES", str(size)) for size in (40, 50, 60, 70, 80, 90, 100)],
             "How wide the HUD is, in degrees of your view.", None),
            ("MENU DISTANCE:", "vr.menu_distance",
             [(f"{distance:.1f} M", f"{distance:g}") for distance in (1.5, 2, 2.5, 3, 3.5, 4)],
             "How far in front of you the menus are.", None),
            ("MENU WIDTH:", "vr.menu_width",
             [(f"{width:.1f} M", f"{width:g}") for width in (1.6, 2, 2.3, 2.6, 3, 3.5, 4)],
             "How wide the menus are.", None),
            (f"RESOLUTION {RESTART}:", "vr.resolution_scale",
             [(f"{round(scale * 100)}%", f"{scale:g}") for scale in (0.5, 0.6, 0.75, 0.9, 1, 1.1, 1.25, 1.5)],
             "Of the size the headset recommends: less is\nfaster, more sharper. From the next start.", None),
            (f"REFRESH RATE {RESTART}:", "vr.refresh_rate",
             [("HEADSET'S", "0"), *((f"{rate} HZ", str(rate)) for rate in (72, 80, 90, 120, 144))],
             "The headset's rate, if it offers it; the game\ndraws a frame each refresh. From the next start.",
             None),
            ("HUD AND VISOR...", None, VR_VISOR,
             "The HUD on your helmet's visor: its depths, its\nglass and glow, the helmet's rim, the shields' energy.",
             None),
        ],
    },
}
# the visor's page of VR Display (titled as it is: its title's picture is
# VR Display's), every value of it read again as it changes (vr_visor.c)
VR_SCREENS["vr_visor"] = {
    "screen": "vr_visor_screen",
    "header": ("header_vr_display", f"{PE}/vr_display/header_vr_display"),
    # (twelve rows: closer than VR Display's 28px, the help line lower)
    "spacing": 24,
    "help_top": 366,
    "rows": [
        ("HUD:", "vr.hud", [("VISOR", "visor"), ("FLAT", "flat")],
         "On your helmet's visor, curved about your eyes,\nor a flat screen before them.", None),
        ("HUD DEPTH:", "vr.hud_depth",
         [("NONE", "0"), *((f"{depth:g}X", f"{depth:g}") for depth in (0.5, 0.75, 1, 1.25, 1.5, 2))],
         "How far apart in depth the HUD's parts are: the\ntracker nearest, the waypoints farthest.", None),
        ("HUD WRAP:", "vr.hud_wrap",
         [("NONE", "0"), *((f"{round(wrap * 100)}%", f"{wrap:g}") for wrap in (0.5, 1, 1.5, 2))],
         "The HUD's corners turned out along the visor's\ncurve, toward the edges of your view.", None),
        ("HUD LAG:", "vr.hud_lag",
         [("NONE", "0"), *((f"{round(lag * 1000)} MS", f"{lag:g}") for lag in (0.02, 0.03, 0.05, 0.08, 0.12))],
         "How far the HUD trails your head's turns.", None),
        ("HUD GLOW:", "vr.hud_glow",
         [("NONE", "0"), *((f"{round(glow * 100)}%", f"{glow:g}") for glow in (0.25, 0.5, 0.75, 1))],
         "The HUD's glow on the visor's glass.", None),
        ("HUD ON GLASS:", "vr.hud_glass",
         [("NONE", "0"), *((f"{round(glass * 100)}%", f"{glass:g}") for glass in (0.25, 0.5, 0.75, 1))],
         "The HUD as light on the glass: the world through\nit, the glass's tint, its bloom and reflection.", None),
        ("HUD REACTS:", "vr.hud_react",
         [("NONE", "0"), *((f"{round(react * 100)}%", f"{react:g}") for react in (0.25, 0.5, 0.75, 1))],
         "More contrast in bright light, dimmer in the dark,\na flicker as your shields are hit.", None),
        ("HELMET RIM:", "vr.helmet_rim",
         [("NONE", "0"), *((f"{round(rim * 100)}%", f"{rim:g}") for rim in (0.25, 0.5, 0.75, 1))],
         "The helmet's faceplate about the visor, at the\nedges of your view, in depth.", None),
        ("RIM DEPTH:", "vr.helmet_rim_depth",
         [(f"{round(depth * 100)} CM", f"{depth:g}") for depth in (0.06, 0.08, 0.1, 0.12, 0.15, 0.2)],
         "How far before your eyes the helmet's rim is.", None),
        ("VISOR SHADE:", "vr.visor_frame",
         [("NONE", "0"), *((f"{round(frame * 100)}%", f"{frame:g}") for frame in (0.2, 0.4, 0.6, 0.8, 1))],
         "The helmet's shadow on the glass at the edges of\nyour view, and the glass's faint gold.", None),
        ("SHIELD EFFECTS:", "vr.visor_effects", ON_OFF,
         "The shields on the glass: their energy, the red\nwarning, the HUD's flicker as they break.", None),
        ("SHIELD ENERGY:", "vr.visor_energy",
         [(f"{round(energy * 100)}%", f"{energy:g}") for energy in (0.25, 0.5, 0.75, 1, 1.5, 2)],
         "How strong the shields' energy across the glass\nis, as they are hit and as they recharge.", None),
    ],
}
SCREENS.update(VR_SCREENS)
# the profile menu's VR Setup item (in VR only), below About, its
# description and picture the list's tenth (the video settings' picture)
VR_ITEM = f"{PE}/vr_setup_profile_item"
FRAMES_ADDED = {
    f"{PE}/profile_options": [
        '<frame png="ce/shell/main_menu/settings_select/player_setup/player_profile_edit/profile_options__5.png" '
        'width="512" height="256"/>'],
}

# ---------- Settings: its tabs and their pages (port/linux/game/menu_functions.c's
# "port settings ..." functions)
#
# Settings opens straight to its tabs, VIDEO to PROFILE across the top (LB and
# RB, Q and E, or a click switch them), each a page of rows under section
# headers, and on the right a panel saying what the row chosen does: its name,
# its value in large type and plain words. A row ending in "›" opens a page of
# its own (Advanced Graphics, Controller...), laid out the same, with its own
# title, OK and Cancel. A change is kept, on every tab and page, until an OK
# (Settings' or a page's) writes them all (config.toml's, and the profile's
# controller settings, name and colour, saved), or Settings' Cancel forgets
# them all; a page's Cancel forgets its own.
#
# Only the controller's settings, the name and the colour are the profile's
# ("profile.<field>"): their page and the PROFILE tab, with no profile, offer
# to make one there (CREATE PROFILE), and Settings opens without one.
#
# Each page is generated laid out for every row (its rows and section headers
# a step apart, the tabs' from SETTINGS_TOP, the pages' from PAGE_TOP), and
# menu_functions.c packs the rows shown each frame at the same steps: a row of
# another platform (left out as the menus load), or one hidden (in a game, with
# no profile, a row that follows another's value), leaves no gap, and a
# section with no row shown is hidden. A tab's page keeps the folder and the
# screen's name of the settings screen it replaces, which the PC version's
# profile menu (still generated, no longer opened) names.
#
# A row: (label, setting, [(shown, value)], help, platform[, key]); a link: (label,
# None, the screen it opens, help, platform[, key, function]); a section:
# (SECTION, title). platform is None for every platform, else the platforms
# (MENU_PLATFORMS) separated by spaces; key names the row's widgets where two
# rows set one setting, else the setting's name does. A help is plain words,
# wrapped to the panel by menu_functions.c; after a "|", a note in smaller
# words. (A value added to a setting is one more (shown, value) pair here,
# then python3 tools/port_settings.py writes the pages' files.)

SECTION = "section"


class Slider(NamedTuple):
    """a slider row's choices, in place of [(shown, value)]: its track runs
    from least to most, left and right step it by step (holding them steps
    on), a click on the track or the mouse dragged along it sets it, and A, or
    a click on its number, types a value (Enter keeps it) from typed_least to
    typed_most. menu_functions.c's settings_sliders has the same"""
    least: float
    most: float
    step: float
    typed_least: float
    typed_most: float


# a slider's track, its thumb and its number, in the value's column
SLIDER_TRACK_WIDTH, SLIDER_THUMB_WIDTH, SLIDER_TRACK_HEIGHT = 92, 8, 14
SLIDER_VALUE_X, SLIDER_VALUE_WIDTH, SLIDER_VALUE_HEIGHT = 94, 46, 20
# input.mouse_sensitivity's: CS2's sensitivity (port/linux/include/mouse_sensitivity.h)
MOUSE_SENSITIVITY = Slider(0.1, 10.0, 0.05, 0.01, 100.0)
# the geometry, in the 640x480 menu units (menu_functions.c's SETTINGS_*
# have the same): the rows' column and the panel beside it
SETTINGS_ROW_X, SETTINGS_ROW_WIDTH, SETTINGS_ROW_HEIGHT, SETTINGS_ROW_STEP = 28, 344, 20, 21
# (a section after rows has SETTINGS_SECTION_GAP more above it)
SETTINGS_SECTION_HEIGHT, SETTINGS_SECTION_STEP, SETTINGS_SECTION_GAP = 18, 20, 6
SETTINGS_TOP, PAGE_TOP = 104, 80
SETTINGS_TABS_TOP = 76
# (the tabs from SETTINGS_TABS_X, LB and RB, or Q and E, either side)
SETTINGS_TAB_WIDTH, SETTINGS_TAB_HEIGHT, SETTINGS_TAB_STEP, SETTINGS_TABS_X = 90, 22, 92, 40
SETTINGS_PANEL_X, SETTINGS_PANEL_WIDTH, SETTINGS_PANEL_BOTTOM = 388, 224, 400
# the value's column in a row, and the arrows either side of it
SETTINGS_SPINNER_X, SETTINGS_SPINNER_WIDTH = 188, 140
# the Quality row's pictures in the panel (its stills: SETTINGS_STILLS)
SETTINGS_STILL_WIDTH, SETTINGS_STILL_HEIGHT = 96, 60
SETTINGS_BLUE = "#FF2896FF"
SETTINGS_DIM = "#FF6F9CD0"
SETTINGS = "main_menu/settings"
SETTINGS_TITLE = f"{SETTINGS}/header_settings"
SETTINGS_FUNCTIONS = "port settings page"

DISPLAY_MODES = [("FULLSCREEN", "fullscreen"), ("BORDERLESS", "borderless"), ("WINDOWED", "windowed")]
FIELDS_OF_VIEW = [("DEFAULT", "0")] + [(str(n), str(n)) for n in range(80, 151, 5)]
ANTI_ALIASING_HELP = "Smooths jagged edges. FXAA and SMAA cost little; {} sharper and {} more of the graphics card."
STREAMER_LEVELS = [("OFF", "off"), ("SEVERE", "severe"), ("STRONG AND WORSE", "strong"), ("MILD AND WORSE", "mild")]
SEED_UPLOADS = (64, 128, 256, 512, 1024, 2048, 4096, 8192, 10240)
VR_CONTROLS = f"{PE}/vr_controls/vr_controls_screen"


def _page_screen(folder: str, screen: str) -> str:
    return f"{PE}/{folder}/{screen}"


# the pages that rows open (and the tabs'): folder below PE, screen, title
# (a <bitmap> name: TITLES), and for a tab its name
ADVANCED = ("video_settings/advanced", "advanced_graphics_screen")
FIRST_PERSON = ("video_settings/first_person", "first_person_screen")
CONTROLLER = ("gamepad_setup", "gamepad_setup_screen")
KEY_BINDINGS = ("controls_setup", "controls_settings_screen")
STREAMER = ("interface_settings/streamer_mode", "streamer_mode_screen")
# (making a profile: as the main menu's items do, its name asked, then the
# profile made and edited, menu_functions.c's settings_profile_new)
CREATE_PROFILE = ("CREATE PROFILE ›", None, None,
                  "Your name, colour and controller layout are kept in a player profile. Make one to set them; the "
                  "rest of Settings works without one.", None, "create_profile", "port settings new profile")

PAGES = {
    # ---- the tabs
    "video_settings": {
        "screen": "video_settings_screen", "tab": "VIDEO",
        "about": "The window, the frame rate and the graphics.",
        "rows": [
            (SECTION, "DISPLAY"),
            ("WINDOW", "display.mode", DISPLAY_MODES,
             "How the game takes the screen. Fullscreen sets the display to the game's resolution; borderless covers "
             "the screen with a window, so that other windows open over it at once; windowed is a window you can "
             "move and resize. F11 switches between the window and the screen.", "computer"),
            # (in Window Size's place, the one the display mode shown uses:
            # menu_functions.c's video_rows_show; port/linux/game/menu_tags.c
            # adds the display's resolutions, and puts the sizes that fit the
            # desktop in place of Window Size's own)
            ("RESOLUTION", "display.resolution", [("NATIVE", "native")],
             "What fullscreen and borderless draw at. Native is the display's own, the sharpest; a lower one is "
             "lighter on a slow computer, and softer. Fullscreen sets the display to it; borderless scales it.",
             "computer"),
            ("WINDOW SIZE", "display.window_size", [("1280 x 960", "1280x960")],
             "The window's size: 4:3 first, then 16:10, 16:9 and 21:9. Its edges can also be dragged.", "computer"),
            ("FRAME RATE LIMIT", "display.max_fps",
             [("AUTO", "0"), ("30", "30"), ("60", "60"), ("120", "120"), ("144", "144"), ("165", "165"),
              ("240", "240"), ("NONE", "-1")],
             "The most frames drawn a second while V-Sync is off. Auto is twice the display's refresh rate. More "
             "feels smoother and uses more power; None draws as fast as it can.", "desktop web"),
            ("VSYNC", "display.vsync", ON_OFF,
             "Waits for the display before showing each frame, so that the picture never tears. It adds a little "
             "input lag, and holds the frame rate to the display's refresh rate.", None),
            ("FIELD OF VIEW", "display.fov", FIELDS_OF_VIEW,
             "How wide you see on foot, in degrees across a 16:9 screen. Default keeps the Xbox's view; wider shows "
             "more around you.", None),
            ("FIRST PERSON ›", None, _page_screen(*FIRST_PERSON),
             "Your weapon's field of view and whether it is drawn, your legs and arms, your shield's colour, and the "
             "view from a vehicle's passenger seat.", None),
            (SECTION, "GRAPHICS"),
            # (Classic or Enhanced, CUSTOM while any setting it chooses is
            # another value: menu_functions.c's graphics_preset_show; choosing
            # either sets them all again, as config_write does)
            ("QUALITY", "display.graphics", [("ENHANCED", "enhanced"), ("CLASSIC", "classic"), ("CUSTOM", "custom")],
             "Enhanced: smoother edges, sharper textures and shadows, smoother lighting. Classic looks as the "
             "Xbox did.|Changing one shows CUSTOM.", None),
            ("SHADOW QUALITY", "display.shadow_resolution",
             [("XBOX", "128"), ("MEDIUM", "256"), ("HIGH", "512"), ("ULTRA", "1024")],
             "How sharp the shadows of characters, vehicles and weapons are. Xbox is the original's soft, blocky "
             "shadow; each step up is crisper, and costs a little more of the graphics card.", None),
            # (display.hdr's rows, on the renderers that draw it: menu_files.c's
            # "hdr")
            ("HDR", "display.hdr", [("AUTO", "auto"), ("ON", "on"), ("OFF", "off")],
             "Brighter highlights on an HDR display. Auto turns it on while the display shows HDR; a display "
             "without HDR shows the game as it was.", "hdr"),
            ("ADVANCED GRAPHICS ›", None, _page_screen(*ADVANCED),
             "Anti-aliasing, texture filtering, model detail, the lighting and the shields' glow, HDR's look, "
             "smooth motion and the tick rate.", None),
            (SECTION, "BATTERY"),
            # (display.battery_saver: port/linux/src/battery_saver.c)
            ("BATTERY SAVER", "display.battery_saver", [("OFF", "off"), ("ON", "on"), ("AUTO", "auto")],
             "Up to 60 frames a second and Classic graphics, for a longer battery and a cooler device. Auto turns "
             "it on while the device runs on its battery. Your own settings come back as it turns off.", None),
        ],
    },
    "audio_settings": {
        "screen": "audio_settings_screen", "tab": "AUDIO",
        "about": "The devices, the volumes, voice chat and the sound.",
        "rows": [
            (SECTION, "DEVICES"),
            # (port/linux/game/menu_tags.c adds the devices SDL finds)
            ("OUTPUT DEVICE", "audio.output_device", [("SYSTEM DEFAULT", "default")],
             "Where the game's sound and the other players' voices play.", "desktop"),
            ("INPUT DEVICE", "audio.input_device", [("SYSTEM DEFAULT", "default")],
             "The microphone voice chat listens to.", "desktop"),
            (SECTION, "VOLUME"),
            ("MASTER VOLUME", "audio.volume", VOLUMES, "The volume of everything the game plays.", None),
            ("MUSIC VOLUME", "audio.music_volume", VOLUMES, "The music's volume.", None),
            ("EFFECTS VOLUME", "audio.effects_volume", VOLUMES,
             "Every other sound: weapons, vehicles, the world and the characters' speech.", None),
            # voice chat: this machine's own (port/linux/game/network_voice.c)
            (SECTION, "VOICE CHAT"),
            ("VOICE CHAT", "audio.voice_chat",
             [("PUSH TO TALK", "push_to_talk"), ("OPEN MIC", "open_mic"), ("OFF", "off")],
             "Talk to the other players in network games: while Push to Talk is held (Key Bindings sets its key), "
             "whenever you speak, or never.", None),
            ("VOICE VOLUME", "audio.voice_volume", VOLUMES, "How loud the other players' voices are.", None),
            # (the reverb and the resampler: port/linux/src/dsound_sdl.c)
            (SECTION, "SOUND"),
            ("REVERB", "audio.reverb", ON_OFF,
             "Echoes sounds as the place you are in would, and muffles those behind walls, as the Xbox did. It "
             "costs a little of the processor.", None),
            ("RESAMPLER", "audio.resampler", [("CLEAN", "sinc"), ("CLASSIC", "linear")],
             "How sounds are converted to your speakers' rate. Clean is clear; Classic is brighter but harsher, and "
             "costs a little less.", None),
            ("SOUND", "audio.enabled", ON_OFF,
             "Turns all sound off or on, from the next time the game starts.", None),
        ],
    },
    # (CONTROLS: the mouse's rows, in Mouse Setup's place)
    "mouse_settings": {
        "screen": "mouse_settings_screen", "tab": "CONTROLS",
        "about": "The mouse, the controller and the keys.",
        "rows": [
            (SECTION, "MOUSE"),
            ("SENSITIVITY", "input.mouse_sensitivity", MOUSE_SENSITIVITY,
             "How far the view turns as the mouse moves, on Counter-Strike 2's scale: your CS2 number turns the "
             "same here. Drag the slider, or press left and right. Click the number (or press A) to type an exact "
             "value, e.g. 1.7, then press Enter.|No acceleration.",
             None),
            ("VERTICAL SENSITIVITY", "input.mouse_vertical_sensitivity", VERTICAL_SENSITIVITIES,
             "How far the view turns up and down, of how far it turns side to side. 1X turns it as far.", None),
            ("ZOOM AIM SPEED", "input.zoom_sensitivity", [("CS2", "cs2"), ("ORIGINAL", "halo")],
             "How fast you turn while zoomed in. CS2 matches Counter-Strike 2's scoped feel; Original is how Halo "
             "felt.|Mouse only: controllers always turn as Halo did.", None),
            ("INVERT MOUSE", "input.invert_mouse", YES_NO,
             "Moving the mouse forward looks down, as a flight stick does.", None),
            ("MOUSE AIM ASSIST", "input.mouse_aim_assist", ON_OFF,
             "Slows and drags your aim along with a target while you aim with the mouse, as it does with a "
             "controller.", None),
            ("INSTANT AIM", "display.direct_camera", ON_OFF,
             "Turns the view the moment the mouse moves, not up to two of the game's updates later. Off feels as "
             "the original did.", "computer web"),
            (SECTION, "CONTROLLER"),
            ("CONTROLLER ›", None, _page_screen(*CONTROLLER),
             "The look sensitivity and inversion, the button and stick layouts, and vibration, kept in your player "
             "profile.", None),
            (SECTION, "KEYBOARD"),
            ("KEY BINDINGS ›", None, _page_screen(*KEY_BINDINGS),
             "The keys and mouse buttons for each action, two for each.", None),
            ("BUTTON PROMPTS", "input.button_prompts",
             [("AUTO", "auto"), ("CONTROLLER", "controller"), ("KEYBOARD", "keyboard")],
             "The buttons the HUD's prompts show. Auto shows the keys or the controller's buttons, as you last "
             "played with.", None),
            # (VR Setup, the VR build's, while it plays in the headset)
            (SECTION, "VR"),
            ("VR SETUP ›", None, VR_CONTROLS,
             "Your hands, aiming, turning, your height, the HUD and the menus in VR.", "vr"),
        ],
    },
    "network_setup": {
        "screen": "network_settings_screen", "tab": "ONLINE",
        "about": "Internet play, sharing maps and updates.",
        "rows": [
            # (in a game, the rows that would drop it or are the main menu's
            # are hidden: menu_functions.c's settings_rows_show)
            (SECTION, "INTERNET"),
            ("INTERNET PLAY", "network.online", ON_OFF,
             "Host and join games over the internet with invite links, and find public games. Off keeps to the "
             "local network.", "desktop android web"),
            ("OPEN ROUTER PORT", "network.allow_upnp", ON_OFF,
             "Asks your router to open the game's port (UPnP), so that players can reach the games you host.",
             "desktop android"),
            ("JOIN FROM CLIPBOARD", "network.join_from_clipboard", ON_OFF,
             "Joins a game when you switch to the game with its invite link copied.", "desktop android"),
            # (port/linux/game/map_share.c: this machine's part in another's
            # game)
            (SECTION, "MAP SHARING"),
            ("DOWNLOAD MAPS", "network.map_download", ON_OFF,
             "Joining a game on a Custom Edition map you do not have downloads it from the players, if they "
             "share it.", "desktop android"),
            ("HELP SEND MAPS", "network.map_seed", ON_OFF,
             "In another player's game, sends its map to the players who join without it, as a torrent's peers "
             "do.", "desktop android"),
            ("UPLOAD LIMIT", "network.map_seed_upload",
             [(f"{kb} KB/S" if kb < 1024 else f"{kb // 1024} MB/S", str(kb)) for kb in SEED_UPLOADS],
             "The most of your upload that sending maps takes, so that the game keeps the rest.",
             "desktop android"),
            (SECTION, "UPDATES"),
            ("CHECK FOR UPDATES", "update.auto", ON_OFF,
             "Looks for a new version when the game starts.", "desktop android"),
        ],
    },
    "interface_settings": {
        "screen": "interface_settings_screen", "tab": "INTERFACE",
        "about": "The HUD, the players' names, the scoreboard and the chat.",
        "rows": [
            (SECTION, "HUD"),
            ("HIGH-RES HUD", "display.high_res_hud", ON_OFF,
             "Draws the HUD from sharp high-resolution redraws. Off draws the game's own pictures.", None),
            # (shown while HIGH-RES HUD is: menu_functions.c's
            # hud_button_style_row_show)
            ("BUTTON STYLE", "display.hud_button_style",
             [("CLASSIC", "classic"), ("GEMS", "gems"), ("DARK GEMS", "menu_gems")],
             "The buttons the HUD draws. Classic: plain Xbox buttons. Gems: shaded Xbox buttons. Dark Gems: the "
             "menus' button artwork.", None),
            ("HIGH-RES TEXT", "display.high_res_text", ON_OFF,
             "Draws text and titles with sharp high-resolution fonts. Off draws the game's own.", None),
            ("MATCH TIMER", "display.match_timer", ON_OFF,
             "Draws a clock at the bottom right in multiplayer, counting up from the start of the game.", None),
            (SECTION, "MULTIPLAYER"),
            ("PLAYER NAMES", "display.player_names",
             [("ALL", "all"), ("ALLIES", "allies"), ("ENEMIES", "enemies"), ("NONE", "none")],
             "In multiplayer, whose names are drawn above their heads.", None),
            ("NAME SIZE", "display.player_name_scale",
             [(f"{value:g}x", f"{value:g}") for value in (0.5, 0.75, 1, 1.25, 1.5, 2)],
             "How large the players' names are drawn.", None),
            ("SCOREBOARD", "display.scoreboard_team_layout", [("TEAMS", "teams"), ("BY SCORE", "score")],
             "A team game's scoreboard: a column for each team, or every player in order of score.", None),
            ("SCOREBOARD PANEL", "display.scoreboard_background", ON_OFF,
             "Draws a dark panel behind the scoreboard, for clearer text.", None),
            # (the text chat's: port/linux/game/network_chat.c)
            (SECTION, "CHAT"),
            ("TEXT CHAT", "display.text_chat", [("SHOW", "true"), ("HIDE", "false")],
             "Shows the text chat. Hide draws none of its lines, and its keys and D-pad Up open nothing.", None),
            ("CHAT TEXT", "display.chat_style", [("COLORS", "colors"), ("WHITE", "white")],
             "Colors draws the names in their team's colour; White draws it all white, as Custom Edition does.",
             None),
            ("STREAMER MODE ›", None, _page_screen(*STREAMER),
             "Hides offensive words in names and text from other players, maps and mods, as stars.", None),
        ],
    },
    "profile_settings": {
        "screen": "profile_settings_screen", "tab": "PROFILE",
        "about": "Your player profile: your name and your colour.",
        "rows": [
            # (with no profile, NAME and COLOR are hidden and CREATE PROFILE
            # shown; in a game, those whose screens are ui.map's: the name's
            # keyboard, the colours' pictures: settings_rows_show)
            (SECTION, "PLAYER"),
            ("NAME ›", None, f"{PE}/name_edit/profile_name_screen",
             "The name the other players see, kept in your player profile.", None, "name",
             "player profile change name"),
            ("COLOR ›", None, f"{PE}/color_edit/color_select_screen",
             "Your armour's colour in free-for-all multiplayer games, kept in your player profile.", None, "color"),
            CREATE_PROFILE,
            (SECTION, "ABOUT"),
            ("OPENH1", None, None,
             "Halo: Combat Evolved, the Xbox game, on this computer, played from your own copy of its maps. OpenH1 "
             "is a free fan project.", None, "about"),
        ],
    },
    # ---- the pages the rows open
    ADVANCED[0]: {
        "screen": ADVANCED[1], "title": "ADVANCED GRAPHICS",
        "rows": [
            (SECTION, "IMAGE"),
            ("RENDER RESOLUTION", "display.resolution_scaling", [("FULL", "native"), ("XBOX 640X480", "original")],
             "Full draws the game at your screen's resolution. Xbox draws it at the Xbox's 640x480 and scales it "
             "up: softer, and very light on the graphics card.", "computer web"),
            ("ANTI-ALIASING", "display.anti_aliasing",
             [("OFF", "off"), ("FXAA", "fxaa"), ("SMAA", "smaa"), ("SSAA 2X", "ssaa2x"), ("MSAA 2X", "msaa2x"),
              ("MSAA 4X", "msaa4x"), ("MSAA 8X", "msaa8x")],
             ANTI_ALIASING_HELP.format("SSAA and MSAA are", "cost"), "desktop"),
            # (Android's Vulkan draws SMAA; the web's OpenGL ES has FXAA alone)
            ("ANTI-ALIASING", "display.anti_aliasing",
             [("OFF", "off"), ("FXAA", "fxaa"), ("SMAA", "smaa"), ("MSAA 2X", "msaa2x"), ("MSAA 4X", "msaa4x")],
             ANTI_ALIASING_HELP.format("MSAA is", "costs"), "android", "anti_aliasing_android"),
            ("ANTI-ALIASING", "display.anti_aliasing",
             [("OFF", "off"), ("FXAA", "fxaa"), ("MSAA 2X", "msaa2x"), ("MSAA 4X", "msaa4x")],
             "Smooths jagged edges. FXAA costs little; MSAA is sharper and costs more of the graphics card.",
             "web", "anti_aliasing_web"),
            ("TEXTURE FILTERING", "display.anisotropic_filtering",
             [("OFF", "0"), ("2X", "2"), ("4X", "4"), ("8X", "8"), ("16X", "16")],
             "Keeps floors and walls sharp into the distance, at a small cost. Off looks as the Xbox did: surfaces "
             "blur a few steps away.", None),
            ("MODEL DETAIL", "display.model_detail",
             [("AUTO", "auto"), ("XBOX", "stock"), ("FULL", "native"), ("HIGHEST", "highest")],
             "How detailed characters and vehicles stay in the distance. Xbox simplifies them as on a TV; Full keeps "
             "their detail for your screen; Highest always draws the most. Auto is Full in VR.", None),
            ("SMOOTH MOTION", "display.interpolation", ON_OFF,
             "Draws movement smoothly between the game's 30 updates a second, so that it looks fluid at any frame "
             "rate. Off shows each update as it is.", None),
            (SECTION, "LIGHTING AND HDR"),
            ("SMOOTH LIGHTING", "display.per_pixel_lighting", ON_OFF,
             "Lights characters and weapons smoothly, without the flat facets of the Xbox's lighting, at a small "
             "cost.", None),
            ("DYNAMIC LIGHTS", "display.dynamic_lights", [("CLASSIC", "classic"), ("ENHANCED", "enhanced")],
             "Enhanced: plasma bolts, muzzle flashes and blasts light their surroundings smoothly, with a glow. "
             "Classic lights them as the Xbox did.", "desktop android"),
            ("SHIELD GLOW", "display.shield_glow", ON_OFF,
             "Shields light their surroundings as they flare, in the shield's own colour. Online: the host's "
             "choice.", None),
            ("GLOW INTENSITY", "display.shield_glow_intensity", [("DEFAULT", "default"), ("LIGHT SHOW", "light_show")],
             "Light Show: the shields' glow reaches three times as far, and is brighter. Online: the host's choice.",
             None),
            ("HDR MODE", "display.hdr_mode", [("FAITHFUL", "faithful"), ("PUNCHY", "punchy")],
             "Faithful keeps the original picture and brightens its highlights. Punchy lifts the whole scene, as a "
             "remaster would.|The pictures are shown approximately: no SDR picture can show HDR.", "hdr"),
            ("HDR BRIGHTNESS", "display.hdr_paper_white", [(f"{value}%", str(value)) for value in PAPER_WHITES],
             "With HDR, how bright white surfaces and the HUD are, in percent of your system's white. Raise it in a "
             "bright room.", "hdr"),
            # (OpenCE's OVERSHIELD and HDR DISPLAY rows are left out: OpenH1's
            # renderers do not draw the overshield's rise, and HDR output is
            # display.hdr's own; port/linux/README.md)
            (SECTION, "GAME"),
            # (locked at ORIGINAL (30) in a game that plays at 30 whatever it
            # says, a network game of other machines, a film's playback:
            # menu_functions.c's sim_rate_row_show)
            ("TICK RATE", "game.sim_rate", [("ORIGINAL (30)", "30"), ("60", "60"), ("120", "120")],
             "How often the world updates each second, in games on this device: the campaign, split screen and "
             "bots. 60 and 120 respond faster and take more of the processor. Online games play at their host's "
             "tick rate: an OpenH1 game's (OpenH1 60 Tick), a Legacy game's 30.", None),
        ],
    },
    FIRST_PERSON[0]: {
        "screen": FIRST_PERSON[1], "title": "FIRST PERSON",
        "rows": [
            (SECTION, "VIEW"),
            ("WEAPON FIELD OF VIEW", "display.viewmodel_fov", FIELDS_OF_VIEW,
             "How wide your weapon and hands are drawn, in degrees across a 16:9 screen. Default keeps the "
             "weapon's own view.", None),
            ("WEAPON AND HANDS", "display.viewmodel_visible", [("SHOW", "true"), ("HIDE", "false")],
             "Draws your weapon, your hands and what they hold. Hide clears the view; you still fire, reload and "
             "hear them.", None),
            ("LEGS", "display.first_person_legs", ON_OFF,
             "Your legs when you look down, moving as you run: your body as the others see it.", None),
            ("PASSENGER VIEW", "display.passenger_third_person", [("BEHIND", "true"), ("SEAT", "false")],
             "Riding in a Warthog or a Scorpion in the campaign and co-op: the view from behind the vehicle, or "
             "from your seat. Behind, the weapon cannot zoom.", None),
            (SECTION, "SHIELDS"),
            ("ARM SHIELDS", "display.viewmodel_shield", ON_OFF,
             "Your shield's flare on your first-person arms too, as on your body.", None),
            ("SHIELD COLOR", "display.shield_color", [("DEFAULT", "default")] + [(c.upper(), c) for c in SHIELD_COLORS],
             "Your shield's flare in an armour colour, whatever your armour. Only you see it. Default keeps its "
             "own.", None),
        ],
    },
    CONTROLLER[0]: {
        "screen": CONTROLLER[1], "title": "CONTROLLER", "profile": True,
        "rows": [
            (SECTION, "LOOK"),
            ("LOOK SENSITIVITY", "profile.look_sensitivity", [(str(step), str(step)) for step in range(1, 11)],
             "How fast the right stick turns the view.", None),
            ("INVERT LOOK", "profile.invert_look", YES_NO, "Pushing the right stick up looks down.", None),
            ("INVERT FLIGHT", "profile.flight_inversion", YES_NO,
             "Flying vehicles pitch as aircraft do: pushing up flies down.", None),
            ("AUTO-CENTER LOOK", "profile.autocenter", YES_NO, "Levels the view while you walk.", None),
            (SECTION, "LAYOUT"),
            ("BUTTON LAYOUT", "profile.button_preset",
             [("DEFAULT", "0"), ("SWAP TRIGGERS", "1"), ("SWAP A, L TRIGGER", "2"), ("SWAP B, L TRIGGER", "3"),
              ("SWAP B, R STICK", "4"), ("BUMPER JUMPER", "5")],
             "Which of the controller's buttons does what. The keyboard's keys are Key Bindings'.", None),
            ("STICK LAYOUT", "profile.joystick_preset",
             [("DEFAULT", "0"), ("SOUTHPAW", "1"), ("LEGACY", "2"), ("LEGACY SOUTHPAW", "3")],
             "Which stick moves and which looks.", None),
            (SECTION, "FEEDBACK"),
            ("VIBRATION", "profile.vibration", ON_OFF, "Rumbles the controller.", None),
            ("IN-GAME HELP", "profile.ingame_help", ON_OFF, "Shows the game's hints about its controls.", None),
            (SECTION, "PROFILE"),
            CREATE_PROFILE,
        ],
    },
    STREAMER[0]: {
        "screen": STREAMER[1], "title": "STREAMER MODE",
        "rows": [
            # (port/linux/game/text_filter.c: words hidden in what other
            # players, maps and mods show, each group from a severity of Surge
            # AI's on, port/assets/text_filter)
            (SECTION, "STREAMER MODE"),
            ("STREAMER MODE", "streamer.enabled", ON_OFF,
             "Hides offensive words in names and text from other players, maps and mods, as stars.", None),
            (SECTION, "WORDS HIDDEN"),
            ("SLURS", "streamer.slurs", STREAMER_LEVELS,
             "Slurs about race, sexuality, gender and disability.", None),
            ("SWEARING", "streamer.swearing", STREAMER_LEVELS,
             "Swearing and sexual words. Strong and Worse leaves the mildest showing.", None),
            ("INSULTS", "streamer.insults", STREAMER_LEVELS,
             "Name-calling and insults that are not slurs.", None),
        ],
    },
}
SETTINGS_TAB_PAGES = [folder for folder, page in PAGES.items() if "tab" in page]
# the panel's words for the buttons, on a tab and on a page the rows open
SETTINGS_BUTTON_HELP = {
    "tab": ["Puts this tab's settings back to their defaults. Nothing changes until OK.",
            "Keeps your changes, on every tab, and applies them.",
            "Leaves Settings without keeping any change."],
    "page": ["Puts this page's settings back to their defaults.",
             "Saves your changes, on this page and every tab, and applies them.",
             "Goes back without these changes."],
}
# the pages' titles (TITLES): Settings', over every tab, and each page's
SETTINGS_TITLES = {SETTINGS_TITLE: "SETTINGS"}
SETTINGS_TITLES.update({f"{PE}/{folder}/header_{page['screen'][:-len('_screen')]}": page["title"]
                        for folder, page in PAGES.items() if "title" in page})
SETTINGS_TITLES[f"{PE}/controls_setup/header_key_bindings"] = "KEY BINDINGS"

# Controls Setup: the keyboard and mouse's actions, in groups (the order of
# port/linux/game/menu_functions.c's table of them)
CONTROL_GROUPS = ["MOVEMENT", "WEAPONS", "ACTIONS", "COMMUNICATION"]
CONTROL_ROWS = 7

# the settings whose rows have the wider spinner (a device's name)
WIDE_SETTINGS = {"audio.output_device", "audio.input_device", "streamer.slurs", "streamer.swearing",
                 "streamer.insults"}

# the host's voice chat and vote kicks, on Teamplay Options
# (_teamplay_options_extras): each row's key, its label, its choices' words
# (menu_functions.c's gametype_options give their settings' values) and
# their helps
TEAMPLAY_EDIT = "main_menu/settings_select/multiplayer_setup/teamplay_options_edit"
VOICE_QUALITIES = (8, 12, 16, 24, 32, 48, 64)
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
        "Halo: Combat Evolved, the Xbox\\ngame, on this computer.\\n\\nProfile:",
        # (VR Setup's, in VR only)
        "Your hands, aiming, turning,\\nthe HUD and the menus in VR.\\n\\nProfile:",
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
    lines += _widget(f"{base}/{spec['screen']}",
                     [("width", 640), ("height", 480), ("flags", "pass_unhandled_to_focused_child pause_game"),
                      ("bitmap", "bitmaps/gradient")],
                     ['<on event="b" back="true"/>', '<on event="back" back="true"/>',
                      f'<child{attributes([("widget", f"{base}/{header}")])}/>',
                      f'<child{attributes([("widget", f"{base}/options_menu")])}/>'])
    lines += _widget(f"{base}/{header}", [("controller", 1), ("left", 35), ("top", 11), ("width", 605), ("height", 59),
                                          ("bitmap", header_bitmap)], [])
    lines += _widget(f"{base}/help", [("type", "text"), ("controller", 1), ("left", 68),
                                      ("top", spec.get("help_top", 350)), ("width", 482),
                                      ("height", 60), ("string_list", f"{base}/help_strings"),
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
                      f'<child{attributes([("widget", f"{base}/button_ok"), ("x", 380), ("y", 1)])}/>',
                      '<child widget="common_button_cancel" x="510" y="1"/>'])
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
    # platforms, else a child for each place)
    places = {"desktop": -1, "android": -1, "web": -1}
    for index, (label, setting, choices, _, platform, *named) in enumerate(spec["rows"]):
        if setting is None:
            # (a row that opens another screen, choices: its widget)
            key = choices.rsplit("/", 1)[1]
            row = f"{base}/op_{key}"
            place += 1
            rows.append((row, platform, place))
            extra += _widget(row, [("width", 512), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                                   ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF"), ("platform", platform)],
                             [f'<on event="a" run="port settings save" open="{choices}"/>',
                              f'<on event="start" run="port settings save" open="{choices}"/>',
                              '<on event="left_mouse" run="mouse emit accept event"/>',
                              f'<child{attributes([("widget", f"{base}/{key}_label")])}/>'])
            extra += _widget(f"{base}/{key}_label",
                             [("type", "text"), ("controller", 1), ("width", 480), ("height", 22),
                              ("string_list", f"{base}/labels"), ("string_index", index), ("font", "ui\\large_ui"),
                              ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 4)], [])
            continue
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
                         ['<on event="created" run="port setting load"/>'])
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
    # (the help of the row whose label is string n is n + 1: the buttons' is 0)
    extra += _strings(f"{base}/help_strings",
                      [""] + [help_text.replace("\n", "\\n") for _, _, _, help_text, *_ in spec["rows"]] +
                      [help_text.replace("\n", "\\n") for _, _, help_text, *_ in spec.get("categories", ())])
    return _screen(folder, spec, rows, ["port settings help"], [], extra)


def _controls_screen() -> list:
    folder = "controls_setup"
    base = f"{PE}/{folder}"
    spec = {"screen": "controls_settings_screen", "spacing": 30,
            "header": ("header_key_bindings", f"{base}/header_key_bindings")}
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
        "Press a key, a mouse button or turn the wheel...\\nEscape cancels, Delete clears it.",
    ])
    return _screen(folder, spec, rows, ["controls update menu"], ['<on event="created" run="controls screen init"/>'],
                   extra)


def _vr_item() -> list:
    """the profile menu's VR Setup item, below About (as its items are)"""
    return _widget(VR_ITEM, [("type", "text"), ("left", 51), ("top", 375), ("width", 232), ("height", 32),
                             ("bitmap", "bitmaps/list_item_bkd"), ("text", "VR SETUP"), ("font", "ui\\large_ui"),
                             ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 5)],
                   [f'<on event="a" open="{PE}/vr_controls/vr_controls_screen"/>',
                    f'<on event="start" open="{PE}/vr_controls/vr_controls_screen"/>',
                    '<on event="left_mouse" run="mouse emit accept event"/>'])


def _settings_text(name: str, pairs: list, inner: list = ()) -> list:
    """a text widget of Settings' (the small font, unless pairs say)"""
    pairs = [("type", "text"), ("controller", 1)] + pairs
    if not any(key == "font" for key, _ in pairs):
        pairs.append(("font", "ui\\small_ui"))
    return _widget(name, pairs, list(inner))


def _settings_platform(rows: list, start: int):
    """a section's platforms: those of its rows (None: every platform)"""
    platforms = set()
    for entry in rows[start + 1:]:
        if entry[0] == SECTION:
            break
        if entry[4] is None:
            return None
        platforms.update(entry[4].split())
    return " ".join(name for name in MENU_PLATFORMS if name in platforms) or None


# the rows in the place of the row before them, each platform having one of
# them at most, or menu_functions.c showing one (video_rows_show): Window Size
# in Resolution's, Android's and the web's anti-aliasing in the desktop's
SETTINGS_SAME_PLACE = {"window_size", "anti_aliasing_android", "anti_aliasing_web"}


def _settings_page(folder: str, page: dict) -> list:
    """a tab of Settings, or a page a row opens: the title, the tabs (a
    tab's), the rows under their sections, the panel (the list's
    description), and Defaults, OK and Cancel"""
    base = f"{PE}/{folder}"
    tab = "tab" in page
    kind = "tab" if tab else "page"
    y = SETTINGS_TOP if tab else PAGE_TOP
    # (a tab's title is Settings'; a page's, its own: its widget named as
    # its picture)
    header = SETTINGS_TITLE if tab else f"{base}/header_{page['screen'][:-len('_screen')]}"
    cancel = "port settings cancel" if tab else "port settings page cancel"
    labels, helps, children, extra = [], [page.get("about", "")], [], []
    for index, entry in enumerate(page["rows"]):
        if entry[0] == SECTION:
            # (a section's title over its line, which the list passes over:
            # menu_functions.c's settings_rows_pack disables it)
            platform = _settings_platform(page["rows"], index)
            name = f"{base}/section_{index}"
            if labels:
                y += SETTINGS_SECTION_GAP
            children.append(f'<child{attributes([("widget", name), ("x", SETTINGS_ROW_X), ("y", y), ("platform", platform)])}/>')
            extra += _widget(name, [("width", SETTINGS_ROW_WIDTH), ("height", SETTINGS_SECTION_HEIGHT),
                                    ("bitmap", f"{SETTINGS}/section"), ("platform", platform)],
                             [f'<child widget="{name}_title"/>'])
            extra += _settings_text(f"{name}_title", [("width", SETTINGS_ROW_WIDTH), ("height", SETTINGS_SECTION_HEIGHT),
                                                      ("string_list", f"{base}/labels"),
                                                      ("string_index", len(labels) or None), ("color", SETTINGS_DIM),
                                                      ("text_x", 2), ("text_y", 1)])
            labels.append(entry[1])
            helps.append("")
            y += SETTINGS_SECTION_STEP
            continue
        label, setting, choices, help_text, platform, *named = entry
        # (a link's by its label: "ADVANCED GRAPHICS ›", advanced_graphics)
        key = named[0] if named else setting.split(".", 1)[1] if setting else \
            re.sub(r"[^a-z0-9]+", "_", label.lower()).strip("_")
        row = f"{base}/op_{key}"
        if key in SETTINGS_SAME_PLACE or setting == "display.window_size":
            y -= SETTINGS_ROW_STEP
        children.append(f'<child{attributes([("widget", row), ("x", SETTINGS_ROW_X), ("y", y), ("platform", platform)])}/>')
        y += SETTINGS_ROW_STEP
        label_widget = _settings_text(f"{base}/{key}_label",
                                      [("width", SETTINGS_SPINNER_X - 4 if setting else SETTINGS_ROW_WIDTH - 12),
                                       ("height", SETTINGS_ROW_HEIGHT), ("string_list", f"{base}/labels"),
                                       ("string_index", len(labels) or None), ("color", SETTINGS_BLUE),
                                       ("text_x", 10), ("text_y", 2)])
        row_pairs = [("width", SETTINGS_ROW_WIDTH), ("height", SETTINGS_ROW_HEIGHT),
                     ("flags", "pass_unhandled_to_focused_child"), ("bitmap", f"{SETTINGS}/row"),
                     ("color", SETTINGS_BLUE), ("platform", platform)]
        if setting and isinstance(choices, Slider):
            # (a slider: its track, the thumb on it, and its number, which
            # menu_functions.c's "port slider ..." set and its page update
            # places and shows; while its number is typed into, Enter is
            # Start and Escape B)
            run = "port slider"
            extra += _widget(row, row_pairs,
                             [f'<on event="left" run="{run} left"/>', f'<on event="right" run="{run} right"/>',
                              f'<on event="a" run="{run} edit"/>', f'<on event="start" run="{run} edit"/>',
                              '<on event="left_mouse" run="mouse emit accept event"/>',
                              f'<child widget="{base}/{key}_label"/>',
                              f'<child widget="{base}/{key}_track" x="{SETTINGS_SPINNER_X}" '
                              f'y="{(SETTINGS_ROW_HEIGHT - SLIDER_TRACK_HEIGHT) // 2}"/>',
                              f'<child widget="{base}/{key}_value" x="{SETTINGS_SPINNER_X + SLIDER_VALUE_X}" '
                              f'y="{(SETTINGS_ROW_HEIGHT - SLIDER_VALUE_HEIGHT) // 2}"/>'])
            extra += label_widget
            extra += _widget(f"{base}/{key}_track", [("controller", 1), ("width", SLIDER_TRACK_WIDTH),
                                                     ("height", SLIDER_TRACK_HEIGHT),
                                                     ("bitmap", f"{SETTINGS}/slider_track")],
                             [f'<child widget="{base}/{key}_thumb"/>'])
            extra += _widget(f"{base}/{key}_thumb", [("controller", 1), ("width", SLIDER_THUMB_WIDTH),
                                                     ("height", SLIDER_TRACK_HEIGHT),
                                                     ("bitmap", f"{SETTINGS}/slider_thumb")], [])
            # (its number in a field's frame, lit while typed into:
            # menu_functions.c's settings_sliders_show)
            extra += _settings_text(f"{base}/{key}_value", [("width", SLIDER_VALUE_WIDTH),
                                                            ("height", SLIDER_VALUE_HEIGHT),
                                                            ("bitmap", f"{SETTINGS}/slider_field"),
                                                            ("color", SETTINGS_BLUE), ("align", "center"),
                                                            ("text_y", 0),
                                                            ("text_flags", "no_focus_test")])
        elif setting:
            extra += _widget(row, row_pairs, [f'<child widget="{base}/{key}_label"/>',
                                              f'<child widget="{base}/{key}_spinner" x="{SETTINGS_SPINNER_X}"/>'])
            extra += label_widget
            extra += _widget(f"{base}/{key}_spinner",
                             [("type", "spinner"), ("top", 2), ("width", SETTINGS_SPINNER_WIDTH), ("height", 16),
                              ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
                              ("strings", "|".join(shown for shown, _ in choices)), ("setting", setting),
                              ("values", "|".join(value for _, value in choices)), ("font", "ui\\small_ui"),
                              ("color", SETTINGS_BLUE), ("align", "center"), ("text_y", 0),
                              ("header_bitmap", "bitmaps/arrow_sm_left"), ("footer_bitmap", "bitmaps/arrow_sm_right"),
                              ("header_bounds", "2 -6 14 0"),
                              ("footer_bounds", f"2 {SETTINGS_SPINNER_WIDTH} 14 {SETTINGS_SPINNER_WIDTH + 6}")],
                             ['<on event="created" run="port setting load"/>'])
        else:
            # (a row that opens a page, choices: its screen; run: a function
            # first; none: nothing to open)
            run = named[1] if len(named) > 1 else None
            handlers = []
            if choices or run:
                branch = "true" if run == "port settings new profile" else None
                handlers = [f'<on{attributes([("event", event), ("run", run), ("open", choices), ("branch", branch)])}/>'
                            for event in ("a", "start")]
                handlers.append('<on event="left_mouse" run="mouse emit accept event"/>')
                if branch:
                    handlers.append(f'<conditional widget="{SETTINGS}/settings_creating_profile" if_failed="true"/>')
            extra += _widget(row, row_pairs, handlers + [f'<child widget="{base}/{key}_label"/>'])
            extra += label_widget
        labels.append(label)
        helps.append(help_text)
    lines = []
    lines += _widget(f"{base}/{page['screen']}",
                     [("width", 640), ("height", 480), ("flags", "pass_unhandled_to_focused_child pause_game"),
                      ("bitmap", "bitmaps/gradient")],
                     [f'<on event="b" run="{cancel}" back="true"/>',
                      f'<on event="back" run="{cancel}" back="true"/>',
                      *(['<on event="white" run="port settings tab previous"/>',
                         '<on event="black" run="port settings tab next"/>'] if tab else []),
                      f'<child widget="{header}"/>',
                      *([f'<child widget="{SETTINGS}/tab_prompt_previous" x="6" y="{SETTINGS_TABS_TOP}"/>',
                         f'<child widget="{SETTINGS}/tab_prompt_next" '
                         f'x="{SETTINGS_TABS_X + 5 * SETTINGS_TAB_STEP + SETTINGS_TAB_WIDTH + 4}" '
                         f'y="{SETTINGS_TABS_TOP}"/>'] if tab else []),
                      f'<child widget="{base}/options_menu"/>'])
    if not tab:
        lines += _widget(header, [("controller", 1), ("left", 35), ("top", 11), ("width", 605), ("height", 59),
                                  ("bitmap", header)], [])
    # (the tabs last: up from the first row, down from the buttons)
    lines += _widget(f"{base}/options_menu",
                     [("type", "column_list"), ("width", 640), ("height", 480),
                      ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                      ("description", f"{SETTINGS}/{kind}_panel")],
                     [f'<data input="{SETTINGS_FUNCTIONS}"/>', *children,
                      f'<child widget="{base}/button_bar" y="414"/>',
                      *([f'<child widget="{base}/tabs" y="{SETTINGS_TABS_TOP}"/>'] if tab else [])])
    lines += _widget(f"{base}/button_bar",
                     [("type", "column_list"), ("width", 640), ("height", 28),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     [f'<child widget="{base}/button_defaults" y="1"/>',
                      f'<child widget="{base}/button_ok" x="380" y="1"/>',
                      f'<child widget="{base}/button_cancel" x="510" y="1"/>'])
    if tab:
        # (each tab: its name, the current one framed; A or a click opens it)
        items = []
        for index, other in enumerate(SETTINGS_TAB_PAGES):
            current = other == folder
            name = f"{base}/tab_{PAGES[other]['screen'][:-len('_screen')]}"
            items.append(f'<child widget="{name}" x="{SETTINGS_TABS_X + index * SETTINGS_TAB_STEP}"/>')
            lines += _settings_text(name, [("width", SETTINGS_TAB_WIDTH), ("height", SETTINGS_TAB_HEIGHT),
                                           ("bitmap", f"{SETTINGS}/{'tab_current' if current else 'tab'}"),
                                           ("string_list", f"{SETTINGS}/tab_names"), ("string_index", index or None),
                                           ("color", "#FFFFFFFF" if current else SETTINGS_BLUE),
                                           ("align", "center"), ("text_y", 3)],
                                    ['<on event="a" run="port settings tab"/>',
                                     '<on event="start" run="port settings tab"/>',
                                     '<on event="left_mouse" run="mouse emit accept event"/>'])
        lines += _widget(f"{base}/tabs", [("type", "column_list"), ("width", 640), ("height", SETTINGS_TAB_HEIGHT),
                                          ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")], items)
    lines += _button(f"{base}/button_defaults", 3, ['<on event="a" run="port settings defaults"/>',
                                                     '<on event="start" run="port settings defaults"/>'])
    ok = "port settings ok" if tab else "port settings keep"
    lines += _button(f"{base}/button_ok", 1, [f'<on event="a" run="{ok}" back="true"/>',
                                               f'<on event="start" run="{ok}" back="true"/>'])
    lines += _button(f"{base}/button_cancel", 2, [f'<on event="a" run="{cancel}" back="true"/>',
                                                   f'<on event="start" run="{cancel}" back="true"/>'])
    lines += extra
    lines += _strings(f"{base}/labels", labels)
    lines += _strings(f"{base}/help_strings", helps)
    return lines


def _settings_shared() -> list:
    """what every page of Settings shares: its title, the panels, the tabs'
    names, the buttons' words, and the screens that make a profile"""
    lines = _widget(SETTINGS_TITLE, [("controller", 1), ("left", 35), ("top", 11), ("width", 605), ("height", 59),
                                     ("bitmap", SETTINGS_TITLE)], [])
    for kind, top in (("tab", SETTINGS_TOP), ("page", PAGE_TOP)):
        # (menu_functions.c's settings_panel_show fills it in, and shows the
        # Quality row's pictures)
        height = SETTINGS_PANEL_BOTTOM - top
        # (a description's children are placed on the screen, not in it)
        left, still_y = SETTINGS_PANEL_X + 14, top + 52
        children = [f'<child widget="{SETTINGS}/panel_name" x="{left}" y="{top + 12}"/>',
                    f'<child widget="{SETTINGS}/panel_value" x="{left}" y="{top + 26}"/>']
        for index, still in enumerate(("classic", "enhanced")):
            x = SETTINGS_PANEL_X + 12 + index * (SETTINGS_STILL_WIDTH + 8)
            children += [f'<child widget="{SETTINGS}/panel_{still}" x="{x}" y="{still_y}"/>',
                         f'<child widget="{SETTINGS}/panel_{still}_frame" x="{x}" y="{still_y}"/>',
                         f'<child widget="{SETTINGS}/panel_{still}_caption" x="{x}" y="{still_y + SETTINGS_STILL_HEIGHT + 2}"/>']
        children += [f'<child widget="{SETTINGS}/panel_picture_{key}" '
                     f'x="{SETTINGS_PANEL_X + (SETTINGS_PANEL_WIDTH - SETTINGS_PICTURE_WIDTH) // 2}" y="{still_y}"/>'
                     for key in SETTINGS_VALUE_PICTURES]
        children += [f'<child widget="{SETTINGS}/panel_text" x="{left}" y="{top + 52}"/>',
                     f'<child widget="{SETTINGS}/panel_note" x="{left}" y="{top + height - 40}"/>']
        lines += _widget(f"{SETTINGS}/{kind}_panel",
                         [("controller", 1), ("left", SETTINGS_PANEL_X), ("top", top),
                          ("width", SETTINGS_PANEL_WIDTH), ("height", height),
                          ("bitmap", f"{SETTINGS}/{kind}_panel")], children)
    lines += _settings_text(f"{SETTINGS}/panel_name", [("width", 196), ("height", 18), ("color", SETTINGS_DIM)])
    lines += _settings_text(f"{SETTINGS}/panel_value", [("width", 196), ("height", 24), ("font", "ui\\large_ui"),
                                                       ("color", "#FFFFFFFF")])
    lines += _settings_text(f"{SETTINGS}/panel_text", [("width", 196), ("height", 220), ("color", SETTINGS_BLUE)])
    lines += _settings_text(f"{SETTINGS}/panel_note", [("width", 196), ("height", 40), ("color", SETTINGS_DIM)])
    for index, still in enumerate(("classic", "enhanced")):
        lines += _widget(f"{SETTINGS}/panel_{still}", [("controller", 1), ("width", SETTINGS_STILL_WIDTH),
                                                       ("height", SETTINGS_STILL_HEIGHT),
                                                       ("bitmap", f"{SETTINGS}/graphics_{still}")], [])
        lines += _widget(f"{SETTINGS}/panel_{still}_frame", [("controller", 1), ("width", SETTINGS_STILL_WIDTH),
                                                             ("height", SETTINGS_STILL_HEIGHT),
                                                             ("bitmap", f"{SETTINGS}/still_frame")], [])
        lines += _settings_text(f"{SETTINGS}/panel_{still}_caption",
                                [("width", SETTINGS_STILL_WIDTH), ("height", 18),
                                 ("string_list", f"{SETTINGS}/still_captions"), ("string_index", index or None),
                                 ("color", SETTINGS_BLUE), ("align", "center")])
    for key in SETTINGS_VALUE_PICTURES:
        lines += _widget(f"{SETTINGS}/panel_picture_{key}",
                         [("controller", 1), ("width", SETTINGS_PICTURE_WIDTH), ("height", SETTINGS_PICTURE_HEIGHT),
                          ("bitmap", f"{SETTINGS}/pictures_{key}")], [])
    # (making a profile, from CREATE PROFILE: its name asked, as the main
    # menu's items ask it ("new campaign chosen", settings_profile_new), the
    # profile made and edited ("new campaign decision", which edits it for
    # a screen of this name), then back to the page, which shows its rows)
    lines += _widget(f"{SETTINGS}/settings_creating_profile",
                     [("width", 640), ("height", 480), ("flags", "pass_unhandled_to_focused_child no_history"),
                      ("auto_close", 1)],
                     [f'<on event="deleted" run="new campaign decision" open="{SETTINGS}/settings_saving_profile"/>'])
    lines += _widget(f"{SETTINGS}/settings_saving_profile",
                     [("width", 640), ("height", 480), ("flags", "pass_unhandled_to_focused_child no_history"),
                      ("auto_close", 1000), ("auto_close_fade", 500)],
                     ['<on event="b" run="NULL"/>', '<on event="back" run="NULL"/>',
                      '<on event="deleted" run="NULL" back="true"/>',
                      '<child widget="error/warning_creating_player_profile_screen"/>'])
    # (LB and RB, or Q and E while the keyboard and mouse play, either side
    # of the tabs: the HUD's button pictures, display.hud_button_style, or
    # its key caps, ui_widget.c and hud_key_prompts.c)
    for name, icon in (("previous", "white"), ("next", "black")):
        lines += _settings_text(f"{SETTINGS}/tab_prompt_{name}", [("width", 30), ("height", SETTINGS_TAB_HEIGHT),
                                                                 ("text", f"%{icon}-button"), ("color", SETTINGS_BLUE), ("text_x", 2),
                                                                 ("text_y", 3)])
    lines += _strings(f"{SETTINGS}/tab_names", [PAGES[folder]["tab"] for folder in SETTINGS_TAB_PAGES])
    lines += _strings(f"{SETTINGS}/tab_about", [PAGES[folder]["about"] for folder in SETTINGS_TAB_PAGES])
    lines += _strings(f"{SETTINGS}/button_help", SETTINGS_BUTTON_HELP["tab"] + SETTINGS_BUTTON_HELP["page"])
    lines += _strings(f"{SETTINGS}/still_captions", ["CLASSIC", "ENHANCED"])
    return lines


def settings_files() -> dict:
    """the files of the port's screens, by their names in ce/"""
    files = {}
    for folder, spec in SCREENS.items():
        files[f"{PE}/{folder}".replace("/", ".") + ".xml"] = _setting_screen(folder, spec)
    for folder, page in PAGES.items():
        files[f"{PE}/{folder}".replace("/", ".") + ".xml"] = _settings_page(folder, page)
    files[SETTINGS.replace("/", ".") + ".xml"] = _settings_shared()
    files[f"{PE}/vr_controls".replace("/", ".") + ".xml"] += _vr_item()
    files[f"{PE}/controls_setup".replace("/", ".") + ".xml"] = _controls_screen()
    return {name: ['<?xml version="1.0" encoding="UTF-8"?>',
                   "<!-- The port's settings screen, in the PC version's style (tools/port_settings.py) -->",
                   "<menus>", *lines, "</menus>", ""] for name, lines in files.items()}


def replaced(folder: str) -> bool:
    """whether the PC version's folder of widgets is replaced by ours (and
    those below it: the controls' advanced screen, the video test)"""
    return any(folder == path or folder.startswith(f"{path}/")
               for path in [*(f"{PE}/{name}" for name in [*SCREENS, *PAGES, "controls_setup"]), *REPLACED_FOLDERS])


# ---------- Settings' pictures (tools/ce_menus.py draws them, with the
# titles): the rows' highlight, a section's line, the tabs, the panel, the
# frames of the Quality row's stills, in the redraws' blue and navy

def _rounded(width: float, height: float, inset: float, radius: float, fill: str = "none",
             fill_opacity: float = 1.0, stroke: str = "none", stroke_width: float = 0) -> str:
    return (f'  <rect x="{inset:g}" y="{inset:g}" width="{width - 2 * inset:g}" height="{height - 2 * inset:g}" '
            f'rx="{radius:g}" fill="{fill}" fill-opacity="{fill_opacity:g}" stroke="{stroke}" '
            f'stroke-width="{stroke_width:g}"/>')


def _svg(width: int, height: int, body: list) -> str:
    return "\n".join([f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
                      f'viewBox="0 0 {width} {height}">', *body, "</svg>", ""])


def settings_pictures() -> list:
    """(bitmap name, [(SVG, width, height)] for its frames) of Settings'
    pictures"""
    row_w, row_h = SETTINGS_ROW_WIDTH, SETTINGS_ROW_HEIGHT
    tab_w, tab_h = SETTINGS_TAB_WIDTH, SETTINGS_TAB_HEIGHT
    still_w, still_h = SETTINGS_STILL_WIDTH, SETTINGS_STILL_HEIGHT
    pictures = [
        # (a row: nothing, and while chosen the option_bkds redraw's framed
        # bar)
        (f"{SETTINGS}/row", [(_svg(row_w, row_h, []), row_w, row_h),
                             (_svg(row_w, row_h, [_rounded(row_w, row_h, 1, 8, "#002244", 0.9, "#2299ff", 2)]),
                              row_w, row_h)]),
        (f"{SETTINGS}/section", [(_svg(row_w, SETTINGS_SECTION_HEIGHT, [
            f'  <rect x="0" y="{SETTINGS_SECTION_HEIGHT - 1.5:g}" width="{row_w}" height="1" fill="#2995ff" '
            'fill-opacity="0.45"/>']), row_w, SETTINGS_SECTION_HEIGHT)]),
        (f"{SETTINGS}/tab", [(_svg(tab_w, tab_h, []), tab_w, tab_h),
                             (_svg(tab_w, tab_h, [_rounded(tab_w, tab_h, 1, 8, "#002244", 0.9, "#2299ff", 2)]),
                              tab_w, tab_h)]),
        (f"{SETTINGS}/tab_current",
         [(_svg(tab_w, tab_h, [_rounded(tab_w, tab_h, 1, 8, "#0a2a5a", 0.75, "#2299ff", 2)]), tab_w, tab_h),
          (_svg(tab_w, tab_h, [_rounded(tab_w, tab_h, 1, 8, "#0d3a7a", 0.9, "#8cc8ff", 2)]), tab_w, tab_h)]),
    ]
    # (a slider's track, a groove in the redraws' navy and blue rim, and its
    # thumb, a blue tab)
    track_w, track_h, thumb_w = SLIDER_TRACK_WIDTH, SLIDER_TRACK_HEIGHT, SLIDER_THUMB_WIDTH
    pictures.append((f"{SETTINGS}/slider_track", [(_svg(track_w, track_h, [
        f'  <rect x="1" y="{track_h / 2 - 2.5:g}" width="{track_w - 2}" height="5" rx="2.5" fill="#000e23" '
        'fill-opacity="0.9" stroke="#2995ff" stroke-width="1"/>']), track_w, track_h)]))
    pictures.append((f"{SETTINGS}/slider_thumb", [(_svg(thumb_w, track_h, [
        _rounded(thumb_w, track_h, 1, 2.5, "#8cc8ff", 1.0, "#2299ff", 1.5)]), thumb_w, track_h)]))
    # (its number's field, as the CE text fields are framed: a navy box with
    # a blue rim and a line under the number; typed into, a brighter rim
    # and field)
    field_w, field_h = SLIDER_VALUE_WIDTH, SLIDER_VALUE_HEIGHT
    underline = (f'  <rect x="6" y="{field_h - 3.5:g}" width="{field_w - 12}" height="1" fill="#2995ff" '
                 'fill-opacity="0.8"/>')
    pictures.append((f"{SETTINGS}/slider_field", [
        (_svg(field_w, field_h, [_rounded(field_w, field_h, 2, 3, "#000e23", 0.9, "#2995ff", 1.5), underline]),
         field_w, field_h),
        (_svg(field_w, field_h, [_rounded(field_w, field_h, 2, 3, "#0d3a7a", 0.95, "#8cc8ff", 1.5),
                                 underline.replace("#2995ff", "#8cc8ff")]), field_w, field_h)]))
    for kind, top in (("tab", SETTINGS_TOP), ("page", PAGE_TOP)):
        height = SETTINGS_PANEL_BOTTOM - top
        # (list_field's framed panel: a navy field, a blue rim)
        pictures.append((f"{SETTINGS}/{kind}_panel", [(_svg(SETTINGS_PANEL_WIDTH, height, [
            _rounded(SETTINGS_PANEL_WIDTH, height, 1, 9.5, "#000e23", 0.88, "#2995ff", 2)]),
            SETTINGS_PANEL_WIDTH, height)]))
    pictures.append((f"{SETTINGS}/still_frame", [
        (_svg(still_w, still_h, [_rounded(still_w, still_h, 0.75, 4, stroke="#1d4f8a", stroke_width=1.5)]),
         still_w, still_h),
        (_svg(still_w, still_h, [_rounded(still_w, still_h, 1, 4, stroke="#2995ff", stroke_width=2)]),
         still_w, still_h)]))
    return pictures


# the stills the Quality row's panel shows (port/assets/menus/stills, made as
# NON_HANDDRAWN.md says): bitmap name, file, width and height in the menu's
# units, and what it is
SETTINGS_STILLS = [
    (f"{SETTINGS}/graphics_classic", "stills/graphics_classic.png", SETTINGS_STILL_WIDTH, SETTINGS_STILL_HEIGHT,
     "Quality CLASSIC (`display.graphics = \"classic\"`)"),
    (f"{SETTINGS}/graphics_enhanced", "stills/graphics_enhanced.png", SETTINGS_STILL_WIDTH, SETTINGS_STILL_HEIGHT,
     "Quality ENHANCED (`display.graphics = \"enhanced\"`)"),
]
# a picture for each value of these rows (by their keys), in the panel as
# their row is chosen (menu_functions.c's settings_panel_show): each value's
# file (port/assets/menus/stills), in the row's order, and what each file is
SETTINGS_PICTURE_WIDTH, SETTINGS_PICTURE_HEIGHT = 160, 100
_FOV_PICTURES = (["stills/fov_default.png"] * 4 + ["stills/fov_100.png"] * 5 + ["stills/fov_130.png"] * 7)
SETTINGS_VALUE_PICTURES = {
    "fov": _FOV_PICTURES,
    "viewmodel_fov": None,
    "anti_aliasing": ["stills/aa_off.png", "stills/aa_smaa.png", "stills/aa_smaa.png"] + ["stills/aa_msaa.png"] * 4,
    "anti_aliasing_android": ["stills/aa_off.png", "stills/aa_smaa.png", "stills/aa_smaa.png",
                              "stills/aa_msaa.png", "stills/aa_msaa.png"],
    "anti_aliasing_web": ["stills/aa_off.png", "stills/aa_smaa.png", "stills/aa_msaa.png", "stills/aa_msaa.png"],
    "resolution_scaling": ["stills/aa_smaa.png", "stills/render_xbox.png"],
    "high_res_hud": ["stills/hud_high_res.png", "stills/hud_game.png"],
    "hud_button_style": ["stills/buttons_classic.png", "stills/buttons_gems.png", "stills/buttons_dark_gems.png"],
    "hdr_mode": ["stills/fov_default.png", "stills/hdr_punchy.png"],
}
del SETTINGS_VALUE_PICTURES["viewmodel_fov"]
SETTINGS_PICTURE_FILES = {
    "stills/fov_default.png": "Field of view DEFAULT (`display.fov = 0`); HDR mode FAITHFUL",
    "stills/fov_100.png": "Field of view 100, for 95 to 115",
    "stills/fov_130.png": "Field of view 130, for 120 to 150",
    "stills/aa_off.png": "Anti-aliasing OFF, close on an edge (each pixel twice its size)",
    "stills/aa_smaa.png": "Anti-aliasing SMAA (and FXAA), the same; render resolution FULL",
    "stills/aa_msaa.png": "Anti-aliasing MSAA 4X (and SSAA and MSAA's others), the same",
    "stills/render_xbox.png": "Render resolution XBOX 640X480, the same edge, its pixels as drawn",
    "stills/hud_high_res.png": "High-res HUD ON: the health and shields meters, close",
    "stills/hud_game.png": "High-res HUD OFF, the same",
    "stills/hdr_punchy.png": "HDR mode PUNCHY, approximately: the DEFAULT scene lifted in SDR, as no SDR picture can "
                             "show HDR",
    "stills/buttons_classic.png": "Button style CLASSIC: A, B, X and Y, the HUD's own pictures "
                                  "(port/assets/buttons), not a screenshot",
    "stills/buttons_gems.png": "Button style GEMS, the same",
    "stills/buttons_dark_gems.png": "Button style DARK GEMS, the same",
}
# NON_HANDDRAWN.md's words for them (tools/ce_menus.py)
SETTINGS_STILLS_MADE = [
    "Settings' Quality row shows these two small screenshots of the game, the same view drawn by each preset",
    "(`port/assets/menus/stills`, shipped as they are). They are this port's own drawing of the game: a local",
    "Slayer game on Blood Gulch (`debug.network_test = \"local:bloodgulch:slayer\"`, `debug.random_seed = 7`,",
    "four bots), the same frame of each run captured offscreen by the macOS build at 1600x1200",
    "(`debug.screenshot_every`), cropped to 16:10, scaled to 288x180 (three times their size in the menu) and",
    "reduced to 96 colours for size. The pictures of other rows' values are made the same way at the frame 12 s",
    "into the game with the setting set, 320x200 (twice their size in the menu), reduced to 64 colours: the whole",
    "view, or a part of it at 1:1 or larger.",
]


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
    # (the gametype editor's OPENCE OPTIONS' words, after the PC version's:
    # menu_functions.c's playlist_edit_description)
    f"{PLAYLIST_EDIT}/playlist_edit_list_extended_dsc_text": [(8, [
        "Choose the rules of OpenCE's\\ngametypes: SWAT, Infection,\\nGun Game and Hardcore.\\nThis gametype:"])],
    f"{PLAYER_EDIT}/player_options_labels": [(8, ["AIM ASSIST:", "SHIELD EFFECTS:"])],
    f"{PLAYER_EDIT}/cap_player_options": [(28, AIM_ASSIST_HELP + SHIELD_EFFECTS_HELP)],
    # (Teamplay Options' voice chat and vote kick rows, after its own:
    # TEAMPLAY_ROWS)
    f"{TEAMPLAY_EDIT}/teamplay_options_labels": [(3, [label for _, label, *_ in TEAMPLAY_ROWS])],
    f"{TEAMPLAY_EDIT}/cap_teamplay_options": [(10, [text for *_, helps in TEAMPLAY_ROWS for text in helps])],
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
    # (plain names: LOCAL NETWORK for the PC version's LAN, JOIN BY LINK,
    # and the two co-ops by how they are played)
    f"{MT}/multiplayer_options": ["JOIN GAME", "CREATE GAME", "INTERNET", "LOCAL NETWORK", "JOIN BY LINK",
                                  "EDIT GAMETYPES", "SERVER BROWSER", "SPLIT-SCREEN CO-OP", "ONLINE CO-OP"],
    # (the items' in the list's order: menu_functions.c's description of
    # the item chosen)
    f"{MT}/multiplayer_option_descriptions": [
        "Every public game on the\\nInternet. Press A to see\\nthem all.",
        "Games on your Wi-Fi or local\\nnetwork.",
        "Join a friend's game with the\\ninvite link they send you, or\\na Discord invite.",
        "Host a game on the Internet.\\nFriends join with your invite\\nlink.",
        "Host a game for players on\\nyour Wi-Fi or local network.",
        "Play the campaign together\\nover the Internet: friends\\njoin with your invite link.",
        "Play the campaign with a\\nfriend on this screen.\\nPlayer 2 uses a controller.",
        "Make your own gametypes:\\nthe rules, weapons and\\nvehicles, kept for next time.",
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
    f"{MT}/multiplayer_options": [0, 1, 2, None, None, 5, None, None, None],
    f"{MT}/multiplayer_option_descriptions": [None] * 8,
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

# Multiplayer's panel's counts: the digits' bitmap (tools/ce_menus.py's
# Art.digits), a frame's size in units and the digits' height, white
STATUS_DIGITS = f"{MT}/status_digits"
DIGIT_SIZE = (24, 36)
DIGIT_CAP = 28
DIGIT_COLOR = (0xFF, 0xFF, 0xFF, 255)
# a game's ping as bars, a frame each: not measured (grey), poor (one, red),
# fair (two, yellow), good (three, green): menu_functions.c's PING_GOOD and
# PING_FAIR
PING_BARS = f"{MT}/ping_bars"
# the browsers' rows' star (menu_functions.c's browser_row_star): its widget
# and bitmap, whose frames are none, a favorite (gold), a favorite offline
# (grey) and the outline a click on the row focused stars (Lucide's star,
# port/assets/icons/lucide)
FAVORITE_STAR_WIDGET = f"{MT}/join_game/server_item_favorite"
FAVORITE_STAR = f"{MT}/join_game/favorite_star"
FAVORITE_STAR_STYLES = [None, ("#E8C547", "#E8C547"), ("#7F8A99", "#7F8A99"), ("none", "#2896FF")]
# the digits of a count (to 999), and the games BEST FOR YOU names
STATUS_DIGIT_PLACES = 3
BEST_GAMES = 3
PING_BAR_COLORS = [None, "#E86A5A", "#E8C547", "#5ED38C"]


# Multiplayer's rows, a step closer than the PC version's 33 units for
# Create Game's ONLINE CO-OP and SPLIT-SCREEN CO-OP: each item's and
# header's place (x, y) in the list, then the line and EDIT GAMETYPES below
# them, the highlight inside the description panel's bottom curve
# (tools/ce_menus.py's CHILD_OFFSETS)
MULTIPLAYER_ROW_STEP = 31
MULTIPLAYER_ROWS = {
    name: (-32 if name in ("multiplayer_type_join_item", "multiplayer_type_create_item") else 0,
           78 + MULTIPLAYER_ROW_STEP * index)
    for index, name in enumerate([
        "multiplayer_type_join_item", "multiplayer_type_join_internet_item", "multiplayer_type_join_lan_item",
        "multiplayer_type_join_direct_item", "multiplayer_type_create_item", "multiplayer_type_create_internet_item",
        "multiplayer_type_create_lan_item", "multiplayer_type_online_coop_item", "multiplayer_type_coop_item"])}
MULTIPLAYER_LINE_PLACE = (64, 363)
# the screen's children for one platform only (tools/ce_menus.py): the
# computer's list and its line, the touchscreen's in their place
# (_multiplayer_touch)
CHILD_PLATFORMS = {
    (f"{MT}/multiplayer_type_select_screen", f"{MT}/multiplayer_type_select_list"): "pointer",
    (f"{MT}/multiplayer_type_select_screen", "main_menu/blueline"): "pointer",
}
MULTIPLAYER_GAMETYPES_PLACE = (0, 370)

# changes to the PC version's widgets (by our names): attributes set, all
# their handlers replaced, children added, game data inputs added
WIDGET_PATCHES = {
    # The host returns here after a round with no menu history. Back must
    # reopen the main menu when it closes the last screen, as the lobby does.
    f"{MT}/connected/connected_map_select_wrapper": {"set": {
        "flags": "pass_unhandled_to_focused_child pause_game main_menu_if_no_history tag_controller_index force_handle_mouse",
    }},
    # (Settings opens its first tab at once, with a profile or without one:
    # menu_functions.c's settings_begin)
    "main_menu/main_menu_item_settings": {"handlers": [
        f'<on event="{event}" run="port settings begin" open="{PE}/video_settings/video_settings_screen"/>'
        for event in ("a", "start")] + ['<on event="left_mouse" run="mouse emit accept event"/>']},
    # (the main menu's MODS, before Quit: _mods)
    "main_menu/main_menu_select_list": {"insert_before": {"main_menu/main_menu_item_quit_game": [
        f'<child widget="main_menu/main_menu_item_mods" x="{MODS_ITEM_PLACE[0]}" y="{MODS_ITEM_PLACE[1]}"/>']}},
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
    # (the profile settings' picture: on Gamepad Setup's row, the profile's
    # button settings, BITMAP_FRAMES; menu_functions.c's
    # profile_gamepad_layout)
    f"{PE}/profile_edit_extended_desc_pic": {"inputs": ["port gamepad layout preview"]},
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
    f"{MT}/multiplayer_type_join_direct_item": {"children": [f'<child widget="{MT}/invite_note"/>'], "handlers": [
        f'<on event="a" run="mp type set mode" open="{MT}/join_game/join_game_screen"/>',
        f'<on event="start" run="mp type set mode" open="{MT}/join_game/join_game_screen"/>',
        '<on event="left_mouse" run="mouse emit accept event"/>',
    ]},
    # (Multiplayer's panel: _multiplayer_status, filled each frame; the menu
    # stops its own looking for games as it closes)
    f"{MT}/multiplayer_type_list_ext_desc": {"children": [
        f'<child widget="{MT}/{name}"/>' for name in [
            "multiplayer_status", "multiplayer_status_detail", "online_now_label",
            *[f"online_{count}_{digit}" for count in ("games", "players") for digit in range(STATUS_DIGIT_PLACES)],
            "online_games_label", "online_players_label", "best_label",
            *[f"best_{row}_{part}" for row in range(BEST_GAMES) for part in ("name", "about", "players", "ping")]]]},
    f"{MT}/multiplayer_type_join_lan_item": {"children": [f'<child widget="{MT}/item_note"/>']},
    f"{MT}/multiplayer_type_button_bar": {"set": {"flags": "pass_unhandled_to_focused_child left_right_tabs_items "
                                                           "anchor_right"}, "insert_before": {
        "common_button_back": [f'<child widget="{MT}/multiplayer_button_refresh" x="380" y="1"/>']}},
    f"{MT}/multiplayer_type_select_screen": {"handlers": [
        '<on event="created" run="mp type menu initialize"/>',
        '<on event="deleted" run="port multiplayer status end"/>'], "children": [
        f'<child widget="{MT}/touch/multiplayer_touch_list" platform="touch"/>',
        f'<child widget="{MT}/touch/multiplayer_touch_open_bar" y="346" platform="touch"/>',
        f'<child widget="{MT}/touch/multiplayer_touch_hint" platform="touch"/>']},
    f"{MT}/header_multiplayer": {"set": {"flags": "anchor_left"}},
    # (the list's headers, which the touchscreen's list shows too, at its rows'
    # left edge)
    f"{MT}/multiplayer_type_join_item": {"set": {"flags": "anchor_left"}},
    f"{MT}/multiplayer_type_create_item": {"set": {"flags": "anchor_left"}},
    # (Co-op, after Create Game's: _coop)
    f"{MT}/multiplayer_type_select_list": {"inputs": ["port multiplayer status"], "insert_before": {
        f"{MT}/multiplayer_type_gametypes_item": [
            f'<child widget="{MT}/{name}" y="{MULTIPLAYER_ROWS[name][1]}"/>'
            for name in ("multiplayer_type_online_coop_item", "multiplayer_type_coop_item")],
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
    # (Join's local network list: LOCAL NETWORK, not the PC version's LAN
    # LOBBY: TITLES)
    f"{MT}/join_game/header_lan": {"set": {"bitmap": f"{MT}/join_game/header_local_network"}},
    # (the small font's line does not fit below their 4 units of offset)
    f"{MT}/join_game/ticker_player_info": {"set": {"text_y": 1}},
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
            f'<child widget="{TEAMPLAY_EDIT}/op_{key}" x="54" y="{163 + 30 * index}"/>'
            for index, (key, *_) in enumerate(TEAMPLAY_ROWS)]}},

    # (VR Setup, below About, in VR only: _vr_item)
    f"{PE}/profile_edit_select_list": {"insert_before": {
        f"{PE}/profile_edit_button_bar": [f'<child widget="{VR_ITEM}" platform="vr"/>']}},
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
    # (and FAVORITES in FILTERS': only the favorite games, or all again;
    # and the kind button before it, in the clipboard's place, which Direct
    # Link alone shows: every game, OpenH1's own only, or Legacy ones only)
    f"{MT}/join_game/join_game_button_bar": {"swap": {
        f"{MT}/join_game/join_game_button_refresh": f"{MT}/join_game/button_clipboard",
        f"{MT}/join_game/join_game_button_update": f"{MT}/join_game/join_game_button_refresh",
        f"{MT}/join_game/join_game_button_filters": f"{MT}/join_game/button_favorites",
    }, "insert_before": {
        f"{MT}/join_game/join_game_button_filters": [f'<child widget="{MT}/join_game/button_kind_filter" x="128" y="1"/>'],
    }},
    # (the browsers' rows: each game's star, in the place of the PC version's
    # dedicated server column, which the rows leave empty, and Y (F on the
    # keyboard) stars or unstars the game: menu_functions.c's favorites)
    **{f"{MT}/join_game/server_item_{row}": {
        "handlers": [
            '<on event="a" run="gamespy select item"/>',
            '<on event="start" run="gamespy select item"/>',
            '<on event="left_mouse" run="mouse emit accept event"/>',
            '<on event="double_click" run="gamespy select item"/>',
            '<on event="y" run="port browser favorite"/>',
            # (X: the list sorted by the next column: menu_functions.c's
            # browser_sort_choose)
            '<on event="x" run="port browser sort"/>',
        ],
        "children": [f'<child widget="{FAVORITE_STAR_WIDGET}" x="20"/>'],
    } for row in range(1, 16)},
}

# frames added after a PC bitmap's (by our names): (the Xbox map's bitmap,
# its frame, the size drawn at, where in the widget). The profile settings'
# picture gets the Xbox's Controller Setup's pictures of its five button
# settings (its first four frames are the thumbstick settings'), which show
# 512 by 235 of their 512 by 512: drawn at 279 wide, in the middle of the
# picture's 279 by 202
XBOX_CONTROLLER_PICTURES = f"ui\\shell\\{PE.replace('/', chr(92))}\\controller_edit\\config_controller"
BITMAP_FRAMES = {
    f"{PE}/profile_options": [(XBOX_CONTROLLER_PICTURES, 4 + preset, 279, 279, 0, 34) for preset in range(5)],
}

# the titles this port has that the PC version has not, set as its headers
# are (port/assets/menus/port_svg): bitmap name, text
TITLES = {
    f"{PE}/vr_controls/header_vr_controls": "VR CONTROLS",
    f"{PE}/vr_display/header_vr_display": "VR DISPLAY",
    "main_menu/mods/header_mods": "MODS",
    f"{MT}/join_game/header_server_browser": "SERVER BROWSER",
    f"{MT}/join_game/header_direct_link": "JOIN BY LINK",
    f"{MT}/join_game/header_local_network": "LOCAL NETWORK",
    f"{MT}/join_game/header_password": "PASSWORD",
    f"{MT}/lobby/header_lobby": "GAME LOBBY",
    f"{MT}/coop/header_player_2": "PLAYER 2 PROFILE",
    f"{MT}/lobby/header_add_player": "ADD PLAYER",
    **SETTINGS_TITLES,
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


def _connect_screen() -> list:
    """internet play's connecting box, laid out as the PC version's Quit is
    (ce/main_menu.quit_select.xml): opened over the browser as a game is
    joined (menu_functions.c's connect_dialog), it names the step the join
    is at (finding the host, opening a path to it, joining), and once it
    gives up (about 10 seconds) why, and what to do; RETRY (shown then)
    asks again, BACK (and B) gives the join up. Its header and its text are
    the game's ("port connect update")"""
    base = f"{MT}/join_game/connect"
    lines = _widget(f"{base}/connect_screen",
                    [("type", "text"), ("controller", 1), ("left", 180), ("top", 160), ("width", 280), ("height", 160),
                     ("flags", "pass_unhandled_to_focused_child"), ("bitmap", "bitmaps/alert_bkd"),
                     ("font", "ui\\small_ui"), ("color", "#FF2896FF"), ("align", "center"), ("text_x", -10),
                     ("text_y", 36)],
                    ['<data input="port connect update"/>',
                     '<on event="b" run="port connect back"/>', '<on event="back" run="port connect back"/>',
                     f'<child widget="{base}/button_bar" x="182" y="293"/>',
                     f'<child widget="{base}/header_connect" x="182" y="164"/>'])
    lines += _widget(f"{base}/header_connect",
                     [("type", "text"), ("controller", 1), ("width", 270), ("height", 20),
                      ("font", "ui\\large_ui"), ("color", "#FF2896FF")], [])
    # (BACK first, with the focus: RETRY is shown only once the join failed)
    lines += _widget(f"{base}/button_bar",
                     [("type", "column_list"), ("width", 256), ("height", 20),
                      ("flags", "pass_unhandled_to_focused_child up_down_tabs_items left_right_tabs_items")],
                     [f'<child widget="{base}/button_back" x="128"/>', f'<child widget="{base}/button_retry"/>'])
    lines += _text_button(f"{base}/button_retry", "RETRY", ['<on event="a" run="port connect retry"/>',
                                                           '<on event="start" run="port connect retry"/>'])
    lines += _text_button(f"{base}/button_back", "BACK", ['<on event="a" run="port connect back"/>',
                                                         '<on event="start" run="port connect back"/>'])
    return lines


def _join_game_extras() -> list:
    """the browser's added titles, Direct Link's clipboard button, and the
    password screen"""
    base = f"{MT}/join_game"
    lines = _password_screen()
    lines += _header(f"{base}/header_server_browser", f"{base}/header_server_browser")
    lines += _header(f"{base}/header_direct_link", f"{base}/header_direct_link")
    lines += _connect_screen()
    lines += _widget(f"{base}/button_clipboard", [("type", "text"), ("width", 128), ("height", 24),
                                                  ("bitmap", "bitmaps/text_button_background"),
                                                  ("text", "PASTE LINK"), ("font", "ui\\small_ui"),
                                                  ("color", "#FFFFFFFF"), ("align", "center"), ("text_y", 2)],
                     ['<on event="a" run="direct ip connect go"/>', '<on event="start" run="direct ip connect go"/>',
                      '<on event="left_mouse" run="mouse emit accept event"/>'])
    # (FAVORITES, which menu_functions.c names ALL GAMES while it shows only
    # the favorites)
    # (its words, FAVORITES or ALL GAMES, by menu_functions.c's
    # browser_favorites_button)
    lines += _words_button(f"{base}/button_favorites", ["FAVORITES", "ALL GAMES"],
                           ['<on event="a" run="gamespy select button"/>',
                            '<on event="start" run="gamespy select button"/>'])
    # (the kind button: menu_functions.c's browser_kind_button names what it
    # shows)
    lines += _words_button(f"{base}/button_kind_filter", ["KIND: ALL", "KIND: OPENH1", "KIND: LEGACY"],
                           ['<on event="a" run="gamespy select button"/>',
                            '<on event="start" run="gamespy select button"/>'])
    # (each row's star: WIDGET_PATCHES puts it in the rows)
    lines += _widget(FAVORITE_STAR_WIDGET, [("width", 16), ("height", 18), ("bitmap", FAVORITE_STAR)], [])
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


# Server Setup's GAME: the kind of game hosted and its tick rate
# (network.game_mode and network.sim_rate: menu_functions.c's
# server_settings_game_modes, in this order), each choice's help
GAME_MODES = [
    ("OPENH1 30 TICK", "OpenH1 players only, at the original 30 ticks a\nsecond. Legacy (OpenCE) players cannot join."),
    ("OPENH1 60 TICK", "OpenH1 players only, smoother at 60 ticks a\nsecond. Legacy (OpenCE) players cannot join."),
    ("OPENH1 120 TICK", "OpenH1 players only, smoothest at 120 ticks a\nsecond, and the most work for each machine."),
    ("LEGACY", "Plays with OpenCE players too, at 30 ticks a\nsecond, as OpenCE does."),
]


def _server_settings() -> list:
    """Create Game's server settings (in the PC version's place): the game's
    name, the most players (up to the port's 128), the kind of game (OpenH1
    at a tick rate, or Legacy), its invite link, and whether the server
    browser lists it (PUBLIC or PRIVATE: an internet game's)"""
    base = f"{MT}/server_settings"
    # (thirteen rows: closer together, the help lower)
    spec = {"screen": "server_settings_screen", "spacing": 22, "help_top": 366,
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
    # GAME: OpenH1 at its tick rate, or Legacy (menu_functions.c's
    # server_settings_update), each choice its help
    extra += _widget(f"{base}/op_game_mode", [("width", 512), ("height", 28),
                                              ("flags", "pass_unhandled_to_focused_child"),
                                              ("bitmap", "bitmaps/option_bkds"), ("color", "#FF2896FF")],
                     [f'<child widget="{base}/game_mode_label"/>',
                      f'<child widget="{base}/game_mode_spinner" x="296" y="1"/>'])
    extra += _widget(f"{base}/game_mode_label", [("type", "text"), ("controller", 1), ("width", 280),
                                                 ("height", 22), ("string_list", f"{base}/labels"),
                                                 ("string_index", 12 + len(COOP_SETUP_SCREENS)),
                                                 ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("text_x", 13),
                                                 ("text_y", 4)], [])
    extra += _widget(f"{base}/game_mode_spinner",
                     [("type", "spinner"), ("left", 3), ("top", 2), ("width", 195), ("height", 20),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
                      ("strings", "|".join(name for name, _ in GAME_MODES)), ("font", "ui\\large_ui"),
                      ("color", "#FF2896FF"), ("align", "center"), ("text_y", 1),
                      ("header_bitmap", "bitmaps/arrow_sm_left"), ("footer_bitmap", "bitmaps/arrow_sm_right"),
                      ("header_bounds", "7 -6 19 0"), ("footer_bounds", "7 198 19 204")],
                     ['<on event="left_mouse" run="mouse spinner 1wide click"/>'])
    rows.append((f"{base}/op_game_mode", None))
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
                                         f"{MAP_SHARE_SETUP[1]}:", "GAME:"])
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
        # (LISTING's, by its choice)
        "Anyone can see and join your game: it is listed\\nin everyone's Server Browser.",
        "Only players with your invite link can join.",
        # (co-op's options' rows: COOP_SETUP_SCREENS)
        *(help_text for _, _, help_text, _ in COOP_SETUP_SCREENS),
        # (PASSWORD's)
        "Players from the Server Browser must type it to\\njoin (an invite link needs none). Enter sets it.",
        # (MAP SHARING's)
        MAP_SHARE_SETUP[2],
        # (GAME's, by its choice)
        *(help_text for _, help_text in GAME_MODES),
    ])
    lines = _screen(base, spec, rows, ["server settings update"],
                    ['<on event="created" run="server settings init"/>'], extra)
    # (co-op's options' screens, and map sharing's)
    for key, title, _, screen_rows in [*COOP_SETUP_SCREENS, MAP_SHARE_SETUP]:
        lines += _setup_option_screen(f"{base}/{key}", key, screen_rows)
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
    ("coop_options", "CO-OP OPTIONS", "Friendly fire, the enemies and player collisions,\\nfor this game.", [
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
SETUP_OPTIONS_PLACE = 7
# (their screens' titles)
TITLES.update({f"{MT}/server_settings/{key}/header_{key}": title
               for key, title, _, _ in [*COOP_SETUP_SCREENS, MAP_SHARE_SETUP]})
# (the rows of a co-op options screen in one place, the one shown: PER
# PLAYER's and MULTIPLIER's, by EXTRA ENEMIES')
SETUP_OPTION_SAME_PLACE = set()


def _setup_option_screen(base: str, key: str, rows: list) -> list:
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
    place = -1
    for row_key, *_ in rows:
        if row_key not in SETUP_OPTION_SAME_PLACE:
            place += 1
        children.append(f'<child widget="{base}/op_{row_key}" x="54" y="{73 + 30 * place}"/>')
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
                     f'<on event="start" run="port lobby join" open="{base}/player_profile_screen" branch="true"/>',
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
    lines += _widget(f"{base}/lobby_button_bar", [("type", "column_list"), ("width", 640), ("height", 28),
                                                  ("flags", "pass_unhandled_to_focused_child left_right_tabs_items")],
                     # (SWITCH TEAM first: hidden in a game without teams, which
                     # moves the focus to the first shown, START NOW; at the end
                     # it traps none)
                     [f'<child widget="{base}/lobby_button_team" x="120" y="1"/>',
                      f'<child widget="{base}/lobby_button_start" x="250" y="1"/>',
                      f'<child widget="{base}/lobby_button_add" x="380" y="1"/>',
                      f'<child widget="{base}/lobby_button_leave" x="510" y="1"/>',
                      # (COPY INVITE, on the left as Defaults is on the
                      # settings screens: the host's of an internet game,
                      # shown while it has an invite link; last, so that the
                      # focus starts on START NOW)
                      f'<child widget="{base}/lobby_button_invite" y="1"/>'])
    for key, caption, handlers in (
        ("invite", "COPY INVITE", ['<on event="a" run="ss copy invite"/>', '<on event="start" run="ss copy invite"/>']),
        ("add", "ADD PLAYER", ['<on event="a" run="port lobby add player"/>',
                               '<on event="start" run="port lobby add player"/>']),
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
    # a split screen player's profile, chosen as they join the lobby (any
    # controller's presses: Co-op's rows; the host's countdown waits)
    lines += _widget(f"{base}/player_profile_screen", [("width", 640), ("height", 480),
                                                       ("flags", "pass_unhandled_to_focused_child"),
                                                       ("bitmap", "bitmaps/gradient")],
                     ['<on event="created" run="net server defer start"/>',
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
                      f'<on event="start" run="port lobby preview add" open="{base}/player_profile_screen" branch="true"/>',
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
        lines += _widget(f"{base}/preview_button_{key}", [("type", "text"), ("width", 128), ("height", 24),
                                                         ("bitmap", "bitmaps/text_button_background"),
                                                         ("text", caption), ("font", "ui\\small_ui"),
                                                         ("color", "#FFFFFFFF"), ("align", "center"), ("text_y", 2)],
                         [f'<on event="a" run="{run}"/>', f'<on event="start" run="{run}"/>',
                          '<on event="left_mouse" run="mouse emit accept event"/>'])
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
                    [f'<on event="a" run="port coop begin" open="{base}/player_2_profile_screen"/>',
                     f'<on event="start" run="port coop begin" open="{base}/player_2_profile_screen"/>',
                     '<on event="left_mouse" run="mouse emit accept event"/>'])
    # (ONLINE CO-OP, under Create Game: hosting over the Internet, the map
    # list opening on the campaign's levels, menu_functions.c's
    # multiplayer_mode_set and map_list_initialize)
    lines += _widget(f"{MT}/multiplayer_type_online_coop_item",
                     [("type", "text"), ("left", 51), ("width", 232), ("height", 32), ("bitmap", "bitmaps/list_item_bkd"),
                      ("string_list", f"{MT}/multiplayer_options"), ("string_index", 8), ("font", "ui\\large_ui"),
                      ("color", "#FF2896FF"), ("text_x", 13), ("text_y", 5)],
                     ['<on event="a" run="join controller to mp game"/>',
                      f'<on event="a" run="mp type set mode" open="{MT}/connected/connected_map_select_wrapper"/>',
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
    """Teamplay Options' rows of the port's: the host's voice chat
    (port/linux/game/network_voice.c) and vote kicks (network_votekick.c),
    in Server Setup's copy only (menu_functions.c's gametype_options_init),
    kept in config.toml"""
    lines = []
    for index, (key, label, strings, _) in enumerate(TEAMPLAY_ROWS):
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
    return lines


def favorite_star_frames(icons: Path) -> list:
    """the browsers' star's frames, 16 pixels square (FAVORITE_STAR_STYLES):
    Lucide's star filled and stroked in each frame's colours, the first
    frame empty"""
    blank = '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24"/>\n'
    star = (icons / "star.svg").read_text()
    return [(blank, 16, 16)] + [
        (star.replace('fill="none"', f'fill="{fill}"').replace('stroke="currentColor"', f'stroke="{stroke}"')
         .replace('width="24"', 'width="16"').replace('height="24"', 'height="16"'), 16, 16)
        for fill, stroke in FAVORITE_STAR_STYLES[1:]]


def ping_bars_frames() -> list:
    """the ping bars' frames: three bars rising, those lit in the frame's
    colour, the rest dim"""
    frames = []
    for lit, color in enumerate(PING_BAR_COLORS):
        bars = []
        for bar in range(3):
            height = 6 + 4 * bar
            fill = color if color and bar < lit else "#2A4A6A"
            bars.append(f'<rect x="{3 + 7 * bar}" y="{16 - height}" width="5" height="{height}" rx="1" '
                        f'fill="{fill}"/>')
        frames.append(('<svg xmlns="http://www.w3.org/2000/svg" width="24" height="16" viewBox="0 0 24 16">'
                       + "".join(bars) + "</svg>\n", 24, 16))
    return frames


# the touchscreen's Multiplayer (the menus' "touch" platform: the Android and
# iOS apps): its rows 46 units tall (44 points and more on a phone), Create
# Game's three in one row, the panel stretched to the screen's right edge
# (the port's flags anchor_left, anchor_right and stretch: menu_tags.c), a
# large OPEN button in it, and the hint that a tap chooses and another opens
# (ui_widget.c's ui_mouse_tap_chooses_first)
TOUCH = f"{MT}/touch"
TOUCH_ITEM = f"{MT}/touch_item_bkd"
TOUCH_ITEM_SIZE = (300, 46)
TOUCH_PANEL_BODY = f"{MT}/touch_panel_body"
TOUCH_PANEL_CAP = f"{MT}/touch_panel_cap"
TOUCH_OPEN_BUTTON = f"{MT}/touch_open_button"
# the panel's body: its width as the 640 have it, and the picture's (the
# widest screen's stretch: a phone of 2.2 to 1 adds 2 x 208)
TOUCH_PANEL_WIDTH = 262
TOUCH_PANEL_PICTURE_WIDTH = 704
TOUCH_PANEL = (345, 78, 322)
TOUCH_ROW_STEP = 48


def touch_item_frames() -> list:
    """a touchscreen row's background: nothing, and chosen the menus' list
    item's (a rounded left end, a blue outline, open to the right), taller"""
    width, height = TOUCH_ITEM_SIZE
    blank = f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}"/>\n'
    chosen = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
              f'viewBox="0 0 {width} {height}">'
              f'<path d="M12,0 H{width} V{height} H12 A12,12 0 0 1 0,{height - 12} V12 A12,12 0 0 1 12,0 Z" '
              'fill="#000e23"/>'
              f'<path d="M{width},1 H12 A11,11 0 0 0 1,12 V{height - 12} A11,11 0 0 0 12,{height - 1} H{width}" '
              'fill="none" stroke="#2995ff" stroke-width="2"/></svg>\n')
    return [(blank, width, height), (chosen, width, height)]


def touch_panel_frames() -> list:
    """the panel's body (the menus' description field's left end, its top
    and bottom running on, which a stretched widget shows as much of as it
    is wide) and its right end"""
    width, height = TOUCH_PANEL_PICTURE_WIDTH, TOUCH_PANEL[2]
    body = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}">'
            f'<rect x="2" y="2" width="{width - 2}" height="{height - 4}" fill="#000e23"/>'
            f'<path d="M9.5,0 H{width} V2 H10.5 A8.5,8.5 0 0 0 2,10.5 V{height - 10.5} A8.5,8.5 0 0 0 10.5,{height - 2} '
            f'H{width} V{height} H9.5 A9.5,9.5 0 0 1 0,{height - 9.5} V9.5 A9.5,9.5 0 0 1 9.5,0 Z" '
            'fill="#2995ff"/></svg>\n')
    cap = (f'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="{height}" viewBox="0 0 16 {height}">'
           f'<rect x="0" y="2" width="14" height="{height - 4}" fill="#000e23"/>'
           f'<path d="M0,0 H6.5 A9.5,9.5 0 0 1 16,9.5 V{height - 9.5} A9.5,9.5 0 0 1 6.5,{height} H0 V{height - 2} '
           f'H5.5 A8.5,8.5 0 0 0 14,{height - 10.5} V10.5 A8.5,8.5 0 0 0 5.5,2 H0 Z" fill="#2995ff"/></svg>\n')
    return [(body, width, height)], [(cap, 16, height)]


def touch_open_frames() -> list:
    """the panel's OPEN button: the menus' text buttons' pill, larger, and
    brighter while chosen"""
    width, height = 300, 40
    frames = []
    for fill, opacity in (("#004488", "0.6"), ("#1a5fb4", "0.9")):
        frames.append((f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
                       f'viewBox="0 0 {width} {height}">'
                       f'<rect x="1.5" y="1.5" width="{width - 3}" height="{height - 3}" rx="{(height - 3) / 2}" '
                       f'fill="{fill}" fill-opacity="{opacity}" stroke="#3399ff" stroke-width="2"/></svg>\n',
                       width, height))
    return frames


def _multiplayer_touch() -> list:
    """the touchscreen's Multiplayer: its list, its panel, its OPEN button
    and its hint (TOUCH)"""
    width, height = TOUCH_ITEM_SIZE
    left, top, panel_height = TOUCH_PANEL

    # (each row anchored to the screen's left edge, not the list: the list's
    # description, its panel, is drawn in it and is anchored by its own parts)
    def item(name, text=None, string_index=None, handlers=(), x_width=width, bitmap=TOUCH_ITEM, extra=(),
             anchored=True):
        return _widget(f"{TOUCH}/{name}", [("type", "text"), ("left", 51), ("width", x_width), ("height", height),
                                           ("flags", "anchor_left" if anchored else None),
                                           ("bitmap", bitmap), ("text", text),
                                           ("string_list", f"{MT}/multiplayer_options" if string_index is not None
                                            else None), ("string_index", string_index),
                                           ("font", "ui\\large_ui"), ("color", "#FF2896FF"), ("text_x", 15),
                                           ("text_y", 14)],
                       [*handlers, '<on event="left_mouse" run="mouse emit accept event"/>', *extra])

    def joins(mode_open):
        return [f'<on event="a" run="mp type set mode" open="{mode_open}"/>',
                f'<on event="start" run="mp type set mode" open="{mode_open}"/>']

    def creates():
        return ['<on event="a" run="join controller to mp game"/>',
                f'<on event="a" run="mp type set mode" open="{MT}/connected/connected_map_select_wrapper"/>']

    lines = []
    rows = [("multiplayer_type_join_item", -32, 78)]
    y = 104
    for name in ("multiplayer_touch_browser_item", "multiplayer_touch_join_lan_item", "multiplayer_touch_join_direct_item"):
        rows.append((f"touch/{name}", 0, y))
        y += TOUCH_ROW_STEP
    rows.append(("multiplayer_type_create_item", -32, y + 2))
    y += 28
    rows.append(("touch/multiplayer_touch_create_row", 0, y))
    y += TOUCH_ROW_STEP + 6
    rows.append(("touch/multiplayer_touch_gametypes_item", 0, y))
    lines += _widget(f"{TOUCH}/multiplayer_touch_list",
                     [("type", "column_list"), ("width", 640), ("height", 480),
                      ("flags", "pass_unhandled_to_focused_child up_down_tabs_items"),
                      ("description", f"{TOUCH}/multiplayer_touch_desc")],
                     ['<data input="port multiplayer status"/>'] +
                     [f'<child widget="{MT}/{name}"' + (f' x="{x}"' if x else "") + f' y="{y}"/>'
                      for name, x, y in rows] +
                     [f'<child widget="{MT}/multiplayer_type_button_bar" y="414"/>'])
    lines += item("multiplayer_touch_browser_item", string_index=6, handlers=joins(f"{MT}/join_game/join_game_screen"))
    lines += item("multiplayer_touch_join_lan_item", string_index=3, handlers=joins(f"{MT}/join_game/join_game_screen"),
                  extra=[f'<child widget="{TOUCH}/item_note"/>'])
    lines += item("multiplayer_touch_join_direct_item", string_index=4,
                  handlers=joins(f"{MT}/join_game/join_game_screen"), extra=[f'<child widget="{TOUCH}/invite_note"/>'])
    # (the notes at the rows' ends, as the computer's: menu_functions.c's
    # multiplayer_status_notes)
    for name, color in (("item_note", "#FF7FB2FF"), ("invite_note", "#FF5ED38C")):
        lines += _widget(f"{TOUCH}/{name}", [("type", "text"), ("width", width - 12), ("height", height),
                                             ("font", "ui\\small_ui"), ("color", color), ("align", "right"),
                                             ("text_y", 17), ("text_flags", "no_focus_test")], [])
    lines += item("multiplayer_touch_gametypes_item", string_index=5, handlers=[
        f'<on event="a" open="main_menu/settings_select/multiplayer_setup/playlist_select/gametype_select_screen"/>'])
    # (Create Game's row: INTERNET, LOCAL and CO-OP side by side, each chosen
    # by left and right or a tap)
    lines += _widget(f"{TOUCH}/multiplayer_touch_create_row",
                     [("type", "column_list"), ("left", 51), ("width", width), ("height", height),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items anchor_left"),
                      ("bitmap", TOUCH_ITEM)],
                     [f'<child widget="{TOUCH}/multiplayer_touch_create_inter_item"/>',
                      f'<child widget="{TOUCH}/multiplayer_touch_create_lan_item" x="118"/>',
                      f'<child widget="{TOUCH}/multiplayer_touch_online_coop"  x="204"/>'])
    for name, text, x_width in (("multiplayer_touch_create_inter_item", "INTERNET", 110),
                                ("multiplayer_touch_create_lan_item", "LOCAL", 82),
                                ("multiplayer_touch_online_coop", "CO-OP", 90)):
        lines += item(name, text=text, handlers=creates(), x_width=x_width, bitmap=None, anchored=False)
    # (its panel: the menus' description field stretched to the screen's
    # right edge; the Server Browser's counts, a headline and a line for the
    # rest, the item's description, as the computer's panel has them, by the
    # same names: menu_functions.c's multiplayer_status_update)
    def text(name, x, y, w, h, font, color, value=None):
        return _widget(f"{TOUCH}/{name}", [("type", "text"), ("controller", 1), ("left", x), ("top", y),
                                           ("width", w), ("height", h), ("flags", "anchor_left"), ("text", value),
                                           ("font", font), ("color", color), ("text_flags", "no_focus_test")], [])
    x = left + 18
    lines += _widget(f"{TOUCH}/multiplayer_touch_desc", [("width", 640), ("height", 480)],
                     [f'<child widget="{TOUCH}/{name}"/>' for name in [
                         "panel_body", "panel_cap", "multiplayer_status", "multiplayer_status_detail",
                         "online_now_label",
                         *[f"online_{count}_{digit}" for count in ("games", "players")
                           for digit in range(STATUS_DIGIT_PLACES)],
                         "online_games_label", "online_players_label", "multiplayer_options_txt"]])
    lines += _widget(f"{TOUCH}/panel_body", [("left", left), ("top", top), ("width", TOUCH_PANEL_WIDTH),
                                             ("height", panel_height), ("flags", "anchor_left stretch"),
                                             ("bitmap", TOUCH_PANEL_BODY)], [])
    lines += _widget(f"{TOUCH}/panel_cap", [("left", left + TOUCH_PANEL_WIDTH), ("top", top), ("width", 16),
                                            ("height", panel_height), ("flags", "anchor_right"),
                                            ("bitmap", TOUCH_PANEL_CAP)], [])
    lines += text("multiplayer_status", x, 96, 420, 30, "ui\\large_ui", "#FFFFFFFF")
    lines += text("multiplayer_status_detail", x, 128, 420, 90, "ui\\large_ui", "#FF2896FF")
    lines += text("online_now_label", x, 88, 200, 18, "ui\\small_ui", "#FF2896FF", "ONLINE NOW")
    for count, count_left in (("games", x), ("players", x + 170)):
        for digit in range(STATUS_DIGIT_PLACES):
            lines += _widget(f"{TOUCH}/online_{count}_{digit}",
                             [("controller", 1), ("left", count_left + DIGIT_SIZE[0] * digit), ("top", 106),
                              ("width", DIGIT_SIZE[0]), ("height", DIGIT_SIZE[1]), ("flags", "anchor_left"),
                              ("bitmap", STATUS_DIGITS)], [])
    lines += text("online_games_label", x, 124, 140, 20, "ui\\large_ui", "#FF2896FF")
    lines += text("online_players_label", x + 170, 124, 150, 20, "ui\\large_ui", "#FF2896FF")
    lines += text("multiplayer_options_txt", x, 200, 420, 80, "ui\\large_ui", "#FF2896FF")
    # (OPEN: a bar of its own, so that one tap uses it; it opens the item
    # chosen, "port multiplayer open")
    lines += _widget(f"{TOUCH}/multiplayer_touch_open_bar",
                     [("type", "column_list"), ("width", 640), ("height", 44),
                      ("flags", "pass_unhandled_to_focused_child left_right_tabs_items anchor_left")],
                     [f'<child widget="{TOUCH}/multiplayer_touch_open" x="{x}"/>'])
    lines += _widget(f"{TOUCH}/multiplayer_touch_open",
                     [("type", "text"), ("width", 300), ("height", 40), ("bitmap", TOUCH_OPEN_BUTTON),
                      ("font", "ui\\large_ui"), ("color", "#FFFFFFFF"), ("align", "center"), ("text_y", 12)],
                     ['<on event="a" run="port multiplayer open"/>', '<on event="start" run="port multiplayer open"/>',
                      '<on event="left_mouse" run="mouse emit accept event"/>'])
    lines += _widget(f"{TOUCH}/multiplayer_touch_hint",
                     [("type", "text"), ("controller", 1), ("left", 51), ("top", 446), ("width", 320), ("height", 24),
                      ("flags", "anchor_left"), ("text", "Tap to choose, tap again to open."),
                      ("font", "ui\\small_ui"), ("color", "#FF2896FF"), ("text_y", 5),
                      ("text_flags", "no_focus_test")], [])
    return lines


def _multiplayer_status() -> list:
    """Multiplayer's panel: in the place of the PC version's pictures, what
    is live for the item chosen, a headline and a line below it (the games
    online, an invite link copied, the campaign's next level:
    menu_functions.c's multiplayer_status_update), over the item's
    description"""
    lines = _widget(f"{MT}/multiplayer_status", [("type", "text"), ("controller", 1), ("left", 290), ("top", 96),
                                                 ("width", 295), ("height", 30), ("font", "ui\\large_ui"),
                                                 ("color", "#FFFFFFFF"), ("text_flags", "no_focus_test")], [])
    lines += _widget(f"{MT}/multiplayer_status_detail", [("type", "text"), ("controller", 1), ("left", 290),
                                                         ("top", 128), ("width", 295), ("height", 150),
                                                         ("font", "ui\\large_ui"), ("color", "#FF2896FF"),
                                                         ("text_flags", "no_focus_test")], [])
    # (the Server Browser's: ONLINE NOW, its games and players in the
    # titles' type, and BEST FOR YOU, the games most worth joining)
    def text(name, left, top, width, height, font, color, align=None, value=None):
        return _widget(f"{MT}/{name}", [("type", "text"), ("controller", 1), ("left", left), ("top", top),
                                        ("width", width), ("height", height), ("text", value),
                                        ("font", font), ("color", color), ("align", align),
                                        ("text_flags", "no_focus_test")], [])
    lines += text("online_now_label", 290, 84, 200, 18, "ui\\small_ui", "#FF2896FF", value="ONLINE NOW")
    for count, left in (("games", 290), ("players", 436)):
        for digit in range(STATUS_DIGIT_PLACES):
            lines += _widget(f"{MT}/online_{count}_{digit}",
                             [("controller", 1), ("left", left + DIGIT_SIZE[0] * digit), ("top", 102),
                              ("width", DIGIT_SIZE[0]), ("height", DIGIT_SIZE[1]), ("bitmap", STATUS_DIGITS)], [])
    lines += text("online_games_label", 290, 120, 140, 20, "ui\\large_ui", "#FF2896FF")
    lines += text("online_players_label", 436, 120, 150, 20, "ui\\large_ui", "#FF2896FF")
    lines += text("best_label", 290, 146, 200, 18, "ui\\small_ui", "#FF2896FF", value="BEST FOR YOU")
    for row in range(BEST_GAMES):
        top = 164 + 38 * row
        lines += text(f"best_{row}_name", 290, top, 195, 20, "ui\\large_ui", "#FFFFFFFF")
        lines += text(f"best_{row}_about", 290, top + 18, 195, 18, "ui\\small_ui", "#FF2896FF")
        lines += text(f"best_{row}_players", 486, top + 6, 72, 20, "ui\\large_ui", "#FFFFFFFF", "right")
        lines += _widget(f"{MT}/best_{row}_ping", [("controller", 1), ("left", 564), ("top", top + 9), ("width", 24),
                                                   ("height", 16), ("bitmap", PING_BARS)], [])
    # (at the end of the items, beside their names, what is live for them:
    # the local games found, an invite link copied, in green; SERVER
    # BROWSER's name leaves no room, and the panel counts its games)
    for name, color in (("item_note", "#FF7FB2FF"), ("invite_note", "#FF5ED38C")):
        lines += _widget(f"{MT}/{name}", [("type", "text"), ("width", 226), ("height", 32),
                                          ("font", "ui\\small_ui"), ("color", color), ("align", "right"),
                                          ("text_y", 8), ("text_flags", "no_focus_test")], [])
    lines += _text_button(f"{MT}/multiplayer_button_refresh", "REFRESH",
                          ['<on event="a" run="port multiplayer refresh"/>',
                           '<on event="start" run="port multiplayer refresh"/>'])
    return lines


def multiplayer_files() -> dict:
    """the port's multiplayer widgets: the browser's additions, the server
    settings, the lobby"""
    head = ['<?xml version="1.0" encoding="UTF-8"?>',
            "<!-- The port's multiplayer screens, in the PC version's style (tools/port_settings.py) -->", "<menus>"]
    return {
        f"{MT}/join_game".replace("/", ".") + ".port.xml": head + _join_game_extras() + ["</menus>", ""],
        f"{MT}/server_settings".replace("/", ".") + ".xml": head + _server_settings() + ["</menus>", ""],
        f"{MT}/lobby".replace("/", ".") + ".xml": head + _lobby() + ["</menus>", ""],
        f"{MT}/coop".replace("/", ".") + ".xml": head + _coop() + ["</menus>", ""],
        MT.replace("/", ".") + ".port.xml": head + _multiplayer_status() + ["</menus>", ""],
        TOUCH.replace("/", ".") + ".xml": head + _multiplayer_touch() + ["</menus>", ""],
        "main_menu/new_select".replace("/", ".") + ".port.xml": head + _map_kind() + ["</menus>", ""],
        "main_menu/settings_select/multiplayer_setup/item_options_edit".replace("/", ".") + ".port.xml": head + _item_options_extras() + ["</menus>", ""],
        PLAYER_EDIT.replace("/", ".") + ".port.xml": head + _player_options_extras() + ["</menus>", ""],
        TEAMPLAY_EDIT.replace("/", ".") + ".port.xml": head + _teamplay_options_extras() + ["</menus>", ""],
        OPENCE_EDIT.replace("/", ".") + ".port.xml": head + _opence_options() + ["</menus>", ""],
        MODS.replace("/", ".") + ".xml": [head[0], "<!-- The port's Mods screen, in the PC version's style "
                                          "(tools/port_settings.py) -->", head[2]] + _mods() + ["</menus>", ""],
        UPDATE.replace("/", ".") + ".xml": [head[0], "<!-- The update question (the Steam Frame's, gamescope's), in "
                                            "the style of the PC version's Quit (tools/port_settings.py) -->",
                                            head[2]]
        + _update() + ["</menus>", ""],
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
MENU_ITEM_SPACES = {"MO": 3.17, "OD": 2.8, "DS": 2.3}
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


def _words_button(name: str, words: list, handlers: list) -> list:
    """a button of the button bar whose words change: its string list's, the
    one shown chosen by its string_list_index (menu_functions.c), as a text
    set on a button of fixed words is drawn over by them"""
    pairs = [("type", "text"), ("width", 128), ("height", 24), ("bitmap", "bitmaps/text_button_background"),
             ("string_list", f"{name}_words"), ("font", "ui\\small_ui"), ("color", "#FFFFFFFF"), ("align", "center"),
             ("text_y", 2)]
    return (_widget(name, pairs, handlers + ['<on event="left_mouse" run="mouse emit accept event"/>']) +
            _strings(f"{name}_words", words))


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
    (menu_functions.c)"""
    base = MODS
    # (its game data input notes the main menu shown, over which the update
    # question opens: _update)
    lines = _widget("main_menu/main_menu_item_mods",
                    [("type", "text"), ("width", 256), ("height", 33), ("bitmap", "main_menu/menu_mods")],
                    ['<data input="port update question"/>',
                     f'<on event="a" open="{base}/mods_screen"/>', f'<on event="start" open="{base}/mods_screen"/>',
                     '<on event="left_mouse" run="mouse emit accept event"/>'])
    lines += _widget(f"{base}/mods_screen", [("width", 640), ("height", 480),
                                             ("flags", "pass_unhandled_to_focused_child pause_game"),
                                             ("bitmap", "bitmaps/gradient")],
                     ['<on event="b" back="true"/>', '<on event="back" back="true"/>',
                      '<child widget="main_menu/new_select/sel_list_desc_bkd"/>',
                      f'<child widget="{base}/mods_list"/>',
                      f'<child widget="{base}/header_mods"/>'])
    lines += _header(f"{base}/header_mods", f"{base}/header_mods")
    # (the lobby's rows, MOD_ROWS of them, scrolling over more mods)
    rows = [f'<child widget="{base}/op_mod_{index + 1}" x="20" y="{73 + 30 * index}"/>' for index in range(MOD_ROWS)]
    lines += _widget(f"{base}/mods_list", [("type", "column_list"), ("width", 640), ("height", 480),
                                           ("flags", "pass_unhandled_to_focused_child up_down_tabs_children"),
                                           ("description", f"{base}/mod_details")],
                     ['<data input="port mods update"/>', '<on event="created" run="port mods init"/>',
                      '<on event="deleted" run="port mods close"/>', *rows,
                      f'<child widget="{base}/button_bar" y="414"/>'])
    for index in range(MOD_ROWS):
        lines += _widget(f"{base}/op_mod_{index + 1}",
                         [("width", 390), ("height", 28), ("flags", "pass_unhandled_to_focused_child"),
                          ("bitmap", "bitmaps/sel_list_item_bkd_top" if index == 0 else "bitmaps/sel_list_item_bkd"),
                          ("color", "#FF2896FF")],
                         ['<on event="a" run="port mods toggle"/>', '<on event="start" run="port mods toggle"/>',
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
                     [f'<child widget="{base}/button_up" y="1"/>', f'<child widget="{base}/button_down" x="130" y="1"/>',
                      f'<child widget="{base}/button_ok" x="380" y="1"/>',
                      '<child widget="common_button_cancel" x="510" y="1"/>'])
    lines += _text_button(f"{base}/button_up", "MOVE UP", ['<on event="a" run="port mods move up"/>',
                                                          '<on event="start" run="port mods move up"/>'])
    lines += _text_button(f"{base}/button_down", "MOVE DOWN", ['<on event="a" run="port mods move down"/>',
                                                              '<on event="start" run="port mods move down"/>'])
    lines += _button(f"{base}/button_ok", 1, ['<on event="a" run="port mods save" back="true"/>',
                                              '<on event="start" run="port mods save" back="true"/>'])
    return lines


def _update() -> list:
    """the update question of the Steam Frame and of gamescope (the Steam
    Deck's Game Mode: port/linux/src/updater.c, where a system dialog would
    not show), laid out as the PC version's Quit is
    (ce/main_menu.quit_select.xml): its box, its header, and its two
    buttons, UPDATE ("port update now": the game closes, and its launcher,
    or the game itself under gamescope, puts the update in place and starts
    it) and NOT NOW (back to
    the main menu, which menu_functions.c's pc_menu_update_question_open
    opened it over); B is NOT NOW. To stop the questions: Network Setup's
    CHECK FOR UPDATES"""
    base = UPDATE
    lines = _widget(f"{base}/update_screen",
                    [("type", "text"), ("controller", 1), ("left", 180), ("top", 160), ("width", 280), ("height", 160),
                     ("flags", "pass_unhandled_to_focused_child"), ("bitmap", "bitmaps/alert_bkd"),
                     ("string_list", f"{base}/update_prompt"), ("font", "ui\\small_ui"), ("color", "#FF2896FF"),
                     ("align", "center"), ("text_x", -10), ("text_y", 40)],
                    ['<on event="b" back="true"/>', '<on event="back" back="true"/>',
                     f'<child widget="{base}/button_bar" x="182" y="293"/>',
                     f'<child widget="{base}/header_update" x="182" y="164"/>'])
    lines += _widget(f"{base}/header_update",
                     [("type", "text"), ("controller", 1), ("width", 200), ("height", 20), ("text", "NEW VERSION"),
                      ("font", "ui\\large_ui"), ("color", "#FF2896FF")], [])
    lines += _widget(f"{base}/button_bar",
                     [("type", "column_list"), ("width", 256), ("height", 20),
                      ("flags", "pass_unhandled_to_focused_child up_down_tabs_items left_right_tabs_items")],
                     [f'<child widget="{base}/button_update"/>', f'<child widget="{base}/button_later" x="128"/>'])
    lines += _text_button(f"{base}/button_update", "UPDATE", ['<on event="a" run="port update now"/>',
                                                             '<on event="start" run="port update now"/>'])
    lines += _text_button(f"{base}/button_later", "NOT NOW", ['<on event="a" back="true"/>',
                                                              '<on event="start" back="true"/>'])
    lines += _strings(f"{base}/update_prompt",
                      ["A new version is ready.\\nUpdate now? The game will\\nclose, update and start again."])
    return lines


def main() -> None:
    """writes the port's own screens' files (port/assets/menus/ce), as
    tools/ce_menus.py does with the rest, which needs the PC version's tags:
    for a change to them alone (a value added to a setting's spinner).
    A new file is listed in menus.json by hand, a new title drawn by
    ce_menus.py"""
    from pathlib import Path

    folder = Path(__file__).resolve().parent.parent / "port/assets/menus/ce"
    for name, lines in [*settings_files().items(), *multiplayer_files().items()]:
        (folder / name).write_text("\n".join(lines))


if __name__ == "__main__":
    main()
