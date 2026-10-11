# Linux

`ninja linux` compiles the game with clang for 32-bit x86 Linux. The result
is a native executable, `build/linux/halo`. The game shows its graphics with
OpenGL 4.5. It plays sound through SDL3. It accepts keyboard, mouse and
gamepad input.

The game is 32-bit code because its data (tags, cache files, saved games)
contains 32-bit pointers, as on the Xbox.

`ninja linux_arm64` builds the game for 64-bit ARM Linux. Refer to
"64-bit ARM".

## Requirements

You do not need the Xbox SDK. The declarations that the game uses are in
`port/include/xdk`.

To build:

- Python and ninja.
- clang. The option `--linux-cc` of `configure.py` selects a different
  compiler.
- The 32-bit glibc development files: `lib32-glibc` on Arch Linux,
  `gcc-multilib` and `libc6-dev-i386` on Debian and Ubuntu.
- The 32-bit SDL3: `lib32-sdl3` on Arch Linux, `libsdl3-dev:i386` on Debian
  and Ubuntu. The portable build does not need it: refer to "Portable
  build".

To start the game:

- glibc 2.29 or later (32-bit), for the builds from GitHub Actions, which
  are built against glibc 2.31. They start on SteamOS 3 (Steam Deck). They
  should also start on Debian 11, Ubuntu 20.04, Fedora 32 and later
  distributions, but this is not tested. A build that you make without
  `--portable` needs the glibc of the computer that built it, or a later
  one.
- The 32-bit OpenGL libraries (`lib32-mesa`).
- The 32-bit PipeWire or PulseAudio client libraries (`lib32-pipewire` or
  `lib32-libpulse`).
- The 32-bit X11 libraries (`lib32-libx11`, `lib32-libxext`). In a Wayland
  session, the game uses them through XWayland. SDL uses Wayland itself
  only if the 32-bit Wayland libraries are version 1.20 or later
  (`lib32-wayland`, `lib32-libxkbcommon`; Debian 11, Ubuntu 20.04 and
  Fedora 32 have older ones) and the compositor has the fifo-v1 protocol.
  On GNOME, the window then has borders only with the 32-bit libdecor
  (`lib32-libdecor`).

SteamOS has all of these. Its system is read-only, and the builds from
GitHub Actions need no package: they bring their own SDL3
(`libSDL3.so.0`, next to the executable).

### Portable build

The builds from GitHub Actions are portable builds (`--portable`; refer to
"Build options" in the main [README](../../README.md#build-options)). The
portable build starts on more systems than the build machine's:

- It is compiled and linked against the 32-bit glibc 2.31 of Debian 11, the
  glibc of the Steam Runtime 3 ("sniper"), not against the glibc of the
  build machine. An executable needs the glibc version that it was built
  against, or a later one.
- It brings SDL 3 (`libSDL3.so.0`), built from source against the same
  glibc. The executable looks for it in its own folder first (a `DT_RPATH`
  of `$ORIGIN`, which comes before `LD_LIBRARY_PATH`), so that it uses this
  SDL even when Steam sets `LD_LIBRARY_PATH`. Few distributions have a
  32-bit SDL 3. This SDL loads X11, Wayland, libdecor, PipeWire, PulseAudio
  and ALSA when it starts, so it starts with whichever of them the system
  has. It is linked only to glibc. Its license is `SDL3-LICENSE.txt`.

At the first build, `tools/linux_sysroot.py` downloads the Debian packages
(about 25 MB) and the source of SDL. Each package comes from
`archive.debian.org` or `deb.debian.org`, or else from
`snapshot.debian.org`; the source of SDL comes from GitHub or from
`libsdl.org`. It checks each download against its SHA-256 sum. The system root is in
`build/linux/third_party/sysroot`. `tools/linux_sysroot.cmake` builds SDL
against it.

To make the portable build you need, in addition to the tools above,
CMake, `pkg-config` (`pkgconf`) and `wayland-scanner` (`wayland` on Arch
Linux, `libwayland-bin` on Debian and Ubuntu). You do not need the 32-bit
SDL3. You need the 32-bit glibc only for the tests.

To see the glibc version that a build needs, enter
`readelf -V build/linux/halo | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1`.

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja linux`.

## Start the game

Enter `build/linux/halo`.

To play on a Steam Deck, unpack `halo-linux-release.zip` into a folder (in
Desktop Mode), and add `halo` to Steam as a non-Steam game ("Add a Game" in
the Games menu of Steam). The game then starts in Game Mode as well. Refer
to "Steam Deck".

The game data is the folder that contains `maps/`, from an Xbox disc image
of any version of the game. The game looks for this folder in this
sequence:

1. `paths.data` in `config.toml`.
2. The current folder.
3. The folder of the executable.
4. `assets/` in the current folder, and `assets/` in the repository that
   contains the executable.

If the game finds no data, it asks for an Xbox disc image (`.xiso` or
`.iso`). This occurs at the first start:

- Select "No" to stop the game.
- Select "Yes" to open a file picker. Select the disc image. The game copies
  `maps/` next to the executable and shows the progress.

The game writes the copy to `maps.partial`. When the copy is complete, the
game changes the name to `maps`. If the copy stops before it is complete,
the game asks for the disc image again at the next start.

### Steam Deck

Do the set-up in Desktop Mode: unpack the release, put `maps/` next to
`halo` (or start the game one time to copy it from a disc image), and add
`halo` to Steam. Then start the game from the library in Game Mode.

- Screen: the game fills the screen without borders (`display.mode` empty,
  thus borderless). The 3D view and the HUD have the shape of the screen
  (16:10) and are drawn at its resolution; the menus are at the center.
- Controls: Steam Input gives the game a virtual controller, which the game
  operates as the controller of the Xbox. With the template that Steam selects
  ("Gamepad With Joystick Trackpad"), the buttons, sticks and triggers have
  the functions of the same controls on an Xbox controller, the right
  trackpad operates as the right stick, and the back buttons (L4, L5, R4,
  R5) do nothing until you assign them in the controller settings of the
  game in Steam. With a template that makes a trackpad a mouse, the mouse
  aims in the game and moves the pointer in the menus.
- Text: names of profiles and gametypes use the keyboard of the game, which
  the controller operates. The text fields of the menus (the name and the
  password in Server Setup, and the password of a game in the server
  browser) open the keyboard of Steam. Type the text, then select Enter on
  that keyboard.
- Frame rate: the game shows one frame for each refresh of the display. It
  follows the refresh rate and the frame limit of Quick Access >
  Performance (40 to 60 Hz on the LCD model, up to 90 Hz on the OLED model).
  The world is calculated at 30 Hz at all rates. Keep `display.vsync =
  true`. Refer to "Frame rate".
- Sleep: the clocks of the game do not count the time that the Deck sleeps,
  so the game continues from where it stopped. A network game does not
  wait: the host drops a machine that it has not heard from for 15 seconds,
  and while a Deck that hosts sleeps, the other players have no host.

## Files and folders

| Xbox drive | Folder |
| --- | --- |
| `d:\` | The data root: the folder that contains `maps/`. |
| `z:\` | `z/` in the save root. This folder contains the cache (approximately 800 MB of map data) and the saved games. |
| `u:\` | `u/` in the save root. This folder contains the user data. |

The save root is `paths.saves` in `config.toml`. If that setting is empty,
the save root is `$XDG_DATA_HOME/halo-linux` (usually
`~/.local/share/halo-linux`).

The game makes the folders when it needs them. Names of files and folders
are not case-sensitive, as on the Xbox.

These files are in the data root:

| File | Contents |
| --- | --- |
| `debug.txt` | The log of the game. At start-up, the game shows the data root in the terminal. A crash writes its report (the faulting address and the calls that led to it) here as well; the `reference address` line at the top of each session places those addresses in the build. |
| `init.txt` | Console commands that the game does at start-up. For example, `map_name levels\a10\a10` starts the first campaign level. |
| `tags/` | Sound tag files that replace the sounds of the maps (`audio.loose_sounds`). |

The settings are in `config.toml` next to the executable. Refer to
"Settings". Internet play's MQTT brokers are in `brokers.txt` next to it
(`network.brokers_file`).

If the game stops because of a fatal signal, it writes the address and a
backtrace to the standard error. To find the function at the address, enter
`addr2line -e build/linux/halo <address>`.

## Controls

The keyboard and the mouse are a control scheme of their own for the player
of controller 1: each action has up to two keys or mouse buttons, which
Settings > Controls Setup (or `[controls]` in `config.toml`) changes. The
game adds the input of the first gamepad to controller 1. The other
gamepads operate controllers 2 to 4. With two or more players on this
computer (co-op, or split screen in a network game) and only one gamepad,
that gamepad is controller 2 (player 2) and the keyboard and mouse stay
controller 1. The gamepad changes controller only when none of its buttons
is held. The profile's button layout (Settings > Gamepads) is the
gamepads' only.

| Action | Keys and buttons (default) |
| --- | --- |
| move | W, A, S, D |
| aim | mouse (direct aim) |
| fire | left mouse button |
| throw a grenade | right mouse button, G |
| jump (and skip a cutscene) | space |
| crouch | left ctrl, C (C alone in the web build) |
| melee | F, mouse button 4 |
| reload | R |
| action (pick up, hold to swap weapons, enter or leave a vehicle; never reloads) | E |
| change the weapon | mouse wheel, 1 |
| change the grenade | X |
| flashlight | Q |
| zoom | Z, middle mouse button |
| show the scores (hold) | tab |
| pause menu | escape |
| screenshot | F10 |
| talk in voice chat (hold) | V |

Always: \` opens the developer console, F12 releases or captures the mouse,
F11 changes between fullscreen and window. Screenshot (default F10,
rebindable under Controls Setup > Actions, below Pause Menu; not on Android) saves a PNG of the completed
frame to `screenshots/` beside `maps/`, named `YYYY-MM-DD_HH.MM.SS.png` in
local time, and prints the filename in the console. Captures in the same
second get a numeric suffix so previous screenshots are preserved.

One movement of the mouse wheel changes the weapon one time. A second
movement after a short pause changes it again.

In the menus, the mouse moves a pointer:

- The item below the pointer gets the focus.
- A left click selects the item. On a setting with values, a click on the
  left or right half changes the value. On a button in the key of a screen
  (for example "B = Back"), a click pushes that button.
- On the on-screen keyboard (a profile's name), a left click presses the
  key below the pointer, or pushes the "B =BACK" or "A =ENTER" legend.
- A right click goes back.
- The mouse wheel moves through the items.

The keyboard also operates the menus, with keys of its own: the arrow keys
(and W, A, S, D) move, space or enter selects, escape or backspace goes
back (escape resumes the game from the pause menu), delete deletes. When the game continues, the mouse aims again. A mouse button that you hold from the menu does not fire until
you push it again.

## Menus

The menus are the PC version's (Halo Custom Edition's): its main menu and
every screen it leads to (campaign, profiles, multiplayer with its server
browser, direct IP and server setup, the gametype editor, and the profile's
settings: controls, gamepads, mouse, audio, video, network and colour),
laid out as it has them. Their pictures are the high-res redraws; the few
that are not yet (3D renders, screenshots) are drawn from the Xbox's own
menus where it has the same, else a placeholder, listed in
`port/assets/menus/NON_HANDDRAWN.md`.

The campaign is wired: Campaign, on player 1's profile (the one last used),
continues its saved game, starts a level it has reached at a difficulty (New
Game), or loads or deletes any profile's saved game (Load Game). Many of the
other functions behind the screens are not wired to this game yet
(`port/assets/menus/UNWIRED.md`): the lists they fill (profiles, maps,
servers, key bindings) are empty, and the settings they change do not
change. The screens open, close and move between each other as the PC
version's. `display.menus = "xbox"` gives the Xbox's menus, which start every
kind of game.

Multiplayer > CO-OP CAMPAIGN is the Xbox's cooperative play, which the PC
version does not have: two players on this computer play the campaign in
split screen. Player 1 is the player who chose it, on the current profile.
Player 2 then chooses a profile with their own controller (a gamepad), and
New Game's levels are those either profile has reached. Either player's
controller chooses the level and the difficulty. A co-op game does
not continue a saved game of one player.

Network games have split screen too: up to 4 players on each computer. In
the game lobby, another controller presses START to join, and the new
player's profile is chosen on the ADD PLAYER screen that opens (with any
controller). With one gamepad, choose the lobby's ADD PLAYER button first:
until then that gamepad shares controller 1 with the keyboard. Two players on one profile get different names from the host. A
player's B in the lobby leaves the game alone, and the last player of the
computer leaves it for all of them. In the game, each player's pause menu
opens on their part of the screen, and its LEAVE GAME is theirs: their part
of the screen stays until the game ends. A game under way shows its own
screen before JOIN GAME: players join there the same way, START or ADD
PLAYER then START, and JOIN GAME brings them all into the game.

In a multiplayer game, the pause menu (escape) has SETTINGS, which opens
the profile's settings while the game goes on, and for the host END GAME.
END GAME ends the game as its time limit does: the players stay, and after
the carnage report the host picks the next map and gametype (PICK GAME).
LEAVE GAME of the host still ends the game for everyone. The buttons go into
the map's own pause menu, before LEAVE GAME, so the buttons of a custom map
stay. The Xbox's pause box is drawn taller to hold them (a redraw,
`port/assets/menus/port_svg/pause`); a custom map's box keeps its size, and
what is below its list moves down. The few pictures of the settings that
come from the main menu's map are not drawn there.

In a single-player campaign, the pause menu has SETTINGS too, before REVERT
TO SAVED, in the same box: a list with room for it centres its rows, and
one without keeps its size, its rows closer. It opens the same settings,
while the game stays paused, with only Controls, Gamepads, Mouse, Audio and
Video Setup; Gamepads' OK saves the profile at once.

The menus are XML files in `port/assets/menus` (`tools/ce_menus.py` writes
them from the PC version's tags), which the game contains. To change them,
put files in a `menus` folder next to `config.toml`: a file with the same
path replaces one of the game's, and another `.xml` file is added.
`port/assets/menus/README.md` describes the files. If a file has a problem,
the game uses the Xbox's menus and the log names the file, the line and the
problem.

## Settings

The settings are in `config.toml` next to the executable
(`build/linux/config.toml`). At the first start, the game writes the file
with a comment for each setting and the setting commented out at its
default (`# vsync = true`): a commented setting follows the default, also a
newer version's. To keep a value of your own, take the `# ` out (or change
it in the Settings menu, which does that). A newer version adds its new
settings the same way. To get the default values again, delete the file.

A file written before this (without the note "A setting commented out ...
is at its default" in its head) keeps its values; the game adds the note,
and a value that was only the older versions' default and has changed
since becomes the new default, once: `vr.hands = "floating"` becomes
`# hands = "arms"`.

The game reads the file at start-up. If a key is not correct, or a value
has the wrong type, the game writes the line to the log and uses the default
value. The Settings menu (Video, Mouse, Audio, Network and Controls Setup)
changes the useful settings, writes them into the file (only their lines
change) and applies them at once, but `audio.enabled`, and `display.menus`
from the next main menu.

Each setting has an environment variable. The environment variable changes
the setting for one start of the game. It has priority over the file.

| Setting | Default | Environment variable | Function |
| --- | --- | --- | --- |
| `display.mode` | `""` | `HALO_DISPLAY_MODE` | `"fullscreen"`: the display, taken at the mode of `display.resolution` (the nearest the display has), or at its desktop mode. `"borderless"`: a window over the whole desktop, whose mode does not change. `"windowed"`: a window of `display.window_size`. Empty: `display.fullscreen` decides (`true`: borderless). F11 changes between the window and the fullscreen mode. Video Setup sets it. |
| `display.fullscreen` | `true` | `HALO_FULLSCREEN` | `true`: borderless, as `display.mode = "borderless"`. `false`: a window, as `display.mode = "windowed"`. Used when `display.mode` is empty. |
| `display.resolution` | `"native"` | `HALO_RESOLUTION` | What fullscreen and borderless draw at: `"native"`, the display's own resolution, or `"<width>x<height>"`, such as `"1920x1080"`, 640x480 or more. Fullscreen sets the display to it. Borderless draws at it and scales the picture to the display, where the display has room for it. The picture has 480 lines of the game and the width of the resolution's shape. A window draws at its own size instead. Video Setup's Resolution sets it, from the display's modes; it shows with Fullscreen and Borderless. |
| `display.resolution_scaling` | `"native"` | `HALO_RESOLUTION_SCALING` | `"native"`: the game draws at the resolution of the window, or of the display (or `display.resolution`) fullscreen. `"original"`: the game draws the 640x480 picture of the Xbox and scales it up to the window or the display, whatever `display.mode` is. Video Setup sets it. |
| `display.window_size` | `""` | `HALO_WINDOW_SIZE` | The size of the window, as `"<width>x<height>"`, such as `"1920x1080"`, 640x480 or more. You can change the size of the window; the game's picture takes its shape. Empty: `display.window_scale` decides. Video Setup's Window Size sets it, from sizes of each shape (4:3, 16:10, 16:9 and 21:9) that fit the desktop; it shows with Windowed. |
| `display.window_scale` | `2` | `HALO_WINDOW_SCALE` | Used when `display.window_size` is empty: the size of the window, as a multiple of 640x480. |
| `display.vsync` | `true` | `HALO_NO_VSYNC=1` sets `false` | `true`: each frame waits for the display. |
| `display.max_fps` | `0` | `HALO_MAX_FPS` | With vsync off, the most frames each second. `0`: twice the display's refresh rate. `-1`: no limit, which can hang some Intel graphics (Raptor Lake), resetting the desktop's graphics too. |
| `display.anti_aliasing` | `"off"` | `HALO_ANTI_ALIASING` | The smoothing of jagged edges, which the Xbox did not have. `"off"`: none, as on the Xbox. `"fxaa"` or `"smaa"`: a pass over the 3D view after the game draws it. The HUD and the menus stay sharp. SMAA is the sharper and costs more. `"ssaa2x"`: the game draws at two times the resolution in each direction (at most the GPU's largest texture), and the picture is scaled down. The GPU does four times the work. Not with `display.resolution_scaling = "original"`. `"msaa2x"`, `"msaa4x"` or `"msaa8x"`: each pixel of the 3D view has that many samples (at most the GPU's). On Android, `"smaa"` gives FXAA and `"ssaa2x"` none. A change applies from the next frame. Refer to "Anti-aliasing" in "What operates". |
| `debug.gpu_flush_draws` | `-1` | `HALO_GPU_FLUSH_DRAWS` | Flush the GPU's pipeline every this many draws. `-1`: every 3 on Intel graphics with Mesa's driver, which can otherwise hang in the game's long runs of small draws and reset the desktop's graphics too. `0`: never. |
| `display.interpolation` | `true` | `HALO_INTERPOLATION` | `true`: one frame for each refresh of the display. `false`: 30 frames each second, as on the Xbox. Refer to "Frame rate". |
| `display.direct_camera` | `true` | `HALO_DIRECT_CAMERA` | `true`: in first person, on foot, the view points where the player aims in each frame, not where the last tick left it. Refer to "Frame rate". |
| `display.fov` | `0.0` | `HALO_FOV` | The first-person view's field of view on foot, in degrees across at 16:9, from 20 to 150. `0`: the stock view. Refer to "Field of view". |
| `display.viewmodel_fov` | `0.0` | `HALO_VIEWMODEL_FOV` | The first-person weapon's and hands' field of view, in degrees across at 16:9, from 20 to 150. `0`: the weapon's stock view, also when `display.fov` widens the world. Refer to "Field of view". |
| `display.viewmodel_visible` | `true` | `HALO_VIEWMODEL_VISIBLE` | `true`: the first-person weapon, hands and what is attached to them are drawn. `false`: they are not; firing, animation, sound and lights go on, and other players' models are drawn. |
| `display.viewmodel_shield` | `false` | `HALO_VIEWMODEL_SHIELD` | `true`: the energy shield's flare (its hits, its charging, its fall) is drawn on the first-person arms too, as on the body, with the unit's own shield shader, kept closer to the arms than to the body. The stock game draws it on the body only. |
| `display.first_person_legs` | `false` | `HALO_FIRST_PERSON_LEGS` | `true`: your own body is drawn from the waist down in first person, so you see your legs when you look down, moving as the body's animation moves them. Refer to "Field of view". |
| `display.shield_color` | `default` | `HALO_SHIELD_COLOR` | Your energy shield's flare as this machine draws it: `default` (the shield's own) or a multiplayer armor color (`white`, `black`, `red`, `blue`, `gray`, `yellow`, `green`, `pink`, `purple`, `cyan`, `cobalt`, `orange`, `teal`, `sage`, `brown`, `tan`, `maroon`, `salmon`), whatever the armor's, on the body and, with `display.viewmodel_shield`, on the first-person arms. The shield's own shader is drawn with its two colors replaced at their brightness; the flare is added to what is behind it, so `black` is drawn a faint grey. Only this machine sees it. |
| `display.high_res_hud` | `true` | `HALO_HIGH_RES_HUD` | `true`: the HUD (meters, counters, panels and their outlines, the motion sensor, reticles, waypoints, scopes) is drawn from the high-res assets in `port/assets/hud`, 8x the size of the maps' bitmaps. The bitmaps with English text keep the maps' own. `false`: the maps' own bitmaps. |
| `display.high_res_text` | `true` | `HALO_HIGH_RES_TEXT` | `true`: the menus' and HUD's text is drawn with the fonts in `port/assets/fonts` (Overpass, in place of the maps' Interstate) at the resolution the game draws at, laid out as before; the menus' titles are drawn from the high-res pictures in `port/assets/titles`, and the controller button icons from those in `port/assets/buttons`. `false`: the maps' bitmap fonts, titles and button icons. |
| `display.shadow_resolution` | `128` | `HALO_SHADOW_RESOLUTION` | The size of the maps that the shadows of the objects are drawn in, in pixels each way: `128`, `256`, `512` or `1024` (other values go down to one of these). The game draws the shadow of each object into a map of 128x128 pixels, blurs it and projects it onto the ground. On a large screen, the edges of these shadows show steps that move when the object moves. A larger map makes the edges smooth; the blur is made wider to match, so the shadows are as soft as on the Xbox. Each doubling adds two passes of the blur. `128`: as on the Xbox. |
| `display.menus` | `"pc"` | `HALO_MENUS` | `"pc"`: the PC version's menus, from the files in `port/assets/menus` and a `menus` folder next to `config.toml`. Refer to "Menus". `"xbox"`: the Xbox's menus. |
| `display.player_names` | `"all"` | `HALO_PLAYER_NAMES` | In multiplayer, whose names are drawn above their heads: `"all"`, `"allies"`, `"enemies"` or `"none"`. An ally's name is drawn above the triangle the game shows over teammates. An enemy's name shows only within the motion sensor's reach, while the enemy is in sight and not camouflaged, so it never shows where an enemy hides. The gametype's motion tracker setting also applies: no names if it shows no players, only allies' if it shows only friends. |
| `display.player_name_scale` | `1.0` | `HALO_PLAYER_NAME_SCALE` | How large the players' names are drawn: `1.0` is three quarters of the size of the HUD's text, from `0.25` to `4`. With high-res text, larger names are rasterized at their size, so they stay sharp. |
| `display.scoreboard_team_layout` | `"teams"` | `HALO_SCOREBOARD_TEAM_LAYOUT` | How the multiplayer scoreboard (hold BACK, or tab) lists a team game's players. `"teams"`: a column for each team, red on the left and blue on the right. `"score"`: all the players in order of score. With more players than fit, the mouse wheel and Page Up / Page Down scroll the scoreboard. |
| `display.scoreboard_background` | `true` | `HALO_SCOREBOARD_BACKGROUND` | `true`: the multiplayer scoreboard (hold BACK, or tab) has a panel behind its text, for clearer text. |
| `display.scoreboard_background_color` | `"16, 16, 16, 150"` | `HALO_SCOREBOARD_BACKGROUND_COLOR` | The colour of the scoreboard's panel: `"red, green, blue, alpha"`, each from `0` to `255`. Alpha `0` is see-through, `255` is solid. |
| `display.per_pixel_lighting` | `false` | `HALO_PER_PIXEL_LIGHTING` | `false`: the models (characters, weapons, vehicles, scenery) are lit at each vertex and the light is blended between them, as on the Xbox. The light across a curved surface then shows facets, and a point light that passes close lights only the vertices it reaches. `true`: the models are lit at each pixel by the same lights (the ambient light, two distant lights and two point lights), which changes their look. |
| `display.shield_glow` | `false` | `HALO_SHIELD_GLOW` | `true`: as a unit's energy shield flares (hit, charging, falling), it lights what is around it and itself, as a plasma bolt does, in the shield shader's own color (a Spartan's gold, an Elite's blue), rising and falling with the flare seen on the body. An ordinary dynamic light of the game's, drawn with a light definition the map has (`shield_glow.c`). Up to 8 units near the camera. |
| `display.shield_glow_intensity` | `default` | `HALO_SHIELD_GLOW_INTENSITY` | The shield glow's reach and brightness: `default`, or `light_show` (three times as far, and at full strength from a third of the flare: a light is no brighter than full). The units glowing are those near any local player's camera, so each of a split screen's views has the same. |
| `audio.enabled` | `true` | `HALO_NO_AUDIO=1` sets `false` | `false`: no audio device. The sound continues without output. |
| `audio.volume` | `1.0` | `HALO_VOLUME` | The master volume. |
| `audio.music_volume` | `1.0` | `HALO_MUSIC_VOLUME` | The music's volume, of the master volume. |
| `audio.effects_volume` | `1.0` | `HALO_EFFECTS_VOLUME` | The volume of the other sounds (effects and speech), of the master volume. |
| `audio.reverb` | `true` | `HALO_REVERB` | `true`: the sounds of the world reverberate as the place the player is in does: the sound environments of the maps (a corridor, a cave, a large hall, outdoors) set the reverberation, as the I3DL2 reverb of the Xbox did. A sound behind a wall or a door is muffled in it too. `false`: no reverberation (sounds behind a wall are still muffled). |
| `audio.voice_chat` | `"push_to_talk"` | `HALO_VOICE_CHAT` | How you talk in voice chat: `"push_to_talk"` (while `controls.push_to_talk` is held; the microphone opens when you first press it), `"open_mic"` (when the microphone hears speech), or `"off"`. You hear the other players in every case. Refer to "Voice chat". |
| `audio.voice_volume` | `1.0` | `HALO_VOICE_VOLUME` | The volume of the voices of the other players, `0` to `2`. |
| `audio.output_device`, `audio.input_device` | `"default"` | `HALO_AUDIO_OUTPUT_DEVICE`, `HALO_AUDIO_INPUT_DEVICE` | The speakers and the microphone, by the name that Settings > Audio shows, or `"default"` for the device of the system. If the device is not found, the game uses the device of the system. Not on Android. |
| `audio.loose_sounds` | `false` | `HALO_LOOSE_SOUNDS` | For those who make sounds. `true`: each sound of a map that has a sound tag file of its name in `tags/` in the data root (for example `tags/sound/sfx/weapons/assault rifle/fire.sound`) plays from that file. The files are Halo PC tag files, as the Halo Editing Kit and Invader write them. At the console, `loose_sounds_reload` reads the files again, and `loose_sounds false` plays the sounds of the map again. When a file changes, all sounds stop. |
| `input.mouse_sensitivity` | `1.0` | `HALO_MOUSE_SENSITIVITY` | The multiplier for the mouse aim. |
| `input.mouse_vertical_sensitivity` | `0.0` | `HALO_MOUSE_VERTICAL_SENSITIVITY` | The multiplier for the vertical mouse aim. `0`: the same as `input.mouse_sensitivity`. |
| `input.invert_mouse` | `false` | `HALO_MOUSE_INVERT=1` sets `true` | `true`: the vertical mouse aim is inverted. |
| `input.mouse_aim_assist` | `false` | `HALO_MOUSE_AIM_ASSIST` | `true`: the magnetism of the controller also operates for the mouse. `false`: when the mouse moved after the right stick, the view is not slowed or dragged by a target. The autoaim of the bullets operates in both cases. |
| `controls.<action>` | (the table in "Controls") | `HALO_KEY_<ACTION>` | The keys and mouse buttons of an action, up to two, separated by a comma: `move_forward`, `move_backward`, `strafe_left`, `strafe_right`, `jump`, `crouch`, `fire`, `throw_grenade`, `melee`, `reload`, `zoom`, `switch_weapon`, `switch_grenade`, `action`, `flashlight`, `scoreboard`, `pause`, `screenshot`, `push_to_talk`. Keys by their names (`"W"`, `"Space"`, `"Left Ctrl"`, `"F1"`), and `"Mouse Left"`, `"Mouse Right"`, `"Mouse Middle"`, `"Mouse 4"`, `"Mouse 5"`, `"Wheel"` (either way), `"Wheel Up"`, `"Wheel Down"`. |
| `game.console_log` | `"important"` | `HALO_CONSOLE_LOG` | What the console shows on the screen. `"important"`: bans, players that the host drops for cheating, the reasons that the game refuses a command, and the asserts that stop the game. `"all"`: all the lines. `"none"`: only the asserts that stop the game. The output of a command always shows. `debug.txt` gets all the lines. |
| `game.language` | `""` | `HALO_LANGUAGE` | The language of the menus: `ja`, `de`, `fr`, `es` or `it`. Empty: English. |
| `game.enhanced_animations` | `true` | `HALO_ENHANCED_ANIMATIONS` | `true`: the player bipeds' grenade throws keep their legs moving, blended by speed and direction (crouched throws stay crouched, throws in the air use the jump's legs), Warthog and Scorpion riders stay seated to throw and let go of the grips to throw and reload, and a player turns with the aim while throwing, as while meleeing. `false`: the original animations, which freeze the legs during a throw and stand a rider up. Only in config.toml, not in the menus. |
| `paths.data` | `""` | `HALO_DATA_ROOT` | The data root. Refer to "Start the game". |
| `paths.saves` | `""` | `HALO_SAVE_ROOT` | The save root. Refer to "Files and folders". |
| `network.address` | `""` | `HALO_NET_ADDRESS` | The IPv4 address of this machine for system link. Refer to "Play on one computer". |
| `network.broadcast` | `""` | `HALO_NET_BROADCAST` | IPv4 addresses, with commas between them, that get the broadcasts of the game. Empty: 255.255.255.255. |
| `network.online` | `true` | `HALO_NET_ONLINE` | `true`: internet play. `false`: system link on the local network only. |
| `network.join_from_clipboard` | `true` | `HALO_NET_JOIN_FROM_CLIPBOARD` | `true`: when the game comes to the front, it joins the game of an invite link on the clipboard. |
| `network.tunnel_port` | `0` | `HALO_NET_TUNNEL_PORT` | The UDP port for internet play. `0`: the game selects a port. Refer to "Internet play". |
| `network.allow_upnp` | `true` | `HALO_NET_ALLOW_UPNP` | `true`: internet play can ask the router to forward its port (UPnP). `false`: the game does not ask. Refer to "Internet play". |
| `network.public_lobby` | `true` | `HALO_NET_PUBLIC_LOBBY` | `true`: the server browser. Public games are listed, and Join Game > Server Browser shows them. `false`: no games are listed or shown. Refer to "Server browser". |
| `network.host_public` | `true` | `HALO_NET_HOST_PUBLIC` | `true`: a new game of Create Game > Internet starts as PUBLIC. `false`: it starts as PRIVATE. LISTING in Server Setup changes it for each game. Refer to "Server browser". |
| `network.coop_friendly_fire` | `"on"` | `HALO_NET_COOP_FRIENDLY_FIRE` | Whether the players of an online co-op game hurt each other: `"off"`, `"on"`, `"shields_only"` or `"explosives_only"`. FRIENDLY FIRE in co-op's Server Setup > Co-op Options writes its choice here. Their AI allies they always can, as in the campaign. |
| `network.coop_player_collisions` | `true` | `HALO_NET_COOP_PLAYER_COLLISIONS` | Whether the players of an online co-op game bump into each other. `false`: they walk through each other, so that one cannot block a doorway or stand on another; they still bump into the AI's characters. PLAYER COLLISIONS in co-op's Server Setup > Co-op Options writes its choice here. |
| `network.coop_enemies_mode` | `"per_player"` | `HALO_NET_COOP_ENEMIES_MODE` | Online co-op's extra enemies: `"none"`; `"per_player"`, each squad of enemies that a level places grows by `network.coop_enemies` for each player past the first; or `"multiplier"`, each squad is `network.coop_enemies_multiplier` times as large, for any number of players. The extra enemies stand around the squad's places, and those that a dropship has no seats for drop out of it after its passengers. EXTRA ENEMIES in co-op's Server Setup > Co-op Options writes its choice here. |
| `network.coop_enemies` | `50` | `HALO_NET_COOP_ENEMIES` | The extra enemies per player, a percentage from `25` to `200`: for each player past the first, each squad of enemies gets this much of itself more (`100`: as many again, so four players meet four times the squad), up to 8 times the squad however many players there are. PER PLAYER in co-op's Server Setup > Co-op Options writes its choice here. |
| `network.coop_enemies_multiplier` | `2` | `HALO_NET_COOP_ENEMIES_MULTIPLIER` | The static multiplier of the enemies, `2` to `32`: each squad of enemies is this many times as large. MULTIPLIER in co-op's Server Setup > Co-op Options writes its choice here. |
| `network.voice_lobby` | `true` | `HALO_NET_VOICE_LOBBY` | When you host: `true`, all players hear all players in the lobby, before and after a game. |
| `network.voice_mode` | `"team_global_enemy_proximity"` | `HALO_NET_VOICE_MODE` | When you host: who hears whom during a game. `"off"`; `"team_proximity"` (teammates who are near); `"team_enemy_proximity"` (all players who are near); `"team_global"` (all teammates); `"team_global_enemy_proximity"` (all teammates, and enemies who are near). Refer to "Voice chat". |
| `network.voice_kbps` | `24` | `HALO_NET_VOICE_KBPS` | When you host: the voice quality in the lobby and in a game, in kilobits per second, `8` to `64`. |
| `network.voice_proximity` | `15.0` | `HALO_NET_VOICE_PROXIMITY` | When you host: the distance in world units (1 unit is approximately 3 metres) at which players are near, for voice chat. `5` to `100`. |
| `network.votekick` | `true` | `HALO_NET_VOTEKICK` | When you host: `true`, the players can vote to kick a player. Refer to "Security". `false`: no votes. VOTE KICK in Server Setup > Teamplay Options (in co-op, Voice and Voting) writes its choice here. |
| `network.votekick_minutes` | `5` | `HALO_NET_VOTEKICK_MINUTES` | When you host: the minutes that a player must play on the server before the player can start a vote to kick (`0` to `60`). To vote, a player must play for 2 minutes, or for this time if it is less. |
| `network.votekick_ban_minutes` | `30` | `HALO_NET_VOTEKICK_BAN_MINUTES` | When you host: the minutes that a player who is kicked by a vote cannot join again (`1` to `1440`). |
| `network.coop_public` | `false` | `HALO_NET_COOP_PUBLIC` | `true`: an online co-op game (Create Game > Internet, a SINGLEPLAYER map) starts as PUBLIC. `false`: it starts as PRIVATE. LISTING in co-op's Server Setup writes its choice here. Refer to "Server browser". |
| `network.brokers_file` | `"brokers.txt"` | `HALO_NET_BROKERS_FILE` | The file of the public MQTT brokers that let the machines of an invite find each other, and that carry the listings of the server browser: next to `config.toml`, unless a full path. One `host:port` on each line, up to 4; `#` starts a comment. |
| `network.stun_servers` | Google and Cloudflare | `HALO_NET_STUN` | The public STUN servers (`host:port`, with commas between them) that give the internet address of a machine. |
| `discord.application_id` | the application of the project | `HALO_DISCORD_APPLICATION` | The Discord application for invites. Empty: no Discord. |
| `update.auto` | `true` | `HALO_UPDATE_AUTO` | `true`: at start-up, the game looks for a new version. Refer to "Updates". `false`: the game does not look. |
| `crash_reports.upload` | `"ask"` | `HALO_CRASH_REPORTS` | Windows only. `"yes"`: the game sends a report of each crash to the developers. `"no"`: the game sends no reports. `"ask"`: the game asks at the next crash and writes the answer here. Refer to "Crash reports" in [port/windows/README.md](../windows/README.md#crash-reports). |
| `debug.update_answer` | `""` | `HALO_UPDATE_ANSWER` | The answer to the update question, for automatic tests: `yes`, `no` or `never`. Empty: the game asks. |
| `debug.exit_after` | `0.0` | `HALO_EXIT_AFTER` | The game stops after this number of seconds. `0`: never. |
| `debug.screenshot_directory`, `debug.screenshot_every` | `""`, `0` | `HALO_SCREENSHOT_DIR`, `HALO_SCREENSHOT_EVERY` | The game writes each Nth frame to this folder as a BMP file. |
| `debug.hidden_window`, `debug.null_renderer` | `false` | `HALO_HIDDEN_WINDOW`, `HALO_NULL_RENDERER` | `true`: no visible window, or no graphics. |
| `debug.gpu_stats`, `debug.gpu_trace_frame`, `debug.gpu_trace_constants`, `debug.gpu_dump_shaders`, `debug.texture_dump_directory`, `debug.texture_log`, `debug.gl_debug`, `debug.texture_no_cache` | off | `HALO_GPU_STATS`, `HALO_GPU_TRACE`, `HALO_GPU_TRACE_CONSTANTS`, `HALO_GPU_DUMP_SHADERS`, `HALO_TEXTURE_DUMP`, `HALO_TEXTURE_LOG`, `HALO_GL_DEBUG`, `HALO_TEXTURE_NO_CACHE` | Tools to find problems in the graphics: counts for each frame, all the GL state of one frame, the GLSL code, the textures. |
| `debug.skip_cinematics` | `false` | `HALO_SKIP_CINEMATICS` | Skip each cinematic of a local game: as the skip button does where the level lets it be skipped, else run at sixteen times its speed (`main_skip`), so that a test reaches play in seconds (b30's opening, which cannot be skipped, in about 100 frames rather than 2,400). |
| `debug.menu_open` | `""` | `HALO_MENU_OPEN` | Start on this screen of the menus (`main_menu/settings_select/...`, as `port/assets/menus` names it), a player profile being edited, to look at it. |
| `debug.gpu_skip_vertex_shaders`, `debug.gpu_debug_expression`, `debug.gpu_debug_flat`, `debug.gpu_debug_texture0` | off | `HALO_GPU_SKIP_VS`, `HALO_GPU_DEBUG_EXPR`, `HALO_GPU_DEBUG_FLAT`, `HALO_GPU_DEBUG_T0` | Tools to find problems in the graphics: skip the draws of a vertex shader, or replace the output of all pixel shaders with a GLSL expression (for example `t0.rgb`). |
| `debug.network_test`, `debug.network_test_start`, `debug.network_test_kill`, `debug.network_test_score`, `debug.network_test_shoot`, `debug.network_test_vehicle`, `debug.network_test_pickup`, `debug.network_test_pickup_weapon`, `debug.network_test_public`, `debug.test_input` | off | `HALO_NETWORK_TEST`, `HALO_NETWORK_TEST_START`, `HALO_NETWORK_TEST_KILL`, `HALO_NETWORK_TEST_SCORE`, `HALO_NETWORK_TEST_SHOOT`, `HALO_NETWORK_TEST_VEHICLE`, `HALO_NETWORK_TEST_PICKUP`, `HALO_NETWORK_TEST_PICKUP_WEAPON`, `HALO_NETWORK_TEST_PUBLIC`, `HALO_TEST_INPUT` | Automatic tests of system link (`game/network_test.c`). Refer to `NETCODE.md`. |
| `debug.touch_targets` | `false` | `HALO_TOUCH_TARGETS` | Outlines the tap targets of the menus (item green, value blue, list slot yellow, legend button red, the band beside the slots of a list orange, keys of the on-screen keyboard white), marks where the last finger went down and the last tap landed for 3 seconds, and logs each tap with the target that it hit (for a value, also where it splits into previous and next): to judge the accuracy of touch. |
| `debug.network_latency`, `debug.network_loss`, `debug.network_corrupt`, `debug.network_corrupt_stream`, `debug.network_corrupt_after` | `0` | `HALO_NETWORK_LATENCY`, `HALO_NETWORK_LOSS`, `HALO_NETWORK_CORRUPT`, `HALO_NETWORK_CORRUPT_STREAM`, `HALO_NETWORK_CORRUPT_AFTER` | The game holds all the data that it receives for this number of milliseconds, ignores this percentage of the datagrams, and damages this percentage of the datagrams it receives, and this percentage of its reads of streams, at random (bytes changed, cut short, stretched or replaced), from this many seconds after the start. Use the first two to test the netcode as on the internet, and the others to test that nothing another machine sends can crash the game (a damaged stream is closed, so a little goes a long way; a host's messages to its own client are damaged too, so start damaging once the game has started). |
| `debug.voice_test` | `false` | `HALO_VOICE_TEST` | Automatic tests of voice chat: a tone replaces the microphone, and each voice that the game hears is written to the log once each second. |
| `debug.telnet_console`, `debug.telnet_console_port` | `false`, `2323` | `HALO_TELNET_CONSOLE`, `HALO_TELNET_CONSOLE_PORT` | The game listens on 127.0.0.1, on this port, for a script console (connect with telnet). The console has no password, so only this computer can reach it. |
| `debug.profile_record`, `debug.profile_record_when`, `debug.profile_memory` | `false`, `"start"`, `256` | `HALO_PROFILE_RECORD`, `HALO_PROFILE_RECORD_WHEN`, `HALO_PROFILE_MEMORY` | Profiling builds only (`configure.py --profile`). Record without a command; `"start"` records from the first frame until the first map change, `"game"` records each game outside the main menu. Memory is 4–1024 MB. Numbered `profile_<stamp>_<role>.part<n>.json` files remain in `profiles/` after recording. Refer to "Profiling builds" in the main README. |


With Mesa drivers, the game sends its GL calls through the GL thread of
Mesa. To stop this, set the environment variable `mesa_glthread=false`.

## Updates

The builds from GitHub Actions (refer to the main [README](../../README.md#download))
can update themselves. At start-up, the game asks GitHub for the latest
release. The game does not wait for the answer. If the latest release is not
newer, the game does nothing.

If the latest release is newer, the game asks: "Do you want to update?"

- Select "Yes" to update. The game downloads the release for this platform,
  replaces its files and starts the new version. The old files get the
  extension `.old`. The new version deletes them.
- Select "No" to continue. The game asks again at the next start.
- Select "Do not ask again", then "Yes", to stop the questions. The game
  writes `auto = false` in the `[update]` section of `config.toml`. To get
  the questions again, set `auto = true`.

The game downloads through HTTPS. It examines the certificate of the server
against the certificate authorities of the system: on Linux, the bundle of
the distribution (`src/posix_update.c`, with Mbed TLS); on Windows, the
certificate store of Windows (WinHTTP). The folder of the executable must
let the game write to it.

Builds that you make yourself have no build number. They do not look for
updates.

The 64-bit ARM build downloads `halo-linux-arm64-release.zip` (or
`-debug.zip`), not the x86 build.

## Frame rate

The game calculates its world at 30 Hz, as on the Xbox. On the Xbox, the
game showed one frame for each calculation (tick). This port shows one frame
for each refresh of the display, for example at 60, 120 or 240 Hz.

Each frame shows the world between the last two ticks
(`game/render_interpolation.c`):

- After each tick, the game keeps the camera, the position of each part of
  each object, and the first-person weapon.
- Each frame mixes the last two ticks. The mix agrees with the time since
  the last tick.
- Rotations use quaternions. Positions and scales are linear.
- A teleport, a respawn or a cut of the camera does not mix. It jumps.
- After a long frame (several ticks in one frame), the camera mixes the
  last tick only, as the objects do.
- In cinematics, a camera that moves with an object (the lifepod in a30,
  a Pelican) moves with the object as it is drawn.

Thus the frames are one tick (33 ms) after the calculation. The calculation
does not change.

The direction of the view is an exception. The game reads the mouse and the
sticks in each frame. In first person, on foot, each frame points the view
where the player aims at that time (`display.direct_camera`). Thus the view
turns in the frame that the mouse moves. In a vehicle and in cinematics, the
view mixes as the other things do. On Android, the view mixes as before.

To get 30 frames each second, set `display.interpolation = false`.

To see the frame rate:

1. Push \` to open the developer console.
2. Enter `display_framerate true`.

In the game of another host, the console runs only the commands that change
nothing of the game (such as `display_framerate`), and the game puts back
cheats, the game speed and the settings of the drawing that show more of
the world (such as `rasterizer_wireframe`). Refer to `NETCODE.md`.

The frame rate shows at the bottom right of the screen. It is the mean over
half a second.

## Field of view

Video Setup's FOV AND VIEWMODELS screen sets the field of view of the
first-person view (`display.fov`) and of the weapon and hands
(`display.viewmodel_fov`), from 80 to 150 degrees in steps of 5, and can
hide the weapon (`display.viewmodel_visible`). DEFAULT, the setting's `0`,
keeps the stock view. `config.toml` takes any angle from 20 to 150.

- An angle is across the screen at 16:9. Another shape keeps the same
  angle up and down, as the stock view does.
- The field of view applies on foot in first person only. Vehicles, death,
  cinematics and scripted cameras keep their own view.
- A scope's zoom levels keep their stock view: the extra width fades out as
  the zoom comes in. A view narrower than the stock one narrows the zoom
  too.
- The reticles scale with the view, so that they stay on the aim. The
  scopes' pictures keep their place.
- By default the weapon keeps its stock view when the world is wider:
  arms and a gun right against the camera stretch at a wide angle.

LEGS (`display.first_person_legs`) draws your own body in first person
from the waist down, so you see your legs when you look down, walking,
running and crouching as everyone else sees them. It is the body others
see, posed as they see it: the upper body is folded away, and the body is
set back behind the eye as far as keeps the open waist below the view
(the way Halo 2 keeps its first-person body's open top out of sight), more
the further down you look. It is drawn with the world's camera, so it keeps
its place and size at any field of view. Hit boxes, markers and everything
the game does with the body are unchanged. In the VR mode the chest is drawn
too, under the real head (VR, "Your body").

Only this machine's view changes. Nothing the machines send each other
changes, so players with different settings play together.

## System link

The Xbox game lets 16 players on 4 machines play a system link game. This
port lets up to 128 players on up to 128 machines play. Each machine can
have up to 4 players (split screen).

- `include/halo_port_limits.h` sets the limits.
- `include/halo_port_capacity.h` sets the memory for the limits. The game
  state is 16 MB at `0x81A00000` (3.3 MB on the Xbox). The pools of objects,
  effects, particles, contrails, lights and sounds are also larger.

Obey these rules:

- All the machines in a game must use a build with the same limits.
- The port uses protocol version 2. It does not see the Xbox game or older
  builds of the port. They do not see the port.

These are the differences from the Xbox:

- The host waits up to 60 seconds (15 seconds on the Xbox) for the other
  machines to load the map.
- If a machine does not read the messages of the host for two seconds, the
  host removes it from the game.
- The saved games contain all the game state. Thus a saved game is 16 MB.
  Saved games from older builds of the port do not operate.
- In campaign and in games of up to 16 players, the game removes garbage
  (bodies, dropped weapons) as on the Xbox. In larger games, it keeps more
  garbage, in proportion to the players.
- The lobby shows the local machine and the first three remote machines.
  The other machines are also in the game.
- In free-for-all games, each player is a team.

Linux, Windows and Android machines can play in the same game. Each machine
simulates the players from the same inputs, and the host does not correct
all of the game. Thus each machine must calculate the same floating-point
results, and all the ports:

- Compile without fused multiply-add (`-ffp-contract=off`).
- Use the math functions of musl (`port/include/halo_math.h`,
  `port/third_party/musl-math`), not the math functions of the system.

### Play on one computer

More than one copy of the game can play on one computer. Each copy must
have a different loopback address. A copy with an address gets no
broadcasts. Thus each copy must send its broadcasts to the other copies.

For a host and two clients, enter these commands in three terminals:

```sh
HALO_NET_ADDRESS=127.0.0.200 HALO_NET_BROADCAST=127.0.0.201,127.0.0.202 build/linux/halo
HALO_NET_ADDRESS=127.0.0.201 HALO_NET_BROADCAST=127.0.0.200 build/linux/halo
HALO_NET_ADDRESS=127.0.0.202 HALO_NET_BROADCAST=127.0.0.200 build/linux/halo
```

Do not give 127.0.0.1 to a copy. Each copy gets to its own address through
127.0.0.1. Linux and Windows send all of 127.0.0.0/8 to the loopback
interface.

### Test with many machines

`tools/system_link_bots.py` adds simple machines to a game. Each machine has
one player. The machines obey the system link protocol, but they do not
calculate the game or move their players.

1. Start a game on the host.
2. Enter `python tools/system_link_bots.py --host 127.0.0.200 --machines 127 --start`.

Each machine uses its own loopback address, from 127.0.0.2. The option
`--start` starts the game when all the machines are in the lobby. If the
host has no `network.address`, do not give `--host`.

## Internet play

Machines with an invite link can play system link on the internet. This
project has no server.

When a copy of the game starts to host a system link game, it makes an
invite link: `halo://join/<64 hexadecimal digits>`. The game writes the link
to the standard error and puts it on the clipboard. The links of older
versions of the game (44 digits) do not operate. The game writes a message
when it gets one.

To join a game, do one of these steps:

- Open the link. The game is the handler of `halo://` links. If the game
  already operates, the new copy gives the link to it and stops. A key in a
  file that only the user can read (`halo-ce-universal.key` in
  `$XDG_RUNTIME_DIR`, else `~/.halo-ce-universal.key`; on Windows in
  `%LOCALAPPDATA%`) encrypts the link, so the programs of other users cannot
  read it.
- Copy the link (or the 64 digits) and go to the game.
- Enter `halo <link>`.
- Accept a Discord invite. Refer to "Discord".

The game also takes the address of the web build's page with an invite
(`https://halocombatevolved.com/?join=<64 digits>`), as a link.

When the machines connect, the game of the host shows in Multiplayer,
System Link. Join the game as on a local network. System link on a local
network does not need an invite.

### Browsers

The web build ([port/web/README.md](../web/README.md)) plays internet games
with the Linux, Windows and Android builds: it hosts and joins by invite, and
the server browser lists its public games and theirs. A browser has no UDP
socket, only WebRTC, so a native build takes a browser's WebRTC on the port
of its tunnel (`network.tunnel_port`), as little of it as one data channel
needs (`src/p2p_webrtc.c`):

- ICE: the native build is an ICE-lite agent. It answers the checks of the
  browser, and the browser chooses the path. The credentials come from the
  secret of the session, which both machines have. Until the browser
  connects, the native build sends STUN indications to the addresses of the
  browser, which open its NAT to the checks of the browser.
- DTLS 1.2 (`src/posix_dtls.c`, Mbed TLS: `port/third_party/mbedtls`): the
  native build is the server, with a certificate that it makes when it
  starts (ECDSA P-256). Each machine sends the SHA-256 of its certificate in
  its signalling messages, which the token seals and the proof of the
  session authenticates; the other machine accepts only that certificate.
- SCTP, over DTLS: the browser opens the association, and one data channel,
  agreed before (unordered, without retransmissions), carries the packets of
  the tunnel. They are sealed as on UDP, and KCP makes the streams reliable
  as on UDP.

Two browsers connect to each other with their own WebRTC. There is no relay:
as between native builds, two networks that do not let a direct connection
through (some company and school networks block WebRTC) cannot play
together.

### Voice chat

Network games have voice chat, on the local network and on the internet.
Hold V (`controls.push_to_talk`, Settings > Controls Setup) to talk, or set
VOICE CHAT in Settings > Audio to OPEN MIC. VOICE VOLUME sets the volume of
the other players. On Android, set VOICE CHAT to OPEN MIC to talk: the game
then asks for the microphone.

OUTPUT DEVICE and INPUT DEVICE in Settings > Audio select the speakers and
the microphone (SYSTEM DEFAULT, the default, follows the system). The list
has the devices that the computer had when the game started.

The host sets voice chat for all players in Server Setup > Teamplay Options
(`network.voice_*` in `config.toml`):

- In the lobby, before and after a game, all players hear all players
  (LOBBY VOICE CHAT).
- During a game (VOICE CHAT): OFF; TEAM NEAR; ANYONE NEAR; TEAM; or TEAM,
  ENEMIES NEAR (the default). Near is VOICE NEAR DISTANCE (45 metres is the
  default), and both players must be alive. A player who is near sounds
  quieter when further, and from their direction. In a game without teams
  all players are enemies; in co-op, all are teammates.
- The quality (VOICE QUALITY), 8 to 64 kilobits per second (24 is the
  default), in the lobby and in the game.

In co-op, Server Setup has no Teamplay Options: the same settings are in
Server Setup > Voice and Voting.

A speaker shows next to the name of a player who talks: in the lobby, on
the scoreboard, in a list at the left of the screen during a game, and
beside the name above their head (when names are shown there:
`display.player_names`). To mute a player, open the scoreboard,
right-click, click the player, and click Mute voice. You no longer hear
them, and nobody else is told.

The host sends each voice on to the players who can hear it. The host
accepts a voice only from the machine of the player (with a key that it
gives each machine on its connection), only at the quality it set, at most
50 frames each second from a machine, and from at most 8 players at a time.
The voices are compressed with Opus (`port/third_party/opus`).

### Server browser

Server Setup in Create Game > Internet has a LISTING row:

- PUBLIC (the default): the game also shows in Join Game > Server Browser
  on every machine. Anyone can see and join the game.
- PRIVATE: only players with the invite link can join.

Each new game starts as PUBLIC (`network.host_public = false` makes new
games start as PRIVATE). An online co-op game starts as PRIVATE, and keeps
the last choice of its LISTING (`network.coop_public`). A LAN game is never listed. `network.public_lobby = false` turns the server browser off.

A PUBLIC game can also have a PASSWORD (a row of Server Setup, below
LISTING). The Server Browser shows a lock at the left of a game with a
password. A player who selects that game must type the password, and JOIN
GAME joins only with the correct password. The invite link of the game joins
it without the password. The host keeps the password only while the game
runs.

In the Server Browser, select a game to join it. The game joins the invite
of the game, as for a link. When it reaches the host, it opens the lobby.
If it cannot reach the host in 30 seconds, it marks the game FAILED. A game
that is full or starting shows CLOSED. REFRESH asks the hosts for their
listings again.

How it operates (`src/p2p_lobby.c`):

- The host of a public game publishes a listing: the invite, and the name,
  map, gametype and player counts of the game. The listing goes to the
  same MQTT brokers as the invites (`network.brokers_file`), retained,
  to a topic of the host (`hceu/3/lobby/s/<hash of its key>`).
- The key of the host signs the listing (Ed25519). The key is the key of the
  invite, so no other machine can list the invite of the host, change its
  listing, or list a game with false details.
- The host publishes the listing again every 30 seconds, when the game
  changes, and when a browser asks. If the topic of the host is empty or
  holds another listing, the host publishes again in 5 seconds or less. Thus
  a broker that deletes the listing does not remove the game.
- When the host stops (it stops hosting, the game becomes private, or the
  game quits), it publishes a closed listing, and then empties its topic.
  If the host loses its connection, the broker empties its topic (the will
  of the connection). A browser removes a game that it does not hear for 90
  seconds.
- When a public game becomes private, the host makes a new invite. Thus a
  player who saw the listing cannot join with the old invite.
- The listing of a game with a password does not hold the invite in clear
  text. The secret part of the invite (its token) is encrypted with a key
  from the password (Argon2id, salted with the key of the host, then
  XChaCha20-Poly1305). The browser makes the key from the password that the
  player types, and opens the invite only if the password is correct. When
  the host sets or changes the password, it makes a new invite. A player who
  has the listing can try passwords on their own machine without the host,
  so use a long password.

A public game does not publish the address of the host. But any machine
with the invite can ask the host to connect, and the host then sends its
addresses. Thus anyone can learn the address of the host of a public game,
as for any public server.

The brokers are in `brokers.txt` next to the executable (from
`port/assets/network/brokers.txt`; on Android, the app writes it next to
`config.toml` at each start), one `host:port` on each line. The game uses
all of them at once (up to 4), so one that works is enough. An update
replaces `brokers.txt`: to use brokers of your own, put them in another
file and name it in `network.brokers_file`. All the players must use the
same broker to see each other's games. The game uses
MQTT 5 if the broker has it, else MQTT 3.1.1. A broker that does not keep
retained messages, or does not let clients subscribe with wildcards, carries
only invites, not listings.

### Security

Only machines with the invite can find the game:

- Each copy of the game makes an Ed25519 key pair when it starts, and from
  it an X25519 key pair (Monocypher, `port/third_party/monocypher`). Its
  identifier is from the hash of its X25519 public key. The Ed25519 key
  signs the listing of a public game.
- The link contains a 16-byte hash of the public key of the host and a
  random 16-byte token. The identifier of the host is from the first 6
  bytes of the hash.
- The machines exchange their public keys and addresses through public MQTT
  brokers (`network.brokers_file`). The topics are HMACs of the token.
  A key from the token encrypts and authenticates the messages
  (`src/p2p_signal.c`, `src/p2p_crypto.c`). The host authenticates its answer
  with a key that only it and the player can calculate. Its public key must
  agree with the hash in the link. The hash is long, so no other machine can
  find a key with the same hash.
- Then the player shows in the same way that it has the private key of its
  public key. Only then does the host make a session for the player. Thus
  other machines with the invite cannot make sessions in the name of a
  player (such a session would keep the player out).
- Each two machines get the keys of their packets from their key pairs and
  a random number from each. The keys do not go through the brokers. Thus
  other machines with the invite cannot read or change the packets.
- Each packet is encrypted and authenticated, with a different key in each
  direction. A machine ignores a packet that it already received. To and
  from a browser, the packets also go through DTLS, with the certificates
  that the signalling messages named.
- A machine can send only to the ports of the game on the other machine.
- The host makes one session from each request of a player. If a person
  sends a copy of an old request again, the host ignores it. A player that
  must ask again sends a new request.
- The host tries to reach at most 8 new players at the same time. The
  other players ask again.
- The host answers a request that is not proven at most one time each
  second through each broker. It answers at most 20 of these requests each
  second, after a first 32. Each answer goes only through the broker that
  brought the request. Thus a flood of requests does not use much of the
  bandwidth of the host.
- The host does the key work of at most 20 requests each second from keys
  that it does not know, after a first 32. It keeps the key work of the
  last 256 keys. Thus the proof of a player does not need more key work. A
  flood of requests can make players join more slowly. A player asks again
  for 90 seconds.
- The host refuses the predicted movement of a player whose game runs
  faster than time (a speed hack). If the messages on the player's
  connection were also ahead for ten seconds, the host drops and bans the
  player: each player sees who in red on the console, and the host adds a
  line to `cheaters.txt` and `bans.txt` (beside `debug.txt`) with the
  address and hardware id of the player, and the Discord name and id that
  the game of the player told it, marked `(self-reported)` (a player can
  change these). The host refuses a machine whose address or hardware id is
  in `bans.txt`. If only the player's datagrams were ahead, the host does
  not drop the player, because another machine can send datagrams with the
  player's address: it adds an `unverified` line to `cheaters.txt`.
- The host can ban a player with `ban <player name>` in the developer
  console (Tab completes the name). Remove a line from `bans.txt` to unban.
  Refer to `NETCODE.md`. `kick <player name>` drops the player the same
  way, but keeps nothing: no line in `bans.txt`, and the player can join
  again at once. In co-op, `bringto` brings every player to the host.
- Players can vote to kick a player. The host turns this on or off with
  VOTE KICK in Server Setup > Teamplay Options (`network.votekick`; in
  co-op, Server Setup > Voice and Voting). Hold the scoreboard key, right-click to
  show the pointer, and click the name of the player. Then click **Start
  a vote to kick**. Other players vote in the same way, and see the vote on
  the scoreboard. `votekick <player name>` in the developer console does
  the same. The host also gets **Kick** and **Ban** in this menu: these do
  the same as the `kick` and `ban` commands (click **Ban** two times). The
  host counts the votes, and these rules prevent abuse:
  - The vote passes when more than half of the players vote for it, and at
    least two players. The player of the vote is counted, but cannot vote.
    Thus, in a game of two equal teams, one team cannot kick a player of
    the other team without help.
  - The host counts one vote for each internet address (for internet
    play, the real address of the player, not the address of the tunnel).
    Two machines at one address, or with one hardware id, have one vote.
  - To start a vote, a player must have played for
    `network.votekick_minutes` (5) on this server. To vote, a player must
    have played for 2 minutes (or less, if that setting is less). The host
    counts the time. When a player joins again, the time starts again.
    Players who cannot vote yet are not counted.
  - The host only accepts a vote that comes on the connection of the
    player, not a datagram, which another machine can send with the
    address of the player.
  - One vote runs at a time, for 45 seconds, with 30 seconds before the
    next vote. If a vote fails, the player who started it cannot start a
    vote for 5 minutes, and nobody can start a vote against the same player
    for 10 minutes.
  - Nobody can vote to kick a player of the host.
  - A player kicked by a vote cannot join again for
    `network.votekick_ban_minutes` (30), by address and hardware id.
  - A player who leaves during a vote against them is kept out the same
    way, as if the vote passed. While the vote runs, the host refuses the
    player if they try to join again. Thus nobody can avoid a vote by
    leaving.
  So that every player can be named, the host trims the spaces around a
  name and removes characters that draw as nothing. A
  letter with a mark is typed as the plain letter (`ban jose` for "José").
  A name with nothing left to type becomes "Player", and a name that another
  player already has gets a number ("Player 2"). The game refuses a profile
  name that is blank, and a multiplayer game refuses a profile whose name was
  made blank before this check.
- An invite operates while the copy of the game that made it operates.

### Connection

Each machine gets its public address from public STUN servers. Then the two
machines send packets to each other until the packets get through (UDP hole
punching). There is no relay.

Some networks give a different port for each destination (for example some
mobile and company networks). Two machines behind such networks cannot
connect. To connect, forward `network.tunnel_port` on the router of one of
the machines.

The game can ask the router to forward the port (UPnP,
`src/posix_upnp.c`, with `port/third_party/miniupnpc`):

- The host asks its router when a player uses its invite.
- A player that joins asks its router when it does not reach the host in
  5 seconds.
- The forwarded port is one more address that the machine gives to the
  other machine.
- The forward has a duration of one hour. The game makes it longer while
  it operates. When the game stops normally, it removes the forward. It
  does not remove the forward after a crash, or if a request to the router
  is still under way 3 seconds after the game starts to stop. Some routers only make forwards without a duration.
- When the game finds the router, it removes the forwards to this machine
  that have the description "Halo internet play" and that no copy of the
  game uses now (forwards that a copy of the game did not remove).
- UPnP does not help behind a second NAT, for example the NAT of a mobile
  network provider. Then the router has a private address, and the game
  does not ask.

To stop all UPnP requests, set `network.allow_upnp` to `false`.

In the game, each machine has an address in 100.64.0.0/10:

- `src/xnet.c` gives the datagrams that the bound UDP sockets of the game
  send to such an address to `src/p2p.c`. Other datagrams (of sockets that
  are not bound yet, or that are connected to the address) and the TCP
  connections of the game go through local sockets on 127.0.0.1 (or
  `network.address`). The traffic from the other machines comes to the
  game from local sockets too.
- `src/p2p.c` sends that traffic through one UDP socket. UDP datagrams go
  as they are. TCP connections go as KCP streams (`port/third_party/kcp`).
- The broadcasts of the game go to all the machines. Thus the game of the
  host shows on the other machines.

### Discord

If the Discord desktop client operates, the game of the host shows in
Discord (through the application of `discord.application_id`). The activity
has a private party with the invite as its join secret. The host can send
the invite with the invite button of Discord. When a person accepts it, that
person joins the game. If the game does not operate, Discord starts it.
The game sends the activity only to a Discord client of the same user.

## Map checks

The game reads a map's tags straight into memory and uses them as its own
structures: every pointer, count, index and enum in them is the map's, and
the game writes values into tags as it runs. So before anything reads a
map's tags, the port checks every tag against a schema of its group
(`game/tag_schema_*.c`, read by `game/tag_validate.c`), and each structure
BSP as it loads:

- Every block and every piece of data must lie in the tags (or the BSP) and
  overlap no other. Otherwise the game refuses the map.
- A block with more elements than the game has room for is cut to the
  maximum. A tag reference that is not a tag of the right group becomes
  none. So do an index past its block and an enum past its values (or they
  become 0, where the game cannot take none). A string gets its terminator.
  Values that the game sets as it runs are reset.
- Checks that the schema cannot express run last: the BSPs' and the models'
  graphs, vertex and index buffers, and indices into other tags.

Each correction goes to `debug.txt`. The game's own maps need none.
`build/linux/map_validate [--strict] map.map...` runs the same checks on map
files without the game, and `tools/test_linux_port.py` runs it on the maps
in `assets/maps`. `map_validate --fuzz <runs> map.map` changes a few words
of the tags at random in each run. The checks must not crash or hang, and a
map that they let through must need no more corrections.

A map's scripts can call only the script functions that a map needs (the
allowlist in `hs/hs.c`). They cannot call the functions for files, the
saved state of the game, the console, debugging or cheats. A script that
calls one does not run. The developer console can call every function.

Halo Custom Edition maps get the same checks (those that need OpenSauce are
refused). Their own loader (`game/cache_file_formats.c`) reads them into
their tag cache at 0x40440000 and converts what Custom Edition lays out
differently, then the validator checks their tags and each of their BSPs as
it checks this build's maps, before the game converts their models, BSP
geometry and scripts. Put them with `bitmaps.map`, `sounds.map` and
`loc.map` in `custom_maps`, beside `maps`, or set `paths.custom_edition` to
a Custom Edition install; the map lists show them as CUSTOM SINGLEPLAYER and
CUSTOM MULTIPLAYER, played as campaign levels (alone, or as network co-op)
or as multiplayer maps by their scenario type, and `game.custom_edition =
false` refuses them. `map_validate` checks them too, with the resource maps
beside each map or in `--maps <folder>`. See
`docs/custom_edition_caches.md`.

Defensive checks stay in the game code too. An index into a tag block, the
tags or a tag's data that is out of range gets zeros (`tag_empty_data` in
`tag_files/tag_groups.c`), not other memory.

A map's name must be its file's: the cache file slots are found by the name
in the map's header, so a map file whose header names another map (a
renamed one) is refused, not copied again for ever. The `loading.tga` a map
pack may put in the maps folder is read only if it is an uncompressed 24-bit
picture of 320 by 240, the loading screen's texture.

A checkpoint (`savegame.bin`, in the profile's folder) and a core are
images of the game state's memory: with the data arrays' pointers to their
elements, the objects' memory pool's blocks and the references to them, and
the caches' procedures. Before one is taken, each of those is checked
against what the game made at startup (`game_state_image_accept` in
`saved games/game_state.c`): an image that does not match (a damaged or
crafted file) is refused, and the level starts over.

## 64-bit ARM

`ninja linux_arm64` builds the game for 64-bit ARM Linux. It was tested on
a Steam Frame (SteamOS, Adreno 750 with Mesa). Enter it on a 64-bit ARM
computer: on such a computer, `ninja` without a target builds it.

| Result | Item |
| --- | --- |
| `build/linux_arm64/halo` | The game, an AArch64 executable |
| `build/linux_arm64/libSDL3.so.0` | SDL 3.4.16. The game finds it next to the executable. |

The game data, the settings, the saved games, the controls and system link
are the same as on x86. Linux arm64, x86 Linux, Windows and Android
machines can play in the same game.

### Requirements

- Python and ninja.
- clang with the `arm64_32` target (clang 22 operates), `ld.lld` and
  `llvm-ar`. The option `--linux-arm64-cc` of `configure.py` selects a
  different clang.
- The OpenGL ES and EGL headers (`libgles-dev` and `libegl-dev` on Debian
  and Ubuntu).
- CMake, and the development files that SDL3 uses for X11, Wayland and
  sound. Refer to the SDL documentation, or to the `linux-arm64` job in
  `.github/workflows/build.yml`.
- A network connection for the first build. `configure.py` downloads musl
  1.2.5 and SDL 3.4.16 to `build/linux_arm64/third_party`.

To start the game: OpenGL ES 3 (`libGLESv2.so.2`, `libEGL.so.1`, from Mesa
for example). The release builds of GitHub Actions use glibc 2.35 or later.

### How it operates

Recent 64-bit ARM processors (the Steam Frame's, for example) cannot
execute 32-bit ARM code, and few distributions have 32-bit ARM libraries
(SDL3, Mesa). Thus this build operates as the Android port does: the game is
ILP32 AArch64 code, a guest image in a 64-bit process. Refer to "How the
port operates" in [port/android/README.md](../android/README.md#how-the-port-operates).

- The guest image is the image of the Android build
  (`generate_guest_image` in `tools/android_build.py`), with the desktop
  code paths of the platform layer. The guest code has `HALO_ARM64_GUEST`
  and `HALO_GLES`, not `HALO_ANDROID`.
- The host is the Android host (`port/android/host`) as a Linux executable.
  `arm64/host_main.c` replaces its `host_main.c`. The executable contains
  the guest image (`arm64/guest_image.S`). The guest gets the variables of
  the environment that it reads (the `HALO_` settings, `HOME`, the XDG
  folders), so the `HALO_` settings operate.
- The desktop code paths use more of SDL than the Android app:
  `arm64/guest_desktop.c` and `arm64/host_desktop.c` supply it, and
  `arm64/host_imports.list` lists their imports. The guest does SDL's
  threads, mutexes, atomic values, files and `SDL_GlobDirectory` itself.
  The host does the windows, the displays, the 2D renderer, the dialogs
  and the downloads of the self-updater (`src/posix_update.c`).
- The renderer uses OpenGL ES, as on Android.
- The host keeps the GPU driver from the kernel's `trace_marker`, as the
  x86 build does (`src/posix_trace_marker.c`, under "The platform layer"
  below): on the Steam Frame, its markers took the game from the headset's
  72 Hz to about 50 frames a second.

## VR

`python3 configure.py --vr`, then `ninja linux_arm64`, builds the 64-bit
ARM game with a VR mode: the game plays in an OpenXR headset, in stereo,
with the head tracked in 6 degrees of freedom. It was made on a Steam Frame
with SteamVR. The VR build draws with desktop OpenGL 4.5
(`--linux-arm64-gl=desktop`, which `--vr` implies); it needs no OpenXR
package, as it loads the active runtime itself (or a
`libopenxr_loader.so.1` where there is one). Configuring it downloads the
OpenXR SDK's four headers (to `build/linux_arm64/third_party/openxr`,
checked against their SHA-256 in `tools/linux_arm64_build.py`).

To play, start SteamVR, then start `build/linux_arm64/halo` (from a
terminal of the headset's desktop session, or as a non-Steam game in
Steam). Without a runtime or a headset the game plays flat, in its window.
`vr.enabled = false` (or `HALO_VR=0`) plays flat too.

The VR build doesn't update itself (`update.auto` has no effect): the
releases it would update to have no VR mode. Install a new VR build by
hand.

### Controls

The Steam Frame's controllers are the halves of an Xbox controller:

| Controller | Game |
| --- | --- |
| Left stick | Move (click: crouch) |
| Head | Crouch: crouch in the room, and the game crouches (`vr.physical_crouch`) |
| Right stick, left and right | Turn: in steps (`vr.turn = "snap"`) or smoothly (`"smooth"`) |
| Right stick click | Zoom (with `vr.scopes`, a raised scope's magnification: below) |
| Head | Look |
| Right controller | Aim: it holds the weapon, and the reticle shows where it points (`vr.aim = "controller"`). With `vr.aim = "head"`, the head aims and the reticle stays in the middle of the view. |
| Left controller | The left hand, where the controller is; the left grip closes it (`vr.hands`). The right trigger pulls the right index finger too. |
| Right trigger, left trigger | Fire, throw a grenade |
| A, B, X, Y | Jump, melee, action and reload, switch weapon |
| Left grip, the left hand on a long gun's foregrip | Aim it with both hands, along the line from the right hand to the left (`vr.two_handed`) |
| A punch of the right controller | Melee (`vr.melee_gesture`) |
| The right hand over the right shoulder or at the right hip, gripping | Draw the weapon carried there (`vr.holsters`, below) |
| The sniper rifle's or the pistol's scope raised to an eye | Look through it, magnified (`vr.scopes`, below) |
| The right stick pushed down, then the left hand gripping at the left hip and pushing what it took into the weapon | Reload by hand: eject the magazine, take another and push it in (`vr.manual_reload`, below) |
| The left grip held, the left hand drawn back and swung, the grip let go | Throw a grenade where the hand threw it, as hard as it did (`vr.grenade_throw`, below) |
| Left bumper, right bumper | Flashlight, switch grenade |
| D-pad (left controller), View, Menu | The menus' d-pad, Back (scoreboard), Start (pause) |
| Both grips, held | Recentre: the head's place and heading become the player's eye and facing (not while the left hand holds a foregrip) |
| A gamepad's Back (View), held 1 s | Recentre, as both grips do |

Other controllers that SteamVR maps to the Touch layout operate as well
(the grips as the bumpers).

#### Throwing a grenade with the left hand (`vr.grenade_throw = "gesture"`)

Throw it as you would a ball: squeeze the left grip to take a grenade (a
short buzz says you hold one; only with a grenade to throw, on foot), draw
the hand back (beside your head, or low for a lob), swing it forward and
open the hand. The grenade leaves from your hand as you let go, along the
way the hand was going (lifted 8 degrees), as hard as you threw it: a
throw of 4 m/s or faster throws as far as the left trigger does, slower
ones shorter (to 30% of it), never farther. The left trigger still throws
as ever, and the right bumper switches the grenade. From the moment you
take it until it flies, the grenade (frag or plasma, the one selected) is
in your left hand, whatever `vr.hands` is, and it leaves from just where
you held it.

- What counts as a swing: the hand moving forward (the way your head
  faces) faster than 1.6 m/s, with the grip held for 0.1 s at least, from
  no farther than 15 cm ahead of your eyes since you took it. A hand held
  out (on a foregrip, reaching) and pushed forward is no throw; nor is the
  grip let go of before a swing.
- The swing starts the game's throw (as the left trigger does), and the
  game's grenade is then yours: a swing held without letting go throws it
  anyway after 0.75 s.
- The left grip on a long gun's foregrip takes the foregrip instead
  (`vr.two_handed`), and both grips held recentre: neither takes a grenade.
- Your left hand and arm are the throw in first person: the weapon stays
  in your right hand (with `vr.hands = "game"` the game's arms throw, timed
  to yours). The player's body throws as you do: its throw waits before
  letting go while you hold on, and lets go when you do (below, "How it
  operates", for what the other players of a network game see).

`debug.vr_throw_log = true` logs each step of a throw (the grip, the
swing or why it was not one, the release's speed and direction, the
game's throw timed to it); the thresholds are named constants at the top
of `src/vr.c`.

#### Holsters (`vr.holsters`)

You carry two weapons and hold one; the other is on your body, where you
can reach for it: a long gun over your right shoulder, a pistol or the
needler at your right hip. Reach there with your right hand and squeeze
the right grip to draw it: the game switches to it (as Y does, through the
game's switch, which others in the game see as ever), and the one you held
takes its place in its own holster. The weapon in its holster lights up as
your hand comes near it, and the right controller ticks as your hand gets
there, so you can find it without looking: your shield's energy wraps it,
in pale blue, rising from about 35 cm away and full once your hand is
there, and its metal takes on the blue. The shoulder's holster, out of
sight, ticks too as your hand nears it, from about 30 cm, the ticks
quicker and stronger the nearer, until the tick of getting there (the
hip's does not: your hand passes by it and rests by it).

- The holsters follow your body, which faces where your head does, but
  only once your head turns more than 40 degrees from it, and then slowly:
  looking about does not move them, nor does looking down (they hang from
  your neck, as your body does, not from your eyes, which a nod moves
  forward). Recentring (both grips) sets them again. Your body drawn
  (below) and your arms' shoulders face the same way.
- The shoulder's holster is behind and just above your right shoulder,
  the hip's at your right hip, about 70 cm below your eyes: from about
  20 cm away from either, your hand is there.
- A gripping hand in the other holster (with nothing in it) does nothing.
  A punch does not melee while your hand is in a holster, nor for half a
  second after you draw (drawing is a fast move forward).
- Only on foot, with `vr.aim = "controller"`.

#### Scopes (`vr.scopes`)

The sniper rifle's and the pistol's scopes magnify as real ones do: raise
one to your eye and look through it, and the view within its lens is
magnified, while the rest of your view stays as it was (the flat game
narrows the whole view instead, which in a headset would swing the world
with your head). The lens shows to the eye looking through it; your other
eye sees the scope from outside, as it would.

- The sniper rifle's is a rifle scope, held at the eye: bring the back of
  its scope (where its display is) within about 12 cm of an eye, looking
  along it. Its lens, about 5 cm across, fills a good part of the view.
- The pistol's (the smart-linked scope on the front of its slide) is a
  pistol scope, of long eye relief: hold the pistol out, up to about 65 cm
  from your eye, and line your eye up with the scope (within about 4 cm of
  its line). Its lens is shown as a window of at least 12 degrees, larger
  than the scope itself: its sight's display.
- Raised, it is as many times larger in the lens as the game's zoom is
  (the sniper rifle 2x, the pistol 2x), and the right stick's click steps
  through the levels (the sniper rifle's 2x and 10x, and back). A scope
  raised again has the level it had last, until you switch weapons.
  Lowered, nothing zooms, and the click does nothing.
- The scope also counts as the game's zoom (sent to the other players as
  ever: the zoom's sounds, and what it does to the weapon), so what
  unzooms the game unzooms the scope: a hit, a reload, a grenade. Its lens
  then shows the world at its own size until you click.
- The lens has a reticle (fine lines through the middle, thick posts from
  its edge, and a faint red dot), which is where the shot goes: the world's
  reticle is not drawn while a scope is raised. It must point within about
  20 degrees of where your head looks; it is lowered past 25 degrees, or
  once your eye leaves it by a quarter more than it came in.
- Only with `vr.aim = "controller"`, on foot. Other weapons that zoom (the
  rocket launcher) zoom as the game does, which magnifies nothing in the
  headset; the first-person weapon stays in your hand while zoomed.

#### Reloading by hand (`vr.manual_reload`)

Reload as you would a real gun: push the right stick down to eject the
magazine (a buzz in the right hand; the weapon is empty now, and does not
fire), reach to your left hip with your left hand and squeeze the left grip
to take a magazine (a tick as your hand gets there, a firmer one as you
take it), and push it into the weapon where it goes, still gripping (a
knock in both hands). The game then reloads, as X does: the same
ammunition, the same time, and others see your body reload; but your
weapon stays in your hand, without the flat screen's reload animation.

- Where it goes is where the weapon's own magazine is, in its first-person
  model (on the assault rifle, a bullpup, behind the grip, under your
  forearm). The magazine you hold is the weapon's own, drawn in your hand;
  the needler, whose model has none, has its ammunition's pickup, which goes
  in under its body, ahead of your hand. While your hand holds one, where it goes lights up
  in your shield's energy, as the holsters do, the more the nearer it comes;
  within about 7 cm of it, it goes in. While the magazine is out, the
  weapon is drawn without it, and the magazine at your hip lights up as your
  hand comes to it.
- The shotgun is fed a shell at a time, with nothing to eject: take a shell
  from your hip whenever it has room, and push it in at the loading port,
  under the receiver ahead of the grip. Each shell is one round of the
  game's reload; the game stops after it unless you have pushed in another
  meanwhile (on the flat screen it loads to full).
- The rocket launcher: the right stick ejects its tubes, and new ones are
  behind your left shoulder (where your left hand reaches over it, as the
  right one does for a long gun).
- The plasma weapons have no magazine, and are as ever.
- An empty weapon does not reload itself, as it does on the flat screen:
  reload it by hand, or with X, which reloads as ever (with its animation).
- A grip that took a magazine is the reload's until you let go: it holds no
  foregrip and takes no grenade. Letting go before the magazine is in drops
  it; take another.
- In a network game the host runs your weapon: as a client, its copy
  reloads an empty weapon by itself and loads the shotgun to full, and
  yours then takes the host's ammunition, as it always does. Your hand's
  reload is a press of reload there, as X is.
- Only on foot, with `vr.aim = "controller"`.

#### Your body (`display.first_person_legs`)

With LEGS on (Video Setup > FOV and Viewmodels, `display.first_person_legs`),
looking down in the headset shows Master Chief's body under you: the
chest, the legs and the feet, but not his head nor his arms (your arms are
the VR mode's own, `vr.hands`, from shoulders over his). The body is where
yours is, not where the game's eye is:

- Its neck is under your head: at the top of your neck, about 10 cm behind
  and 10 cm below your eyes as your head is turned and tilted, so that it
  stays put as you nod and look down, and moves with you as you lean or
  step about the play space. It stands on the ground as the game has it.
- It faces your body's heading, the holsters' (above): where your head
  faces, once your head turns more than 40 degrees from it, and then
  slowly; not where the gun aims. Your arms' shoulders (`vr.shoulders`)
  face the same way, from the same neck.
- It stands upright and square over its hips, as you do: the game's body
  leans into its aim, its chest turned to the gun while its feet stay
  planted. Its legs walk, run and crouch as the game animates them.
- It is not set back behind the eye as on the flat screen: its chest
  closes its waist. Looking straight down, the Chief's broad chest hides
  his feet below it; they show as they step forward, or looking less
  steeply.

The body is the one everyone else sees, drawn differently for you alone:
hit boxes, markers and what others see are the game's. It is drawn on
foot only, as on the flat screen (not in a vehicle's seat).

#### Crouching (`vr.physical_crouch`)

Crouch in the room and you crouch in the game; stand up and you stand.
Your head about 35 cm below your standing eyes (`vr.height = "standing"`:
`vr.player_height` above the floor), or 25 cm below where it was at the
recentring (seated), crouches; back up within 7 cm of that, you stand. The
game crouches as the left stick's click does, held while your head is down
(Halo's crouch is held, not toggled), and through its input and the
netcode as ever: your body is lower, and moves as a crouched player does
(at a full stick too, which would stand a crouch by the stick).

- The view follows your head alone: the game lowers its eye as it crouches
  (0.82 m for the Master Chief, at `vr.world_scale` 1), which is taken back
  out while the crouch is your head's. A crouch by the stick's click keeps
  the game's drop.
- Only on foot, not in a vehicle's seat nor in the menus; any `vr.aim`.

#### With a gamepad (`vr.aim = "gamepad"`)

A gamepad (an Xbox controller, by Bluetooth or USB, or the headset's
controllers as its two halves) plays as on the flat screen, with the
profile's button layout, look sensitivity and acceleration, and the aim
assist: the left stick moves, the right stick turns and aims, and the
triggers, bumpers, buttons, d-pad, Start and Back are the game's. The
headset shows the world, and the head looks about from where the stick
points the view (`vr.gamepad_view`):

| | Does |
| --- | --- |
| Right stick | Turns and aims the player, as on the flat screen. With `vr.gamepad_view = "camera"` (the default) it is the camera, as on the Xbox: it turns and pitches the view smoothly, and you aim at the middle of the view. With `"level"` it turns the view smoothly but never pitches it: its pitch moves the weapon and the reticle, not the horizon. With `"snap"` the view's heading turns by `vr.snap_turn_angle` each time the aim is more than that from it (the aim sweeps across the view, which turns in steps), and its pitch moves only the weapon and the reticle. |
| Head | Looks about from the view the stick points, in 6 degrees of freedom, and never moves the aim: with the head straight the reticle is in the middle (with `"camera"`), and a glance looks elsewhere while the reticle stays where the aim is. |
| Back (View), held 1 s | Recentre: where the head looks becomes where the player aims |

`"camera"` is the most like the Xbox game, and the most likely to make you
sick: the stick turns and tilts the whole world while your head is still.
`"level"` keeps the horizon level, and `"snap"` turns it in steps, which
most people find the most comfortable.

The weapon is posed as on the flat screen from the player's eye looking
along the aim, so it stays before the body whichever way the head looks.
The reticle is in the world, where the aim meets it (as far as what it
aims at, in both eyes), as large as in the HUD.
The headset's controllers are an Xbox controller's halves here: their
sticks, buttons and triggers are a gamepad's (the right stick looks, never
snap-turns), where they point aims nothing, and a punch does not melee;
they rumble with the gamepad. Both grips held still recentre. In the headset
every gamepad is the first player's: Steam may list a gamepad of its own
(Steam Input's) besides the one in the hands, and all of them merge into
controller 1 (each stick pushed furthest wins). In a vehicle the view is
as `vr.vehicle_view` says, and the vehicle steers and aims where the stick
does.

### Settings

In the headset, Settings' VR Setup (below About; only there while the
game plays in VR) sets most of these with the left d-pad: VR Controls
(the hands, the aim, the gamepad's view, two-handed aiming, the melee
punch, the holsters, the physical crouch, the scopes, reloading by hand, turning and the vehicles' view) and, from its last row, VR Display (the play position,
the eye height, the world's scale, the HUD's and the menus' distance and
size, the resolution and the refresh rate) and, from its last row, its
visor page (the HUD on the visor or flat, its depth, lag and glow, the
helmet's rim and its depth, the glass's shade, the shields' effects and
the strength of their energy). OK writes them to
`config.toml`, and they apply at once, but those marked RESTART:
`vr.height`, `vr.resolution_scale` and `vr.refresh_rate`, which the
headset's session is opened with, apply from the next start.

| Setting | Default | Environment variable | Function |
| --- | --- | --- | --- |
| `vr.enabled` | `true` | `HALO_VR` | `false`: play flat. |
| `vr.resolution_scale` | `1.0` | `HALO_VR_RESOLUTION_SCALE` | The eyes' images, as a multiple of the size that the headset recommends (from `0.25` to `2`). |
| `vr.refresh_rate` | `90.0` | `HALO_VR_REFRESH_RATE` | The refresh rate to ask the runtime for. `0`: the runtime's. |
| `vr.aim` | `"controller"` | `HALO_VR_AIM` | `"controller"`: the right controller aims and holds the weapon. `"head"`: the head aims. `"gamepad"`: a gamepad aims and plays, as on the flat screen, and the head looks about freely (above, "With a gamepad"). |
| `vr.gamepad_view` | `"camera"` | `HALO_VR_GAMEPAD_VIEW` | With `vr.aim = "gamepad"`: `"camera"`, the stick turns and pitches the view as the flat screen's camera, the head's turn on top (the most like the Xbox game, and the most likely to make you sick); `"level"`, it turns the view smoothly, and its pitch moves only the reticle; `"snap"`, the view turns a step of `vr.snap_turn_angle` each time the aim is more than that from it, and its pitch moves only the reticle. |
| `vr.weapon_offset` | `"0.15, -0.22, 0.30"` | `HALO_VR_WEAPON_OFFSET` | With `vr.hands = "game"`: where the weapon's grip is from the eye that the game poses it for, in metres: right, up, forward. |
| `vr.depth` | `false` | `HALO_VR_DEPTH` | `true`: give the runtime the eyes' depth too (`XR_KHR_composition_layer_depth`), for its reprojection. SteamVR on the Steam Frame cannot make a depth swapchain for an OpenGL session yet. |
| `vr.melee_gesture` | `true` | `HALO_VR_MELEE_GESTURE` | A punch with the right controller (fast, in the direction it points) melees, as B does. |
| `vr.holsters` | `true` | `HALO_VR_HOLSTERS` | With `vr.aim = "controller"`: the weapon you carry but do not hold is on your body, and the right hand reaching there and gripping draws it (above, "Holsters"). Y switches weapons as ever. |
| `vr.physical_crouch` | `true` | `HALO_VR_PHYSICAL_CROUCH` | The head crouching crouches the player: about 0.35 m below the standing eyes (`vr.height = "standing"`), or 0.25 m below the recentred ones (seated), and standing within 0.07 m of that stands (above, "Crouching"). The left stick's click crouches as ever. |
| `vr.scopes` | `true` | `HALO_VR_SCOPES` | With `vr.aim = "controller"`: the sniper rifle's and the pistol's scopes magnify the view within their lens while raised to an eye (above, "Scopes"); lowered, they do not zoom. `false`: they zoom as the game does (nothing is magnified in the headset). |
| `vr.manual_reload` | `true` | `HALO_VR_MANUAL_RELOAD` | With `vr.aim = "controller"`: reload by hand (above, "Reloading by hand"): the right stick pushed down ejects the magazine, the left hand takes another from the left hip and pushes it in; the shotgun's shells one at a time; an empty weapon does not reload itself. X reloads as ever. |
| `vr.grenade_throw` | `"gesture"` | `HALO_VR_GRENADE_THROW` | With `vr.aim = "controller"`: `"gesture"`, a grenade is thrown by throwing it with the left hand (above, "Throwing a grenade with the left hand") or with the left trigger; `"button"`, with the left trigger alone. |
| `vr.two_handed` | `true` | `HALO_VR_TWO_HANDED` | With `vr.aim = "controller"`: a long gun (any but the pistols, the needler, the flag and the ball) is aimed along the line from the right hand to the left while the left grip holds it by the foregrip (pressed within 18 cm of the line ahead of the right hand). The aim turns to that line over 0.15 s, smoothed, and back when the grip is let go. |
| `vr.hands` | `"arms"` | `HALO_VR_HANDS` | With `vr.aim = "controller"`: each first-person hand where its controller is (the right holding the weapon, its index finger on the trigger as far as the trigger is pulled; the left open, closing as the grip is pulled, and on a long gun's foregrip while it holds it). `"arms"`: the arms reach the hands from the shoulders (`vr.shoulders`). `"floating"`: no arms above the wrists. `"game"`: the game's arms, posed from the weapon in the right hand. A hand whose controller is not tracked is not drawn, nor its arm. |
| `vr.shoulders` | `"0.19, -0.20, -0.07"` | `HALO_VR_SHOULDERS` | With `vr.hands = "arms"`: where each shoulder is from the eyes, in metres: out to its side, up, forward, turned with the body (the holsters' heading: it faces where the head does, follows it past a 40 degree turn, and slowly). From the eyes as they would be with the head level and turned with the body: the shoulders hang from the neck, so a nod or a turn of the head does not move them. |
| `vr.vehicle_view` | `"first_person"` | `HALO_VR_VEHICLE_VIEW` | `"first_person"`: in a vehicle's seat, the view is from the seated player's head, level with the horizon. `"third_person"`: the game's camera following the vehicle. |
| `vr.seat_motion` | `"still"` | `HALO_VR_SEAT_MOTION` | With `vr.vehicle_view = "first_person"`: `"still"`, the eye stays put in the vehicle, riding with it but not with the body climbing in and out, a turret's turning or flinches, and blinks (darkens and lightens in a quarter second) where it moves between places; `"head"`, it follows the seated body's head as the game animates it. |
| `vr.turn` | `"snap"` | `HALO_VR_TURN` | `"snap"` or `"smooth"` (with `vr.aim = "gamepad"`, `vr.gamepad_view` instead). |
| `vr.snap_turn_angle` | `30.0` | `HALO_VR_SNAP_TURN_ANGLE` | Degrees of each snap turn. |
| `vr.smooth_turn_speed` | `150.0` | `HALO_VR_SMOOTH_TURN_SPEED` | Degrees each second of smooth turning. |
| `vr.height` | `"seated"` | `HALO_VR_HEIGHT` | `"seated"`: the head where it is at the recentring is the player's eye. `"standing"`: the head `vr.player_height` above the floor is. |
| `vr.player_height` | `1.65` | `HALO_VR_PLAYER_HEIGHT` | Standing, the height of your eyes, in metres. |
| `vr.world_scale` | `1.0` | `HALO_VR_WORLD_SCALE` | More than `1` makes the world look larger. |
| `vr.hud` | `"visor"` | `HALO_VR_HUD` | `"visor"`: the HUD on the helmet's visor (below, "The HUD on the visor"). `"flat"`: a flat screen in front of the eyes. |
| `vr.hud_distance`, `vr.hud_size` | `2.0`, `60.0` | `HALO_VR_HUD_DISTANCE`, `HALO_VR_HUD_SIZE` | The HUD's distance in front of the eyes (metres) and width (degrees of the view). On the visor, the radius of its curve and the angle that it spans. |
| `vr.hud_lag` | `0.03` | `HALO_VR_HUD_LAG` | With `vr.hud = "visor"`: how far the HUD trails the head's turns, as the time constant of its smoothing in seconds (up to `0.2`), never more than 1.5 degrees behind. `0`: the HUD turns with the head exactly. |
| `vr.hud_depth` | `1.0` | `HALO_VR_HUD_DEPTH` | With `vr.hud = "visor"`: how far apart in depth the HUD's parts are, times their own distances from the HUD's (the four below), from `0` (all at `vr.hud_distance`, one layer) to `2`. Never nearer than 1 m. |
| `vr.hud_depth_tracker`, `vr.hud_depth_status`, `vr.hud_depth_weapon`, `vr.hud_depth_world` | `-0.55`, `-0.35`, `-0.25`, `0.5` | `HALO_VR_HUD_DEPTH_TRACKER`, `HALO_VR_HUD_DEPTH_STATUS`, `HALO_VR_HUD_DEPTH_WEAPON`, `HALO_VR_HUD_DEPTH_WORLD` | The distances from the HUD's (metres, at `vr.hud_depth` 1; nearer less than 0) of the motion tracker; the shields and the health; the weapon, its ammunition and the grenades; and the waypoints and the players' names. At the defaults: 1.45, 1.65, 1.75 and 2.5 m, the rest of the HUD (the crosshair, the messages) 2 m. |
| `vr.hud_tracker_tilt` | `20.0` | `HALO_VR_HUD_TRACKER_TILT` | How far the motion tracker tilts back, in degrees (-45 to 45). |
| `vr.hud_glow` | `0.5` | `HALO_VR_HUD_GLOW` | With `vr.hud = "visor"`: the HUD's glow on the glass, from `0` (none) to `1`. |
| `vr.helmet_rim` | `1.0` | `HALO_VR_HELMET_RIM` | With `vr.hud = "visor"`: the helmet's faceplate about the visor, from `0` (none) to `1`: its lip at the very edges of the view, in depth, the brow and the chin, never a frame. |
| `vr.helmet_rim_depth`, `vr.helmet_rim_reach` | `0.10`, `0.07` | `HALO_VR_HELMET_RIM_DEPTH`, `HALO_VR_HELMET_RIM_REACH` | How far before the eyes the rim's lip is (metres, `0.05` to `0.25`), and how far in from the edges of the view it begins (a fraction of them, `0` to `0.2`): at the defaults, about 2 degrees of each eye's view at its outer edge, its top and its bottom (the log says how much). |
| `vr.visor_frame` | `0.4` | `HALO_VR_VISOR_FRAME` | With `vr.hud = "visor"`: the glass: the helmet's shadow on it from the brow and the chin, faint at the sides, and its faint gold, from `0` (none) to `1`. |
| `vr.visor_effects` | `true` | `HALO_VR_VISOR_EFFECTS` | With `vr.hud = "visor"`: the shields on the glass: their energy across it (the armour's own, below, "The HUD on the visor") as they are hit and as they recharge, rising to their level and flashing as they are full; its rim pulses red while they are low or gone (or while the health is low without them), giving way to their energy as they recharge; the HUD flickers as they break, and comes up as you take control. |
| `vr.visor_energy` | `1.0` | `HALO_VR_VISOR_ENERGY` | With `vr.visor_effects`: how strong the shields' energy on the glass is, from `0` (none) to `2`. |
| `vr.menu_distance`, `vr.menu_width` | `2.5`, `2.6` | `HALO_VR_MENU_DISTANCE`, `HALO_VR_MENU_WIDTH` | The menus' distance and width, in metres. |
| `debug.vr_force_render` | `false` | `HALO_VR_FORCE_RENDER` | Draw the frames while the runtime says not to (the headset not worn), to measure them. |
| `debug.vr_test_turn` | `0.0` | `HALO_VR_TEST_TURN` | Turn the player this many degrees a second, for automated tests without hands. |
| `debug.vr_test_jitter` | `0.0` | `HALO_VR_TEST_JITTER` | Move the head by up to this many millimetres (and turn it as many hundredths of a degree) at random each frame, as a worn headset moves: for automated tests of what flickers when the view barely moves. |
| `debug.vr_test_head` | `""` | `HALO_VR_TEST_HEAD` | Hold the head still and level where it is first located (a simulated headset's head wobbles), for automated tests: `"0"`, or `"<degrees> <seconds>"` turns it left and right by as many degrees over as many seconds, smoothly (a sine of the frames' display times); `"<degrees> <seconds> <pitch>"` also pitches it up by as many degrees (less than 0: down), as looking down at the hip's holster. |
| `debug.vr_test_hands` | `""` | `HALO_VR_TEST_HANDS` | Hold the controllers still, for automated tests without hands: `"lx ly lz lyaw lpitch lroll, rx ry rz ryaw rpitch rroll[, grip[, buttons[, trigger[, head[, frames[, move[, down]]]]]]]"`, the left and right hands in metres right, up and forward from the recentred head and degrees of yaw (left), pitch (up) and roll (right) (a hand 10 metres or more out is not tracked), the left grip's pull (0 to 1), the buttons pressed for the pose's first 20 frames (A 1, B 2, X 4, Y 8, the right trigger 65536, the left 131072, the right grip 262144, the right stick pushed down 524288), the right trigger's pull (0 to 1), the head turned (degrees left), the frames the pose lasts (`0`: 288), the frames the hands take to move to it from the pose before, a step a frame (`0`: none; a swing, as fast as its steps), and the head lowered (metres, as a crouch; moved with the hands: below). A pose shorter than 20 frames presses its buttons only as long. Up to 16 poses separated by `;` are taken in turn (the log says from which frame). With the hands posed, a cross marks each controller's grip and a line where it points, and white lines the arms' bones. Below, a grenade throw scripted. |
| `debug.vr_test_weapons` | `""` | `HALO_VR_TEST_WEAPONS` | Give the first player these weapons in place of their own, the first in hand, once a map, 2 s after the player first aims (a level's script gives its own as the player takes control), for automated tests of the scopes: `"<name>[, <name>]"`, parts of the weapons' tag names (`"sniper rifle, pistol"`; b30 has no sniper rifle, a50 has both). |
| `debug.vr_throw_log` | `false` | `HALO_VR_THROW_LOG` | Log each step of a grenade thrown with the left hand (`vr.grenade_throw`): the grip taken (or why it holds nothing), the swing (or why a fast hand was none), the release's speed, power and direction, and the game's throw: when it began, its animations held or moved on to their release, and the grenade's place and speed. |
| `debug.vr_test_weapon` | `""` | `HALO_VR_TEST_WEAPON` | Put a weapon in the first player's hand, for automated tests of the reload by hand: `"<text> <seconds>"`, that many seconds into the level the player's weapons are taken and a new one is in hand, the first of the map's whose tag name has the text in it (`"shotgun 15"` on d20, `"rocket 15"` on b30). |
| `debug.vr_test_seat` | `""` | `HALO_VR_TEST_SEAT` | Get into a vehicle's seat and out again, for automated tests of the view in a seat: `"<label> <seconds> [<seconds seated>]"`, the first player gets into the seat whose label has this text in it (`"gunner"`, `"W-driver"`, `"GT-gunner"`: the Shade) that many seconds into the level, as the action button does, in a vehicle of the kind made before them three seconds earlier (standing at the seat's entrance from two seconds earlier), and out again after as many seconds seated (10 by default). From a second before, the log says how far the view moved and turned each frame, how dark a blink made it, and each phase's sums (on foot, entering, seated, exiting) with how much of the motion was seen (not in a blink's full darkness), and each blink: with `debug.vr_test_head = "0"`, the motion the player did not make. |
| `debug.vr_test_fov` | `""` | `HALO_VR_TEST_FOV` | Each eye's field of view instead of the runtime's, for automated tests of what the edges of a wide view show: `"<outward> <inward> <up> <down>[ <cant>]"`, in degrees from the eye's axis, each eye turned outward by `<cant>` degrees (as canted displays are; the game's 3D view assumes the eyes look the same way, so only the visor and the helmet's rim are right with it). |
| `debug.vr_gpu_time` | `false` | `HALO_VR_GPU_TIME` | With `debug.gpu_stats`, log the GPU's time for each frame too, waiting for it at the present (which costs the overlap of the GPU and the game, and lets the GPU's clock drop: an upper bound). |

`debug.test_input = "pad"` (`HALO_TEST_INPUT=pad`) puts the gamepad mode
through its paces without a gamepad: it turns, looks up and down, fires,
throws a grenade, holds Back (recentring) and walks, a few seconds each in
turn, and logs each.

A grenade thrown with the left hand, scripted (b30, whose first throw the
game allows about 45 s in at 72 Hz), the right hand holding the weapon
ahead (`R` here stands for `0.2 -0.3 0.4 0 0 0`), the head still
(`HALO_VR_TEST_HEAD=0`), with `HALO_VR_THROW_LOG=true`:

```
HALO_VR_TEST_HANDS="-0.25 -0.45 0.15 0 0 0, R, 0, 0, 0, 0, 3200, 0;
  -0.20 0.05 -0.10 0 30 0, R, 1, 0, 0, 0, 90, 36;
  -0.15 0.0 0.45 0 0 0, R, 1, 0, 0, 0, 8, 8;
  -0.12 -0.1 0.6 0 -20 0, R, 0, 0, 0, 0, 400, 6"
```

(on one line): the left hand low for 3200 frames, then taking a grenade
and drawn back beside the head over half a second, swung 0.55 m forward
in 8 frames (5 m/s) and let go of, which logs the swing, the game's throw
beginning, its animation moved on to the release, and the grenade thrown
from the hand.

A crouch scripted (seated, `R` and `L` the hands at their places, `R4` and
`L4` 0.4 m lower), with the head still:

```
HALO_VR_TEST_HANDS="L, R, 0, 0, 0, 0, 1200, 0, 0;
  L4, R4, 0, 0, 0, 0, 576, 72, 0.40;
  L, R, 0, 0, 0, 0, 576, 72, 0"
```

stands, crouches 0.4 m over 72 frames, holds it, and stands up again: the
log says when the crouch is on and off (`vr: crouch:`) and, once a tick
while the crouch or the head moves, how high the view is above the
player's feet: the game's eye, what of its drop is taken back out, and the
head's own (`vr crouch:`).

A reload by hand, scripted (b30, the assault rifle; `R` as above, the head
pitched down, `HALO_VR_TEST_HEAD="0 1 -35"`):

```
HALO_VR_TEST_HANDS="-0.25 -0.45 0.15 0 0 0, R, 0, 0, 0, 0, 1300, 0;
  -0.25 -0.45 0.15 0 0 0, R, 0, 65536, 1, 0, 60, 0;
  -0.25 -0.45 0.15 0 0 0, R, 0, 524288, 0, 0, 60, 0;
  -0.22 -0.62 0.06 0 0 0, R, 0, 0, 0, 0, 40, 15;
  -0.22 -0.62 0.06 0 0 0, R, 1, 0, 0, 0, 40, 0;
  0.10 -0.60 0.22 0 0 0, R, 1, 0, 0, 0, 100, 20;
  0.16 -0.43 0.27 0 0 0, R, 1, 0, 0, 0, 200, 12;
  -0.25 -0.45 0.15 0 0 0, R, 0, 0, 0, 0, 400, 20"
```

(on one line): the right trigger fires a burst, the right stick ejects,
the left hand goes to the left hip and takes a magazine, comes under the
rifle and pushes it home behind the grip. The log says each step (`vr:
reload:` and `vr reload:`): where the magazine goes, from the recentred
head as the hands are placed here, how far off it was pushed home, the
game's reload with the rounds loaded and carried before and after, and
that the first-person weapon stayed in the hand.

`debug.gpu_stats` also logs the VR frames' timing, and
`debug.screenshot_every` writes both eyes (`eyes*.bmp`), the HUD
(`hud*.bmp`) and both eyes with the layers over them, as the headset shows
them (`view*.bmp`), and while a scope is raised its image (`scope*.bmp`); with the visor's glass, the log says whether its edges
are out of view (how many of each eye's pixels are past them).

The GPU's time is best read from the kernel: the process's
`drm-engine-gpu` time in `/proc/<pid>/fdinfo`. On the Steam Frame (b30's
first outdoor area, SteamVR holding 72 Hz with the headset idle, the GPU at
its 903 MHz) the game keeps the GPU busy about 8 ms a frame at
`vr.resolution_scale = 1.0` (1728x1728 eyes), about 13 ms at 1.25 (on the
edge of 72 Hz), and at 1.5 the runtime drops to 36 Hz. At 90 Hz (11.1 ms)
the default scale leaves about 3 ms, so there is no anti-aliasing:
supersampling costs as above, and multisampling the eye pass
(`GL_OVR_multiview_multisampled_render_to_texture`, which Zink offers) cost
15 ms a frame at 2 samples, and drew the multi-pass shaders wrongly.

### How it operates

- The host opens the OpenXR session on SDL's EGL context
  (`XR_KHR_opengl_enable` with `XR_MNDX_egl_enable`;
  `arm64/host_vr.c`). The guest gets the swapchains' GL textures, the
  views and the controllers through `src/vr_host.h`, whose structures
  hold only 32-bit values.
- The game draws its 3D view once a frame, from the centre of the head,
  with a field of view that covers both eyes (`game/vr_render.c`): the
  culling and the game's work are those of one view. That view has the
  game's 480-line viewport, so the size of an object on the screen, which
  picks its model's detail level, its lighting's refresh and its shadow, is
  scaled to an eye's lines over the eye's field of view (about 3.6 times,
  at 1728 lines an eye). The renderer draws
  each draw into both eyes at once with `GL_OVR_multiview2`, directly into
  the eyes' swapchain image, a 2-layer texture array. Each vertex that the
  game projected through clip space moves to its eye by a 4x4 for each
  eye, which `game/vr_render.c` makes each time the game sets a projection
  (`rasterizer_set_frustum_z`). The eyes are beside the centre, looking
  the same way, so only x and y move: each eye's depth is exactly the
  centre's. (Made through the inverse of the centre's projection, the depth
  lost its precision, differently each frame: some frames all beyond about
  100 world units went past the far plane, the water's edge flickered on
  b30's horizon, and, depth-clamped, a10's Pillar of Autumn showed its far
  side over its near hull.) The screen's other render targets have a
  layer for each eye. Draws in screen space are the same in both eyes.
- The HUD and the menus are drawn into a target of their own, which is
  copied each frame into a layer: in front of the head in a game, in
  front of the place of the last recentring in the menus. The HUD's camera
  has the field of view of the layer, so the waypoints are in the correct
  place.
- The HUD on the visor (`vr.hud = "visor"`, `src/vr_visor.c`) is curved
  about the eyes: the game's 640x480 HUD is laid out in angles, across
  and up, so that each element is as large and as square at the edges as
  in the middle, and all of it at one distance. Where the runtime has
  cylinder layers (`XR_KHR_composition_layer_cylinder`, as Monado does) it
  is on a cylinder about the head; else (SteamVR 2.18 on the Steam Frame)
  on a quad whose image the copy draws curved, which from the eyes looks
  the same, its edges only a little farther (the log says which).
  `HALO_VR_NO_CYLINDER=1` does without cylinders, to test that. What the
  HUD projects from the world (the waypoints, the names, the reticle where
  the head aims) goes through the same curve
  (`vr_render_hud_to_screen`). The HUD's orientation trails the head's
  (`vr.hud_lag`): smoothed toward it, without a spring, so that it never
  overshoots and is still while the head is, and held within 1.5 degrees;
  its camera looks where the layer does, so the waypoints stay on what they
  mark, and the reticle where the head aims. The HUD's parts have depths
  of their own (`vr.hud_depth`): its draws are grouped by the anchor the
  game places each element by (`hud_draw.c`; `hud.c` holds the waypoints
  and the players' names, and the messages, to groups of their own), each
  group but the middle's drawn into a target of its own and shown on a
  quad of its own (four more layers, `VR_SWAPCHAIN_PANEL`), its region of
  the HUD curved in its image as the HUD is, at its own distance, placed
  with the HUD's layer: from the head, each element is where it was, only
  nearer or farther, and the waypoints stay on what they mark. Quads,
  which every runtime has; the stereo depth is the compositor's, and the
  HUD's pixels never enter the eyes' images, whose depth is the world's.
  The visor's glass is a layer of its own 0.105 m before the head, over
  the HUD's, on the head exactly (the helmet does not lag): the helmet's
  shadow on it from the brow and the chin, the glass a faint gold, and
  the shields on it, from the player's unit as the game has it. Its
  edges are never seen: its quad reaches past every corner of both eyes'
  views as the runtime gives them (`xrLocateViews`: their fields of view
  and their turns, on any headset), and all of it fades to nothing in a
  band beyond them before its edges (a view past 80 degrees from the
  head's forward, which no quad this near can reach, sees the fade, never
  an edge). Their
  energy is the armour's own: the unit's modifier shader (the Chief's
  `characters\cyborg\shaders\shield hit`, a plasma), its two noise maps
  (`characters\elite\bitmaps\plasma shield noise`) moving through the
  glass at their scales, directions and periods, combined as its pixel
  shader combines them (`rasterizer_xbox_plasma_energy.c`), coloured amber
  facing and blue grazing as its colours are, and as bright as the game
  makes the armour glow (the unit's function the plasma's intensity comes
  from: the Chief's 'shield glow source', from the shields' recent damage).
  As the shields recharge, it fills the glass from below to their level,
  brightest at its front, and flashes as they are full (the Chief's
  collision model has no recharging effect of its own, and its depleted
  effect is particles: no flash of the screen's to move onto the glass).
  More at the glass's edges than in its middle. The red warning stays
  toward the rim; the HUD's flicker as the shields break is three dips in
  under half a second. The helmet's rim (`vr.helmet_rim`) is drawn into
  the eyes' images (`vr_draw_rim`, multiview), before the head as the
  faceplate is: the visor's opening, wide and low, its lip just in from the
  edges of each eye's own view, its bevel and frame curving back toward the
  face, only its brow and chin to see (the sides barely, the corners not at
  all), fading in smoothly from its lip and out again beyond the view,
  with no highlight along it (no line, no edge); each eye sees its own
  side, both the brow and the chin, in depth. It is sized by the runtime's fields of view,
  so it stays at their edges on any headset. Other effects on the glass
  (water, dirt, cracks, reflections) would be drawn with it
  (`vr_draw_visor`). The copy adds the HUD's glow, a soft ring of its own
  light about each element.
  `debug.screenshot_every` also writes what the headset shows
  (`view*.bmp`): the layers drawn over each eye's image as the compositor
  would place them. With `vr.aim = "controller"` or `"gamepad"` the reticle is not in
  the layer but in the eyes (`vr_render_crosshairs`): drawn at the end of
  the eye pass where the aim meets the world (a line-of-sight test, its
  distance smoothed), in each eye by a scale and offset from the middle of
  the screen where the game draws it, as large as in the layer, each eye
  in a draw of its own (`halo_vr_draw_eye`). In the layer, in front of the
  head, it was placed for the head's pose as the frame was drawn, which
  the compositor showed it with at the pose of the display while it moved
  the eyes' images to that: it swam against the world as the head moved,
  stepped by the layer's pixels, and at the layer's 2 metres the eyes saw
  it doubled against what it aimed at.
- The runtime paces the game (`xrWaitFrame`). A frame ends when the next
  starts, because SteamVR's `xrEndFrame` waits for the GPU. The views are
  located again just before the frame's draws. The game's ticks are
  interpolated, as on the flat screen. The frame is begun before the game
  reads its clock, and the game's time moves on by the time between the
  frames' predicted displays, so that the frames are as evenly apart in
  the game as the headset shows them (read when the last frame's work
  ended, they were 9 to 19 ms apart at 72 Hz, and what moved fast
  juddered).
- The player's facing follows the right controller (or the head). Thus
  the netcode sees a normal player's aim. The first-person weapon is
  posed from a camera behind the controller, and drawn in both eyes as
  the world is. With both hands on a long gun, the facing and the weapon
  follow the line between the hands instead: still only an aim for the
  netcode.
- With a gamepad (`vr.aim = "gamepad"`) the facing is the game's own, from
  the stick (`player_control.c`), and the netcode sees it as ever; the
  view before the head's turn and move is the facing's yaw and pitch
  (`vr.gamepad_view = "camera"`), its yaw alone (`"level"`) or its yaw in
  steps (`"snap"`), and the first-person weapon is posed from the player's eye along
  the facing (not the camera, which shakes as the weapon fires), the
  game's arms with it. The reticle is where the facing meets the world, as
  the controller's is where its line does.
- The first-person weapon and the arms are two models posed by one
  animation graph, whose nodes the game builds each frame
  (`first_person_weapon_build_node_matrices`); `game/vr_hands.c` poses
  them again. Each hand's middle (its wrist and knuckles) is at its
  controller's grip pose, turned with the controller's pointing pose as
  the weapon's idle animation has it turned from the camera. For the
  right hand the camera is placed so (the weapon and the hand then play
  their animation about it, reloads and throws too); the left wrist
  (`frame l wriste`) and its fingers are put there in the idle pose's
  shape once the nodes are built, the fingers curled about the knuckles'
  line as the grip is pulled, and blended onto the weapon's foregrip as
  the left hand takes it. The right index finger curls as the trigger is
  pulled, and the weapon's `frame trigger` (the pistol's) moves back.
- The game throws a grenade in an animation: the throw button starts the
  unit's throw animation, its third frame puts the grenade in the unit's
  left hand, and its key frame (the cyborg's with the assault rifle: frame
8, 0.27 s in) lets it go, from
  just before the eyes, along the aim, at the unit's grenade velocity (let
  go of sooner, as when a flinch cuts the throw short, more weakly). The
  first-person weapon plays a throw of its own beside it, which lowers the
  weapon. A grenade thrown with the left hand (`src/vr.c`,
  `game/vr_grenade.c`): the swing presses the left trigger, so that the
  game's own throw begins, through its input and the netcode as any throw
  does; while the hand holds on, the throw animation (and the first-person
  one, which marks no release of its own: the unit's frame) waits a frame
  before its release, and when the hand lets go sooner, goes on to it at
  once (the grenade put in the hand first if it is not there yet); the
  grenade is then moved to the hand and its velocity turned along the
  hand's throw, its speed the game's times the throw's power. With the
  hands the controllers', the first-person throw is not played: the left
  hand is the player's, and the weapon stays in the right. The game's
  grenade, in the hand of the player's body from the throw's third frame,
  is not drawn in first person (nor is the body), so the grenade in the
  left hand is drawn by `game/vr_grenade.c` after the first-person weapon:
  the selected type's projectile model at the controller's grip, from the
  grip taking it until the game throws it (let go of, where the hand let it
  go). The thrown one starts at the point last drawn, and its first tick is
  drawn from there (`render_interpolation_object_from`), not swept from the
  body's hand. The hand's
  velocity is measured over 35 ms of its poses (at their display times),
  and the release's is the fastest of the last 0.1 s, as the hand slows
  as it opens. While the hand throws, the player's facing turns along the
  throw, which the netcode sends: on a client, its own copy of the
  grenade is the one that deals damage (reported to the host), and the
  host's copy, which the other players see, flies the same way, but at
  the game's speed, let go of when the host's animation lets go of it.
- With arms, the shoulders are below and beside the eyes, turned with the
  body: the heading between the head's and the hands', followed past a
  40 degree turn of the head, and slowly (so that looking about does not
  swing them). Each arm is two bones, the model's (0.31 to 0.33 m each):
  the elbow is where they meet (the law of cosines), bent down, out and a
  little back, and each bone is turned from the idle pose's to lie along
  the arm, the forearm twisted with the hand. Out of reach, the shoulder
  comes forward up to 5 cm and the arm stretches the rest. With floating
  hands, the arms are scaled to almost nothing at the wrists.
- The physical crouch (`vr.physical_crouch`, `src/vr.c`): the head's
  height from the recentred eyes (with 7 cm of hysteresis) presses the
  left stick's click, the game's crouch, which the game holds only while it
  is pressed; with it, the crouch is held at a full stick too
  (`player_control.c`, which otherwise stands a player pushing the stick
  all the way). The game's eye drops by its crouch (`biped->biped.crouch`
  times the biped's standing camera height less its crouching one,
  `biped_get_sight_position`) over a few ticks; `game/vr_render.c` adds
  that back to the eye while the head is low (just out of a vehicle the
  player is still crouched from before it, a frame before the crouch is
  pressed again), the crouch blended between the last two ticks as the
  camera is, and keeps doing so until the game has stood the player up
  again (else standing up would drop the view by the crouch still left). A crouch by a hand, with the head up, keeps the
  game's drop. With `vr.aim = "gamepad"` the weapon, posed from the eye,
  comes down with the head's crouch, and the reticle stays where the aim
  from the game's eye meets the world.
- In a vehicle's seat (`vr.vehicle_view = "first_person"`) the eye is in
  the seat, and the player's body is not drawn. The game animates the
  seated body, none of it the player's own motion: getting in sweeps the
  head 2.7 to 3.4 m over most of a second, getting out 1.7 to 2.7 m, and a
  turret's seat swings about the turret's pivot as it aims (where the
  controller or head points), the Shade's eye 0.65 m from it, with flinches
  when hit. With `vr.seat_motion = "still"` the eye is held in the
  vehicle (its root node's frame, posed between ticks as it is drawn):
  where it was standing as the body climbs in, then where the head is once
  seated, until the player is out. It rides with the vehicle, but the gun
  swivels in front of it instead of the eye about the gun. With `"head"`
  the eye is the seated player's head marker. The view turns as on foot
  (the stick and the head, never the vehicle): the vehicle steers and aims
  where the controller (or head) points, and the horizon stays level
  whatever the vehicle does.
- Where the eye would jump more than 0.1 m (into the seat once seated, back
  to the player's own eye once out, to the game's camera following the
  vehicle or the dead's, and back), it blinks: the view darkens over 0.1 s
  with the eye held, moves in the dark, and lightens over 0.15 s. The
  darkness is a black screen flash in the eye pass, after the game's own
  (the HUD's layer stays lit). The game's director cuts between its
  first-person camera and the one following a vehicle in the VR mode,
  rather than flying between them for a second (`camera_change_pause`),
  which swept the view some 6 m, from behind the player into their head,
  on getting out.
- The sky is drawn about the camera at a thousandth of its size, a few
  metres away: its draws move to each eye as from the centre of the head,
  so that it is at infinity. Its draws are depth-clamped rather than
  clipped by the near and far planes (its far side is near the far plane,
  and the union view, moved by the head, clipped parts of it some frames).
  The rest of the eye pass is clipped as the flat screen is.
  What the game projects to the screen itself (lens flares,
  their visibility tests, the light volumes' sprites) is drawn in screen
  space and moved to each eye as its point moves, by a scale and offset.
  The sun's glow (`rasterizer_ray_of_buddha`), a blur of the screen about
  the sun, is not moved.
- A raised scope (`vr.scopes`, `game/vr_scope.c`) is drawn in a pass of
  its own before the eye pass (whose state, the clusters seen, the lights
  and the scene, is then the eyes'): the game's 3D view once more, from the
  scope's eyepiece along the aim, its field of view the angle the lens
  spans from the eye over the zoom's magnification, into the middle 480x480
  of the 640x480 screen at a scale of its own (480 lines are 480 pixels at
  `vr.resolution_scale` 1, more or fewer with it, 256 to 768), in the
  game's ordinary one-layer targets (no multiview, no eye transforms). It
  draws no first-person weapon, no lens flares (their visibility tests are
  the eyes'), no objects' shadows, no weather and no HUD. At the present
  the middle square is copied into the scope's swapchain image, upside
  down for OpenXR, round, darkening to black at its edge, with its reticle,
  and shown on a quad layer at the eyepiece (`VR_SWAPCHAIN_SCOPE`), facing
  back along the scope, over the eyes' images and under the HUD's layers,
  for the eye looking through it alone (`eyeVisibility`): the compositor
  reprojects it as it does the eyes'. Its pass's shaders, without
  multiview, are a driver's to make as they are first drawn with; the pass
  is drawn once, unseen, the first time a scope is in hand in a map, so
  that the first raise does not wait for most of them (on spark the first
  raise took 0.44 s without it, 0.22 s with). Where each scope's eyepiece
  is on its first-person model is a table in `vr_scope.c`. On spark (b30,
  the pistol raised, against the same run with `vr.scopes = false`) the
  pass costs about 0.5 to 1.3 ms of the GPU's time and 1.5 ms of the
  game's a frame; confirm it on a Steam Frame.
- The visibility tests (the lens flares') are counted by the pixel shaders'
  atomic counters, as on ES, not by queries: Vulkan counts a query in a
  multiview pass in as many slots as views, which Zink does not allow for,
  and Turnip's writes past them faulted the GPU (and could lose the
  device). Without atomic counters they would be drawn into the left eye's
  layer alone, outside the multiview pass.

## What operates

| Area | Status |
| --- | --- |
| Game code | All 466 C files of the game. The changes are in "Game source changes". |
| Graphics | Direct3D 8 on OpenGL 4.5 core through SDL3 (`src/d3d8_gl.c`). The port translates the NV2A vertex shaders and register combiners to GLSL. It decodes all the Xbox texture formats. The vertex and index buffers come from a GL copy of the Xbox memory. |
| High-res HUD | The HUD is drawn from high-res assets: redraws at 8x the size of the maps' bitmaps (4x for the largest), in `port/assets/hud`. They cover the meters, counters, panels and their outlines, the motion sensor, reticles, waypoints and scopes, but no bitmap with English text. `tools/hud_assets.py` makes them from the SVG redraws (the weapons' reticles from those of the PC HUD sheet that match the Xbox's exactly, the sniper rifle's 2x and 10x set in Overpass), and the build embeds them in the executable. When the game uploads one of those bitmaps, `src/hud_hires.c` gives the high-res texture in its place, if the bitmap's pixels are those of the English maps: another language's maps keep their own. The game sizes and places the HUD from its tags as before. `display.high_res_hud = false` turns this off. |
| High-res text | The menus' and HUD's text is drawn with Overpass (`port/assets/fonts`, SIL Open Font License) in place of the maps' bitmap fonts, which are Interstate. `src/text_hires.c` rasterizes each glyph with stb_truetype (`port/third_party/stb`) at the resolution the game draws at, into an atlas that a placeholder bitmap of the game stands for. The game lays the text out from its font tags as before. The menus' titles (the screens' headers and the main menu's items) are pictures of text in the maps, so they are drawn as the high-res HUD is: `tools/title_assets.py` sets each one again in OpenCE, Roger White's public-domain Newtown respaced to match the maps' commercial title typeface (`tools/title_font.py`), at 4x the bitmap's size over its own plate or glow, each letter placed where the map's letter is, in `port/assets/titles`. The postgame carnage report's title is set over a hand-made SVG redraw of its panel (`port/assets/titles/svg`) instead. The A, B, X and Y button icons beside the menus' key labels and in their text ("Press A to Join") are drawn from hand-made SVG redraws at 8x the size of the maps' bitmaps (`port/assets/buttons`, made by `tools/button_assets.py`), in the same sizes and places. `display.high_res_text = false` turns it off. |
| Anti-aliasing | Off unless `display.anti_aliasing` is set (`src/d3d8_gl.c`, `src/xgpu_post.c`). FXAA (written in the port) and SMAA (`port/third_party/smaa`, MIT licensed, at its HIGH preset, compiled as GLSL) are passes over the 3D view of each window, after the lens flares and before the HUD and the menus (`render/render.c`). Their programs are built when the setting is chosen. Supersampling draws the render targets the size of the screen at two times the resolution in each direction, and the display blit scales them down. Multisampling draws the back buffer and its depth buffer into multisampled renderbuffers, and with them any target that is drawn together with one of them (a mirror's view, in the secondary target with the back buffer's depth buffer), so that the attachments of a framebuffer are all multisampled or none is. A target's pixels are resolved into its texture before something reads the texture (as a texture, or at the display blit). Visibility tests count samples, divided by the samples of a pixel. An alpha-tested surface (foliage, grates) covers the samples of a pixel in proportion to its alpha past the reference (`gl_SampleMask`, not on Android). |
| Sound | Xbox DirectSound on SDL3 audio (`src/dsound_sdl.c`): PCM and Xbox ADPCM, mixed at 48 kHz, with volume, pitch, mix bins, distance, stereo pan, occlusion and obstruction. There is no Doppler effect, no cones and no reverb. |
| Input | XInput on SDL3 (`src/xinput_sdl.c`): keyboard, mouse, gamepads with rumble, and the debug keyboard for the console. |
| Files | The Win32 file functions and the MSVC file functions on POSIX, with the translation of Xbox paths. |
| Threads | Threads, events, mutexes, critical sections, interlocked operations and alertable waits. |
| Memory | The port reserves the Xbox memory at `0x80000000`. Thus the game gets the fixed addresses that it expects. |
| Saved games | The Xbox `UDATA` layout, with SHA-1 signatures. |
| Networking | Winsock on BSD sockets. System link on a local network and on the internet. |
| Bink video | Not available. The game skips the movies. |

## How the port operates

### The compiler

`tools/linux_build.py` compiles the game with clang and these options, which
give the ABI of the MSVC compiler:

- `--target=i686-linux-gnu`: 32-bit x86.
- `-fms-extensions`: the MSVC extensions.
- `-fshort-wchar`: 16-bit `wchar_t`.
- `-malign-double`: 8-byte alignment of 64-bit members.
- `-fcommon`: tentative definitions, as in C89.

glibc gives only ISO C (`__STRICT_ANSI__`). Thus POSIX names, for example
`random`, do not conflict with the names of the game.

These files supply the MSVC functions that clang does not have:

| File | Contents |
| --- | --- |
| `include/halo_linux_prefix.h` | The first header of each file: the architecture macros of the SDK, MSVC `__inline`, SEH keywords, `__declspec(selectany)`. |
| `include/` | Headers that add MSVC names to the C runtime headers. |
| `port/include/xdk` | The Xbox SDK declarations. The compiler reads this folder after all the other folders. |
| `tools/linux_msvc_semantics.py` | Makes a header that declares each struct tag at file scope, as MSVC does. It also makes the header inline functions weak, as the COMDAT functions of MSVC. `game/msvc_comdat.c` gives one external copy of each. |
| `include/halo_linux_winsock_names.h` | Gives new names to the Winsock functions of the SDK. Thus they do not link to the glibc functions with the same names. |
| `include/halo_linux_source_fixups.h` | Declares the port's functions that the game's sources call. |

`tools/linux_link_check.py` stops the link if a weak reference has no
definition. Without this check, the linker gives the reference the address
0.

### The platform layer (`src/`)

- The files `posix_*.c` use glibc. The compiler uses the ABI of the host
  for these files, because some glibc structures have a different layout
  with `-malign-double`.
- The other files include the SDK declarations through `platform.h`. Thus
  the compiler examines each definition against the SDK prototype.
- `src/halo_linker_common.c` gives weak storage for some globals of the
  January link, and for `fast_ftol_C` and `main_crash`.
- `main/d3d_intimacy.cpp` reads a private structure of the Xbox Direct3D.
  The Linux build does not use this file. `src/d3d8_gl.c` gives
  `d3d_find_flipcount`.
- The build returns small structures and unions in registers
  (`-freg-struct-return`), as on Win32.
- The GPU driver cannot open the kernel's `trace_marker`
  (`src/posix_trace_marker.c`). SteamOS keeps kernel tracing on for its GPU
  performance captures (`gpu-trace.service`), and its Mesa then writes a
  marker for each traced driver function: on the Steam Frame, some 480,000
  writes a second, which took the game from the headset's 72 Hz to about
  50 frames a second. The Steam Deck runs the same service, but its Mesa
  (25.3, 32-bit and 64-bit) has no markers to write: there the refusal
  changes nothing (measured: the same frame times and power either way).
  `HALO_GPU_TRACE_MARKERS=1` lets the driver write them, to capture with
  gpuvis.

### Game source changes

Five files of the game have changes for clang. These changes do not change
the MSVC objects: a comparison of all 612 C objects showed no difference in
code or data.

| File | Change |
| --- | --- |
| `cseries/cseries.c` | The naked function `stristr` uses `[ebp+8]` and `[ebp+12]` for its parameters. |
| `bitmaps/bitmap_drawing.c` | `*((word *)p)++` is now `*(*(word **)&p)++`. |
| `rasterizer/xbox/rasterizer_xbox_hardware_bitmaps.c` | `&(T *)x` is now `(T **)&x`. |
| `hs/hs.c` | Local prototypes that did not agree with `ai_script.h` are removed. |
| `units/vehicles.c` | The local prototype of `unit_update_animation` uses the type of `units.h`. |

`math/real_math.h` had a copy of `plane2d_from_points` that did not agree
with the function in `effects/decals.c`. clang used the copy, and parts of
levels were not visible. The copy now agrees with the function.

Other changes:

| File | Change |
| --- | --- |
| `scenario/scenario.c` | The BSP connection tables have names, not MSVC offsets. |
| `rasterizer/xbox/rasterizer_xbox_environment_fog.c` | A local pointer gets its value from the file-scope array with the same name. |
| `game/player_control.c` | The mouse aims the player on controller 1 directly. |
| `sound/game_sound.c` | The game calculates the obstruction of each sound one time for each tick, not for each frame. |
| `cseries/errors.c` | `debug.txt` stays open between lines. |
| `networking/`, `game/`, `interface/`, `bungie_net/network/` and the pools of objects, effects and sounds | The system link limits and the memory for them. |
| `game/`, `objects/`, `units/`, `networking/` | The distributed netcode. Refer to `NETCODE.md`. |
| `cache/cache_files.c` | When a map's tags load and unload, the port finds the bitmaps that the high-res HUD replaces (`game/hud_hires_tags.c`), and adds the tags of the menus to the menus' map (`game/menu_tags.c`). |
| `interface/ui_widget.c`, `interface/ui_widget_event_handler_functions.c`, `interface/ui_widget_game_data_input_functions.c` | The main menu is the PC version's from `port/assets/menus` (`display.menus`); the menus' widgets can call the port's functions (`game/menu_functions.c`) and send the PC version's custom activation event; the widgets' memory is 256 KB, not 16 KB; the main menu and Multiplayer clear co-op's controllers, so that a gamepad going back to controller 1 is not a controller unplugged; the lobby's split screen players leave alone, and one who quits in game is not joined to the next game (`interface/player_ui.c`). |
| `input/input_abstraction.c`, `game/player_control.c`, `game/players.c`, `game/player_queues_new.c`, `units/units.h` | The keyboard and mouse's actions (`src/xinput_sdl.c`, `include/halo_keyboard.h`) join controller 1's game controls; their reload key reloads on its own, and their action key only acts (a control flag of the port's, sent with the player's action, stops the reload the controller's X falls back to). |
| `sound/sound_manager.c`, `interface/hud.c`, `game/game_engine.c` | The music's and the other sounds' volumes; the HUD's and the scoreboard's settings are read again when Settings changes them. |
| `rasterizer/xbox/rasterizer_xbox_shadows.c` | With larger shadow maps (`display.shadow_resolution`), the blur's taps stay half a texel apart, and more passes widen it to cover the same part of the map as on the Xbox. |
| `render/render.c` | The 3D view of each window is antialiased before the HUD is drawn (`display.anti_aliasing`). |
| `interface/hud.c` | In multiplayer, players' names are drawn above their heads (`display.player_names`, `display.player_name_scale`). |
| `rasterizer/rasterizer_text.c`, `text/draw_string.c` | Text is drawn from an atlas of the fonts' glyphs, rasterized at the resolution the game draws at (`src/text_hires.c`), when the font has every character of the string. Text can be drawn scaled about a point (`rasterizer_text_set_scale`), as the players' names are. Each glyph's advance is centred on the font tag character's, so the layout is the same, and a glyph is cut at a text box only where the font tag's character visibly was. |
| `interface/first_person_weapons.c` | The first-person hands are drawn with the unit's shield modifier shader, as its body is (`display.viewmodel_shield`). |
| `render/render_objects.c`, `objects/objects.c` | In first person, your own body is drawn from the waist down with the pose `first_person_legs.c` makes (`display.first_person_legs`); the waist it found and the mesh `object_mesh.c` read are forgotten on a new map. |
| `objects/object_lights.c` | Glows the port lights itself (`shield_glow.c`): dynamic lights attached to an object's node, colored and sized each frame by their maker, drawn with a light definition the map has. |
| `render/render_objects.c`, `interface/first_person_weapons.c`, `rasterizer/xbox/rasterizer_xbox_plasma_energy.c` | A local player's shield plasma is drawn in the chosen armor color (`display.shield_color`, `shield_color.c`), on the body and on the first-person arms; in the VR build, the plasma over the weapon in its holster in its highlight's color, close to the weapon (`vr_holster.c`). |
| `rasterizer/xbox/rasterizer_xbox_text.c`, `rasterizer/rasterizer_text.c` | The characters of a string are one draw, not a draw each (and a second for the drop shadow). The batch is drawn early before a character is cached and before the high-res glyph atlas starts over (`src/text_hires.c`). |
| `rasterizer/xbox/rasterizer_xbox_decals.c` | Decals drawn one after another with the same blend function and bitmap are one draw, with each decal's color in its vertices, not a draw each: a long fight leaves hundreds in view. Within a run of decals whose blend functions commute (multiply, add, min, max, reverse subtract), they are drawn by bitmap, so more of them share a draw. |
| `rasterizer/xbox/rasterizer_xbox_widgets.c`, `objects/widgets/light_volumes.c` | A light volume's sprites (a plasma bolt's glow, a couple of hundred) are one draw. |
| `structures/structure_render.c` | The structure's visible surfaces are put in order by a radix sort, not `qsort_4byte`: the same order. |
| `tag_files/tag_groups.h` | `TAG_BLOCK_GET_ELEMENT` returns an element in the block without a call, and calls `tag_block_get_element_with_size` for anything else. |
| `input/input_xbox.c`, `physics/collision_usage.c` | The clock is read only for a button held, and for collision tests only when their times are shown: a read of the clock is slow in a browser. |

The x86 inline assembly of the game is replaced by C. Thus the compiler
can optimize that code for each processor:

| File | Assembly | Replacement |
| --- | --- | --- |
| `cseries/cseries.h` | x87 `fistp` (`fast_ftol`) | `__builtin_rint` |
| `bitmaps/bitmaps_inlines.h` | x87 conversions | C conversions |
| `math/matrix_math.c` | SSE `matrix4x3_multiply` | a C loop |
| `effects/decals.c` | an x87 conversion | a C conversion |
| `cseries/profile.c` | `rdtsc` | `QueryPerformanceCounter` |
| `cseries/cseries.c` | naked `stristr` | a C `stristr` |
| `cseries/stack_walk_windows.c` | a read of EBP | `__builtin_frame_address` |
| `interface/hud_draw.c` | a read of `[ebp+4]` | `__builtin_return_address(1)` |
| `bink/bink_playback.c` | `int 3` | `__builtin_trap` |

The x87 control and status words (`_control87`, `_statusfp`, `_clearfp` in
`src/msvc_crt.c`) use `fenv.h`. On Android, they use the FPCR and FPSR. In
the web build, the control word is only remembered.

The web build's changes (`HALO_WEB`: the main loop runs one iteration for
each of the browser's frames, the cache thread starts at a function of
`CreateThread`'s type, `debug.txt`'s lines go to the page's log) and the
declarations given their definitions' types, which WebAssembly needs, are
listed in "Game source changes" in [../web/README.md](../web/README.md).
