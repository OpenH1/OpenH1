/*
PORT_CONFIG.C

The native ports' settings (port_config.h), parsed with tomlc17
(port/third_party/tomlc17). Every setting is in the table below with its
type, default, the HALO_* environment variable that overrides it and the
comment written into a new file. The file is read once, on the first
question; unknown keys and values of the wrong type are reported in the log
and the defaults used instead. A setting the player has not set is written
commented out at its default ("# vsync = true", CONFIG_DEFAULTS_NOTE), so
that a newer default reaches it; otherwise the file only gains the settings
it lacks (config_add_missing) and the lines config_write sets, so that the
player's edits and comments stay. display.graphics chooses the drawing's
settings the player has not set (config_graphics), and battery saver reads
them at their "classic" values while it is on (config_battery_saver_set).
*/

#include "platform.h"
#include "install_layout.h"
#include "port_config.h"
#include "tomlc17.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef HALO_WINDOWS
#include <unistd.h>
#endif

/* ---------- the settings */

enum config_type
{
	_config_boolean,
	_config_integer,
	_config_real,
	_config_string,
};

/* how the setting's environment variable sets it */
enum config_environment
{
	/* the variable's text is the value ("0", "false", "no" and "off" are
	false for a boolean) */
	_environment_value,
	/* the variable being set (not empty, "0", "false", "no" or "off") makes it true */
	_environment_set_is_true,
	/* the variable being set (not empty, "0", "false", "no" or "off") makes it false */
	_environment_set_is_false,
};

/* the builds a setting means something in, and is written for */
enum
{
	_platform_desktop = 1,
	_platform_android = 2,
	_platform_all = _platform_desktop | _platform_android,
	/* (of the desktop builds, only Windows) */
	_platform_windows = 4,
	/* (the Linux builds whose game runs as a guest of the Android port's
	host, port/linux/host, which have the desktop's settings, and the
	host's own) */
	_platform_guest = 16,
	/* (the iOS app, a guest build with the desktop's settings, and the
	touch controls': port/ios) */
	_platform_ios = 32,
};

struct config_setting
{
	const char *name;
	enum config_type type;
	/* as it is written in the file */
	const char *default_value;
	/* NULL: none, for a setting the Android app reads from the file itself */
	const char *environment;
	enum config_environment environment_style;
	unsigned platforms;
	const char *comment;
};

/* crouching's keys */
#define CROUCH_KEYS "\"Left Ctrl, C\""

/* display.graphics = "enhanced": the values it gives the settings it
chooses (config_graphics), which are their defaults, the same on every
build: SMAA (on iOS MSAA 4x), 16x anisotropic filtering, 512 shadow maps,
the models' detail by the display's lines and lit for each pixel, the
enhanced dynamic lights */
#ifdef HALO_IOS
/* (an iPhone's tile-based GPU resolves MSAA in its tile memory, the 3D view
drawn in one pass, port/macos/xgpu_metal.m: about the cost of SMAA's
passes over the screen there, and smoother edges) */
#define GRAPHICS_ANTI_ALIASING "\"msaa4x\""
#else
#define GRAPHICS_ANTI_ALIASING "\"smaa\""
#endif
#define GRAPHICS_ANISOTROPIC_FILTERING "16"
#define GRAPHICS_SHADOW_RESOLUTION "512"
#define GRAPHICS_PER_PIXEL_LIGHTING "true"
#define GRAPHICS_MODEL_DETAIL "\"native\""
#define GRAPHICS_DYNAMIC_LIGHTS "\"enhanced\""
/* display.astc_textures: "auto" everywhere (iPhones, through Metal; Android
phones without BC, through Vulkan: seen right on a Pixel 11 Pro's PowerVR) */
#define ASTC_TEXTURES_DEFAULT "\"auto\""
/* display.structure_culling's tests: all of them (in VR too, where the game
culls once from the head's centre and the tests allow for the eyes beside it:
structure_culling_set_camera_radius) */
#define STRUCTURE_CULLING_DEFAULT "31"
/* display.vsync: off on the Linux builds, Windows and macOS, whose players
want the most frames and the least latency (display.max_fps still limits
them, at twice the display's rate; the Mac shows the newest at each refresh,
host_metal_vsync), the Steam Deck's too; on the phones, for their
batteries */
#if defined(HALO_WINDOWS) || (defined(HALO_VULKAN) && !defined(HALO_ANDROID)) || \
	(defined(HALO_MACOS) && !defined(HALO_IOS))
#define VSYNC_DEFAULT "false"
#else
#define VSYNC_DEFAULT "true"
#endif
/* (in the comment of each setting it chooses) */
#define GRAPHICS_CHOOSES "\nCommented out, display.graphics chooses it."
/* in input.mouse_sensitivity's comment: a file with it has the mouse's
settings in CS2's units; one without, the older versions' (config_migrate_mouse) */
#define CONFIG_MOUSE_MARK "Counter-Strike 2's"
/* the older versions' input.mouse_sensitivity times this is CS2's: 0.0022
radians a count over CS2's 0.022 degrees (port/linux/include/mouse_sensitivity.h) */
#define CONFIG_MOUSE_FROM_OLD (0.0022 * 180.0 / 3.14159265358979323846 / 0.022)

static const struct config_setting config_settings[] =
{
	{ "display.fullscreen", _config_boolean, "true", "HALO_FULLSCREEN", _environment_value, _platform_desktop,
		"Where display.mode is empty: start borderless over the whole display;\n"
		"false starts in a window. F11 switches." },
	{ "display.mode", _config_string, "\"\"", "HALO_DISPLAY_MODE", _environment_value, _platform_desktop,
		"\"fullscreen\" takes the display (at display.resolution's mode),\n"
		"\"borderless\" is a window over the whole desktop, \"windowed\" a window\n"
		"(display.window_size). Empty: display.fullscreen's (true: borderless).\n"
		"F11 switches to the window and back." },
	{ "display.resolution", _config_string, "\"native\"", "HALO_RESOLUTION", _environment_value, _platform_desktop,
		"What fullscreen and borderless draw at: \"native\", the display's own, or\n"
		"\"<width>x<height>\" (\"1920x1080\"), 640x480 or more. Fullscreen sets the\n"
		"display to it; borderless draws it scaled to the display." },
	{ "display.resolution_scaling", _config_string, "\"native\"", "HALO_RESOLUTION_SCALING", _environment_value,
		_platform_desktop,
		"\"native\" draws at the window's resolution (fullscreen, the display's or\n"
		"display.resolution); \"original\" draws the Xbox's 640x480 and scales it\n"
		"up." },
	{ "display.render_scale", _config_real, "1.0", "HALO_RENDER_SCALE", _environment_value, _platform_ios,
		"iOS: the fraction of the screen's pixels the game draws, each way (0.25\n"
		"to 1.0), which the display scales up: 0.67 draws less than half the\n"
		"pixels, for a cooler phone and a steadier frame rate. Read at start." },
	{ "display.window_size", _config_string, "\"\"", "HALO_WINDOW_SIZE", _environment_value, _platform_desktop,
		"The window's size, \"<width>x<height>\" (\"1920x1080\"), 640x480 or more (it\n"
		"can be resized). Empty: display.window_scale's." },
	{ "display.window_scale", _config_integer, "2", "HALO_WINDOW_SCALE", _environment_value, _platform_desktop,
		"Where display.window_size is empty: the window's size as a multiple of\n"
		"640x480." },
	{ "display.vsync", _config_boolean, VSYNC_DEFAULT, "HALO_NO_VSYNC", _environment_set_is_false, _platform_all,
		"Wait for the display between frames; false draws as fast as possible\n"
		"(up to display.max_fps)." },
	{ "display.max_fps", _config_integer, "0", "HALO_MAX_FPS", _environment_value, _platform_desktop,
		"With vsync off, the most frames a second: 0 for twice the display's\n"
		"refresh rate, -1 for no limit (which can hang some Intel graphics)." },
	{ "display.battery_saver", _config_string, "\"off\"", "HALO_BATTERY_SAVER", _environment_value, _platform_all,
		"Draw less, for a longer battery and a cooler machine: \"on\", \"off\", or\n"
		"\"auto\", on while the machine runs on its battery. On, each frame stays\n"
		"for a whole number of the display's refreshes, 30 to 60 frames a second\n"
		"(60 at 60 or 120 Hz, 45 at 90), and the settings display.graphics chooses\n"
		"that are not set here are drawn as \"classic\" draws them. The game\n"
		"plays the same. Not in VR, whose headset paces its frames." },
	{ "display.graphics", _config_string, "\"enhanced\"", "HALO_GRAPHICS", _environment_value, _platform_all,
		"The 3D view's look: \"classic\" as the Xbox drew it; \"enhanced\" with this\n"
		"port's improvements together (anti-aliasing, sharper textures and\n"
		"shadows, models' detail and lighting for each pixel, the enhanced\n"
		"dynamic lights), the same on every machine (a renderer that lacks one\n"
		"draws without it). Each of those settings, set here, wins over it." },
	{ "display.anti_aliasing", _config_string, GRAPHICS_ANTI_ALIASING, "HALO_ANTI_ALIASING", _environment_value,
		_platform_all,
		"Smoothing of jagged edges, which the Xbox did not have: \"off\"; \"fxaa\"\n"
		"or \"smaa\" smooth the 3D view once it is drawn (the HUD and menus stay\n"
		"sharp); \"ssaa2x\" draws at twice the resolution each way (four times\n"
		"the work); \"msaa2x\", \"msaa4x\" or \"msaa8x\" draw with that many samples\n"
		"a pixel. Android, macOS and iOS have no \"ssaa2x\"." GRAPHICS_CHOOSES },
	{ "display.interpolation", _config_boolean, "true", "HALO_INTERPOLATION", _environment_value, _platform_all,
		"Draw a frame for every display refresh, blending between the game's 30\n"
		"ticks a second; false keeps the original 30 frames a second." },
	{ "display.direct_camera", _config_boolean, "true", "HALO_DIRECT_CAMERA", _environment_value, _platform_desktop,
		"In first person, point the view where the player aims now instead of\n"
		"where the last tick left it: the view turns the frame the mouse moves,\n"
		"not up to two ticks (66 ms) later. On foot (not in the VR mode), the\n"
		"view also stands where the newest step left the player, and the gun\n"
		"and hands show the newest step's pose, not a tick (33 ms) behind." },
	{ "display.fov", _config_real, "0.0", "HALO_FOV", _environment_value, _platform_all,
		"The first-person view's field of view on foot, in degrees across at\n"
		"16:9 (20 to 150); 0 keeps the stock view. Vehicles, cinematics and\n"
		"scripted cameras keep their own." },
	{ "display.viewmodel_fov", _config_real, "0.0", "HALO_VIEWMODEL_FOV", _environment_value, _platform_all,
		"The first-person weapon's and hands' field of view, in degrees across\n"
		"at 16:9 (20 to 150); 0 keeps the weapon's stock view, also when\n"
		"display.fov widens the world." },
	{ "display.viewmodel_visible", _config_boolean, "true", "HALO_VIEWMODEL_VISIBLE", _environment_value, _platform_all,
		"Draw the first-person weapon, hands and what is attached to them.\n"
		"Off, they are not drawn; firing, animation, sound and lights go on." },
	{ "display.passenger_third_person", _config_boolean, "false", "HALO_PASSENGER_THIRD_PERSON", _environment_value, _platform_all,
		"Third-person Warthog and Scorpion passengers. Weapon zoom is disabled." },
	{ "display.high_res_hud", _config_boolean, "true", "HALO_HIGH_RES_HUD", _environment_value, _platform_all,
		"Draw the HUD (meters, counters, panels, motion sensor, reticles,\n"
		"waypoints, scopes) from the high-res assets (8x the maps' bitmaps);\n"
		"false draws the maps' own bitmaps." },
	{ "display.hud_button_style", _config_string, "\"classic\"", "HALO_HUD_BUTTON_STYLE", _environment_value, _platform_all,
		"High-res gameplay controller buttons: classic uses plain Xbox faces;\n"
		"gems uses shaded Xbox faces; menu_gems uses the menu's button art.\n"
		"Requires display.high_res_hud." },
	{ "display.high_res_text", _config_boolean, "true", "HALO_HIGH_RES_TEXT", _environment_value, _platform_all,
		"Draw the menus' and HUD's text with the fonts in port/assets/fonts\n"
		"(Overpass) at the resolution the game draws at, and the menus' titles\n"
		"from port/assets/titles; false draws the maps' bitmap fonts and titles." },
	{ "display.shadow_resolution", _config_integer, GRAPHICS_SHADOW_RESOLUTION, "HALO_SHADOW_RESOLUTION",
		_environment_value, _platform_all,
		"The size the objects' shadows are drawn at, in pixels each way: 128 as\n"
		"on the Xbox, or 256, 512 or 1024 for smoother edges, as soft." GRAPHICS_CHOOSES },
	{ "display.menus", _config_string, "\"pc\"", "HALO_MENUS", _environment_value, _platform_all,
		"The menus: \"pc\" for the PC version's main menu (port/assets/menus,\n"
		"and a menus folder here for your own), \"xbox\" for the Xbox's." },
	{ "display.menu_language", _config_string, "\"maps\"", "HALO_MENU_LANGUAGE", _environment_value, _platform_all,
		"The PC menus' language: \"maps\" for the language of the Custom Edition\n"
		"whose loc.map is in custom_maps (as that edition shows them; English\n"
		"without one), \"english\" for English. The port's own lines are English." },
	{ "display.player_names", _config_string, "\"all\"", "HALO_PLAYER_NAMES", _environment_value, _platform_all,
		"In multiplayer, whose names are drawn above their heads: \"all\",\n"
		"\"allies\", \"enemies\" or \"none\". An enemy's shows only within the\n"
		"motion sensor's reach, in sight and not camouflaged; none show if the\n"
		"gametype's motion tracker shows no players, only allies' if it shows\n"
		"only friends." },
	{ "display.player_name_scale", _config_real, "1.0", "HALO_PLAYER_NAME_SCALE", _environment_value, _platform_all,
		"How large the players' names are drawn: 1.0 three quarters of the size of\n"
		"the HUD's text, 0.25 to 4." },
	{ "display.match_timer", _config_boolean, "false", "HALO_MATCH_TIMER", _environment_value, _platform_all,
		"Draw the match clock at the bottom right in multiplayer. It counts up\n"
		"from the game's first tick, which the items' respawn periods count from\n"
		"too: a weapon is back on the map at every multiple of its period on the\n"
		"clock." },
	{ "display.scoreboard_team_layout", _config_string, "\"teams\"", "HALO_SCOREBOARD_TEAM_LAYOUT", _environment_value,
		_platform_all,
		"How the scoreboard lists a team game's players: \"teams\" in a column for\n"
		"each team (red on the left, blue on the right), \"score\" all in order of\n"
		"score." },
	{ "display.scoreboard_background", _config_boolean, "true", "HALO_SCOREBOARD_BACKGROUND", _environment_value,
		_platform_all,
		"Draw a panel behind the multiplayer scoreboard, for clearer text." },
	{ "display.scoreboard_background_color", _config_string, "\"16, 16, 16, 150\"", "HALO_SCOREBOARD_BACKGROUND_COLOR",
		_environment_value, _platform_all,
		"The scoreboard panel's colour: \"red, green, blue, alpha\", each 0 to 255\n"
		"(alpha 0 is see-through, 255 solid)." },
	{ "display.anisotropic_filtering", _config_integer, GRAPHICS_ANISOTROPIC_FILTERING, "HALO_ANISOTROPIC_FILTERING",
		_environment_value, _platform_all,
		"Sharpen textures seen at a grazing angle (floors, walls, terrain), which\n"
		"the Xbox did not: 2, 4, 8 or 16 times; 0 filters as the Xbox did. 4 keeps\n"
		"most of 16's sharpness at less cost on slower GPUs." GRAPHICS_CHOOSES },
	{ "display.latency_meter", _config_boolean, "false", "HALO_LATENCY_METER", _environment_value, _platform_all,
		"In a network game, your round trip to the host in milliseconds at the\n"
		"screen's top right, with bars (the host: its slowest player's), and\n"
		"\"Connection problem\" when the host has sent nothing for two seconds;\n"
		"the scoreboard's pings in the same colours, as this machine times them." },
	{ "display.per_pixel_lighting", _config_boolean, GRAPHICS_PER_PIXEL_LIGHTING, "HALO_PER_PIXEL_LIGHTING",
		_environment_value, _platform_all,
		"Light the models (characters, weapons, vehicles, scenery) for each\n"
		"pixel by the lights the game gives them, without the facets the light\n"
		"of each vertex shows across curved surfaces; false lights each vertex,\n"
		"as the Xbox does." GRAPHICS_CHOOSES },
	{ "display.structure_culling", _config_integer, STRUCTURE_CULLING_DEFAULT, "HALO_STRUCTURE_CULLING", _environment_value,
		_platform_all,
		"Which tests skip the level's triangles the camera cannot see, as bits\n"
		"added together: 1 outside the view, 2 groups facing away, 4 triangles\n"
		"facing away, 8 smaller groups, 16 nearer rooms first; 0 walks them as\n"
		"the Xbox does. The console's structure_culling changes it until the\n"
		"game quits." },
	{ "display.model_detail", _config_string, GRAPHICS_MODEL_DETAIL, "HALO_MODEL_DETAIL", _environment_value,
		_platform_all,
		"Which of their levels of detail models are drawn at: \"stock\" chooses by\n"
		"their size on the Xbox's 480 lines, at any resolution; \"native\" by their\n"
		"size on the display's, so that more detail is kept at a distance at high\n"
		"resolutions; \"highest\" draws the models \"native\" does, each at its most\n"
		"detailed level; \"auto\" is \"native\" in VR and \"stock\" on a display." GRAPHICS_CHOOSES },
	{ "display.hdr", _config_string, "\"auto\"", "HALO_HDR", _environment_value, _platform_all,
		"High dynamic range: \"auto\" draws in HDR while the window's display shows\n"
		"it (an XDR or HDR display: macOS's EDR), \"on\" always (on an SDR display\n"
		"the highlights are rolled off under its white), \"off\" never. The frame\n"
		"is drawn in half floats, which keep the light the game's additive\n"
		"effects (plasma, explosions, muzzle flashes, lights, the sun) add past\n"
		"white, and an HDR display shows it brighter than paper white, the\n"
		"display's SDR white; surfaces, the HUD and the menus stay as they are.\n"
		"Off draws 8 bits a channel, as the Xbox did. Metal only for now (the\n"
		"Vulkan renderer draws SDR, and the VR mode does)." },
	{ "display.hdr_mode", _config_string, "\"faithful\"", "HALO_HDR_MODE", _environment_value, _platform_all,
		"With HDR: \"faithful\" keeps the picture as the game draws it, only its\n"
		"highlights brighter; \"punchy\" lifts the whole scene and its bright\n"
		"parts, keeping each colour's hue, for a brighter, remastered look." },
	{ "display.hdr_paper_white", _config_integer, "100", "HALO_HDR_PAPER_WHITE", _environment_value, _platform_all,
		"With HDR, how bright paper white (white surfaces, the HUD, the menus) is,\n"
		"in percent of the system's SDR white (macOS's; 203 nits where the system\n"
		"has none), 50 to 250. The highlights keep the rest of the display's\n"
		"headroom." },
	{ "display.astc_textures", _config_string, ASTC_TEXTURES_DEFAULT, "HALO_ASTC_TEXTURES", _environment_value,
		_platform_android | _platform_guest,
		"The maps' DXT textures on a GPU that cannot sample them (iPhones, most\n"
		"Android phones): \"auto\" transcodes them to ASTC, a quarter of the memory\n"
		"and bandwidth of the decoded textures the GPU drew before and the same\n"
		"picture, where the GPU samples ASTC and not DXT; \"always\" wherever it\n"
		"samples ASTC (to try it on a Mac); \"off\" decodes them. The few blocks\n"
		"that cannot be transcoded exactly are encoded on a worker thread the\n"
		"first time a texture is drawn, the texture drawn decoded meanwhile, and\n"
		"kept in the app's caches folder. \"auto\" by default on iOS; \"off\" on\n"
		"Android, where it is yet to be checked on a device (try \"auto\"). Read\n"
		"by the host from the file, at the start (port/linux/astc/xgpu_astc.h)." },

	{ "audio.enabled", _config_boolean, "true", "HALO_NO_AUDIO", _environment_set_is_false, _platform_all,
		"Play sound." },
	{ "audio.volume", _config_real, "1.0", "HALO_VOLUME", _environment_value, _platform_all,
		"The volume of everything, 0.0 to 1.0." },
	{ "audio.music_volume", _config_real, "1.0", "HALO_MUSIC_VOLUME", _environment_value, _platform_all,
		"The music's volume, 0.0 to 1.0 (of audio.volume)." },
	{ "audio.effects_volume", _config_real, "1.0", "HALO_EFFECTS_VOLUME", _environment_value, _platform_all,
		"The volume of every other sound (effects and speech), 0.0 to 1.0 (of\n"
		"audio.volume)." },
	{ "audio.buffer_frames", _config_integer, "512", "HALO_AUDIO_BUFFER_FRAMES", _environment_value,
		_platform_desktop,
		"The audio device's buffer, in sample frames at 48 kHz (64 to 8192): larger\n"
		"rides out stalls that cut the sound out, smaller has less delay (512\n"
		"is 11 ms, 2048 is 43 ms)." },
	{ "audio.channels", _config_string, "\"stereo\"", "HALO_AUDIO_CHANNELS", _environment_value, _platform_desktop,
		"\"stereo\", \"5.1\" (surround, as the Xbox's Dolby Digital), or \"auto\"\n"
		"for 5.1 when the sound device has six or more channels." },
	{ "audio.reverb", _config_boolean, "true", "HALO_REVERB", _environment_value, _platform_all,
		"Reverberate the world's sounds as the place the player is in does (the\n"
		"maps' sound environments, as the Xbox's I3DL2 reverb did); false keeps\n"
		"them dry." },
	{ "audio.resampler", _config_string, "\"sinc\"", "HALO_AUDIO_RESAMPLER", _environment_value, _platform_all,
		"How sounds are converted to the output's rate: \"sinc\" with a\n"
		"band-limited low pass; \"linear\" between neighbouring samples,\n"
		"brighter for the aliasing it lets through." },
	{ "audio.voice_chat", _config_string, "\"push_to_talk\"", "HALO_VOICE_CHAT", _environment_value, _platform_all,
		"Talking in network games' voice chat: \"push_to_talk\" (while\n"
		"controls.push_to_talk is held; the microphone opens the first time),\n"
		"\"open_mic\" (whenever the microphone hears speech), or \"off\". Others'\n"
		"voices play whatever this is (audio.voice_volume 0 silences them)." },
	{ "audio.voice_volume", _config_real, "1.0", "HALO_VOICE_VOLUME", _environment_value, _platform_all,
		"The volume of the other players' voices (0 to 2)." },
	{ "audio.output_device", _config_string, "\"default\"", "HALO_AUDIO_OUTPUT_DEVICE", _environment_value,
		_platform_desktop,
		"Where the game's sound and the voices play: a device's name as Settings >\n"
		"Audio lists it, or \"default\" for the system's (also if it is not found)." },
	{ "audio.input_device", _config_string, "\"default\"", "HALO_AUDIO_INPUT_DEVICE", _environment_value,
		_platform_desktop,
		"The microphone voice chat listens to: a device's name as Settings >\n"
		"Audio lists it, or \"default\" for the system's (also if it is not found)." },

	{ "audio.loose_sounds", _config_boolean, "false", "HALO_LOOSE_SOUNDS", _environment_value, _platform_all,
		"For those making sounds: play each of a map's sounds that has a Halo PC\n"
		"sound tag file of its name under the data root's tags folder\n"
		"(tags/sound/.../name.sound) from that file. At the console,\n"
		"loose_sounds_reload reads the files again and loose_sounds false gives\n"
		"the mod packs' or map's sounds back. Loose files take priority over mods." },

#ifdef HALO_VR
	/* the VR build's (configure.py --vr; vr.c) */
	{ "vr.enabled", _config_boolean, "true", "HALO_VR", _environment_value, _platform_desktop,
		"Play in the VR headset (OpenXR); false plays flat, in the window." },
	{ "vr.resolution_scale", _config_real, "1.0", "HALO_VR_RESOLUTION_SCALE", _environment_value, _platform_desktop,
		"The eyes' images, as a multiple of the size the headset recommends\n"
		"(0.25 to 2): smaller is faster, larger sharper." },
	{ "vr.refresh_rate", _config_real, "90.0", "HALO_VR_REFRESH_RATE", _environment_value, _platform_desktop,
		"The headset's refresh rate to ask for, in Hz (the game draws a frame for\n"
		"each refresh); 0 leaves the runtime's." },
	{ "vr.aim", _config_string, "\"controller\"", "HALO_VR_AIM", _environment_value, _platform_desktop,
		"What aims: \"controller\" the right controller, which holds the weapon\n"
		"(the reticle shows where it points), \"head\" the head (the reticle in\n"
		"the middle of the view, the weapon before the eyes), \"gamepad\" a\n"
		"gamepad, as on the flat screen (the stick turns the view, and the head\n"
		"looks about from it: vr.gamepad_view)." },
	{ "vr.gamepad_view", _config_string, "\"camera\"", "HALO_VR_GAMEPAD_VIEW", _environment_value,
		_platform_desktop,
		"With vr.aim = \"gamepad\", how the stick moves the view: \"camera\" turns\n"
		"and pitches it, as the flat screen's camera (the reticle in the middle\n"
		"with your head straight; the most like the Xbox game, and the most\n"
		"likely to make you sick), \"level\" turns it smoothly but never pitches\n"
		"it (the stick's pitch moves the reticle), \"snap\" turns it a step of\n"
		"vr.snap_turn_angle each time the aim is that far from it." },
	{ "vr.dominant_hand", _config_string, "\"right\"", "HALO_VR_DOMINANT_HAND", _environment_value,
		_platform_desktop,
		"The hand that holds the weapon: \"right\", or \"left\" (the weapon and the\n"
		"hands drawn mirrored; the left trigger fires, the right hand holds the\n"
		"foregrip and throws, the holsters on your left, the magazines at your\n"
		"right hip). Not with vr.aim = \"gamepad\"." },
	{ "vr.swap_sticks", _config_string, "\"auto\"", "HALO_VR_SWAP_STICKS", _environment_value, _platform_desktop,
		"\"yes\": the right stick moves and the left turns (and zooms, its click;\n"
		"pushed down, ejects the magazine), \"no\": as they are; \"auto\": swapped\n"
		"with vr.dominant_hand = \"left\". Not with vr.aim = \"gamepad\"." },
	{ "vr.weapon_offset", _config_string, "\"0.15, -0.22, 0.30\"", "HALO_VR_WEAPON_OFFSET", _environment_value,
		_platform_desktop,
		"With vr.aim = \"controller\" and vr.hands = \"game\", where the weapon's\n"
		"grip is from the eye it is drawn for, in metres: \"right, up, forward\"." },
	{ "vr.depth", _config_boolean, "false", "HALO_VR_DEPTH", _environment_value, _platform_desktop,
		"Give the runtime the eyes' depth too, for its reprojection (SteamVR on\n"
		"the Steam Frame cannot take it yet)." },
	{ "vr.foveation", _config_string, "\"eye\"", "HALO_VR_FOVEATION", _environment_value, _platform_desktop,
		"Foveated rendering: full detail where you look, half (vr.foveation_inset)\n"
		"about it, which is faster: \"eye\" the inset follows the eyes where the\n"
		"headset tracks them (else as \"fixed\"), \"fixed\" it stays in the\n"
		"middle of each eye's view, \"off\" all at full detail. From the next start." },
	{ "vr.foveation_inset", _config_real, "0.5", "HALO_VR_FOVEATION_INSET", _environment_value,
		_platform_desktop,
		"With vr.foveation: how much of each eye's image across (and up) is at full\n"
		"detail, from 0.3 to 0.8, which is also the detail about it (0.5: half).\n"
		"Smaller is faster. From the next start." },
	{ "vr.late_latch", _config_boolean, "true", "HALO_VR_LATE_LATCH", _environment_value, _platform_desktop,
		"Draw each frame from where your head and the weapon's controller are\n"
		"just before it goes to the headset, not where they were as the game\n"
		"began it: less lag between a turn of the head and the view." },
	{ "vr.melee_gesture", _config_boolean, "true", "HALO_VR_MELEE_GESTURE", _environment_value, _platform_desktop,
		"A swing of the right controller melees, as B does: a hard one in any\n"
		"direction, or a softer one with the right grip held." },
	{ "vr.grenade_throw", _config_string, "\"gesture\"", "HALO_VR_GRENADE_THROW", _environment_value,
		_platform_desktop,
		"With vr.aim = \"controller\", how a grenade is thrown: \"gesture\" by\n"
		"throwing it with the left hand alone (hold the left grip, draw the hand\n"
		"back, swing and let go: it flies where the hand threw it, as hard as it\n"
		"did; the left trigger throws nothing on foot); \"both\" with the hand or\n"
		"the left trigger; \"button\" with the left trigger alone." },
	{ "vr.holsters", _config_boolean, "true", "HALO_VR_HOLSTERS", _environment_value, _platform_desktop,
		"With vr.aim = \"controller\", the weapon you carry but do not hold is on\n"
		"your body: a long gun over your right shoulder, a pistol or the needler\n"
		"at your right hip. Reach there with your right hand and grip to draw it\n"
		"(it lights up as your hand comes, and the controller ticks)." },
	{ "vr.physical_crouch", _config_boolean, "true", "HALO_VR_PHYSICAL_CROUCH", _environment_value,
		_platform_desktop,
		"Crouch in the room and the game crouches (on foot, from about 35 cm\n"
		"below your standing eyes, or 25 cm seated); stand and it stands. The\n"
		"view follows your head alone." },
	{ "vr.scopes", _config_boolean, "true", "HALO_VR_SCOPES", _environment_value, _platform_desktop,
		"With vr.aim = \"controller\", zooming magnifies a view of its own (as\n"
		"vr.scope_view says), the rest of the view as it was. false: the game's\n"
		"zoom, which magnifies nothing in the headset." },
	{ "vr.scope_view", _config_string, "\"smartlink\"", "HALO_VR_SCOPE_VIEW", _environment_value,
		_platform_desktop,
		"With vr.scopes: \"smartlink\", the right stick's click zooms as the game\n"
		"does, whatever the weapon's place, and the zoom shows as a wide window\n"
		"about the reticle, at its depth, magnifying where the weapon aims\n"
		"(vr.smartlink_size wide);\n"
		"\"eye\", the sniper rifle's and the pistol's scopes magnify the view\n"
		"within their lens as you raise them to an eye, and only then (the\n"
		"click changes the magnification); \"off\", as vr.scopes = false." },
	{ "vr.smartlink_size", _config_real, "60.0", "HALO_VR_SMARTLINK_SIZE", _environment_value, _platform_desktop,
		"With vr.scope_view = \"smartlink\", how wide the zoom's window about\n"
		"the reticle is, in degrees of your view (30 to 100; it is 16:9). The\n"
		"sniper rifle's and the pistol's are the flat screen's zoomed view: this\n"
		"is the sniper rifle's scope's frame in it, the pistol's to its scale,\n"
		"the zoomed view about them as large as the HUD (vr.hud_size) or a\n"
		"fifth wider than the sniper rifle's frame." },
	{ "vr.smartlink_place", _config_string, "\"reticle\"", "HALO_VR_SMARTLINK_PLACE", _environment_value,
		_platform_desktop,
		"With vr.scope_view = \"smartlink\", where the zoom's window is:\n"
		"\"reticle\" about the reticle, at its depth, where the weapon aims (it\n"
		"moves with your hand); \"visor\" on your visor where the HUD is (it\n"
		"stays before your eyes, as the flat screen does, and the weapon aims\n"
		"what it shows)." },
	{ "vr.scope_gun", _config_string, "\"hidden\"", "HALO_VR_SCOPE_GUN", _environment_value, _platform_desktop,
		"With vr.scope_view = \"smartlink\", how the weapon in your hand shows\n"
		"where it crosses the zoom's window: \"hidden\" the window is over it,\n"
		"\"ghost\" it shows faintly through the window, \"visible\" it is in\n"
		"front of the window." },
	{ "vr.vehicle_climb", _config_boolean, "true", "HALO_VR_VEHICLE_CLIMB", _environment_value, _platform_desktop,
		"With vr.aim = \"controller\", a vehicle with holds (the Scorpion, the\n"
		"Warthog, the Ghost, the Banshee, the Shade) is climbed with the hands:\n"
		"grip a hold and pull, the view moving as the hands pull it; lowered\n"
		"into a hatch by its rim, pulled to a seat by its holds, or both hands\n"
		"on a gun's handles, you are in the seat, and pulled or pushed out by\n"
		"its holds (the Ghost: off its handlebars), out. X works as ever." },
	{ "vr.vehicle_zoom", _config_string, "\"2 4\"", "HALO_VR_VEHICLE_ZOOM", _environment_value, _platform_desktop,
		"With vr.scope_view = \"smartlink\", in a vehicle's seat that aims its\n"
		"weapon (the Scorpion's driver, a turret's gunner), seen from the seat:\n"
		"the magnifications the right stick's click steps through, in the\n"
		"zoom's window about the reticle, then none (up to 4; \"\": none). A\n"
		"vehicle's weapon with zoom levels of its own has those." },
	{ "vr.aim_assist", _config_real, "1.0", "HALO_VR_AIM_ASSIST", _environment_value, _platform_desktop,
		"With vr.aim = \"controller\", the aim assist: near an enemy the game's\n"
		"magnetism would be drawn to, the aim's turn across it slows, and zoomed,\n"
		"the aim is drawn gently toward it; how strongly, from 0 (none) to 2 (1:\n"
		"the default). The bullets' own autoaim is the game's whatever this is." },
	{ "vr.weapon_haptics", _config_real, "1.0", "HALO_VR_WEAPON_HAPTICS", _environment_value, _platform_desktop,
		"With vr.aim = \"controller\", how strongly the controllers feel each\n"
		"weapon firing, each its own way (the pistol's sharp tap, the assault\n"
		"rifle's buzz, the shotgun's thump...), and the hits on you, in the hand\n"
		"on the side they came from, in place of the gamepad's rumble for them:\n"
		"from 0 (none) to 1 (the default)." },
	{ "vr.recoil", _config_real, "1.0", "HALO_VR_RECOIL", _environment_value, _platform_desktop,
		"With vr.aim = \"controller\", how far the weapon in your hand kicks as\n"
		"it fires, each its own way (drawn alone: the aim and the shots stay\n"
		"where you point): from 0 (none) to 1 (the default)." },
	{ "vr.manual_reload", _config_boolean, "true", "HALO_VR_MANUAL_RELOAD", _environment_value, _platform_desktop,
		"With vr.aim = \"controller\", reload by hand: take another magazine from\n"
		"your left hip with the left grip (the one in the weapon ejects itself,\n"
		"or push the right stick down to eject it) and push it into the weapon,\n"
		"loaded at once (the rocket launcher's tubes from behind your left\n"
		"shoulder; the shotgun's shells one at a time, without ejecting). An\n"
		"empty weapon does not reload itself; X reloads as ever." },
	{ "vr.vehicle_flip", _config_string, "\"lift\"", "HALO_VR_VEHICLE_FLIP", _environment_value, _platform_desktop,
		"How an overturned vehicle the game offers to flip is flipped: \"lift\"\n"
		"by both hands, gripping low beside it (below the waist, out toward it,\n"
		"as a bumper) and lifting together, fast (X reloads there); \"button\" by\n"
		"X, as ever; \"both\" by either. With vr.aim = \"gamepad\", by X." },
	{ "vr.grab_world", _config_boolean, "true", "HALO_VR_GRAB_WORLD", _environment_value, _platform_desktop,
		"With vr.aim = \"controller\", the hands take hold of the world: a weapon\n"
		"on the ground the game offers to pick up is taken by reaching down to it\n"
		"with your right hand and gripping (it lights up as your hand comes, and\n"
		"the controller ticks); a control the game offers to activate (a door's\n"
		"panel, a switch) is pressed by pushing either hand into it. false: X\n"
		"alone, which works either way." },
	{ "vr.two_handed", _config_boolean, "true", "HALO_VR_TWO_HANDED", _environment_value, _platform_desktop,
		"With vr.aim = \"controller\", a long gun (not a pistol or the needler)\n"
		"is aimed with both hands while the left grip holds it by the foregrip:\n"
		"along the line from the right hand to the left." },
	{ "vr.hands", _config_string, "\"arms\"", "HALO_VR_HANDS", _environment_value, _platform_desktop,
		"With vr.aim = \"controller\", the first-person hands: each where its\n"
		"controller is (the left on a long gun's foregrip while it holds it),\n"
		"with \"arms\" the arms reaching them from the shoulders (vr.shoulders),\n"
		"with \"floating\" no arms; \"game\" the game's arms, posed from the\n"
		"weapon in the right hand." },
	{ "vr.shoulders", _config_string, "\"0.19, -0.20, -0.07\"", "HALO_VR_SHOULDERS", _environment_value,
		_platform_desktop,
		"With vr.hands = \"arms\", where each shoulder is from your eyes, in\n"
		"metres: \"out to its side, up, forward\" (turned with your body)." },
	{ "vr.vehicle_view", _config_string, "\"first_person\"", "HALO_VR_VEHICLE_VIEW", _environment_value, _platform_desktop,
		"The view in a vehicle's seat: \"first_person\" from your head where you\n"
		"sit, level with the horizon; \"third_person\" the game's camera following\n"
		"the vehicle." },
	{ "vr.seat_motion", _config_string, "\"still\"", "HALO_VR_SEAT_MOTION", _environment_value, _platform_desktop,
		"With vr.vehicle_view = \"first_person\": \"still\" you stay in the first\n"
		"person all the way in and out, the eye following the body's climb\n"
		"smoothed, then staying put in the vehicle, riding with it but not with\n"
		"a turret's turning or flinches; \"blink\" the eye stays put as the body\n"
		"climbs in and out, and blinks between its places; \"head\" it follows\n"
		"the seated body's head as the game animates it." },
	{ "vr.ghost_steering", _config_string, "\"handlebars\"", "HALO_VR_GHOST_STEERING", _environment_value,
		_platform_desktop,
		"Driving a Ghost with vr.aim = \"controller\": \"handlebars\" grip its\n"
		"handles with both hands and turn them as a bike's, the view turning with\n"
		"the Ghost and its guns aiming where the bars point; \"look\" it steers and\n"
		"aims where the right controller points, the view turned by the stick." },
	{ "vr.warthog_steering", _config_string, "\"wheel\"", "HALO_VR_WARTHOG_STEERING", _environment_value,
		_platform_desktop,
		"Driving a Warthog with vr.aim = \"controller\": \"wheel\" grip its wheel\n"
		"with both hands and turn it, the view turning with the Warthog;\n"
		"\"look\" it steers where the right controller points, the view turned\n"
		"by the stick." },
	{ "vr.scorpion_view", _config_string, "\"feed\"", "HALO_VR_SCORPION_VIEW", _environment_value,
		_platform_desktop,
		"Driving a Scorpion with vr.vehicle_view = \"first_person\": \"feed\" you\n"
		"climb in, in the first person, to a round cockpit inside the hull, the\n"
		"world on a display all the way round you (the tank's feed: true to size,\n"
		"the zoom within its front) and the tank's hologram on the dash; \"hatch\" you\n"
		"stand in the turret's hatch, seeing all about over the hull, the turret\n"
		"aiming where you aim; \"inside\" you sit in the driver's compartment at\n"
		"the front of the hull, seeing out through its vision blocks." },
	{ "vr.comfort_vignette", _config_boolean, "true", "HALO_VR_COMFORT_VIGNETTE", _environment_value, _platform_desktop,
		"The edges of your view dim a little while the view turns fast with a\n"
		"vehicle you steer by its handlebars (vr.ghost_steering), or it speeds\n"
		"up or slows hard." },
	{ "vr.death_view", _config_string, "\"crt\"", "HALO_VR_DEATH_VIEW", _environment_value, _platform_desktop,
		"As you die: \"crt\" the helmet's view switches off as a CRT does and\n"
		"cuts to a death cam, a feed from beside your body (the right stick\n"
		"moves it about the body), until the game brings you back and the HUD\n"
		"boots; \"face\" the view stays where your head was, the world\n"
		"dimmed and reddened, your body falling before you, and turns to it\n"
		"in a blink if it lands out of sight, until the game brings you back\n"
		"from the dark; \"still\" the same, never turned; \"game\" the game's\n"
		"camera pulling away from the body." },
	{ "vr.head_in_wall", _config_string, "\"off\"", "HALO_VR_HEAD_IN_WALL", _environment_value, _platform_desktop,
		"Your head leaned or stepped into a wall or a pillar, or through it:\n"
		"\"fade\" the view fades to black as your eyes go in, and back as\n"
		"they come out, the HUD saying to step back; \"off\" you see through\n"
		"it." },
	{ "vr.turn", _config_string, "\"snap\"", "HALO_VR_TURN", _environment_value, _platform_desktop,
		"How the right stick turns: \"snap\" in steps (vr.snap_turn_angle),\n"
		"\"smooth\" continuously (vr.smooth_turn_speed); with vr.aim = \"gamepad\",\n"
		"vr.gamepad_view instead." },
	{ "vr.snap_turn_angle", _config_real, "30.0", "HALO_VR_SNAP_TURN_ANGLE", _environment_value, _platform_desktop,
		"Degrees of each snap turn." },
	{ "vr.smooth_turn_speed", _config_real, "150.0", "HALO_VR_SMOOTH_TURN_SPEED", _environment_value, _platform_desktop,
		"Degrees a second of smooth turning, the stick pushed all the way." },
	{ "vr.height", _config_string, "\"seated\"", "HALO_VR_HEIGHT", _environment_value, _platform_desktop,
		"\"seated\": the head where it is when recentred (both grips, or a\n"
		"gamepad's Back, held) is the player's eye. \"standing\": the head\n"
		"vr.player_height above the floor is." },
	{ "vr.player_height", _config_real, "1.65", "HALO_VR_PLAYER_HEIGHT", _environment_value, _platform_desktop,
		"Standing, the height of your eyes in metres." },
	{ "vr.world_scale", _config_real, "1.0", "HALO_VR_WORLD_SCALE", _environment_value, _platform_desktop,
		"How large the world looks: more than 1 makes it larger (and your\n"
		"movements and eyes smaller in it)." },
	{ "vr.hud", _config_string, "\"visor\"", "HALO_VR_HUD", _environment_value, _platform_desktop,
		"The HUD: \"visor\" on your helmet's visor, curved about your eyes, its\n"
		"parts at depths of their own, with its glow, the helmet's rim and the\n"
		"shields' effects on the glass (vr.hud_depth, vr.hud_wrap, vr.hud_lag,\n"
		"vr.hud_glow, vr.hud_glass, vr.hud_react, vr.helmet_rim, vr.visor_frame,\n"
		"vr.visor_effects); \"flat\" a flat screen before your eyes." },
	{ "vr.hud_distance", _config_real, "2.0", "HALO_VR_HUD_DISTANCE", _environment_value, _platform_desktop,
		"How far in front of your eyes the HUD is, in metres." },
	{ "vr.hud_size", _config_real, "60.0", "HALO_VR_HUD_SIZE", _environment_value, _platform_desktop,
		"How wide the HUD is, in degrees of your view (20 to 120)." },
	{ "vr.hud_lag", _config_real, "0.03", "HALO_VR_HUD_LAG", _environment_value, _platform_desktop,
		"With vr.hud = \"visor\", how far the HUD trails your head's turns, in\n"
		"seconds (smoothed, never more than 1.5 degrees behind; 0 none, up to 0.2)." },
	{ "vr.hud_depth", _config_real, "1.0", "HALO_VR_HUD_DEPTH", _environment_value, _platform_desktop,
		"With vr.hud = \"visor\", how far apart in depth the HUD's parts are, times\n"
		"their own (vr.hud_depth_*): 0 all at vr.hud_distance, to 2 (never nearer\n"
		"than 1 m)." },
	{ "vr.hud_depth_tracker", _config_real, "-0.55", "HALO_VR_HUD_DEPTH_TRACKER", _environment_value,
		_platform_desktop,
		"With vr.hud = \"visor\", the motion tracker's distance from the HUD's at\n"
		"vr.hud_depth 1, in metres (before it less than 0)." },
	{ "vr.hud_depth_status", _config_real, "-0.35", "HALO_VR_HUD_DEPTH_STATUS", _environment_value,
		_platform_desktop,
		"With vr.hud = \"visor\", the shields' and health's distance from the HUD's\n"
		"at vr.hud_depth 1, in metres." },
	{ "vr.hud_depth_weapon", _config_real, "-0.25", "HALO_VR_HUD_DEPTH_WEAPON", _environment_value,
		_platform_desktop,
		"With vr.hud = \"visor\", the weapon's, its ammunition's and the grenades'\n"
		"distance from the HUD's at vr.hud_depth 1, in metres." },
	{ "vr.hud_depth_world", _config_real, "0.5", "HALO_VR_HUD_DEPTH_WORLD", _environment_value,
		_platform_desktop,
		"With vr.hud = \"visor\", the waypoints' and the players' names' distance\n"
		"from the HUD's at vr.hud_depth 1, in metres." },
	{ "vr.hud_tracker_tilt", _config_real, "20.0", "HALO_VR_HUD_TRACKER_TILT", _environment_value,
		_platform_desktop,
		"With vr.hud = \"visor\" and vr.hud_depth, how far the motion tracker\n"
		"tilts back, in degrees (-45 to 45)." },
	{ "vr.hud_wrap", _config_real, "1.0", "HALO_VR_HUD_WRAP", _environment_value, _platform_desktop,
		"With vr.hud = \"visor\" and vr.hud_depth, the HUD's corners along the\n"
		"visor's curve: the weapon's, the shields' and the motion tracker's panels\n"
		"turned out toward the edges of your view, bending as the visor does (0\n"
		"where the game puts them, to 2)." },
	{ "vr.hud_glow", _config_real, "0.5", "HALO_VR_HUD_GLOW", _environment_value, _platform_desktop,
		"With vr.hud = \"visor\", the HUD's glow on the visor (0 none to 1)." },
	{ "vr.hud_glass", _config_real, "1.0", "HALO_VR_HUD_GLASS", _environment_value, _platform_desktop,
		"With vr.hud = \"visor\", the HUD as light on the visor's glass: the world\n"
		"seen through it, the glass's tint in it and its bloom, and its faint\n"
		"reflection (0 none, as an opaque screen, to 1)." },
	{ "vr.hud_react", _config_real, "1.0", "HALO_VR_HUD_REACT", _environment_value, _platform_desktop,
		"With vr.hud = \"visor\", the HUD reacting to the world: more contrast\n"
		"before bright light (the glass darkening behind it), dimmer in the dark,\n"
		"and with vr.visor_effects disturbed for a moment as the shields are hit,\n"
		"its bloom pulsing red while they are low (0 none to 1)." },
	{ "vr.helmet_rim", _config_real, "1.0", "HALO_VR_HELMET_RIM", _environment_value, _platform_desktop,
		"With vr.hud = \"visor\", the helmet's faceplate about the visor, at the\n"
		"edges of your view, in depth (0 none to 1)." },
	{ "vr.helmet_rim_depth", _config_real, "0.10", "HALO_VR_HELMET_RIM_DEPTH", _environment_value,
		_platform_desktop,
		"The helmet's rim's lip: how far before your eyes it is, in metres (0.05\n"
		"to 0.25)." },
	{ "vr.helmet_rim_reach", _config_real, "0.07", "HALO_VR_HELMET_RIM_REACH", _environment_value,
		_platform_desktop,
		"The helmet's rim's lip: how far in from the edges of your view it begins,\n"
		"a fraction of them (0 to 0.2)." },
	{ "vr.visor_frame", _config_real, "0.4", "HALO_VR_VISOR_FRAME", _environment_value, _platform_desktop,
		"With vr.hud = \"visor\", the visor's glass: the edges of your view\n"
		"darkened, and its faint tint (0 none to 1)." },
	{ "vr.visor_effects", _config_boolean, "true", "HALO_VR_VISOR_EFFECTS", _environment_value, _platform_desktop,
		"With vr.hud = \"visor\", the shields on the visor's glass: their energy\n"
		"across it, the armour's own, as they are hit and as they recharge; its\n"
		"rim pulses red while they are low, and the HUD flickers as they break." },
	{ "vr.visor_energy", _config_real, "1.0", "HALO_VR_VISOR_ENERGY", _environment_value, _platform_desktop,
		"With vr.visor_effects, how strong the shields' energy on the glass is (0\n"
		"none to 2)." },
	{ "vr.menu_distance", _config_real, "2.5", "HALO_VR_MENU_DISTANCE", _environment_value, _platform_desktop,
		"How far in front of you the menus are, in metres." },
	{ "vr.menu_width", _config_real, "2.6", "HALO_VR_MENU_WIDTH", _environment_value, _platform_desktop,
		"How wide the menus are, in metres." },
	{ "vr.hrtf", _config_boolean, "true", "HALO_VR_HRTF", _environment_value, _platform_desktop,
		"Hear the world's sounds as your ears would, on headphones or the\n"
		"headset's speakers: each from its direction by the time and the\n"
		"shadow of your head between your ears, and the tones your ears give\n"
		"a sound from above, below and behind (a model of a head's, HRTF);\n"
		"false pans them left and right, as on the flat screen." },

#endif
	{ "input.touch_controls", _config_string, "\"auto\"", "HALO_TOUCH_CONTROLS", _environment_value,
		_platform_android | _platform_ios,
		"The on-screen touch controls in a game: \"auto\" shows them on a\n"
		"touchscreen while no controller is connected, \"on\" also with a\n"
		"controller, \"off\" never. A device without a touchscreen never shows\n"
		"them. The menus take taps in any case." },
	{ "input.touch_captions", _config_boolean, "true", "HALO_TOUCH_CAPTIONS", _environment_value,
		_platform_android | _platform_ios,
		"The touch controls write what each controller button does in the\n"
		"profile (JUMP, MELEE, FIRE) under it, in the HUD's font and colour.\n"
		"false: the buttons alone, drawn in display.hud_button_style." },
	{ "input.touch_aim_assist", _config_boolean, "true", "HALO_TOUCH_AIM_ASSIST", _environment_value,
		_platform_android | _platform_ios,
		"The touch controls' swipe aiming gets a controller's aim assist: the\n"
		"aim slows over a target and follows a moving one. false: none, as a\n"
		"mouse (the bullets' own autoaim stays)." },
	{ "input.mouse_sensitivity", _config_real, "5.73", "HALO_MOUSE_SENSITIVITY", _environment_value, _platform_desktop,
		"How far the view turns for the mouse's movement, on " CONFIG_MOUSE_MARK " scale:\n"
		"0.022 degrees a count of the mouse times this, with no acceleration,\n"
		"whatever the field of view or the frame rate. The same number as in\n"
		"CS2 turns the same. (Older versions' scale, 0.0022 radians a count,\n"
		"was converted once: times 5.7296.)" },
	{ "input.zoom_sensitivity", _config_string, "\"cs2\"", "HALO_ZOOM_SENSITIVITY", _environment_value,
		_platform_desktop,
		"How the mouse turns while zoomed: \"cs2\", by the zoomed field of view\n"
		"over the unzoomed one (CS2's zoom_sensitivity_ratio 1), at every zoom\n"
		"of every weapon; \"halo\", by one over the zoom's magnification, as the\n"
		"game turns. Controllers and the touch controls keep the game's." },
	{ "input.playstation_kernel_driver", _config_boolean, "false", "HALO_PLAYSTATION_KERNEL_DRIVER",
		_environment_value, _platform_desktop,
		"Linux: a PlayStation controller (DualSense, DualShock 4) through the\n"
		"kernel's driver of it (hid-playstation, hid-sony), which needs no setup;\n"
		"false: through SDL's own, which needs Steam's udev rules to open it. Read\n"
		"at start-up." },
	{ "input.invert_mouse", _config_boolean, "false", "HALO_MOUSE_INVERT", _environment_set_is_true, _platform_desktop,
		"Moving the mouse forward looks down." },
	{ "network.block_mouse_aim_assist", _config_boolean, "false", "HALO_NET_BLOCK_MOUSE_AIM_ASSIST", _environment_value,
		_platform_all, "Host: disable optional mouse magnetism for mouse/keyboard players. Controllers and touch keep their own assist." },
	{ "network.shield_effects", _config_string, "\"stock\"", "HALO_NET_SHIELD_EFFECTS", _environment_value, _platform_all,
		"Host: the shield effects every player of a network game sees, whatever\n"
		"their own: \"stock\", \"glow\", \"light_show\", \"overshield\",\n"
		"\"glow_overshield\" or \"light_show_overshield\" (display.shield_glow,\n"
		"its light_show intensity, display.overshield_effect); \"players\": not\n"
		"synced, each player sees their own settings'. SHIELD EFFECTS in Server\n"
		"Setup > Player Options writes its choice here." },
	{ "input.mouse_aim_assist", _config_boolean, "false", "HALO_MOUSE_AIM_ASSIST", _environment_value, _platform_desktop,
		"Magnetism while aiming with the mouse, as with a controller: the view\n"
		"slowed and dragged along by a target. The last of the mouse and the\n"
		"right stick to move decides. The bullets' autoaim (bent toward the\n"
		"target) stays either way." },
	{ "input.mouse_vertical_sensitivity", _config_real, "1.0", "HALO_MOUSE_VERTICAL_SENSITIVITY", _environment_value,
		_platform_desktop,
		"How far the view turns up and down for the mouse's movement, times\n"
		"input.mouse_sensitivity (as CS2's m_pitch is of m_yaw): 1 the same as\n"
		"side to side, 0.5 half as far." },

	{ "input.button_prompts", _config_string, "\"auto\"", "HALO_BUTTON_PROMPTS", _environment_value, _platform_all,
		"The HUD's button prompts (\"Hold [X] to pick up\", \"Press [flashlight]\"):\n"
		"\"auto\" names the key or mouse button bound to the action (\"E\", \"Left\n"
		"Click\") while the keyboard and mouse were played with last, as Halo\n"
		"PC did, and draws the controller's button while a controller (or the\n"
		"touch controls) was; \"controller\" always the controller's buttons, as\n"
		"the Xbox; \"keyboard\" always the keys. In VR (but \"controller\"):\n"
		"what presses it on the controllers in the hands, as the runtime names\n"
		"it (\"A Button\", \"Right Hand Trigger\"), or the gesture that does." },

	/* the keyboard and mouse's own controls (port/linux/src/xinput_sdl.c) */
	{ "controls.move_forward", _config_string, "\"W\"", "HALO_KEY_MOVE_FORWARD", _environment_value, _platform_all,
		"The keyboard and mouse's controls, which Settings > Controls Setup\n"
		"changes: up to two keys or buttons each, separated by a comma. Keys by\n"
		"their names on a US keyboard (\"W\", \"Space\", \"Left Ctrl\", \"F1\"): a key\n"
		"is the one in that place on any keyboard, which the menus show by its\n"
		"own label. Buttons: \"Mouse Left\", \"Mouse Right\", \"Mouse Middle\",\n"
		"\"Mouse 4\", \"Mouse 5\", \"Wheel\" (either way), \"Wheel Up\" and \"Wheel\n"
		"Down\"; empty for none. Moving forward:" },
	{ "controls.move_backward", _config_string, "\"S\"", "HALO_KEY_MOVE_BACKWARD", _environment_value, _platform_all,
		"Moving backward." },
	{ "controls.strafe_left", _config_string, "\"A\"", "HALO_KEY_STRAFE_LEFT", _environment_value, _platform_all,
		"Moving left." },
	{ "controls.strafe_right", _config_string, "\"D\"", "HALO_KEY_STRAFE_RIGHT", _environment_value, _platform_all,
		"Moving right." },
	{ "controls.jump", _config_string, "\"Space\"", "HALO_KEY_JUMP", _environment_value, _platform_all,
		"Jumping (and skipping cutscenes)." },
	{ "controls.crouch", _config_string, CROUCH_KEYS, "HALO_KEY_CROUCH", _environment_value, _platform_all,
		"Crouching." },
	{ "controls.fire", _config_string, "\"Mouse Left\"", "HALO_KEY_FIRE", _environment_value, _platform_all,
		"Firing." },
	{ "controls.throw_grenade", _config_string, "\"G\"", "HALO_KEY_THROW_GRENADE", _environment_value,
		_platform_all,
		"Throwing a grenade." },
	{ "controls.melee", _config_string, "\"F, Mouse 4\"", "HALO_KEY_MELEE", _environment_value, _platform_all,
		"Melee attack." },
	{ "controls.reload", _config_string, "\"R\"", "HALO_KEY_RELOAD", _environment_value, _platform_all,
		"Reloading." },
	{ "controls.zoom", _config_string, "\"Mouse Right, Z\"", "HALO_KEY_ZOOM", _environment_value, _platform_all,
		"Zooming the scope." },
	{ "controls.switch_weapon", _config_string, "\"Wheel, 1\"", "HALO_KEY_SWITCH_WEAPON", _environment_value,
		_platform_all,
		"Switching weapons." },
	{ "controls.switch_grenade", _config_string, "\"X\"", "HALO_KEY_SWITCH_GRENADE", _environment_value, _platform_all,
		"Switching grenades." },
	{ "controls.action", _config_string, "\"E\"", "HALO_KEY_ACTION", _environment_value, _platform_all,
		"The action: picking up (held: swapping weapons), entering and leaving\n"
		"vehicles, pressing switches; never reloading (the controller's X does\n"
		"when there is nothing to act on)." },
	{ "controls.flashlight", _config_string, "\"Q\"", "HALO_KEY_FLASHLIGHT", _environment_value, _platform_all,
		"The flashlight." },
	{ "controls.scoreboard", _config_string, "\"Tab\"", "HALO_KEY_SCOREBOARD", _environment_value, _platform_all,
		"Showing the scores (the controller's Back)." },
	{ "controls.pause", _config_string, "\"Escape\"", "HALO_KEY_PAUSE", _environment_value, _platform_all,
		"The pause menu (the controller's Start)." },
	{ "controls.screenshot", _config_string, "\"F10\"", "HALO_KEY_SCREENSHOT", _environment_value, _platform_all,
		"Save a PNG screenshot beside maps/ (press once per capture)." },
	{ "controls.push_to_talk", _config_string, "\"V\"", "HALO_KEY_PUSH_TO_TALK", _environment_value, _platform_all,
		"Voice chat: talk while it is held (audio.voice_chat \"push_to_talk\")." },
	{ "controls.chat", _config_string, "\"T\"", "HALO_KEY_CHAT", _environment_value, _platform_all,
		"Network games' text chat: type a line to everyone (Enter sends it,\n"
		"Escape lets it go)." },
	{ "controls.team_chat", _config_string, "\"Y\"", "HALO_KEY_TEAM_CHAT", _environment_value, _platform_all,
		"Network games' text chat: type a line to your team." },

	{ "game.console_log", _config_string, "\"important\"", "HALO_CONSOLE_LOG", _environment_value, _platform_all,
		"What the game's console shows on screen of what it logs: \"important\"\n"
		"(bans, players dropped for cheating, what refuses a command, and the\n"
		"asserts that stop the game), \"all\" (every line, the game's own\n"
		"chatter too), or \"none\" (the asserts that stop the game only). What\n"
		"a command prints shows whatever this is, and debug.txt has every line." },

	{ "game.language", _config_string, "\"\"", "HALO_LANGUAGE", _environment_value, _platform_all,
		"The language the game asks the Xbox for: \"ja\", \"de\", \"fr\", \"es\" or \"it\";\n"
		"empty for English. The game data decides what is translated." },
	{ "display.text_chat", _config_boolean, "true", "HALO_TEXT_CHAT", _environment_value, _platform_all,
		"Show the text chat of network games: false hides its lines and keeps\n"
		"its keys (and a controller's D-pad Up) from opening it, as for purists\n"
		"and streamers." },
	{ "display.chat_style", _config_string, "\"colors\"", "HALO_CHAT_STYLE", _environment_value, _platform_all,
		"The text chat's colours: \"colors\" (names in their team's colour, the\n"
		"text a pale blue) or \"white\" (all white, as Custom Edition)." },
	{ "game.enhanced_animations", _config_boolean, "true", "HALO_ENHANCED_ANIMATIONS", _environment_value, _platform_all,
		"The player bipeds' grenade throws keep their legs moving (crouched,\n"
		"in the air and in a vehicle's seat too), riders' hands leave the grips\n"
		"to throw and reload, and a player turns with the aim while throwing;\n"
		"false: the original animations, which freeze the legs and stand a\n"
		"rider up." },
	{ "game.sim_rate", _config_integer, "60", "HALO_SIM_RATE", _environment_value, _platform_all,
		"The steps a second of what a player feels in their hands: their own\n"
		"movement and aim, projectiles and thrown items: 30 (as the game was\n"
		"made), 60 or 120, in steps inside the game's 30 ticks a second, which\n"
		"everything else keeps. For games of this computer alone: the campaign,\n"
		"split screen and games with bots. Network games play at the rate they\n"
		"agreed on (a Legacy game's 30, an OpenH1 game's network.sim_rate), and\n"
		"saved films at 30, whatever this is." },
	{ "game.custom_edition", _config_boolean, "true", "HALO_CUSTOM_EDITION", _environment_value, _platform_all,
		"Load and run Halo Custom Edition maps (not those that need OpenSauce):\n"
		"put them and Custom Edition's bitmaps.map, sounds.map and loc.map in\n"
		"the custom_maps folder beside the maps folder; the map lists show them\n"
		"as CUSTOM SINGLEPLAYER and CUSTOM MULTIPLAYER. Their tags are checked\n"
		"as the game's own maps' are before they run; false refuses them\n"
		"(docs/custom_edition_caches.md)." },
	{ "bots.count", _config_integer, "0", "HALO_BOTS", _environment_value, _platform_all,
		"Computer players (bots) in a local (split screen) Slayer game: 0 to 15\n"
		"join its lobby with the players, as far as the game has room. Not in\n"
		"system link or internet games. 0: none, as in the stock game." },
	{ "bots.skill", _config_string, "\"normal\"", "HALO_BOT_SKILL", _environment_value, _platform_all,
		"How well the bots play: \"easy\", \"normal\", \"heroic\" or \"legendary\"\n"
		"(how soon they react, how well they aim, how far they see)." },
	{ "bots.teams", _config_string, "\"even\"", "HALO_BOT_TEAMS", _environment_value, _platform_all,
		"In team games: \"even\" puts the bots on both teams, evening them with\n"
		"the players; \"against\" puts them all on the team the players are not on." },
	{ "display.viewmodel_shield", _config_boolean, "false", "HALO_VIEWMODEL_SHIELD", _environment_value, _platform_all,
		"The energy shield's flare on the first-person arms too, as on the body\n"
		"(the stock game draws it on the body only)." },
#ifdef HALO_VR
	/* (the VR build's on: in the headset, your body under you, port/linux/README.md "Your body") */
	{ "display.first_person_legs", _config_boolean, "true", "HALO_FIRST_PERSON_LEGS", _environment_value, _platform_all,
#else
	{ "display.first_person_legs", _config_boolean, "false", "HALO_FIRST_PERSON_LEGS", _environment_value, _platform_all,
#endif
		"Your own legs in first person, seen looking down: your body as others\n"
		"see it, drawn from the waist down, moving as it moves." },
	{ "display.overshield_effect", _config_boolean, "false", "HALO_OVERSHIELD_EFFECT", _environment_value, _platform_all,
		"The overshield as this machine draws it: its flare rising up the body as\n"
		"it charges (2 seconds, 3 from a shield that was down) and staying lit,\n"
		"its energy slower, in its own color (the powerup's), on the first-person\n"
		"arms too with viewmodel_shield; a hit on it plays what the projectile\n"
		"plays on an Elite's shield. Only what is drawn and heard changes.\n"
		"In a network game, the host's network.shield_effects decides, unless\n"
		"it is \"players\" (each player's own)." },
	{ "display.shield_glow", _config_boolean, "false", "HALO_SHIELD_GLOW", _environment_value, _platform_all,
		"Energy shields light what is around them as they flare, as plasma\n"
		"does, in the shield's own color. In a network game, the host's\n"
		"network.shield_effects decides, unless it is \"players\" (each\n"
		"player's own)." },
	{ "display.shield_glow_intensity", _config_string, "\"default\"", "HALO_SHIELD_GLOW_INTENSITY", _environment_value, _platform_all,
		"How far the shield glow reaches: \"default\" or \"light_show\" (three\n"
		"times as far, and at full strength from a third of the flare).\n"
		"In a network game, the host's network.shield_effects decides, unless\n"
		"it is \"players\" (each player's own)." },
	{ "display.dynamic_lights", _config_string, GRAPHICS_DYNAMIC_LIGHTS, "HALO_DYNAMIC_LIGHTS", _environment_value,
		_platform_all,
		"The light of plasma bolts, needles, rockets, muzzle flashes and\n"
		"explosions: \"classic\" (the game's own) or \"enhanced\": for each pixel\n"
		"of the walls and models, through the walls' bump maps, gliding with\n"
		"the bolt between ticks, brighter in its core and fading softly, with\n"
		"a glow about each plasma bolt, and lighting your own weapon and hands." GRAPHICS_CHOOSES },
	{ "display.shield_color", _config_string, "\"default\"", "HALO_SHIELD_COLOR", _environment_value, _platform_all,
		"Your energy shield's color, as this machine draws it: \"default\" (the\n"
		"shield's own) or a multiplayer armor color: white, black, red, blue,\n"
		"gray, yellow, green, pink, purple, cyan, cobalt, orange, teal, sage,\n"
		"brown, tan, maroon or salmon." },

	{ "game.unlock_all_levels", _config_boolean, "false", "HALO_UNLOCK_ALL_LEVELS", _environment_value, _platform_all,
		"true: a profile made from now on starts with every campaign level\n"
		"finished on every difficulty, so any level can be played. false: a new\n"
		"profile starts at The Pillar of Autumn and opens each level as the one\n"
		"before it is finished, as on the Xbox. Profiles already made keep their\n"
		"progress; the console's profile_reset_campaign starts one over." },

	{ "game.downloaded_maps", _config_string, "\"\"", "HALO_DOWNLOADED_MAPS", _environment_value, _platform_all,
		"Maps played as downloaded ones, besides those map sharing downloaded:\n"
		"their names (bloodgulch, hugeass), separated by commas, or \"*\" for\n"
		"every map. A downloaded map's scripts may not change the player's\n"
		"settings or other players' games (debug.txt names what they were\n"
		"refused)." },

	{ "paths.data", _config_string, "\"\"", "HALO_DATA_ROOT", _environment_value, _platform_desktop,
		"The folder holding the game data's maps folder; empty looks in the\n"
		"working directory and its assets folder. Windows paths are easiest in\n"
		"single quotes: 'C:\\Games\\Halo'." },
	{ "paths.saves", _config_string, "\"\"", "HALO_SAVE_ROOT", _environment_value, _platform_desktop,
		"Where saved games and profiles go; empty for the usual place\n"
		"(~/.local/share/halo-linux, or %APPDATA%\\halo on Windows)." },
	{ "paths.custom_edition", _config_string, "\"\"", "HALO_CUSTOM_EDITION_ROOT", _environment_value, _platform_desktop,
		"A Halo Custom Edition install whose maps folder is looked in after the\n"
		"custom_maps folder for Custom Edition maps and their bitmaps.map,\n"
		"sounds.map and loc.map (game.custom_edition); empty for none." },

	{ "network.address", _config_string, "\"\"", "HALO_NET_ADDRESS", _environment_value, _platform_all,
		"This machine's IPv4 address for system link, for a machine on several\n"
		"networks; empty chooses one." },
	{ "network.broadcast", _config_string, "\"\"", "HALO_NET_BROADCAST", _environment_value, _platform_all,
		"Comma-separated IPv4 addresses system link sends its announcements to\n"
		"instead of the local network's broadcast address (for VPNs); empty for\n"
		"the local network." },
	{ "network.online", _config_boolean, "true", "HALO_NET_ONLINE", _environment_value, _platform_all,
		"Internet play: hosting makes an invite link (logged, and put on the\n"
		"clipboard) that lets whoever has it join over the internet; opening a\n"
		"link (or copying one before switching to the game) joins. Only people\n"
		"with the invite can join. Off keeps system link to the local network." },
	{ "network.join_from_clipboard", _config_boolean, "true", "HALO_NET_JOIN_FROM_CLIPBOARD", _environment_value,
		_platform_all,
		"Join the game of an invite link found on the clipboard when the game\n"
		"comes to the front." },
	{ "network.tunnel_port", _config_integer, "0", "HALO_NET_TUNNEL_PORT", _environment_value, _platform_all,
		"The UDP port internet play uses; 0 picks one. A fixed one can be\n"
		"forwarded on the router, for networks whose NAT stops connections." },
	{ "network.allow_upnp", _config_boolean, "true", "HALO_NET_ALLOW_UPNP", _environment_value, _platform_all,
		"Let internet play ask the router (UPnP) to forward its port, for\n"
		"networks whose NAT stops connections: when a player joins this\n"
		"machine's game, and when joining a game takes too long. False never\n"
		"asks." },
	{ "network.public_lobby", _config_boolean, "true", "HALO_NET_PUBLIC_LOBBY", _environment_value, _platform_all,
		"The server browser: public games are listed through the signalling\n"
		"brokers, and Join Game > Server Browser shows them. False lists no\n"
		"game of this machine's and shows none." },
	{ "network.host_public", _config_boolean, "true", "HALO_NET_HOST_PUBLIC", _environment_value, _platform_all,
		"Whether a new game of Create Game > Internet starts as PUBLIC (listed\n"
		"in everyone's server browser: anyone can see and join it) or, false,\n"
		"PRIVATE (only players with its invite link can join). Server Setup's\n"
		"LISTING changes it for each game." },
	{ "network.game_mode", _config_string, "\"openh1\"", "HALO_NET_GAME_MODE", _environment_value, _platform_all,
		"Hosting: \"openh1\", an OpenH1 game at the rate network.sim_rate picks,\n"
		"which OpenCE players join too at 30 (network.allow_opence), or\n"
		"\"legacy\", OpenCE's netcode at 30 ticks a second for everyone. Read as\n"
		"the game is hosted (\"standard\" is \"openh1\" too)." },
	{ "network.sim_rate", _config_integer, "60", "HALO_NET_SIM_RATE", _environment_value, _platform_all,
		"Hosting an OpenH1 game: its tick rate, 30, 60 or 120 (game.sim_rate's\n"
		"steps), which every machine of it takes as the game starts. A Legacy game\n"
		"plays at 30." },
	{ "network.allow_opence", _config_boolean, "true", "HALO_NET_ALLOW_OPENCE", _environment_value, _platform_all,
		"Hosting an OpenH1 game: let OpenCE players join it too. They play at 30\n"
		"ticks a second, as on their own machine, and OpenH1 players at\n"
		"network.sim_rate. False: OpenH1 players only. Read with\n"
		"network.game_mode." },
	{ "network.opence_welcome", _config_boolean, "true", "HALO_NET_OPENCE_WELCOME", _environment_value,
		_platform_all,
		"Hosting: tell each OpenCE player, a few seconds after they load into a\n"
		"game, that this server runs OpenH1 and where to get it (two lines of\n"
		"the text chat, once a player). OpenH1 players do not see them." },
	{ "network.voice_lobby", _config_boolean, "true", "HALO_NET_VOICE_LOBBY", _environment_value, _platform_all,
		"Hosting: voice chat in the lobby, before and after a game, where every\n"
		"player hears every other." },
	{ "network.voice_mode", _config_string, "\"team_global_enemy_proximity\"", "HALO_NET_VOICE_MODE",
		_environment_value, _platform_all,
		"Hosting: voice chat in a game: \"off\"; \"team_proximity\" (teammates\n"
		"near); \"team_enemy_proximity\" (anyone near); \"team_global\" (all\n"
		"teammates); or \"team_global_enemy_proximity\" (all teammates, and\n"
		"enemies near). A game without teams has only enemies; co-op only\n"
		"teammates." },
	{ "network.voice_kbps", _config_integer, "24", "HALO_NET_VOICE_KBPS", _environment_value, _platform_all,
		"Hosting: the voices' quality, in the lobby and in a game, in kilobits a\n"
		"second (8 to 64)." },
	{ "network.voice_proximity", _config_real, "15.0", "HALO_NET_VOICE_PROXIMITY", _environment_value,
		_platform_all,
		"Hosting: how near (world units: 1 is about 3 metres) a player must be\n"
		"to be heard by proximity voice chat (5 to 100)." },
	{ "network.text_chat", _config_string, "\"on\"", "HALO_NET_TEXT_CHAT", _environment_value, _platform_all,
		"Hosting: the text chat: \"on\" (links in lines are refused), \"links\"\n"
		"(links allowed) or \"off\", in either case. Always off in a game listed\n"
		"in the server browser (a public internet game)." },
	{ "network.votekick", _config_boolean, "true", "HALO_NET_VOTEKICK", _environment_value, _platform_all,
		"Hosting: let the players vote to kick a player (the scoreboard's\n"
		"right-click, or the console's votekick). More than half of the players\n"
		"must vote, counted once per address." },
	{ "network.votekick_minutes", _config_integer, "5", "HALO_NET_VOTEKICK_MINUTES", _environment_value,
		_platform_all,
		"Hosting: the minutes a player must have played on this server to start\n"
		"a vote to kick (0 to 60); to vote, 2 minutes or this, the less." },
	{ "network.votekick_ban_minutes", _config_integer, "30", "HALO_NET_VOTEKICK_BAN_MINUTES", _environment_value,
		_platform_all,
		"Hosting: the minutes a player kicked by a vote cannot join again (1 to\n"
		"1440)." },
	{ "network.map_share", _config_boolean, "true", "HALO_NET_MAP_SHARE", _environment_value, _platform_all,
		"Hosting: send a player who joins without the game's Custom Edition map\n"
		"the map (from custom_maps), and the resource maps it needs that the\n"
		"player lacks (bitmaps.map, sounds.map, loc.map). The game's other\n"
		"players help (network.map_seed)." },
	{ "network.map_share_upload", _config_integer, "1024", "HALO_NET_MAP_SHARE_UPLOAD", _environment_value,
		_platform_all,
		"Hosting: the most kilobytes a second the host sends of maps (16 to\n"
		"20480: 20 MB/s), so that the game's own traffic keeps the rest of the\n"
		"upload." },
	{ "network.map_seed", _config_boolean, "true", "HALO_NET_MAP_SEED", _environment_value, _platform_all,
		"Playing in another's game: help send its map to players who join\n"
		"without it, as a torrent's peers do (the pieces this machine has, at\n"
		"most network.map_seed_upload)." },
	{ "network.map_seed_upload", _config_integer, "256", "HALO_NET_MAP_SEED_UPLOAD", _environment_value,
		_platform_all,
		"Playing in another's game: the most kilobytes a second this machine\n"
		"sends of maps to other players (16 to 10240: 10 MB/s)." },
	{ "network.map_download", _config_boolean, "true", "HALO_NET_MAP_DOWNLOAD", _environment_value, _platform_all,
		"Joining a game on a Custom Edition map this machine does not have:\n"
		"download it (and the resource maps it needs) from the game's players\n"
		"into custom_maps, if its host shares it (network.map_share); false\n"
		"leaves the game, saying which map is missing." },
	{ "network.coop_public", _config_boolean, "false", "HALO_NET_COOP_PUBLIC", _environment_value, _platform_all,
		"Whether an online co-op game (Create Game > Internet, a SINGLEPLAYER\n"
		"map) starts as PUBLIC or, false, PRIVATE: Server Setup's LISTING in\n"
		"co-op, which writes its choice here." },
	{ "network.coop_friendly_fire", _config_string, "\"on\"", "HALO_NET_COOP_FRIENDLY_FIRE", _environment_value,
		_platform_all,
		"Whether the players of an online co-op game hurt each other: \"off\",\n"
		"\"on\", \"shields_only\" or \"explosives_only\" (FRIENDLY FIRE in\n"
		"co-op's Server Setup > Co-op Options writes its choice here). Their AI\n"
		"allies they hurt as network.coop_ally_friendly_fire says." },
	{ "network.coop_ally_friendly_fire", _config_boolean, "true", "HALO_NET_COOP_ALLY_FRIENDLY_FIRE",
		_environment_value, _platform_all,
		"Whether the players of an online co-op game hurt their AI allies (the\n"
		"Marines, Captain Keyes, the Sentinels while they are allies), as in the\n"
		"campaign; false, the players' weapons, grenades and vehicles do them\n"
		"no harm. ALLY FRIENDLY FIRE in co-op's Server Setup > Co-op Options\n"
		"writes its choice here." },
	{ "network.coop_own_friendly_fire", _config_boolean, "true", "HALO_NET_COOP_OWN_FRIENDLY_FIRE",
		_environment_value, _platform_all,
		"Whether this machine's players hurt, and are hurt by, the other players\n"
		"of an online co-op game: two players hurt each other only if both have\n"
		"it true. FRIENDLY FIRE in that game's pause menu writes its choice\n"
		"here. A player away from the controls for over 70 seconds is hurt by\n"
		"every player, whatever either chose." },
	{ "network.coop_player_collisions", _config_boolean, "true", "HALO_NET_COOP_PLAYER_COLLISIONS", _environment_value,
		_platform_all,
		"Whether the players of an online co-op game bump into each other;\n"
		"false, they walk through each other (the AI's characters they still\n"
		"bump into). PLAYER COLLISIONS in co-op's Server Setup > Co-op Options\n"
		"writes its choice here." },
	{ "network.coop_enemies_mode", _config_string, "\"per_player\"", "HALO_NET_COOP_ENEMIES_MODE", _environment_value,
		_platform_all,
		"Online co-op's extra enemies: \"none\", \"per_player\" (each squad of\n"
		"enemies grows by coop_enemies for each player past the first) or\n"
		"\"multiplier\" (each is coop_enemies_multiplier times as large, for any\n"
		"number of players). EXTRA ENEMIES in co-op's Server Setup > Co-op\n"
		"Options writes its choice here." },
	{ "network.coop_pickups", _config_string, "\"none\"", "HALO_NET_COOP_PICKUPS", _environment_value, _platform_all,
		"Extra campaign weapons, grenades, health kits, overshields and camouflage.\n"
		"Modes: \"none\", \"per_player\" (coop_enemies percent per extra player),\n"
		"or \"multiplier\" (coop_enemies_multiplier times as many), up to 8 times\n"
		"the original pickups. Only safe nearby places receive copies." },
	{ "network.coop_vehicles", _config_string, "\"none\"", "HALO_NET_COOP_VEHICLES", _environment_value, _platform_all,
		"Extra campaign vehicles: \"none\", \"per_player\" or \"multiplier\".\n"
		"After a player clears its unloaded starting place by both bounding\n"
		"radii plus 15 percent, make one more there, up to\n"
		"floor((players - 1) * coop_enemies * 2 / 300) extras, or floor((coop_enemies_multiplier - 1) * 2 / 3) in static mode.\n"
		"The host alone creates them, and waits while the starting place is blocked." },
	{ "network.coop_enemies", _config_integer, "50", "HALO_NET_COOP_ENEMIES", _environment_value, _platform_all,
		"Online co-op's extra enemies per player, a percentage: for each player\n"
		"past the first, each squad of enemies a level places gets this much of\n"
		"itself more (100: as many again; 25 to 200). PER PLAYER in co-op's\n"
		"Server Setup > Co-op Options writes its choice here." },
	{ "network.coop_enemies_multiplier", _config_integer, "2", "HALO_NET_COOP_ENEMIES_MULTIPLIER", _environment_value,
		_platform_all,
		"Online co-op's shared static multiplier (2 to 32). Pickups are capped\n"
		"at 8 times their original count; extra vehicle growth uses two thirds\n"
		"of (multiplier - 1), rounded down. MULTIPLIER in\n"
		"co-op's Server Setup > Co-op Options writes its choice here." },
	{ "network.brokers_file", _config_string, "\"brokers.txt\"",
		"HALO_NET_BROKERS_FILE", _environment_value, _platform_all,
		"The file of the public MQTT brokers through which the machines of an\n"
		"invite find each other (its messages are encrypted), beside this file\n"
		"(else the game's own, share/openh1/brokers.txt in the Linux release)\n"
		"unless a full path: one host:port on each line, up to 4. Updates\n"
		"replace brokers.txt: keep a list of your own under another name." },
	{ "network.stun_servers", _config_string, "\"stun.l.google.com:19302,stun.cloudflare.com:3478\"",
		"HALO_NET_STUN", _environment_value, _platform_all,
		"Public STUN servers that tell this machine its internet address;\n"
		"comma-separated host:port." },
	{ "discord.application_id", _config_string, "\"1553978809840050229\"", "HALO_DISCORD_APPLICATION",
		_environment_value, _platform_desktop,
		"The Discord application internet play invites go through while the\n"
		"Discord desktop client runs; empty for none." },

	{ "update.auto", _config_boolean, "true", "HALO_UPDATE_AUTO", _environment_value, _platform_all,
		"Look for a new version when the game starts, and offer to update to it;\n"
		"false never looks (the game's \"Do not ask again\" writes false here)." },
	{ "streamer.enabled", _config_boolean, "false", "HALO_STREAMER_MODE", _environment_value, _platform_all,
		"Hide offensive words in names and text from other players, maps and mods\n"
		"as stars, at the levels below (Settings > Network Setup > Streamer Mode)." },
	{ "streamer.slurs", _config_string, "\"mild\"", "HALO_STREAMER_SLURS", _environment_value, _platform_all,
		"With streamer.enabled, the slurs (race, sexuality, gender, disability)\n"
		"hidden: \"off\", \"severe\", \"strong\" (and severe) or \"mild\" (all)." },
	{ "streamer.swearing", _config_string, "\"strong\"", "HALO_STREAMER_SWEARING", _environment_value, _platform_all,
		"With streamer.enabled, the swearing and sexual words hidden: \"off\",\n"
		"\"severe\", \"strong\" (and severe) or \"mild\" (all)." },
	{ "streamer.insults", _config_string, "\"off\"", "HALO_STREAMER_INSULTS", _environment_value, _platform_all,
		"With streamer.enabled, the insults that are not slurs hidden: \"off\",\n"
		"\"severe\", \"strong\" (and severe) or \"mild\" (all)." },
	{ "crash_reports.upload", _config_string, "\"ask\"", "HALO_CRASH_REPORTS", _environment_value, _platform_windows,
		"Send a report of each crash (a minidump and halo.log) to the developers'\n"
		"Sentry project (port/windows/host/host_crash_win.c): \"yes\" sends them, \"no\"\n"
		"never does, \"ask\" asks at the next crash and writes the answer here." },

	{ "debug.join_legacy", _config_boolean, "false", "HALO_DEBUG_JOIN_LEGACY", _environment_value, _platform_all,
		"Join every game as an OpenCE build joins it: an OpenH1 game that OpenCE\n"
		"players join too (network.allow_opence) at 30 ticks a second, with the\n"
		"Legacy game's join token. The tests' stand-in for an OpenCE machine." },
	{ "debug.network_test", _config_string, "\"\"", "HALO_NETWORK_TEST", _environment_value, _platform_all,
		"Automated system link sessions for testing (port/linux/game/network_test.c):\n"
		"\"host:<map>\" hosts a game on that map, \"local:<map>\" plays a local (split\n"
		"screen) game on it, \"join\" joins the first game found,\n"
		"\"browse\" the one with the most players the server browser lists (not\n"
		"co-op, if it can), \"log\" logs the game that runs (and runs its cues);\n"
		"empty for none." },
	{ "debug.network_test_start", _config_real, "15.0", "HALO_NETWORK_TEST_START", _environment_value, _platform_all,
		"Seconds after hosting that an automated test game starts." },
	{ "debug.network_test_kill", _config_real, "0.0", "HALO_NETWORK_TEST_KILL", _environment_value, _platform_all,
		"Every this many seconds an automated test host kills its last player; 0 never." },
	{ "debug.network_test_score", _config_integer, "0", "HALO_NETWORK_TEST_SCORE", _environment_value, _platform_all,
		"The score an automated test host's game type plays to (a short game, to\n"
		"test the next); 0 the game type's own." },
	{ "debug.network_test_shoot", _config_real, "0.0", "HALO_NETWORK_TEST_SHOOT", _environment_value, _platform_all,
		"Every this many seconds each automated test player hits the next with\n"
		"their weapon, within its reach (the host brings far players near the\n"
		"first a second before); 0 never." },
	{ "debug.network_test_vehicle", _config_real, "0.0", "HALO_NETWORK_TEST_VEHICLE", _environment_value, _platform_all,
		"This many seconds into an automated test game the host seats its last\n"
		"player as a vehicle's driver (and out 15 seconds on); 0 never." },
	{ "debug.network_test_hitch", _config_real, "0.0", "HALO_NETWORK_TEST_HITCH", _environment_value, _platform_all,
		"As debug.network_test_vehicle seats the player, the frame stops for this\n"
		"many seconds, as a stall of the renderer or the VR runtime would; 0 never." },
	{ "debug.network_test_pickup", _config_real, "0.0", "HALO_NETWORK_TEST_PICKUP", _environment_value, _platform_all,
		"This many seconds into an automated test game the host stands its last\n"
		"player on a weapon, which a joining player then picks up; 0 never." },
	{ "debug.network_test_chat", _config_real, "0.0", "HALO_NETWORK_TEST_CHAT", _environment_value, _platform_all,
		"This many seconds into an automated test game each machine's player\n"
		"says a chat line to everyone, then to the team and to the vehicle (the\n"
		"host seats the last two players in one first), then the host mutes and\n"
		"unmutes a player, refuses a link and turns the chat off, and the\n"
		"clients type on the on-screen keyboard (port/linux/NETCODE.md); 0 never." },
	{ "debug.network_test_name", _config_string, "\"\"", "HALO_NETWORK_TEST_NAME", _environment_value, _platform_all,
		"The name this machine's player takes in an automated test game (ASCII);\n"
		"empty keeps its profile's." },
	{ "debug.network_test_pickup_weapon", _config_string, "\"\"", "HALO_NETWORK_TEST_PICKUP_WEAPON", _environment_value,
		_platform_all,
		"The weapon network_test_pickup stands the player on: the first whose tag\n"
		"name has this in it (\"sniper\", say); empty any." },
	{ "debug.network_test_cues", _config_string, "\"\"", "HALO_NETWORK_TEST_CUES", _environment_value, _platform_all,
		"Console lines an automated test runs at a game second, from the first tick\n"
		"every player of debug.network_test_cue_players has a unit:\n"
		"\"<second> <line>|<second> <line>...\" (the host's, or a game of one\n"
		"machine)." },
	{ "debug.network_test_cue_players", _config_integer, "1", "HALO_NETWORK_TEST_CUE_PLAYERS", _environment_value,
		_platform_all,
		"The players debug.network_test_cues waits for." },
	{ "debug.network_test_public", _config_boolean, "false", "HALO_NETWORK_TEST_PUBLIC", _environment_value,
		_platform_all,
		"The network test's host lists its game in the server browser, as Create\n"
		"Game > Internet's PUBLIC does (debug.network_test \"browse\" joins it)." },
	{ "debug.telnet_console", _config_boolean, "false", "HALO_TELNET_CONSOLE", _environment_set_is_true, _platform_all,
		"Listen on 127.0.0.1 (port telnet_console_port) for a script console that\n"
		"runs what it is sent as the game's console does, with no password; false\n"
		"none." },
	{ "debug.telnet_console_port", _config_integer, "2323", "HALO_TELNET_CONSOLE_PORT", _environment_value,
		_platform_all,
		"The port of the script console (telnet_console); the Xbox's was 23, which\n"
		"only the administrator can listen on." },
	{ "debug.touch_menus", _config_boolean, "false", "HALO_TOUCH_MENUS", _environment_set_is_true, _platform_all,
		"Lay the menus out as the touchscreen's builds do (Android, iOS): their\n"
		"\"touch\" screens, anchored to the screen's edges, for looking at them on\n"
		"a computer. Read as the menus load." },
	{ "debug.touch_targets", _config_boolean, "false", "HALO_TOUCH_TARGETS", _environment_set_is_true, _platform_all,
		"Outline the menus' tap targets (item green, value blue, list slot yellow,\n"
		"legend button red, the band beside a list's slots orange; the virtual\n"
		"keyboard's keys white), mark where the last finger went down and the\n"
		"last tap landed for 3 seconds, and log each tap with the target it hit\n"
		"(and a value's split); to judge touch accuracy." },
	{ "debug.log_addresses", _config_boolean, "false", "HALO_LOG_ADDRESSES", _environment_set_is_true, _platform_all,
		"Write other machines' internet addresses whole in debug.txt and the\n"
		"console, for debugging your own network. False writes each public\n"
		"address as a tag (addr#3f2a9c) that only matches up within one run;\n"
		"local network addresses are always whole. Do not post a debug.txt\n"
		"written with this on: it has the address of everyone you played with." },
	{ "debug.network_latency", _config_real, "0.0", "HALO_NETWORK_LATENCY", _environment_value, _platform_all,
		"Milliseconds everything received is held back (a round trip between two\n"
		"machines of twice it), to test the netcode as over the internet; 0 none." },
	{ "debug.network_loss", _config_real, "0.0", "HALO_NETWORK_LOSS", _environment_value, _platform_all,
		"Percent of datagrams received that are dropped, for the same; 0 none." },
	{ "debug.network_corrupt", _config_real, "0.0", "HALO_NETWORK_CORRUPT", _environment_value, _platform_all,
		"Percent of the datagrams received that are damaged at random, to test\n"
		"that nothing a machine sends can crash the game; 0 none." },
	{ "debug.network_corrupt_stream", _config_real, "0.0", "HALO_NETWORK_CORRUPT_STREAM", _environment_value,
		_platform_all,
		"Percent of the reads of streams that are damaged at random, for the\n"
		"same (a damaged stream is closed, so a little goes a long way); 0 none." },
	{ "debug.network_corrupt_after", _config_real, "0.0", "HALO_NETWORK_CORRUPT_AFTER", _environment_value,
		_platform_all,
		"Seconds after the start before anything is damaged, so that a game can\n"
		"be set up and started first (a host's messages to its own client are\n"
		"damaged too)." },
	{ "debug.test_input", _config_string, "\"\"", "HALO_TEST_INPUT", _environment_value, _platform_all,
		"\"bot:<seed>\" plays controller 1 with a scripted pattern (automated\n"
		"network tests); \"look:<seed>\" stands still, only turning and looking\n"
		"up and down; \"walk:<seed>\" plays as \"bot\" without turning (the same\n"
		"input each tick at any frame rate); \"pad\" turns, looks, fires, throws, holds Back and walks in\n"
		"turn, logging each (the VR mode's vr.aim = \"gamepad\"); \"latency:<seed>\" pulls the fire\n"
		"control and pushes the stick in turn on the real clock (debug.latency_log);\n"
		"\"menu:<buttons>\" presses menu\n"
		"buttons one a second (a, b, x, y, up, down, left, right, start, back or\n"
		"wait, or click:<x>:<y>, a click of the pointer there, 0 to 1 of the\n"
		"window, separated by spaces or commas), for testing the menus;\n"
		"\"script:<second>[-<second>] <action>;...\" presses, holds and aims (\"hunt\": at the\n"
		"nearest enemy in sight) on the\n"
		"simulation's clock in seconds from the scenario's start (tools/scorecard),\n"
		"the same at any simulation rate; empty for none." },
	{ "debug.trace", _config_string, "\"\"", "HALO_TRACE", _environment_value, _platform_all,
		"A file to write a line for each simulation step to: the players' place,\n"
		"velocity and aim, each projectile's, and what happened in the step\n"
		"(shots, projectiles made, hits and detonations), timed in seconds from\n"
		"the scenario's start (port/linux/game/sim_trace.c, tools/scorecard);\n"
		"empty for none." },
	{ "debug.trace_seconds", _config_real, "0.0", "HALO_TRACE_SECONDS", _environment_value, _platform_all,
		"With debug.trace, quit once the scenario has run this many seconds of\n"
		"simulation; 0 never." },
	{ "debug.trace_mutation", _config_string, "\"\"", "HALO_TRACE_MUTATION", _environment_value, _platform_all,
		"With debug.trace, a known bug put in on purpose, which the scorecard\n"
		"must catch (tools/scorecard): \"head_start\", \"jump\" or \"speed\";\n"
		"empty for none." },
	{ "debug.bot_trace", _config_integer, "0", "HALO_BOT_TRACE", _environment_value, _platform_all,
		"Log each bot's state (bots.count) every this many seconds: where it is\n"
		"and goes, its path, target and weapon; 0 never." },
	{ "debug.bot_player", _config_boolean, "false", "HALO_BOT_PLAYER", _environment_set_is_true, _platform_all,
		"In a local game with bots (bots.count), a bot of its own plays the first\n"
		"player too, its view drawn as the player's (for recorded footage)." },
	{ "debug.bot_player_weapon", _config_string, "\"\"", "HALO_BOT_PLAYER_WEAPON", _environment_value, _platform_all,
		"With debug.bot_player, a weapon (part of its tag's name: \"sniper\",\n"
		"\"rocket\") the player's bot goes for wherever it lies, and keeps." },
	{ "debug.bot_player_grenades", _config_string, "\"\"", "HALO_BOT_PLAYER_GRENADES", _environment_value, _platform_all,
		"With debug.bot_player, \"plasma\": the player's bot spawns with four plasma\n"
		"grenades and no frags, and throws them closer and more often (for\n"
		"sticks); empty as the other bots." },
	{ "debug.bot_player_vehicle", _config_string, "\"\"", "HALO_BOT_PLAYER_VEHICLE", _environment_value, _platform_all,
		"With debug.bot_player, a vehicle (part of its tag's name: \"warthog\",\n"
		"\"ghost\") the player's bot boards when it is on foot, and drives at the\n"
		"enemies it fights." },
	{ "debug.update_answer", _config_string, "\"\"", "HALO_UPDATE_ANSWER", _environment_value, _platform_desktop,
		"The answer to the new version question, for automated tests: \"yes\",\n"
		"\"no\" or \"never\" (do not ask again, confirmed); empty asks." },
	{ "debug.first_start_input", _config_string, "\"\"", "HALO_FIRST_START_INPUT", _environment_value, _platform_desktop,
		"Linux: the buttons an automated test presses on the first start's screen\n"
		"(no game data), one each half second: \"up\", \"down\", \"a\" and \"b\",\n"
		"separated by commas; empty waits for the player." },
	{ "debug.update_from", _config_string, "\"\"", "HALO_UPDATE_FROM", _environment_value, _platform_desktop,
		"A release's zip file to update to, for testing updates: the game offers\n"
		"it as a new version without asking GitHub, even when it has no build\n"
		"number (not in the version it starts); empty asks GitHub." },
	{ "debug.fixed_step", _config_boolean, "false", "HALO_FIXED_STEP", _environment_set_is_true, _platform_all,
		"A game of this machine alone runs one tick a frame, whatever time has\n"
		"passed, so that it plays the same every time (tests); game_speed has no\n"
		"effect." },
	{ "debug.exit_after", _config_real, "0.0", "HALO_EXIT_AFTER", _environment_value, _platform_all,
		"Quit this many seconds after the window opens (without a window, after\n"
		"the first frame); 0 never." },
	{ "debug.hidden_window", _config_boolean, "false", "HALO_HIDDEN_WINDOW", _environment_set_is_true, _platform_desktop,
		"Keep the window hidden (and never fullscreen)." },
	{ "debug.voice_test", _config_boolean, "false", "HALO_VOICE_TEST", _environment_set_is_true, _platform_all,
		"Voice chat's automated tests: a tone instead of the microphone, and\n"
		"each voice heard logged once a second." },
	{ "debug.latency_log", _config_string, "\"\"", "HALO_LATENCY_LOG", _environment_value, _platform_all,
		"A file to write a line to for each press of the fire control and each\n"
		"move from standing of the first player: the milliseconds from the\n"
		"controller's reading to the step that acts on it, to the first frame\n"
		"drawn after it, to the first frame that shows it (the frames draw a\n"
		"tick behind), and that frame's drawing and handing on\n"
		"(port/linux/game/latency_log.c, tools/latency_summary.py); empty for\n"
		"none." },
	{ "debug.determinism_log", _config_string, "\"\"", "HALO_DETERMINISM_LOG", _environment_value, _platform_all,
		"A file to write a line a tick to: a hash of the game state, the random\n"
		"seeds and every taker of a seed by its call site, to compare two runs\n"
		"of the same input (tools/determinism_compare.py); empty for none." },
	{ "debug.determinism_dump", _config_integer, "0", "HALO_DETERMINISM_DUMP", _environment_value, _platform_all,
		"With debug.determinism_log, the tick after which to write the game\n"
		"state too, counted as logged from 1, to <log>.<count>.bin (and its first\n"
		"frame to <log>.<count>.bmp), to find where two runs' game states differ;\n"
		"0 none." },
	{ "debug.frame_time", _config_real, "0.0", "HALO_FRAME_TIME", _environment_value, _platform_all,
		"Seconds of game time each frame runs, whatever time it took, so that two\n"
		"runs of the same input run the same frames (the determinism check); 0\n"
		"the time each frame took." },
	{ "debug.random_seed", _config_integer, "0", "HALO_RANDOM_SEED", _environment_value, _platform_all,
		"The random seed of the network games this machine hosts, so that a test\n"
		"plays the same game again (the spawns, the weapons' spread); 0 a new\n"
		"one each game, from the clock." },
	{ "debug.audio_log", _config_boolean, "false", "HALO_AUDIO_LOG", _environment_value, _platform_desktop,
		"Log the sounds placed in the world playing, five times a second: where\n"
		"each is from the listener (metres ahead, right and up), its gain in\n"
		"each ear (and in the VR mode the HRTF's delays and filters), and the\n"
		"time the mixing takes." },
	{ "debug.audio_record", _config_string, "\"\"", "HALO_AUDIO_RECORD", _environment_value, _platform_desktop,
		"Record the game's sound, as it plays (with or without a device), to\n"
		"this WAV file (48 kHz, 16-bit stereo), for automated tests; empty none." },

#ifdef HALO_VR
	{ "debug.vr_force_render", _config_boolean, "false", "HALO_VR_FORCE_RENDER", _environment_value, _platform_desktop,
		"Draw the VR frames while the runtime says not to (the headset not\n"
		"worn), to measure them." },
	{ "debug.vr_gpu_time", _config_boolean, "false", "HALO_VR_GPU_TIME", _environment_value, _platform_desktop,
		"With gpu_stats, also log elapsed render GPU timelines for VR frames,\n"
		"read without waiting when the graphics device supports them." },
	{ "debug.vr_test_turn", _config_real, "0.0", "HALO_VR_TEST_TURN", _environment_value, _platform_desktop,
		"Turn the VR player this many degrees a second, as the right stick would\n"
		"(automated tests without hands); 0 none." },
	{ "debug.vr_test_jitter", _config_real, "0.0", "HALO_VR_TEST_JITTER", _environment_value, _platform_desktop,
		"Move the VR head by up to this many millimetres (and turn it as many\n"
		"hundredths of a degree) at random each frame, as a worn headset moves:\n"
		"for automated tests of what flickers when the view barely moves; 0 none." },
	{ "debug.vr_test_head", _config_string, "\"\"", "HALO_VR_TEST_HEAD", _environment_value, _platform_desktop,
		"Hold the VR head still and level where it is first, for automated tests\n"
		"(a simulated headset's wobbles): \"0\", or \"<degrees> <seconds>\" turns it\n"
		"left and right by as many degrees over as many seconds, and a third\n"
		"number pitches it up by as many degrees (less than 0: down); empty none." },
	{ "debug.vr_test_gaze", _config_string, "\"\"", "HALO_VR_TEST_GAZE", _environment_value, _platform_desktop,
		"The eyes' gaze, instead of the runtime's, for automated tests of the\n"
		"foveation (vr.foveation = \"eye\"): \"<right> <up>[ <seconds>]\", degrees from\n"
		"the head's forward, jumping to the opposite and back every <seconds>." },
	{ "debug.vr_foveation_show", _config_boolean, "false", "HALO_VR_FOVEATION_SHOW", _environment_value,
		_platform_desktop,
		"With vr.foveation: tint red what is drawn at the periphery's detail alone\n"
		"(its blend with the inset partly), to see the inset." },
	{ "debug.vr_test_fov", _config_string, "\"\"", "HALO_VR_TEST_FOV", _environment_value, _platform_desktop,
		"Each VR eye's field of view, instead of the runtime's, for automated tests\n"
		"of the edges of a wide view: \"<outward> <inward> <up> <down>[ <cant>]\",\n"
		"in degrees from the eye's axis, each eye turned outward by <cant>; empty\n"
		"the runtime's." },
	{ "debug.vr_test_hands", _config_string, "\"\"", "HALO_VR_TEST_HANDS", _environment_value, _platform_desktop,
		"Hold the VR controllers still, for automated tests without hands:\n"
		"\"lx ly lz lyaw lpitch lroll, rx ry rz ryaw rpitch rroll[, grip[, buttons\n"
		"[, trigger[, head[, frames[, move[, down[, nod[, forward[, right[, stick x[,\n"
		"stick y[, right grip]]]]]]]]]]]]]\",\n"
		"the left and right hands in\n"
		"metres right, up and forward from the recentred head and degrees of yaw\n"
		"(left), pitch (up) and roll (right), the left grip's pull (0 to 1), the\n"
		"buttons pressed for the pose's first 20 frames (A 1, B 2, X 4, Y 8, the\n"
		"right trigger 65536, the left 131072, the right grip 262144, the right\n"
		"stick pushed down 524288), the right trigger's pull (0 to 1; less than\n"
		"0: the finger off it, as the trigger's touch senses), the head\n"
		"turned (degrees left), the frames the pose lasts (0: 288)\n"
		"and the frames the hands take to move to it from the pose before (0:\n"
		"none; a swing), the head lowered (metres, as a crouch), nodded\n"
		"about the neck (degrees up) and moved in the room (metres forward\n"
		"and right), each moved with the hands, the left stick (right, forward:\n"
		"-1 to 1) and the right grip's pull (0 to 1) through the pose; poses\n"
		"separated by ';' (512 at\n"
		"most) in turn.\n"
		"Empty: the controllers' own." },
	{ "debug.vr_throw_log", _config_boolean, "false", "HALO_VR_THROW_LOG", _environment_value, _platform_desktop,
		"Log each step of a grenade thrown with the left hand (vr.grenade_throw):\n"
		"the grip taken, the swing (or why not), the release's speed and\n"
		"direction, and the game's throw timed to it." },
	{ "debug.vr_melee_log", _config_boolean, "false", "HALO_VR_MELEE_LOG", _environment_value, _platform_desktop,
		"Log the right hand's swings that did not melee (vr.melee_gesture) too:\n"
		"how fast and far each went, and why it was none." },

	{ "debug.vr_test_weapon", _config_string, "\"\"", "HALO_VR_TEST_WEAPON", _environment_value, _platform_desktop,
		"Put a weapon in the first VR player's hand, for automated tests of the\n"
		"reload by hand: \"<text> <seconds>\", that many seconds into the level the\n"
		"player's weapons are taken and a new one is in hand, the first of the\n"
		"map's whose tag name has the text in it (\"shotgun 20\"); empty none." },
	{ "debug.vr_test_seat", _config_string, "\"\"", "HALO_VR_TEST_SEAT", _environment_value, _platform_desktop,
		"Seat the first VR player in the nearest vehicle's seat whose label has\n"
		"this text in it, as the action button does, and take them out later,\n"
		"logging the view's motion each frame, for automated tests:\n"
		"\"<label> <seconds in> [<seconds seated>]\" (\"gunner\" 5 10); empty none." },
	{ "debug.vr_test_death", _config_string, "\"\"", "HALO_VR_TEST_DEATH", _environment_value, _platform_desktop,
		"Kill the first VR player this many seconds after they first aim,\n"
		"once, and log the view's motion each frame until some seconds after\n"
		"they are back, for automated tests: \"<seconds> [kill|grenade|watch]\"\n"
		"(as the script's unit_kill does, by a frag grenade's blast before\n"
		"them, or killed and then watching the nearest living biped); empty\n"
		"none." },
	{ "debug.vr_test_grenades", _config_string, "\"\"", "HALO_VR_TEST_GRENADES", _environment_value,
		_platform_desktop,
		"Give the first VR player these grenades as they first aim, for tests of\n"
		"the grenade held in the hand: \"<frags> <plasma grenades>\"; empty none." },
	{ "debug.vr_test_finger", _config_string, "\"\"", "HALO_VR_TEST_FINGER", _environment_value, _platform_desktop,
		"The right index finger on the weapon's trigger instead of the weapon's own\n"
		"(vr_hands.c's trigger_fingers), for tests: \"<rest> <rest> <rest> <pull>\n"
		"<pull> <pull> [<travel> [<off> <off> <off>]]\", its curl at each joint\n"
		"from the knuckle at rest and how much more at a full pull (radians), how\n"
		"far back the trigger goes (metres), and its curl off the trigger; empty\n"
		"none." },
	{ "debug.vr_test_weapons", _config_string, "\"\"", "HALO_VR_TEST_WEAPONS", _environment_value, _platform_desktop,
		"Give the first player these weapons as the map starts, in place of their\n"
		"own, the first in hand, for automated tests of the scopes: \"<name>[,\n"
		"<name>]\", parts of the weapons' tag names (\"sniper rifle, pistol\");\n"
		"empty none." },
	{ "debug.vr_test_flip", _config_string, "\"\"", "HALO_VR_TEST_FLIP", _environment_value, _platform_desktop,
		"Overturn a vehicle before the first VR player 3 s after they first aim,\n"
		"once a map, the level's AI erased, for automated tests of the lift:\n"
		"\"<name> [roof|side|upright] [<metres ahead>]\", name a part of the\n"
		"vehicle's tag name (\"warthog\"), on its roof (by default) or side, or\n"
		"upright (to enter), its middle as far ahead as puts it within reach (by\n"
		"default); empty none." },
	{ "debug.vr_test_head_wall", _config_string, "\"\"", "HALO_VR_TEST_HEAD_WALL", _environment_value, _platform_desktop,
		"Put the first VR player's eye at \"<x> <y> <z>\" (world units) once they\n"
		"have been on foot 2 s, once, for automated tests of the head in a wall\n"
		"(vr.head_in_wall) with debug.vr_test_hands, which logs the test." },
	{ "debug.vr_test_grab", _config_string, "\"\"", "HALO_VR_TEST_GRAB", _environment_value, _platform_desktop,
		"Lay weapons on the ground before the first VR player 3 s after they\n"
		"first aim, once a map, the level's AI erased, for automated tests of\n"
		"the grab: \"weapon <name>[ @ <metres ahead> <metres right>][; weapon\n"
		"...]\", name a part of the weapon's tag name (\"plasma rifle\"), 0.5 m\n"
		"ahead and 0.15 m right by default; or \"control [<n> [<metres>]]\",\n"
		"stand them before the nth nearest control they could use, facing it,\n"
		"0.6 m from it by default (as a network game's host, flat, the last\n"
		"player, whose machine turns them to it); empty none." },
	{ "debug.vr_test_target", _config_string, "\"\"", "HALO_VR_TEST_TARGET", _environment_value, _platform_desktop,
		"Place a still biped before the first VR player 3 s after they first aim\n"
		"on foot (again 2 s after it dies), the level's AI erased, point the right\n"
		"controller held by debug.vr_test_hands at its middle, and log each hit\n"
		"on it, for automated tests of the aim: \"<metres ahead> [<metres right>\n"
		"[<name> [free|head]]][; <vehicle>]\", name a part of the biped's tag name\n"
		"(\"grunt\" if none; \"-\": none), head pointing at its head, free not\n"
		"pointing; after a ';', seated in a vehicle whose tag's name has that in\n"
		"it (\"c gun turret\"); empty none." },
	{ "debug.vr_test_shake", _config_string, "\"\"", "HALO_VR_TEST_SHAKE", _environment_value, _platform_desktop,
		"Shake the controllers as a hand does, for automated tests of the aim\n"
		"steadied while zoomed: \"<degrees> [<millimetres>]\", the right\n"
		"controller turned at random by this much (RMS, half a slow wander, half a\n"
		"9 Hz tremor), both moved by this much; empty none." },
	{ "debug.vr_aim_log", _config_boolean, "false", "HALO_VR_AIM_LOG", _environment_value, _platform_desktop,
		"Log the controller's aim once a tick: where its line meets the world,\n"
		"how far the game's aim is turned to meet it there from the game's eye,\n"
		"how far from that point the unit's aim passes, whether the reticle is\n"
		"red, and the aim assist's target; with debug.vr_test_hands, the controller's direction and the\n"
		"aim's, steadied, each frame." },
	{ "debug.vr_audio_log", _config_boolean, "false", "HALO_VR_AUDIO_LOG", _environment_value, _platform_desktop,
		"Log, each VR frame, where the sounds were heard from (the listener: its\n"
		"yaw, pitch, roll and place) against the head the frame was drawn from,\n"
		"and the game's camera." },
	{ "debug.vr_test_sound", _config_string, "\"\"", "HALO_VR_TEST_SOUND", _environment_value, _platform_desktop,
		"Play a sound of the map's at a point held in the world, for automated\n"
		"tests of the listener: \"<text> <seconds>[ <distance> <degrees>]\", the\n"
		"first sound whose tag name has the text in it, again every so many\n"
		"seconds, so many world units (2) from the player's eye (placed again\n"
		"if it is carried a tenth of a unit away), turned so many degrees left\n"
		"of the view's heading (0: ahead); empty none." },
#endif
	{ "debug.null_renderer", _config_boolean, "false", "HALO_NULL_RENDERER", _environment_set_is_true, _platform_all,
		"Run without a window, drawing nothing." },
	{ "debug.no_clipboard", _config_boolean, "false", "HALO_NO_CLIPBOARD", _environment_set_is_true, _platform_all,
		"Leave the clipboard alone (tests and tools): no invite link is put on\n"
		"it, and none found there is joined, as with debug.null_renderer,\n"
		"debug.test_input, debug.fixed_step, debug.network_test or\n"
		"debug.exit_after." },
	{ "display.prepare_shaders", _config_boolean, "true", "HALO_PREPARE_SHADERS", _environment_value, _platform_all,
		"Make a map's shaders and pipelines while it loads, so that none is made\n"
		"in play: its first frame (and each structure's) drawn first, unseen,\n"
		"through every shader its tags hold, in each way the game draws them;\n"
		"false makes each the first time a draw needs it, which the draw waits for." },
	{ "debug.load_trace", _config_boolean, "false", "HALO_LOAD_TRACE", _environment_value, _platform_all,
		"Log map loading checkpoints with monotonic wall, thread and process CPU seconds." },
	{ "debug.render_manifest", _config_string, "\"\"", "HALO_RENDER_MANIFEST", _environment_value, _platform_all,
		"Record the shaders and pipelines the game draws with into this file (each\n"
		"new one added to it), for the renderer's manifest\n"
		"(port/assets/render/README.md); empty for none." },
	{ "debug.gpu_validation", _config_boolean, "false", "HALO_GPU_VALIDATION", _environment_set_is_true, _platform_all,
		"Report the graphics API's errors in the log: Vulkan's validation layers\n"
		"(which must be installed)." },
	{ "debug.gl_debug", _config_boolean, "false", "HALO_GL_DEBUG", _environment_set_is_true, _platform_desktop,
		"Deprecated: gpu_validation's old name, which turns it on too." },
	{ "debug.skip_cinematics", _config_boolean, "false", "HALO_SKIP_CINEMATICS", _environment_set_is_true, _platform_all,
		"Skip each cinematic in a local game: as the skip button does where it can\n"
		"be skipped, else run at sixteen times its speed (a test reaches play in\n"
		"seconds)." },
	{ "debug.menu_open", _config_string, "\"\"", "HALO_MENU_OPEN", _environment_value, _platform_all,
		"Start on this screen of the menus (port/assets/menus) instead of the main\n"
		"menu, a player profile being edited; empty for the main menu." },
	{ "debug.gpu_stats", _config_boolean, "false", "HALO_GPU_STATS", _environment_set_is_true, _platform_all,
		"Log the renderer's draw counts once a second (with Vulkan, also its\n"
		"submissions, passes, clears and pipelines every 600 frames)." },
	{ "debug.gpu_trace_frame", _config_integer, "-1", "HALO_GPU_TRACE", _environment_value, _platform_all,
		"Log every draw of this frame; -1 none." },
	{ "debug.gpu_trace_constants", _config_boolean, "false", "HALO_GPU_TRACE_CONSTANTS", _environment_set_is_true, _platform_all,
		"With gpu_trace_frame, also the vertex shader constants." },
	{ "debug.gpu_skip_vertex_shaders", _config_string, "\"\"", "HALO_GPU_SKIP_VS", _environment_value, _platform_all,
		"Comma-separated ids of vertex shaders not to draw with." },
	{ "debug.gpu_dump_shaders", _config_string, "\"\"", "HALO_GPU_DUMP_SHADERS", _environment_value, _platform_all,
		"A folder to write the generated shaders to (Vulkan's SPIR-V); empty\n"
		"none." },
	{ "debug.dynamic_lights_look", _config_string, "\"\"", "HALO_DYNAMIC_LIGHTS_LOOK", _environment_value, _platform_all,
		"The enhanced dynamic lights' look, for tuning it: up to eight numbers,\n"
		"the lights' brightness, core, spill past white and wrap on models, the\n"
		"glow's size (of the light's radius), brightness, softness (world\n"
		"units) and least size; empty: the defaults." },
	{ "debug.dynamic_lights_log", _config_boolean, "false", "HALO_DYNAMIC_LIGHTS_LOG", _environment_value, _platform_all,
		"Log where the first projectile's light is drawn each frame (the\n"
		"enhanced dynamic lights)." },
	{ "debug.gpu_debug_expression", _config_string, "\"\"", "HALO_GPU_DEBUG_EXPR", _environment_value, _platform_all,
		"Metal only, not Vulkan's SPIR-V: an expression (MSL) every pixel shader\n"
		"shows instead of its result." },
	{ "debug.gpu_debug_texture0", _config_boolean, "false", "HALO_GPU_DEBUG_T0", _environment_set_is_true, _platform_all,
		"Pixel shaders show their first texture." },
	{ "debug.gpu_debug_flat", _config_boolean, "false", "HALO_GPU_DEBUG_FLAT", _environment_set_is_true, _platform_all,
		"Pixel shaders show their vertex colour." },
	{ "debug.screenshot_directory", _config_string, "\"\"", "HALO_SCREENSHOT_DIR", _environment_value, _platform_all,
		"A folder to save frames to (with screenshot_every); empty none." },
	{ "debug.screenshot_every", _config_integer, "0", "HALO_SCREENSHOT_EVERY", _environment_value, _platform_all,
		"Save every this many frames to screenshot_directory; 0 none." },
	{ "debug.screenshot_stream", _config_string, "\"\"", "HALO_SCREENSHOT_STREAM", _environment_value, _platform_all,
		"With screenshot_every, a file (a pipe an encoder reads) each frame's\n"
		"pixels go to instead of screenshot_directory's BMPs: BGRA, the top row\n"
		"first, no header; empty none." },
	{ "debug.screenshot_from", _config_real, "0.0", "HALO_SCREENSHOT_FROM", _environment_value, _platform_all,
		"With screenshot_every, save from this many seconds of the scenario's\n"
		"simulation (debug.trace's clock); 0 from the first frame." },
	{ "debug.screenshot_until", _config_real, "0.0", "HALO_SCREENSHOT_UNTIL", _environment_value, _platform_all,
		"With screenshot_every, save until this many seconds of the scenario's\n"
		"simulation; 0 to the last frame." },
	{ "debug.texture_dump_directory", _config_string, "\"\"", "HALO_TEXTURE_DUMP", _environment_value, _platform_all,
		"A folder to write every texture to as it is uploaded; empty none." },
	{ "debug.sample_seconds", _config_real, "0.0", "HALO_SAMPLE", _environment_value, _platform_android | _platform_guest,
		"Log where every game thread is this often, in seconds (read by the\n"
		"host, port/android/host/host_debug.c); 0 never." },
	{ "debug.memory_watch", _config_boolean, "true", "HALO_MEMORY_WATCH", _environment_value, _platform_android | _platform_guest,
		"Notice the game's writes to cached textures and vertices by page\n"
		"protection; false compares page contents once a frame instead, which is\n"
		"slower. Under ARM translation (the x86 emulator) the app always compares\n"
		"contents. Read by the host from the file (port/android/host/host_common.c)." },
#ifdef HALO_PROFILE
	{ "debug.profile_record", _config_boolean, "false", "HALO_PROFILE_RECORD", _environment_value, _platform_all,
		"Record a profile with no command (configure.py --profile builds): from\n"
		"when profile_record_when says until profile_stop, a map change or the\n"
		"end, into numbered part files in the data folder's profiles folder\n"
		"(tools/net_report.py reads it)." },
	{ "debug.profile_record_when", _config_string, "\"start\"", "HALO_PROFILE_RECORD_WHEN", _environment_value,
		_platform_all,
		"\"start\": from the first frame until the first map change; \"game\":\n"
		"each game that is not the main menu, a recording each, until its map\n"
		"goes." },
	{ "debug.profile_memory", _config_integer, "256", "HALO_PROFILE_MEMORY", _environment_value, _platform_all,
		"Megabytes a recording keeps in memory, 4 to 1024: two halves, each\n"
		"written out as a part when it fills." },
#endif
};

#define NUMBER_OF_CONFIG_SETTINGS (sizeof(config_settings) / sizeof(config_settings[0]))

#ifdef HALO_ANDROID
#define CONFIG_PLATFORM _platform_android
#elif defined(HALO_WINDOWS)
/* (the Windows build: a guest, tools/windows_build.py) */
#define CONFIG_PLATFORM (_platform_desktop | _platform_windows | _platform_guest)
#elif defined(HALO_IOS)
#define CONFIG_PLATFORM (_platform_desktop | _platform_guest | _platform_ios)
#elif defined(HALO_GUEST)
#define CONFIG_PLATFORM (_platform_desktop | _platform_guest)
#else
#define CONFIG_PLATFORM _platform_desktop
#endif

struct config_value
{
	int boolean;
	long integer;
	double real;
	char *string;
};

static struct config_value config_values[NUMBER_OF_CONFIG_SETTINGS];
/* whether the player has set each: in the file (not commented out), by its
variable or by config_write; display.graphics chooses the others it covers
(config_graphics_apply) */
static unsigned char config_set[NUMBER_OF_CONFIG_SETTINGS];
static int config_loaded = 0;
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- the file */

static void config_path(char *path, size_t size)
{
#ifdef HALO_ANDROID
	/* the data folder, which the app names (port/android/host/host_main.c) */
	const char *root = getenv("HALO_DATA_ROOT");

	snprintf(path, size, "%s/config.toml", root && *root ? root : ".");
#elif defined(HALO_WINDOWS)
	/* the executable's folder, with its separator */
	const char *base = SDL_GetBasePath();

	snprintf(path, size, "%sconfig.toml", base ? base : "");
#elif defined(HALO_MACOS)
	/* the macOS app's settings folder in Application Support, as the host
	gives it for SDL's base path (port/linux/host/host_desktop.c: its bundle
	is read-only), or the executable's folder */
	const char *base = SDL_GetBasePath();

	snprintf(path, size, "%sconfig.toml", base ? base : "");
#else
	/* the game's folder (the release's root, or the executable's folder),
	or ~/.config/halo-linux when the game is installed (install_layout.c) */
	snprintf(path, size, "%sconfig.toml", install_config_folder());
#endif
}

/* the whole file, NUL terminated, or NULL; free() it */
static char *config_read_file(const char *path, size_t *size)
{
#ifdef HALO_GUEST
	FILE *file = fopen(path, "rb");
	char *text = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		text = malloc((size_t)length + 1);
		if (text && fread(text, 1, (size_t)length, file) == (size_t)length)
		{
			text[length] = 0;
			*size = (size_t)length;
		}
		else
		{
			free(text);
			text = NULL;
		}
	}
	fclose(file);
	return text;
#else
	/* SDL's, for UTF-8 paths on Windows */
	void *data = SDL_LoadFile(path, size);
	char *text;

	if (!data)
		return NULL;
	text = malloc(*size + 1);
	if (text)
	{
		memcpy(text, data, *size);
		text[*size] = 0;
	}
	SDL_free(data);
	return text;
#endif
}

static int config_write_file(const char *path, const char *text)
{
#ifdef HALO_GUEST
	FILE *file = fopen(path, "wb");
	int written;

	if (!file)
		return 0;
	written = fwrite(text, 1, strlen(text), file) == strlen(text);
	return fclose(file) == 0 && written;
#else
	return SDL_SaveFile(path, text, strlen(text));
#endif
}

/* in the file's head: a setting the player has not set is written
commented out at its default, so that a newer version's default reaches it
(config_append_setting); a file without this note was written by an older
version, which wrote them as values (config_migrate) */
#define CONFIG_DEFAULTS_MARK "is at its default"
#define CONFIG_DEFAULTS_NOTE \
	"# A setting commented out (\"# key = value\") " CONFIG_DEFAULTS_MARK ", which a\n" \
	"# newer version may change: take the \"# \" out to keep a value of your own.\n"

/* the defaults the older versions wrote into the file as values, which a
file without CONFIG_DEFAULTS_NOTE has though the player never chose them:
there, they become the setting's commented default (config_migrate) */
static const struct
{
	const char *name;
	const char *old_default;
} config_old_defaults[] =
{
#ifdef HALO_VR
	/* (before "arms" was the default) */
	{ "vr.hands", "\"floating\"" },
#endif
	{ NULL, NULL },
};

struct config_text
{
	char *buffer;
	size_t length, capacity;
};

static void config_append(struct config_text *text, const char *string)
{
	size_t length = strlen(string);

	if (text->length + length + 1 > text->capacity)
	{
		size_t capacity = (text->capacity ? text->capacity : 4096) * 2 + length;
		char *buffer = realloc(text->buffer, capacity);

		if (!buffer)
			return;
		text->buffer = buffer;
		text->capacity = capacity;
	}
	memcpy(text->buffer + text->length, string, length + 1);
	text->length += length;
}

/* the first length characters of text, as a string of their own */
static char *config_copy(const char *text, size_t length)
{
	char *copy = malloc(length + 1);

	if (copy)
	{
		memcpy(copy, text, length);
		copy[length] = 0;
	}
	return copy;
}

/* one setting as the file holds it: its comment, and its key at the
default, commented out (CONFIG_DEFAULTS_NOTE) */
static void config_append_setting(struct config_text *text, const struct config_setting *setting)
{
	const char *dot = strchr(setting->name, '.');
	const char *line;
	char buffer[256];

	config_append(text, "\n");
	for (line = setting->comment; *line;)
	{
		size_t length = strcspn(line, "\n");

		snprintf(buffer, sizeof(buffer), "# %.*s\n", (int)length, line);
		config_append(text, buffer);
		line += length;
		if (*line)
			line++;
	}
#ifndef HALO_ANDROID
	/* (Android apps have no environment to set; a setting without a variable
	names none) */
	if (setting->environment)
	switch (setting->environment_style)
	{
	case _environment_value:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=<value>)\n", setting->environment);
		break;
	case _environment_set_is_true:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=1 makes it true)\n", setting->environment);
		break;
	case _environment_set_is_false:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=1 makes it false)\n", setting->environment);
		break;
	}
	config_append(text, buffer);
#endif
	snprintf(buffer, sizeof(buffer), "# %s = %s\n", dot + 1, setting->default_value);
	config_append(text, buffer);
}

/* the file with every setting of this build at its default */
static char *config_default_text(void)
{
	struct config_text text = { NULL, 0, 0 };
	char section[32] = "";
	size_t index;

#ifdef HALO_ANDROID
	config_append(&text,
		"# Halo settings\n"
		"#\n"
		"# The game writes this file with the defaults when it is missing: delete\n"
		"# it to go back to them.\n");
#else
	config_append(&text,
		"# Halo settings\n"
		"#\n"
		"# The game writes this file with the defaults when it is missing: delete\n"
		"# it to go back to them. Each setting can also be set for one run with\n"
		"# the environment variable named with it, which wins over this file.\n");
#endif
	config_append(&text, "#\n" CONFIG_DEFAULTS_NOTE);
	/* each section once, where its first setting is, with all of its
	settings: TOML refuses a section twice, and the whole file would go
	unread (config_join_sections) */
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		size_t length, other;
		char buffer[64];

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot)
			continue;
		length = (size_t)(dot - setting->name);
		for (other = 0; other < index; other++)
		{
			if ((config_settings[other].platforms & CONFIG_PLATFORM) &&
				!strncmp(config_settings[other].name, setting->name, length + 1))
				break;
		}
		if (other < index)
			continue;
		snprintf(section, sizeof(section), "%.*s", (int)length, setting->name);
		snprintf(buffer, sizeof(buffer), "\n[%s]\n", section);
		config_append(&text, buffer);
		for (other = index; other < NUMBER_OF_CONFIG_SETTINGS; other++)
		{
			if ((config_settings[other].platforms & CONFIG_PLATFORM) &&
				!strncmp(config_settings[other].name, setting->name, length + 1))
				config_append_setting(&text, &config_settings[other]);
		}
	}
	return text.buffer;
}

static int config_line_key(const char *line, const char *end, const char *key, int commented);
static int config_line_section(const char *line, const char *end, char *section, size_t size);
static const char *config_find_line(const char *text, const char *section, const char *key, int commented);
static char *config_set_line(const char *text, const char *section, const char *key, const char *line_text);
static long config_setting_index(const char *name);

/* a file an older version wrote (without CONFIG_DEFAULTS_NOTE): the note
added to its head, and the defaults it wrote as values that have changed
since (config_old_defaults: strings) commented out at today's default.
Returns the new text, or NULL if the file has the note */
static char *config_migrate(const char *text, toml_datum_t table)
{
	struct config_text out = { NULL, 0, 0 };
	const char *head_end = strstr(text, "\n\n");
	char *result;
	size_t index;

	if (strstr(text, CONFIG_DEFAULTS_MARK))
		return NULL;
	/* (at the end of the head's comment: before its first blank line) */
	if (text[0] == '#' && head_end)
	{
		char *head = config_copy(text, (size_t)(head_end + 1 - text));

		if (head)
			config_append(&out, head);
		free(head);
		config_append(&out, "#\n" CONFIG_DEFAULTS_NOTE);
		config_append(&out, head_end + 1);
	}
	else
	{
		config_append(&out, CONFIG_DEFAULTS_NOTE "\n");
		config_append(&out, text);
	}
	result = out.buffer;
	for (index = 0; result && config_old_defaults[index].name; index++)
	{
		const char *name = config_old_defaults[index].name;
		const char *old = config_old_defaults[index].old_default;
		const char *dot = strchr(name, '.');
		long setting = config_setting_index(name);
		toml_datum_t datum = toml_seek(table, name);
		size_t length = strlen(old);
		char section[64], line_text[300];
		char *updated;

		/* (a string's: without its quotes) */
		if (setting < 0 || !dot || datum.type != TOML_STRING || length < 2 || strlen(datum.u.s) != length - 2 ||
			strncmp(datum.u.s, old + 1, length - 2))
			continue;
		snprintf(section, sizeof(section), "%.*s", (int)(dot - name), name);
		snprintf(line_text, sizeof(line_text), "# %s = %s\n", dot + 1, config_settings[setting].default_value);
		updated = config_set_line(result, section, dot + 1, line_text);
		if (!updated)
			continue;
		free(result);
		result = updated;
		platform_log("settings: %s = %s was an older version's default: now this version's, %s", name, old,
			config_settings[setting].default_value);
	}
	return result;
}

/* whether the line (after spaces) is a commented default, "# key = ..." */
static int config_line_commented_default(const char *line, const char *end)
{
	const char *key;

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	if (line >= end || *line != '#')
		return 0;
	for (line++; line < end && (*line == ' ' || *line == '\t'); line++)
	{
	}
	for (key = line; line < end && (isalnum((unsigned char)*line) || *line == '_'); line++)
	{
	}
	if (line == key)
		return 0;
	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	return line < end && *line == '=';
}

/* text with the setting's block (the comment lines just above its line,
and its line or its commented default) in place of the one it has: its
comment as this version writes it, and value (NULL: its commented
default). NULL if the file has not the setting, or out of memory */
static char *config_replace_setting(const char *text, const struct config_setting *setting, const char *value)
{
	const char *dot = strchr(setting->name, '.');
	struct config_text block = { NULL, 0, 0 };
	struct config_text out = { NULL, 0, 0 };
	const char *line, *start, *end;
	char section[64];
	char *before;

	if (!dot)
		return NULL;
	snprintf(section, sizeof(section), "%.*s", (int)(dot - setting->name), setting->name);
	line = config_find_line(text, section, dot + 1, 0);
	if (!line)
		line = config_find_line(text, section, dot + 1, 1);
	if (!line)
		return NULL;
	end = line + strcspn(line, "\n");
	if (*end)
		end++;
	/* (back over the comment above it, to a blank line, a section, a
	value or another setting's commented default) */
	for (start = line; start > text;)
	{
		const char *previous_end = start - 1;
		const char *previous = previous_end;
		const char *first;

		while (previous > text && previous[-1] != '\n')
			previous--;
		for (first = previous; first < previous_end && (*first == ' ' || *first == '\t'); first++)
		{
		}
		if (first >= previous_end || *first != '#' || config_line_commented_default(previous, previous_end))
			break;
		start = previous;
	}
	config_append_setting(&block, setting);
	if (!block.buffer)
		return NULL;
	if (value)
	{
		/* (its commented default's line, the last, made its value) */
		char *last;

		block.buffer[--block.length] = 0;
		last = strrchr(block.buffer, '\n');
		if (last)
		{
			char line_text[300];

			block.length = (size_t)(last + 1 - block.buffer);
			block.buffer[block.length] = 0;
			snprintf(line_text, sizeof(line_text), "%s = %s\n", dot + 1, value);
			config_append(&block, line_text);
		}
	}
	before = config_copy(text, (size_t)(start - text));
	if (before)
		config_append(&out, before);
	free(before);
	/* (without the blank line config_append_setting starts with) */
	config_append(&out, block.buffer + 1);
	config_append(&out, end);
	free(block.buffer);
	return out.buffer;
}

/* a number as config.toml has a real: at most four decimals, and a point */
static void config_real_text(double number, char *text, size_t size)
{
	size_t length;

	snprintf(text, size, "%.4f", number);
	length = strlen(text);
	while (length > 2 && text[length - 1] == '0' && text[length - 2] != '.')
		text[--length] = 0;
}

/* a file whose mouse settings are the older versions' (without
CONFIG_MOUSE_MARK): input.mouse_sensitivity, a multiplier of 0.0022
radians a count, in CS2's units (times CONFIG_MOUSE_FROM_OLD), and
input.mouse_vertical_sensitivity, the same scale as it (0: the same), made
a multiplier of it; each with this version's comment, which has the mark,
so that it is done once. A setting the player had not set stays at its
default (5.73, the old default's turn). Returns the new text, or NULL */
static char *config_migrate_mouse(const char *text, toml_datum_t table)
{
	long horizontal_index = config_setting_index("input.mouse_sensitivity");
	long vertical_index = config_setting_index("input.mouse_vertical_sensitivity");
	toml_datum_t horizontal = toml_seek(table, "input.mouse_sensitivity");
	toml_datum_t vertical = toml_seek(table, "input.mouse_vertical_sensitivity");
	double old_horizontal = 1.0, old_vertical = 0.0;
	int horizontal_set = 0, vertical_set = 0;
	char value[64];
	char *result, *updated;

	if (strstr(text, CONFIG_MOUSE_MARK) || horizontal_index < 0 || vertical_index < 0 ||
		!(config_settings[horizontal_index].platforms & CONFIG_PLATFORM))
		return NULL;
	if (horizontal.type == TOML_FP64 || horizontal.type == TOML_INT64)
	{
		old_horizontal = horizontal.type == TOML_FP64 ? horizontal.u.fp64 : (double)horizontal.u.int64;
		horizontal_set = old_horizontal > 0.0;
		if (!horizontal_set)
			old_horizontal = 1.0;
	}
	if (vertical.type == TOML_FP64 || vertical.type == TOML_INT64)
	{
		old_vertical = vertical.type == TOML_FP64 ? vertical.u.fp64 : (double)vertical.u.int64;
		vertical_set = old_vertical > 0.0;
	}
	config_real_text(old_horizontal * CONFIG_MOUSE_FROM_OLD, value, sizeof(value));
	result = config_replace_setting(text, &config_settings[horizontal_index], horizontal_set ? value : NULL);
	if (result && horizontal_set)
		platform_log("settings: input.mouse_sensitivity %g is now %s, on CS2's scale (the same turn)", old_horizontal,
			value);
	config_real_text(old_vertical / old_horizontal, value, sizeof(value));
	updated = config_replace_setting(result ? result : text, &config_settings[vertical_index],
		vertical_set ? value : NULL);
	if (updated)
	{
		free(result);
		result = updated;
		if (vertical_set)
			platform_log("settings: input.mouse_vertical_sensitivity %g is now %s, times input.mouse_sensitivity",
				old_vertical, value);
	}
	return result;
}

/* the settings of this build that text (the file, parsed as table) lacks,
added to it in their sections, keeping the rest as it is: a newer version's
settings appear in an older file. Returns the new text, or NULL if nothing
was missing */
static char *config_add_missing(const char *text, toml_datum_t table)
{
	char *result = NULL;
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		const char *current = result ? result : text;
		struct config_text block = { NULL, 0, 0 };
		struct config_text updated = { NULL, 0, 0 };
		char header[40];
		const char *line;
		const char *insert = NULL;

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot || toml_seek(table, setting->name).type != TOML_UNKNOWN)
			continue;
		/* (a commented default is the setting, at its default) */
		snprintf(header, sizeof(header), "%.*s", (int)(dot - setting->name), setting->name);
		if (config_find_line(current, header, dot + 1, 1))
			continue;
		snprintf(header, sizeof(header), "[%.*s]", (int)(dot - setting->name), setting->name);
		/* the end of the section's last line that is not blank */
		for (line = current; *line; )
		{
			const char *start = line;
			size_t length = strcspn(line, "\n");

			while (*start == ' ' || *start == '\t')
				start++;
			if (insert && *start == '[')
				break;
			if (!insert && !strncmp(start, header, strlen(header)))
				insert = line + length;
			else if (insert && start < line + length && *start != '\r')
				insert = line + length;
			line += length;
			if (*line)
				line++;
		}
		if (insert)
		{
			if (*insert)
				insert++;
			config_append_setting(&block, setting);
		}
		else
		{
			/* no such section: a new one at the end */
			insert = current + strlen(current);
			config_append(&block, current[0] && insert[-1] != '\n' ? "\n\n" : "\n");
			config_append(&block, header);
			config_append(&block, "\n");
			config_append_setting(&block, setting);
		}
		if (!block.buffer)
			continue;
		{
			char *before = config_copy(current, (size_t)(insert - current));

			if (before)
				config_append(&updated, before);
			free(before);
		}
		if (insert > current && insert[-1] != '\n')
			config_append(&updated, "\n");
		config_append(&updated, block.buffer);
		config_append(&updated, insert);
		free(block.buffer);
		if (updated.buffer)
		{
			free(result);
			result = updated.buffer;
			platform_log("settings: added %s (new in this version), commented out at its default", setting->name);
		}
	}
	return result;
}

static int config_line_section(const char *line, const char *end, char *section, size_t size);

/* text from start to end, appended */
static void config_append_range(struct config_text *text, const char *start, const char *end)
{
	char *copy = config_copy(start, (size_t)(end - start));

	if (copy)
		config_append(text, copy);
	free(copy);
}

/* a file with a section twice, which TOML refuses (the defaults of the
versions from 2026-10-10 had a second [display] after [game]): each
section's later parts moved to the end of its first, the rest kept as it
is. Returns the new text, or NULL if no section is there twice */
static char *config_join_sections(const char *text)
{
	enum { PARTS_MAXIMUM = 64 };
	struct
	{
		char name[64];
		const char *start, *end;
	} parts[PARTS_MAXIMUM];
	struct config_text joined = { NULL, 0, 0 };
	const char *line;
	const char *first_part = NULL;
	int count = 0, repeated = 0, index, other;

	/* the parts, each from its section's line to the next one's */
	for (line = text; *line; )
	{
		size_t length = strcspn(line, "\n");
		char name[64];

		if (config_line_section(line, line + length, name, sizeof(name)))
		{
			if (count == PARTS_MAXIMUM)
				return NULL;
			if (count)
				parts[count - 1].end = line;
			else
				first_part = line;
			for (other = 0; other < count; other++)
				repeated |= !strcmp(parts[other].name, name);
			strcpy(parts[count].name, name);
			parts[count].start = line;
			count++;
		}
		line += length;
		if (*line)
			line++;
	}
	if (!repeated)
		return NULL;
	parts[count - 1].end = line;

	config_append_range(&joined, text, first_part);
	for (index = 0; index < count; index++)
	{
		for (other = 0; other < index && strcmp(parts[other].name, parts[index].name); other++)
			;
		if (other < index)
			continue;
		config_append_range(&joined, parts[index].start, parts[index].end);
		for (other = index + 1; other < count; other++)
		{
			const char *body;

			if (strcmp(parts[other].name, parts[index].name))
				continue;
			/* (its settings, without its section's line) */
			body = memchr(parts[other].start, '\n', (size_t)(parts[other].end - parts[other].start));
			if (!body)
				continue;
			if (joined.length && joined.buffer[joined.length - 1] != '\n')
				config_append(&joined, "\n");
			config_append_range(&joined, body + 1, parts[other].end);
		}
	}
	return joined.buffer;
}

/* ---------- values */

static int config_text_is_false(const char *text)
{
	char lower[8];
	size_t index;

	for (index = 0; index + 1 < sizeof(lower) && text[index]; index++)
		lower[index] = (char)tolower((unsigned char)text[index]);
	lower[index] = 0;
	return !strcmp(lower, "0") || !strcmp(lower, "false") || !strcmp(lower, "no") || !strcmp(lower, "off");
}

/* whether a variable that only has to be set (_environment_set_is_true or
_environment_set_is_false) is: empty, "0", "false", "no" or "off" is not */
static int config_environment_set(const char *text)
{
	return text[0] && !config_text_is_false(text);
}

static void config_set_from_text(struct config_value *value, enum config_type type, const char *text)
{
	switch (type)
	{
	case _config_boolean:
		value->boolean = !config_text_is_false(text);
		break;
	case _config_integer:
		value->integer = strtol(text, NULL, 10);
		break;
	case _config_real:
		value->real = strtod(text, NULL);
		break;
	case _config_string:
		/* (the old string is kept, not freed: config_string's callers hold
		its pointer, and Settings writes few, seldom) */
		value->string = strdup(text);
		break;
	}
}

/* a value from its text as the table above writes it (a string's quoted,
without escapes) */
static void config_set_from_table_text(struct config_value *value, enum config_type type, const char *text)
{
	size_t length = strlen(text);

	if (type == _config_string && length >= 2 && text[0] == '"')
		value->string = config_copy(text + 1, length - 2);
	else
		config_set_from_text(value, type, text);
}

/* the value in the file, if it is there and of the setting's type: 1 if it
was */
static int config_set_from_file(struct config_value *value, const struct config_setting *setting,
	toml_datum_t table)
{
	toml_datum_t datum = toml_seek(table, setting->name);
	int wrong_type = 0;

	if (datum.type == TOML_UNKNOWN)
		return 0;
	switch (setting->type)
	{
	case _config_boolean:
		if (datum.type == TOML_BOOLEAN)
			value->boolean = datum.u.boolean;
		else
			wrong_type = 1;
		break;
	case _config_integer:
		if (datum.type == TOML_INT64)
			value->integer = (long)datum.u.int64;
		else
			wrong_type = 1;
		break;
	case _config_real:
		if (datum.type == TOML_FP64)
			value->real = datum.u.fp64;
		else if (datum.type == TOML_INT64)
			value->real = (double)datum.u.int64;
		else
			wrong_type = 1;
		break;
	case _config_string:
		if (datum.type == TOML_STRING)
		{
			free(value->string);
			value->string = strdup(datum.u.s);
		}
		else
		{
			wrong_type = 1;
		}
		break;
	}
	if (wrong_type)
	{
		static const char *const expected[] = { "true or false", "a whole number", "a number", "a quoted string" };

		platform_log("config.toml line %d: %s should be %s; using %s", datum.lineno, setting->name,
			expected[setting->type], setting->default_value);
	}
	return !wrong_type;
}

/* how many times a setting has been written (config_write): readers that
keep a setting watch this, to read it again */
static volatile unsigned long config_change_count;

static long config_setting_index(const char *name)
{
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		if (!strcmp(config_settings[index].name, name))
			return (long)index;
	}
	return -1;
}

/* ---------- display.graphics

The settings display.graphics chooses, each at its value in "classic" (as
the Xbox drew) or "enhanced" (its default, GRAPHICS_*: this build's) where
the player has not set it; one set (config_set) wins over it. Settings
writing one at the value the preset gives it, or the preset at the value of
one set, leaves it to the preset again (config_write), so that choosing
Classic or Enhanced there chooses them all. Only the drawing changes. */
static const struct
{
	const char *name;
	/* as the table above writes it */
	const char *classic;
} config_graphics[] =
{
	{ "display.anti_aliasing", "\"off\"" },
	{ "display.anisotropic_filtering", "0" },
	{ "display.shadow_resolution", "128" },
	{ "display.per_pixel_lighting", "false" },
	/* ("native" in VR, whose eyes' many lines the Xbox's 480 stand for) */
	{ "display.model_detail", "\"auto\"" },
	{ "display.dynamic_lights", "\"classic\"" },
};

#define NUMBER_OF_CONFIG_GRAPHICS (sizeof(config_graphics) / sizeof(config_graphics[0]))

/* the setting's entry in config_graphics, or -1 */
static long config_graphics_entry(long setting)
{
	size_t entry;

	for (entry = 0; setting >= 0 && entry < NUMBER_OF_CONFIG_GRAPHICS; entry++)
	{
		if (!strcmp(config_graphics[entry].name, config_settings[setting].name))
			return (long)entry;
	}
	return -1;
}

/* the value the preset (display.graphics's value: "classic", else
"enhanced") gives the setting, as the table above writes it */
static const char *config_graphics_text(const char *graphics, long setting)
{
	long entry = config_graphics_entry(setting);

	return entry >= 0 && graphics && !strcmp(graphics, "classic") ? config_graphics[entry].classic :
		config_settings[setting].default_value;
}

static int config_values_equal(const struct config_value *a, const struct config_value *b, enum config_type type)
{
	switch (type)
	{
	case _config_boolean:
		return !a->boolean == !b->boolean;
	case _config_integer:
		return a->integer == b->integer;
	case _config_real:
		return a->real == b->real;
	case _config_string:
		return a->string && b->string && !strcmp(a->string, b->string);
	}
	return 0;
}

/* whether the setting's value is the one display.graphics gives it */
static int config_graphics_gives(long setting)
{
	struct config_value preset = { 0, 0, 0.0, NULL };
	int equal;

	config_set_from_table_text(&preset, config_settings[setting].type,
		config_graphics_text(config_values[config_setting_index("display.graphics")].string, setting));
	equal = config_values_equal(&config_values[setting], &preset, config_settings[setting].type);
	free(preset.string);
	return equal;
}

/* the setting's line in the file, left to display.graphics: commented out
at the value it gives it ("# anisotropic_filtering = 16") */
static void config_graphics_line(long setting, char *line, size_t size)
{
	snprintf(line, size, "# %s = %s\n", strchr(config_settings[setting].name, '.') + 1,
		config_graphics_text(config_values[config_setting_index("display.graphics")].string, setting));
}

/* the settings display.graphics chooses that the player has not set, at its
values (the strings before are kept, as config_set_from_text keeps them) */
static void config_graphics_apply(void)
{
	const char *graphics = config_values[config_setting_index("display.graphics")].string;
	size_t entry;

	for (entry = 0; entry < NUMBER_OF_CONFIG_GRAPHICS; entry++)
	{
		long setting = config_setting_index(config_graphics[entry].name);

		if (setting >= 0 && !config_set[setting])
			config_set_from_table_text(&config_values[setting], config_settings[setting].type,
				config_graphics_text(graphics, setting));
	}
}

/* ---------- battery saver (display.battery_saver: battery_saver.c)

While battery saver is on, the settings display.graphics chooses that the
player has not set are read (config_boolean, config_integer, config_string)
at their "classic" values, whatever display.graphics says; config_text,
which Settings shows and writes from, keeps the player's own, so that the
file and the menus are as they were when it goes off again. */
static int config_battery_saving;
static struct config_value config_battery_values[NUMBER_OF_CONFIG_GRAPHICS];

/* (config_load: the values read while battery saver is on) */
static void config_battery_values_set(void)
{
	size_t entry;

	for (entry = 0; entry < NUMBER_OF_CONFIG_GRAPHICS; entry++)
	{
		long setting = config_setting_index(config_graphics[entry].name);

		if (setting >= 0)
			config_set_from_table_text(&config_battery_values[entry], config_settings[setting].type,
				config_graphics[entry].classic);
	}
}

void config_battery_saver_set(int saving)
{
	pthread_mutex_lock(&config_lock);
	if (!config_battery_saving != !saving)
	{
		config_battery_saving = saving != 0;
		/* (what keeps one of them reads it again) */
		config_change_count++;
	}
	pthread_mutex_unlock(&config_lock);
}

/* a key in the file that is no setting, likely misspelt or in the wrong
section: said in the log, with the section of a setting of that name */
static void config_report_unknown_key(int line, const char *name)
{
	const char *key = strrchr(name, '.');
	const char *found = NULL;
	size_t index, count = 0;

	key = key ? key + 1 : name;
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const char *dot = strchr(config_settings[index].name, '.');

		if (dot && !strcmp(dot + 1, key))
		{
			found = config_settings[index].name;
			count++;
		}
	}
	/* (only a name one setting has: the section is then sure) */
	if (count == 1)
	{
		platform_log("config.toml line %d: unknown setting %s, ignored (%s goes under [%.*s])", line, name, key,
			(int)(strchr(found, '.') - found), found);
	}
	else
	{
		platform_log("config.toml line %d: unknown setting %s, ignored", line, name);
	}
}

/* keys in the file that are no setting, likely misspelt */
static void config_report_unknown_keys(toml_datum_t table)
{
	int section_index;

	for (section_index = 0; section_index < table.u.tab.size; section_index++)
	{
		toml_datum_t section = table.u.tab.value[section_index];
		int key_index;

		if (section.type != TOML_TABLE)
		{
			config_report_unknown_key(section.lineno, table.u.tab.key[section_index]);
			continue;
		}
		for (key_index = 0; key_index < section.u.tab.size; key_index++)
		{
			char name[128];

			snprintf(name, sizeof(name), "%s.%s", table.u.tab.key[section_index], section.u.tab.key[key_index]);
			if (config_setting_index(name) < 0)
				config_report_unknown_key(section.u.tab.value[key_index].lineno, name);
		}
	}
}

/* the most lines of a file that cannot be read that are skipped to read
the rest */
#define CONFIG_MAXIMUM_SKIPPED_LINES 16

/* the line a parse error is on ("(line 12) ..."), or 0 */
static int config_error_line(const char *message)
{
	const char *at = strstr(message, "(line ");
	int line = 0;

	if (!at || sscanf(at, "(line %d)", &line) != 1)
		return 0;
	return line;
}

/* whether length bytes of text are word, in any case (ASCII) */
static int config_word_is(const char *text, size_t length, const char *word)
{
	size_t index;

	if (strlen(word) != length)
		return 0;
	for (index = 0; index < length; index++)
	{
		if (tolower((unsigned char)text[index]) != word[index])
			return 0;
	}
	return 1;
}

/* the line of text that cannot be read, said in the log with what likely
makes it so, and emptied (its number kept for the lines after it): returns
whether it was there */
static int config_skip_line(char *text, int line_number, const char *message)
{
	char *line = text, *end, *value, *value_end;
	const char *reason = strchr(message, ')');
	char key[64] = "";
	char hint[160] = "";
	int number;

	for (number = 1; number < line_number && *line; number++)
		line += strcspn(line, "\n") + (line[strcspn(line, "\n")] ? 1 : 0);
	if (line_number < 1 || number != line_number || !*line)
		return 0;
	end = line + strcspn(line, "\n");
	reason = reason ? reason + 1 : message;
	while (*reason == ' ')
		reason++;
	/* "key = value": the value as written, to the end of the line or a
	comment */
	value = memchr(line, '=', (size_t)(end - line));
	if (value)
	{
		char *key_start = line, *key_end = value;

		while (key_start < key_end && (*key_start == ' ' || *key_start == '\t'))
			key_start++;
		while (key_end > key_start && (key_end[-1] == ' ' || key_end[-1] == '\t'))
			key_end--;
		snprintf(key, sizeof(key), "%.*s", (int)(key_end - key_start), key_start);
		value++;
		while (value < end && (*value == ' ' || *value == '\t'))
			value++;
		value_end = value + strcspn(value, "#\n");
		if (value_end > end)
			value_end = end;
		while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t' || value_end[-1] == '\r'))
			value_end--;
		if (config_word_is(value, (size_t)(value_end - value), "true") ||
			config_word_is(value, (size_t)(value_end - value), "false"))
		{
			snprintf(hint, sizeof(hint), ": true and false are lowercase, %s = %.*s", key, (int)(value_end - value),
				value_end - value == 4 ? "true" : "false");
		}
		else if (value < value_end && isalpha((unsigned char)*value))
		{
			snprintf(hint, sizeof(hint), ": text needs quotes, %s = \"%.*s\"", key, (int)(value_end - value), value);
		}
	}
	platform_log("config.toml line %d cannot be read (%s), so it is skipped%s", line_number, reason, hint);
	memset(line, ' ', (size_t)(end - line));
	return 1;
}

#ifndef HALO_ANDROID
/* a config.toml the game never reads, likely edited in its place: said in
the log, with the one it reads (path) */
static void config_report_unread(const char *path)
{
	char other[1100];
#ifndef HALO_WINDOWS
	/* in the folder the game was started from, when that is not the
	executable's (the game's is beside the executable) */
	char folder[1024];

	if (getcwd(folder, sizeof(folder)))
	{
		snprintf(other, sizeof(other), "%s/config.toml", folder);
		if (strcmp(other, path) && SDL_GetPathInfo(other, NULL))
		{
			SDL_PathInfo mine, theirs;

			/* (the same file by another name is the one read) */
			if (!SDL_GetPathInfo(path, &mine) || !SDL_GetPathInfo(other, &theirs) ||
				mine.size != theirs.size || mine.modify_time != theirs.modify_time)
			{
				platform_log("settings: %s is not read; the game's settings are %s", other, path);
			}
		}
	}
#endif
	/* the file saved with .txt after it, by an editor that adds it (and
	Windows Explorer hides it) */
	snprintf(other, sizeof(other), "%s.txt", path);
	if (SDL_GetPathInfo(other, NULL))
	{
#ifdef HALO_WINDOWS
		platform_log("settings: %s is not read: Windows saved it as config.toml.txt; rename it to config.toml", other);
#else
		platform_log("settings: %s is not read; rename it to config.toml", other);
#endif
	}
}
#endif

/* ---------- words */

/* the settings that take one of a few words: one in any case is taken (as
written here: "Windowed" is "windowed"), and another is said once in the
log */
static const struct
{
	const char *name;
	/* NULL ended; "" where empty is one */
	const char *words[24];
	/* what the game does with another, for the log (NULL: not said) */
	const char *otherwise;
} config_words[] =
{
	{ "display.mode", { "", "fullscreen", "borderless", "windowed", NULL }, "display.fullscreen's" },
	{ "display.resolution_scaling", { "native", "original", NULL }, NULL },
	{ "display.anti_aliasing", { "off", "fxaa", "smaa", "ssaa2x", "msaa2x", "msaa4x", "msaa8x", NULL }, NULL },
	{ "display.astc_textures", { "auto", "always", "off", NULL }, NULL },
	{ "display.hud_button_style", { "classic", "gems", "menu_gems", NULL }, NULL },
	{ "display.menus", { "pc", "xbox", NULL }, NULL },
	{ "display.player_names", { "all", "allies", "enemies", "none", NULL }, NULL },
	{ "display.scoreboard_team_layout", { "teams", "score", NULL }, NULL },
	{ "display.chat_style", { "colors", "white", NULL }, NULL },
	{ "display.shield_glow_intensity", { "default", "light_show", NULL }, NULL },
	{ "display.shield_color", { "default", "white", "black", "red", "blue", "gray", "yellow", "green", "pink",
		"purple", "cyan", "cobalt", "orange", "teal", "sage", "brown", "tan", "maroon", "salmon", NULL }, NULL },
	{ "audio.resampler", { "sinc", "linear", NULL }, NULL },
	{ "audio.voice_chat", { "push_to_talk", "open_mic", "off", NULL }, NULL },
	{ "input.touch_controls", { "auto", "on", "off", NULL }, NULL },
	{ "input.button_prompts", { "auto", "controller", "keyboard", NULL }, NULL },
	{ "input.zoom_sensitivity", { "cs2", "halo", NULL }, "cs2" },
	{ "game.console_log", { "important", "all", "none", NULL }, NULL },
	{ "game.language", { "", "ja", "de", "fr", "es", "it", NULL }, NULL },
	{ "network.game_mode", { "openh1", "legacy", "standard", NULL }, NULL },
	{ "network.text_chat", { "on", "links", "off", NULL }, NULL },
	{ "network.voice_mode", { "off", "team_proximity", "team_enemy_proximity", "team_global",
		"team_global_enemy_proximity", NULL }, NULL },
	{ "network.coop_friendly_fire", { "off", "on", "shields_only", "explosives_only", NULL }, NULL },
	{ "network.shield_effects", { "stock", "glow", "light_show", "overshield", "glow_overshield",
		"light_show_overshield", "players", NULL }, NULL },
	{ "network.coop_enemies_mode", { "none", "per_player", "multiplier", NULL }, NULL },
	{ "streamer.slurs", { "off", "severe", "strong", "mild", NULL }, NULL },
	{ "streamer.swearing", { "off", "severe", "strong", "mild", NULL }, NULL },
	{ "streamer.insults", { "off", "severe", "strong", "mild", NULL }, NULL },
	{ "crash_reports.upload", { "ask", "yes", "no", NULL }, NULL },
};

#define NUMBER_OF_CONFIG_WORDS (sizeof(config_words) / sizeof(config_words[0]))

/* the word settings' values as their readers compare them (lowercase), and
the log told of one that is none of its words */
static void config_check_words(void)
{
	size_t word_index;

	for (word_index = 0; word_index < NUMBER_OF_CONFIG_WORDS; word_index++)
	{
		long index = config_setting_index(config_words[word_index].name);
		const char *const *words = config_words[word_index].words;
		struct config_value *value;
		char list[400] = "";
		size_t word;

		if (index < 0 || !(config_settings[index].platforms & CONFIG_PLATFORM))
			continue;
		value = &config_values[index];
		for (word = 0; words[word] && !config_word_is(value->string, strlen(value->string), words[word]); word++)
		{
		}
		if (words[word])
		{
			if (strcmp(value->string, words[word]))
			{
				free(value->string);
				value->string = strdup(words[word]);
			}
			continue;
		}
		/* ("a", "b" or "c", and "or empty" for "") */
		for (word = 0; words[word]; word++)
		{
			size_t length = strlen(list);

			if (!words[word][0])
				continue;
			snprintf(list + length, sizeof(list) - length, "%s\"%s\"",
				!length ? "" : !words[word + 1] && words[0][0] ? " or " : ", ", words[word]);
		}
		if (!words[0][0])
			snprintf(list + strlen(list), sizeof(list) - strlen(list), " or empty");
		platform_log("settings: %s = \"%s\" is not %s%s%s", config_words[word_index].name, value->string, list,
			config_words[word_index].otherwise ? "; using " : "",
			config_words[word_index].otherwise ? config_words[word_index].otherwise : "");
	}
}

static void config_load(void)
{
	char path[1024];
	size_t size = 0;
	char *text;
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const char *default_value = config_settings[index].default_value;

		if (config_settings[index].type == _config_string)
		{
			/* written as a TOML basic string without escapes */
			size_t length = strlen(default_value);

			config_values[index].string = length >= 2 ? config_copy(default_value + 1, length - 2) : strdup("");
		}
		else
		{
			config_set_from_text(&config_values[index], config_settings[index].type, default_value);
		}
	}

	config_path(path, sizeof(path));
#ifndef HALO_ANDROID
	config_report_unread(path);
#endif
	text = config_read_file(path, &size);
	if (text)
	{
		char *joined = config_join_sections(text);
		char *migrated;
		toml_result_t result;
		int skipped = 0;

		if (joined)
		{
			free(text);
			text = joined;
			size = strlen(text);
		}
		result = toml_parse(text, (int)size);
		/* (a line that cannot be read is skipped, in memory, so that the
		rest is) */
		while (!result.ok && skipped < CONFIG_MAXIMUM_SKIPPED_LINES &&
			config_skip_line(text, config_error_line(result.errmsg), result.errmsg))
		{
			toml_free(result);
			result = toml_parse(text, (int)strlen(text));
			skipped++;
		}
		migrated = result.ok && !skipped ? config_migrate(text, result.toptab) : NULL;
		if (migrated)
		{
			toml_free(result);
			free(text);
			text = migrated;
			result = toml_parse(text, (int)strlen(text));
		}
		/* (the mouse's settings in CS2's units, once) */
		if (result.ok && !skipped)
		{
			char *mouse = config_migrate_mouse(text, result.toptab);

			if (mouse)
			{
				toml_free(result);
				free(text);
				text = mouse;
				result = toml_parse(text, (int)strlen(text));
				migrated = text;
			}
		}
		if (result.ok)
		{
			char *completed;

			if (joined && !skipped)
			{
				if (config_write_file(path, text))
					platform_log("settings: joined the sections %s had twice", path);
				else
					platform_log("settings: cannot write %s", path);
			}
			for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
				config_set[index] = (unsigned char)config_set_from_file(&config_values[index], &config_settings[index],
					result.toptab);
			config_report_unknown_keys(result.toptab);
			platform_log("settings: %s", path);
			/* (a file with lines skipped is left as the player wrote it, to
			be fixed) */
			if (skipped)
			{
				platform_log("settings: %d line%s of config.toml skipped; every other setting in it is used", skipped,
					skipped == 1 ? "" : "s");
			}
			else
			{
				completed = config_add_missing(text, result.toptab);
				if ((completed || migrated) && !config_write_file(path, completed ? completed : text))
					platform_log("settings: cannot write %s", path);
				free(completed);
			}
		}
		else
		{
			platform_log("config.toml cannot be read (%s): every setting is at its default until that is fixed "
				"(a word value needs quotes: menus = \"pc\")", result.errmsg);
		}
		toml_free(result);
		free(text);
	}
	else
	{
		char *defaults = config_default_text();

		if (defaults && config_write_file(path, defaults))
			platform_log("settings: wrote the defaults to %s", path);
		else
			platform_log("settings: cannot write %s; using the defaults", path);
		free(defaults);
	}

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *environment = setting->environment ? getenv(setting->environment) : NULL;

		if (!environment || (setting->environment_style != _environment_value && !config_environment_set(environment)))
			continue;
		switch (setting->environment_style)
		{
		case _environment_value:
			config_set_from_text(&config_values[index], setting->type, environment);
			break;
		case _environment_set_is_true:
			config_values[index].boolean = 1;
			break;
		case _environment_set_is_false:
			config_values[index].boolean = 0;
			break;
		}
		config_set[index] = 1;
	}

	{
		const char *graphics = config_values[config_setting_index("display.graphics")].string;

		if (strcmp(graphics, "classic") && strcmp(graphics, "enhanced"))
			platform_log("settings: display.graphics \"%s\" is unknown, so enhanced", graphics);
		config_graphics_apply();
	}
	config_battery_values_set();
	config_check_words();
}

static const struct config_value *config_value(const char *name, enum config_type type)
{
	static const struct config_value none = { 0, 0, 0.0, "" };
	long index;

	pthread_mutex_lock(&config_lock);
	if (!config_loaded)
	{
		config_load();
		config_loaded = 1;
	}
	pthread_mutex_unlock(&config_lock);
	index = config_setting_index(name);
	if (index < 0 || config_settings[index].type != type)
	{
		platform_log("settings: no %s setting %s", type == _config_string ? "string" : "such", name);
		return &none;
	}
	return &config_values[index];
}

/* a setting's value as the game reads it: battery saver's where it chooses
it (config_battery_saving) */
static const struct config_value *config_read(const char *name, enum config_type type)
{
	const struct config_value *value = config_value(name, type);
	long setting, entry;

	if (!config_battery_saving)
		return value;
	setting = config_setting_index(name);
	entry = config_graphics_entry(setting);
	return entry >= 0 && config_settings[setting].type == type && !config_set[setting] ?
		&config_battery_values[entry] : value;
}

/* ---------- writing a setting */

/* the line's key, if it is "key = ..." (after spaces), or (commented) a
commented default "# key = ..." (CONFIG_DEFAULTS_NOTE) */
static int config_line_key(const char *line, const char *end, const char *key, int commented)
{
	size_t length = strlen(key);

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	if (commented)
	{
		if (line >= end || *line != '#')
			return 0;
		line++;
		while (line < end && (*line == ' ' || *line == '\t'))
			line++;
	}
	if ((size_t)(end - line) <= length || strncmp(line, key, length) != 0)
		return 0;
	line += length;
	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	return line < end && *line == '=';
}

/* the section the line opens, if it is "[section]" (after spaces) */
static int config_line_section(const char *line, const char *end, char *section, size_t size)
{
	const char *close;

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	if (line >= end || *line != '[')
		return 0;
	close = memchr(line, ']', (size_t)(end - line));
	if (!close || (size_t)(close - line - 1) >= size)
		return 0;
	memcpy(section, line + 1, (size_t)(close - line - 1));
	section[close - line - 1] = 0;
	return 1;
}

/* the key's line in the section of text (commented: its commented
default's), or NULL */
static const char *config_find_line(const char *text, const char *section, const char *key, int commented)
{
	char current[64] = "";
	int in_section = 0;
	const char *line;

	for (line = text; *line;)
	{
		const char *end = line + strcspn(line, "\n");

		if (config_line_section(line, end, current, sizeof(current)))
			in_section = !strcmp(current, section);
		else if (in_section && config_line_key(line, end, key, commented))
			return line;
		line = *end ? end + 1 : end;
	}
	return NULL;
}

/* text with line_text (a line, its new line included) for the key in the
section: in place of the key's line, else of its commented default, else
at the end of the section (or of the file, in a new section); the rest kept
as it is. NULL if out of memory */
static char *config_set_line(const char *text, const char *section, const char *key, const char *line_text)
{
	const char *found = config_find_line(text, section, key, 0);
	struct config_text out = { NULL, 0, 0 };
	char current[64] = "";
	const char *line;
	int written = 0, in_section = 0;

	if (!found)
		found = config_find_line(text, section, key, 1);
	for (line = text; *line;)
	{
		const char *end = line + strcspn(line, "\n");
		const char *next = *end ? end + 1 : end;

		if (config_line_section(line, end, current, sizeof(current)))
		{
			/* (leaving the section without the key: it goes at its end) */
			if (in_section && !written)
			{
				config_append(&out, line_text);
				written = 1;
			}
			in_section = !strcmp(current, section);
		}
		else if (line == found)
		{
			config_append(&out, line_text);
			written = 1;
			line = next;
			continue;
		}
		{
			char *copy = config_copy(line, (size_t)(next - line));

			if (copy)
			{
				config_append(&out, copy);
				free(copy);
			}
		}
		line = next;
	}
	if (!written)
	{
		if (out.length && out.buffer[out.length - 1] != '\n')
			config_append(&out, "\n");
		if (!in_section)
		{
			char header[80];

			snprintf(header, sizeof(header), "\n[%s]\n", section);
			config_append(&out, header);
		}
		config_append(&out, line_text);
	}
	return out.buffer;
}

/* sets a setting, for now and in config.toml, from its value as text
("true", "60", "1.5", "all"): its line there is changed (or its commented
default, or added), the rest of the file kept as it is */
int config_write(const char *name, const char *value)
{
	const char *dot = strchr(name, '.');
	long index = config_setting_index(name);
	char section[64], key[64], line_text[600], path[1024];
	size_t size = 0;
	char *text, *updated;
	int succeeded;

	if (index < 0 || !dot || (size_t)(dot - name) >= sizeof(section) || strlen(value) > 256)
		return 0;
	/* (the file read first, as the other settings are) */
	config_value(name, config_settings[index].type);
	pthread_mutex_lock(&config_lock);
	config_set_from_text(&config_values[index], config_settings[index].type, value);
	snprintf(section, sizeof(section), "%.*s", (int)(dot - name), name);
	snprintf(key, sizeof(key), "%s", dot + 1);
	switch (config_settings[index].type)
	{
	case _config_boolean:
		snprintf(line_text, sizeof(line_text), "%s = %s\n", key, config_values[index].boolean ? "true" : "false");
		break;
	case _config_integer:
		snprintf(line_text, sizeof(line_text), "%s = %ld\n", key, config_values[index].integer);
		break;
	case _config_real:
		/* (with its point: TOML reads 1 as an integer) */
		snprintf(line_text, sizeof(line_text), "%s = %.15g", key, config_values[index].real);
		if (!strpbrk(line_text + strlen(key) + 3, ".en"))
			strcat(line_text, ".0");
		strcat(line_text, "\n");
		break;
	case _config_string:
	{
		char *end = line_text + snprintf(line_text, sizeof(line_text), "%s = \"", key);
		const char *character;

		for (character = config_values[index].string; *character; character++)
		{
			if (*character == '"' || *character == '\\')
				*end++ = '\\';
			*end++ = *character;
		}
		strcpy(end, "\"\n");
		break;
	}
	}
	/* (one display.graphics chooses, at the value it gives it: left to it,
	commented out) */
	config_set[index] = 1;
	if (config_graphics_entry(index) >= 0 && config_graphics_gives(index))
	{
		config_set[index] = 0;
		config_graphics_line(index, line_text, sizeof(line_text));
	}
	config_path(path, sizeof(path));
	text = config_read_file(path, &size);
	updated = config_set_line(text ? text : "", section, key, line_text);
	/* (display.graphics: those it chooses that are set at the value it now
	gives them left to it too, and the others it chooses at its values) */
	if (!strcmp(name, "display.graphics"))
	{
		size_t entry;

		for (entry = 0; entry < NUMBER_OF_CONFIG_GRAPHICS && updated; entry++)
		{
			long setting = config_setting_index(config_graphics[entry].name);
			const char *setting_dot = strchr(config_graphics[entry].name, '.');
			char *next;

			if (setting < 0 || !setting_dot || !config_set[setting] || !config_graphics_gives(setting))
				continue;
			config_graphics_line(setting, line_text, sizeof(line_text));
			snprintf(section, sizeof(section), "%.*s", (int)(setting_dot - config_graphics[entry].name),
				config_graphics[entry].name);
			next = config_set_line(updated, section, setting_dot + 1, line_text);
			if (!next)
				continue;
			free(updated);
			updated = next;
			config_set[setting] = 0;
		}
		config_graphics_apply();
	}
	succeeded = updated && config_write_file(path, updated);
	config_change_count++;
	pthread_mutex_unlock(&config_lock);
	free(updated);
	free(text);
	return succeeded;
}

int config_write_boolean(const char *name, int value)
{
	long index = config_setting_index(name);

	return index >= 0 && config_settings[index].type == _config_boolean && config_write(name, value ? "true" : "false");
}

/* a setting's value as text ("true", "60", "1.5", "all"): 0 if there is no
such setting */
int config_text(const char *name, char *text, size_t size)
{
	long index = config_setting_index(name);
	const struct config_value *value;

	if (index < 0)
		return 0;
	value = config_value(name, config_settings[index].type);
	switch (config_settings[index].type)
	{
	case _config_boolean:
		snprintf(text, size, "%s", value->boolean ? "true" : "false");
		break;
	case _config_integer:
		snprintf(text, size, "%ld", value->integer);
		break;
	case _config_real:
		snprintf(text, size, "%.15g", value->real);
		break;
	case _config_string:
		snprintf(text, size, "%s", value->string ? value->string : "");
		break;
	}
	return 1;
}

void config_folder(char *path, size_t size)
{
	char file[1024];
	char *separator;

	config_path(file, sizeof(file));
	separator = strrchr(file, '/');
#ifndef HALO_ANDROID
	if (!separator || (strrchr(file, '\\') && strrchr(file, '\\') > separator))
		separator = strrchr(file, '\\');
#endif
	if (separator)
		separator[1] = 0;
	else
		file[0] = 0;
	snprintf(path, size, "%s", file);
}

/* ---------- public code */

char *config_file_read(const char *path, size_t *size)
{
	return config_read_file(path, size);
}

unsigned long config_changes(void)
{
	return config_change_count;
}

int config_default(const char *name, char *text, size_t size)
{
	long index = config_setting_index(name);
	const char *value;
	size_t length;

	if (index < 0)
		return 0;
	value = config_settings[index].default_value;
	length = strlen(value);
	/* (a string's without its quotes: the defaults have no escapes) */
	if (config_settings[index].type == _config_string && length >= 2 && value[0] == '"')
	{
		value++;
		length -= 2;
	}
	snprintf(text, size, "%.*s", (int)length, value);
	return 1;
}

/* the name of the entry'th setting display.graphics chooses, else NULL past
the last (Settings' Quality row: menu_functions.c's graphics_preset_show) */
const char *config_graphics_setting(int entry)
{
	return entry >= 0 && (size_t)entry < NUMBER_OF_CONFIG_GRAPHICS ? config_graphics[entry].name : NULL;
}

int config_graphics_value(const char *graphics, const char *name, char *text, size_t size)
{
	long index = config_setting_index(name);
	const char *value;
	size_t length;

	if (index < 0 || config_graphics_entry(index) < 0)
		return 0;
	value = config_graphics_text(graphics, index);
	length = strlen(value);
	/* (a string's without its quotes) */
	if (config_settings[index].type == _config_string && length >= 2 && value[0] == '"')
	{
		value++;
		length -= 2;
	}
	snprintf(text, size, "%.*s", (int)length, value);
	return 1;
}

int config_boolean(const char *name)
{
	return config_read(name, _config_boolean)->boolean;
}

int halo_fixed_step(void)
{
	static int fixed_step = -1;

	if (fixed_step < 0)
		fixed_step = config_boolean("debug.fixed_step") != 0;
	return fixed_step;
}

long config_integer(const char *name)
{
	return config_read(name, _config_integer)->integer;
}

double config_real(const char *name)
{
	return config_value(name, _config_real)->real;
}

const char *config_string(const char *name)
{
	const char *string = config_read(name, _config_string)->string;

	return string ? string : "";
}
