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
# (ce/main_menu.xml): MODS in Credits' place (Credits is left out:
# tools/ce_menus.py's LEFT_OUT), Quit a row below it (its CHILD_OFFSETS)
MODS = "main_menu/mods"
MOD_ROWS = 11
MODS_ITEM_PLACE = (192, 391)
QUIT_ITEM_PLACE = (192, 427)
# the Steam Frame's update question, over the main menu (_update)
UPDATE = "main_menu/update"
YES_NO = [("YES", "true"), ("NO", "false")]
ON_OFF = [("ON", "true"), ("OFF", "false")]
# the multiplayer armor colors, in the profile's order (player_profile.c)
SHIELD_COLORS = ["white", "black", "red", "blue", "gray", "yellow", "green", "pink", "purple", "cyan", "cobalt",
                 "orange", "teal", "sage", "brown", "tan", "maroon", "salmon"]
SENSITIVITIES = [(f"{value:g}", f"{value:g}") for value in (0.1, 0.15, 0.25, 0.4, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3, 4)]
VOLUMES = [(str(step), f"{step / 10:g}") for step in range(11)]
# the platforms a menu element's platform attribute names (menu_files.c's
# MENU_PLATFORM, "vr": the VR build, playing in the headset, and "hdr": a
# renderer that draws display.hdr)
MENU_PLATFORMS = ("desktop", "android", "web", "vr", "hdr")
# display.hdr_paper_white's values, percent of the system's SDR white
PAPER_WHITES = (50, 60, 70, 80, 90, 100, 110, 125, 150, 175, 200, 250)

# each screen: its folder below PE, its screen's widget (the name the profile
# menu opens), its header (widget, bitmap), the row spacing, and its rows:
# (label, setting, [(shown, value)], help, platform[, key]): platform is None
# for every platform, else the platforms the row is for, separated by spaces
# (MENU_PLATFORMS: the menus' platform attribute,
# port/linux/src/menu_files.c); key names the row's widgets where two rows set
# one setting (one for each platform), else the setting's name does
SCREENS = {
    "video_settings": {
        "screen": "video_settings_screen",
        "header": ("header_profile_video_settings", f"{PE}/video_settings/header_profile_video_settings"),
        # (eleven places on the desktop: closer than the other screens' 30px
        # step, the last row where it was at 30.) each platform packs its own
        # rows, so a row hidden on this machine does not leave a gap
        "spacing": 27,
        "help_top": 364,
        "platform_places": True,
        # (rows in the place of the row before them: Window Size in
        # Resolution's, port/linux/game/menu_functions.c showing the one the
        # display mode chosen uses; Android's and the web's anti-aliasing in the
        # desktop's)
        "same_place": ["display.window_size", "anti_aliasing_android", "anti_aliasing_web"],
        "rows": [
            ("DISPLAY MODE:", "display.mode",
             [("FULLSCREEN", "fullscreen"), ("BORDERLESS", "borderless"), ("WINDOWED", "windowed")],
             "Fullscreen takes the display; borderless covers it\nwith a window. F11 switches to the window.",
             "desktop"),
            # (port/linux/game/menu_tags.c adds the display's resolutions)
            ("RESOLUTION:", "display.resolution", [("NATIVE", "native")],
             "What fullscreen and borderless draw at. Fullscreen\nsets the display to it; borderless scales it.",
             "desktop"),
            # (port/linux/game/menu_tags.c puts the sizes that fit the desktop
            # in place of this one)
            ("WINDOW SIZE:", "display.window_size", [("1280 x 960", "1280x960")],
             "The window's size: 4:3, then 16:10, 16:9 and 21:9\n(its edges can also be dragged).", "desktop"),
            ("RENDER RESOLUTION:", "display.resolution_scaling", [("FULL", "native"), ("XBOX 640X480", "original")],
             "Full draws at your screen's resolution; Xbox draws\nthe Xbox's 640x480 and scales it up.", "desktop web"),
            ("V-SYNC:", "display.vsync", ON_OFF,
             "Wait for the display between frames: the picture\nnever tears, at a little more input lag.", None),
            ("FRAME RATE LIMIT:", "display.max_fps",
             [("AUTO", "0"), ("30", "30"), ("60", "60"), ("120", "120"), ("144", "144"), ("165", "165"),
              ("240", "240"), ("NONE", "-1")],
             "With V-Sync off, the most frames a second. Auto:\ntwice the display's refresh rate.", "desktop web"),
            # (display.battery_saver: port/linux/src/battery_saver.c)
            ("BATTERY SAVER:", "display.battery_saver", [("OFF", "off"), ("ON", "on"), ("AUTO", "auto")],
             "Up to 60 frames a second, Classic graphics, for\nbattery and heat. Auto: on while on battery.", None),
            ("SMOOTH MOTION:", "display.interpolation", ON_OFF,
             "Smooth movement at any frame rate, between the\ngame's 30 updates a second.", None),
            ("INSTANT AIM:", "display.direct_camera", ON_OFF,
             "Turn the view the moment the mouse moves, not up\nto two of the game's updates later.", "desktop web"),
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
             "Smooth jagged edges. FXAA and SMAA cost little;\nSSAA and MSAA are sharper and cost more.",
             "desktop"),
            # (Android's Vulkan draws SMAA; the web's OpenGL ES has FXAA alone)
            ("ANTI-ALIASING:", "display.anti_aliasing",
             [("OFF", "off"), ("FXAA", "fxaa"), ("SMAA", "smaa"), ("MSAA 2X", "msaa2x"), ("MSAA 4X", "msaa4x")],
             "Smooth jagged edges. FXAA and SMAA cost little;\nMSAA is sharper and costs more.",
             "android", "anti_aliasing_android"),
            ("ANTI-ALIASING:", "display.anti_aliasing",
             [("OFF", "off"), ("FXAA", "fxaa"), ("MSAA 2X", "msaa2x"), ("MSAA 4X", "msaa4x")],
             "Smooth jagged edges. FXAA costs little; MSAA is\nsharper and costs more.",
             "web", "anti_aliasing_web"),
            ("TEXTURE FILTERING:", "display.anisotropic_filtering",
             [("OFF", "0"), ("2X", "2"), ("4X", "4"), ("8X", "8"), ("16X", "16")],
             "Keep floors and walls sharp into the distance.\nOff looks as the Xbox did.", None),
            ("SHADOW QUALITY:", "display.shadow_resolution",
             [("XBOX", "128"), ("MEDIUM", "256"), ("HIGH", "512"), ("ULTRA", "1024")],
             "How smooth the edges of objects' shadows are:\nXBOX as the original; higher costs more.",
             None),
            ("SMOOTH LIGHTING:", "display.per_pixel_lighting", ON_OFF,
             "Light characters and weapons smoothly, without\nthe flat facets of the Xbox's lighting.", None),
            ("MODEL DETAIL:", "display.model_detail",
             [("AUTO", "auto"), ("XBOX", "stock"), ("FULL", "native"), ("HIGHEST", "highest")],
             "Detail of models in the distance: Xbox as on a TV,\nFull for your screen. Auto: Full in VR.", None),
            ("SHIELD GLOW:", "display.shield_glow", ON_OFF,
             "Shields light their surroundings as they flare,\nin the shield's own color.", None),
            ("GLOW INTENSITY:", "display.shield_glow_intensity", [("DEFAULT", "default"), ("LIGHT SHOW", "light_show")],
             "Light Show: the shield glow reaches three times\nas far, and is brighter.", None),
            ("DYNAMIC LIGHTS:", "display.dynamic_lights", [("CLASSIC", "classic"), ("ENHANCED", "enhanced")],
             "Enhanced: plasma bolts, muzzle flashes and blasts\nlight their surroundings smoothly, with a glow.",
             "desktop android"),
            ("MATCH TIMER:", "display.match_timer", ON_OFF,
             "Draw a clock at the bottom right in multiplayer,\ncounting up from the start of the game.", None),
            # (display.hdr's rows, on the renderers that draw it: menu_files.c's
            # "hdr")
            ("HDR:", "display.hdr", [("AUTO", "auto"), ("ON", "on"), ("OFF", "off")],
             "Brighter highlights on an HDR display. Auto: on\nwhen the display shows HDR.", "hdr"),
            ("HDR MODE:", "display.hdr_mode", [("FAITHFUL", "faithful"), ("PUNCHY", "punchy")],
             "Faithful: the stock picture, brighter highlights.\nPunchy: the whole scene lifted, remaster style.",
             "hdr"),
            ("HDR BRIGHTNESS:", "display.hdr_paper_white", [(f"{value}%", str(value)) for value in PAPER_WHITES],
             "With HDR, how bright white surfaces and the HUD\nare, in percent of the system's white.", "hdr"),
            # (OpenCE's OVERSHIELD and HDR DISPLAY rows are left out: OpenH1's
            # renderers do not draw the overshield's rise, and HDR output is
            # display.hdr's own; port/linux/README.md)
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
            ("BUTTON PROMPTS:", "input.button_prompts",
             [("AUTO", "auto"), ("CONTROLLER", "controller"), ("KEYBOARD", "keyboard")],
             "The HUD's prompts name the key bound to the action.\nAuto: the keys or the buttons, as last played with.",
             None),
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
             "Turn all sound off or on. Takes effect the next\ntime the game starts.", None),
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
             "desktop android web"),
            ("OPEN ROUTER PORT:", "network.allow_upnp", ON_OFF,
             "Ask your router to open the game's port (UPnP),\nso that players can reach your games.",
             "desktop android"),
            ("JOIN FROM CLIPBOARD:", "network.join_from_clipboard", ON_OFF,
             "Join a game when you switch to the game with its\ninvite link copied.", "desktop android"),
            ("CHECK FOR UPDATES:", "update.auto", ON_OFF,
             "Look for a new version when the game starts.", "desktop android"),
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
            ("BUTTON LAYOUT:", "profile.button_preset",
             [("DEFAULT", "0"), ("SWAP TRIGGERS", "1"), ("SWAP A, L TRIGGER", "2"), ("SWAP B, L TRIGGER", "3"),
              ("SWAP B, R STICK", "4"), ("BUMPER JUMPER", "5")],
             "Which of the controller's buttons does what (not\nthe keyboard's: Controls Setup sets those).", None),
            ("STICK LAYOUT:", "profile.joystick_preset",
             [("DEFAULT", "0"), ("SOUTHPAW", "1"), ("LEGACY", "2"), ("LEGACY SOUTHPAW", "3")],
             "Which stick moves and which looks.", None),
            ("VIBRATION:", "profile.vibration", ON_OFF, "Rumble the controller.", None),
            ("IN-GAME HELP:", "profile.ingame_help", ON_OFF, "Show the game's hints about its controls.", None),
        ],
    },
}

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
# Video Setup's categories open ordinary settings screens. Each has the same
# pending edits, Defaults, OK and Cancel as the other settings screens.
_video = SCREENS["video_settings"]
_graphics = {"display.high_res_hud", "display.hud_button_style", "display.high_res_text", "display.anti_aliasing",
             "display.anisotropic_filtering",
             "display.shadow_resolution", "display.per_pixel_lighting",
             "display.dynamic_lights", "display.model_detail", "display.match_timer"}
# (first, display.graphics: Classic or Enhanced, which shows the rows it
# chooses at its values as it changes, port/linux/game/menu_functions.c's
# graphics_preset_show; last, the simulation's rate, game.sim_rate, which
# menu_functions.c's sim_rate_row_show locks at ORIGINAL (30) in a game that
# plays at 30 whatever it says: a network game, split screen, a film's
# playback. Twelve rows, closer than the others' 30px, the help line lower)
# (the visual modifiers: a screen of their own, Extra Effects)
_extra_effects = {"display.shield_glow", "display.shield_glow_intensity", "display.hdr", "display.hdr_mode",
                  "display.hdr_paper_white"}
SCREENS["video_settings/graphics"] = {
    "screen": "graphics_settings_screen", "header": _video["header"], "spacing": 24, "help_top": 366,
    "same_place": ["anti_aliasing_android", "anti_aliasing_web"],
    "rows": [("PRESET:", "display.graphics", [("ENHANCED", "enhanced"), ("CLASSIC", "classic")],
              "Classic draws as the Xbox did; Enhanced turns on\nthe improvements below. Each can still be changed.",
              None)] +
            [row for row in _video["rows"] if row[1] in _graphics] +
            [("TICK RATE:", "game.sim_rate", [("ORIGINAL (30)", "30"), ("60", "60"), ("120", "120")],
              "How often the world updates. 60 and 120 respond\nfaster; online games always play at 30.", None)],
}
SCREENS["video_settings/extra_effects"] = {
    "screen": "extra_effects_settings_screen", "header": _video["header"], "spacing": 30,
    "rows": [row for row in _video["rows"] if row[1] in _extra_effects],
}
_video["rows"] = [row for row in _video["rows"] if row[1] not in _graphics | _extra_effects]
_video["categories"] = [
    ("GRAPHICS:", "video_settings/graphics",
     "Classic or Enhanced, the HUD, text, shadows,\nlighting, anti-aliasing and the tick rate."),
    ("FOV AND VIEWMODELS:", "video_settings/fov_viewmodels",
     "The field of view and the first-person\nweapon. Their defaults keep the stock view."),
    ("EXTRA EFFECTS:", "video_settings/extra_effects",
     "Visual modifiers the stock game does not\nhave, such as the shield glow and HDR."),
]
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
        ("RESAMPLER:", "audio.resampler", [("CLEAN", "sinc"), ("CLASSIC", "linear")],
         "How sounds are converted to your speakers' rate:\nClean is clear; Classic is brighter but harsher.",
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
     "desktop android"),
    ("MULTIPLAYER HUD:", "network_setup/multiplayer_hud",
     "Players' names above their heads, the\nscoreboard, and the text chat."),
    ("STREAMER MODE:", "network_setup/streamer_mode",
     "Hide offensive words in names and text from\nother players, maps and mods, as stars."),
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

# Network Setup's multiplayer HUD: its rows, moved from Network Setup, and the
# text chat's (port/linux/game/network_chat.c)
SCREENS["network_setup/multiplayer_hud"] = {
    "screen": "multiplayer_hud_settings_screen", "header": _network["header"], "spacing": 30,
    "rows": _hud_rows + [
        ("TEXT CHAT:", "display.text_chat", [("SHOW", "true"), ("HIDE", "false")],
         "SHOW the text chat, or HIDE it: no lines, and\nits keys and D-pad Up open nothing.", None),
        ("CHAT TEXT:", "display.chat_style", [("COLORS", "colors"), ("WHITE", "white")],
         "The text chat's colors: COLORS, names in their\nteam's color; WHITE, as Custom Edition.", None),
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
    "platform_places": True, "rows": [], "categories": _network["categories"],
}

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


def settings_files() -> dict:
    """the files of the port's screens, by their names in ce/"""
    files = {}
    for folder, spec in SCREENS.items():
        files[f"{PE}/{folder}".replace("/", ".") + ".xml"] = _setting_screen(folder, spec)
    files[f"{PE}/vr_controls".replace("/", ".") + ".xml"] += _vr_item()
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
AIM_ASSIST_HELP = [
    "Players keep their current mouse aim assist setting.",
    "Disable mouse aim assist for mouse/keyboard players.\\nController and touch aim assist are unchanged.",
]

STRING_INSERTS = {
    f"{PLAYER_EDIT}/player_options_labels": [(8, ["AIM ASSIST:"])],
    f"{PLAYER_EDIT}/cap_player_options": [(28, AIM_ASSIST_HELP)],
    # (Teamplay Options' voice chat and vote kick rows, after its own:
    # TEAMPLAY_ROWS)
    f"{TEAMPLAY_EDIT}/teamplay_options_labels": [(3, [label for _, label, *_ in TEAMPLAY_ROWS])],
    f"{TEAMPLAY_EDIT}/cap_teamplay_options": [(10, [text for *_, helps in TEAMPLAY_ROWS for text in helps])],
    f"{SLAYER_EDIT}/var_kills_to_win": [(5, ["75", "100", "150", "200", "250", "500"])],
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
    "main_menu/gametype_select/var_gametype_banks": ["STANDARD", "CUSTOM"],
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
    "main_menu/gametype_select/var_gametype_banks": [0, 2],
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
MULTIPLAYER_GAMETYPES_PLACE = (0, 370)

# changes to the PC version's widgets (by our names): attributes set, all
# their handlers replaced, children added, game data inputs added
WIDGET_PATCHES = {
    # The host returns here after a round with no menu history. Back must
    # reopen the main menu when it closes the last screen, as the lobby does.
    f"{MT}/connected/connected_map_select_wrapper": {"set": {
        "flags": "pass_unhandled_to_focused_child pause_game main_menu_if_no_history tag_controller_index force_handle_mouse",
    }},
    # (the main menu's MODS, before Quit: _mods)
    "main_menu/main_menu_select_list": {"insert_before": {"main_menu/main_menu_item_quit_game": [
        f'<child widget="main_menu/main_menu_item_mods" x="{MODS_ITEM_PLACE[0]}" y="{MODS_ITEM_PLACE[1]}"/>']}},
    f"{PLAYER_EDIT}/player_options_menu": {"insert_before": {
        f"{PLAYER_EDIT}/player_button_bar": [
            f'<child widget="{PLAYER_EDIT}/op_aim_assist" x="54" y="313"/>']}},
    f"{PLAYER_EDIT}/player_options_help": {"set": {"top": 345, "height": 60}},
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
    f"{MT}/multiplayer_type_button_bar": {"insert_before": {
        "common_button_back": [f'<child widget="{MT}/multiplayer_button_refresh" x="380" y="1"/>']}},
    f"{MT}/multiplayer_type_select_screen": {"handlers": [
        '<on event="created" run="mp type menu initialize"/>',
        '<on event="deleted" run="port multiplayer status end"/>']},
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
    f"{MT}/join_game/join_game_button_bar": {"swap": {
        f"{MT}/join_game/join_game_button_refresh": f"{MT}/join_game/button_clipboard",
        f"{MT}/join_game/join_game_button_update": f"{MT}/join_game/join_game_button_refresh",
    }},
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


def _join_game_extras() -> list:
    """the browser's added titles, Direct Link's clipboard button, and the
    password screen"""
    base = f"{MT}/join_game"
    lines = _password_screen()
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
        # (LISTING's, by its choice)
        "Anyone can see and join your game: it is listed\\nin everyone's Server Browser.",
        "Only players with your invite link can join.",
        # (co-op's options' rows: COOP_SETUP_SCREENS)
        *(help_text for _, _, help_text, _ in COOP_SETUP_SCREENS),
        # (PASSWORD's)
        "Players from the Server Browser must type it to\\njoin (an invite link needs none). Enter sets it.",
        # (MAP SHARING's)
        MAP_SHARE_SETUP[2],
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
SETUP_OPTIONS_PLACE = 6
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
    """The host's aim-assist policy, hidden while editing saved gametypes."""
    base = PLAYER_EDIT
    lines = _widget(f"{base}/op_aim_assist", [("width", 512), ("height", 28),
        ("flags", "pass_unhandled_to_focused_child"), ("bitmap", "bitmaps/option_bkds"),
        ("color", "#FF2896FF")], [f'<child widget="{base}/aim_assist_label"/>',
        f'<child widget="{base}/aim_assist_spinner" x="286" y="1"/>'])
    lines += _widget(f"{base}/aim_assist_label", [("type", "text"), ("controller", 1),
        ("width", 280), ("height", 22), ("string_list", f"{base}/player_options_labels"),
        ("string_index", 8), ("font", "ui\\large_ui"), ("color", "#FF2896FF"),
        ("text_x", 13), ("text_y", 4)], [])
    lines += _widget(f"{base}/aim_assist_spinner", [("type", "spinner"), ("top", 2),
        ("width", 206), ("height", 20), ("flags", "pass_unhandled_to_focused_child left_right_tabs_items"),
        ("string_list", f"{base}/var_aim_assist"), ("font", "ui\\large_ui"),
        ("color", "#FF2896FF"), ("align", "center"), ("text_y", 4),
        ("list_flags", "items_from_strings"), ("header_bitmap", "bitmaps/arrow_sm_left"),
        ("footer_bitmap", "bitmaps/arrow_sm_right"), ("header_bounds", "7 -13 19 -7"),
        ("footer_bounds", "7 208 19 214")], ['<on event="left_mouse" run="mouse spinner 1wide click"/>'])
    return lines + _strings(f"{base}/var_aim_assist", ["ALLOWED", "BLOCK M+KB"])


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
        "main_menu/new_select".replace("/", ".") + ".port.xml": head + _map_kind() + ["</menus>", ""],
        "main_menu/settings_select/multiplayer_setup/item_options_edit".replace("/", ".") + ".port.xml": head + _item_options_extras() + ["</menus>", ""],
        PLAYER_EDIT.replace("/", ".") + ".port.xml": head + _player_options_extras() + ["</menus>", ""],
        TEAMPLAY_EDIT.replace("/", ".") + ".port.xml": head + _teamplay_options_extras() + ["</menus>", ""],
        MODS.replace("/", ".") + ".xml": [head[0], "<!-- The port's Mods screen, in the PC version's style "
                                          "(tools/port_settings.py) -->", head[2]] + _mods() + ["</menus>", ""],
        UPDATE.replace("/", ".") + ".xml": [head[0], "<!-- The Steam Frame's update question, in the style of the "
                                            "PC version's Quit (tools/port_settings.py) -->", head[2]]
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
    # (its game data input notes the main menu shown, over which the Steam
    # Frame's update question opens: _update)
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
    """the Steam Frame's update question (port/linux/src/updater.c, which a
    system dialog would not show in the headset), laid out as the PC
    version's Quit is (ce/main_menu.quit_select.xml): its box, its header,
    and its two buttons, UPDATE ("port update now": the game quits, and its
    launcher installs the update and starts it again) and NOT NOW (back to
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
