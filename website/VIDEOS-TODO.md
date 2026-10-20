# Clips still wanted for openh1.com

Two of the page's comparisons are stills for now (lights is done: `capture/README.md`). Each should become a
pair of looping clips, the Original and OpenH1, of the same moment, frame
for frame. Swapping one in is a file drop and a small edit: each place has a
`VIDEO TODO` comment in `public/index.html`, and the page's script already
loads, plays and syncs any `<video>` pair (README.md, "Pictures and clips").

## The settings of each side

| Side | Settings |
| --- | --- |
| Original | `HALO_GRAPHICS=classic` (the Classic preset: no anti-aliasing, classic lights), `HALO_INTERPOLATION=false` (one frame a tick, as on the Xbox), `HALO_RESOLUTION_SCALING=original` (the Xbox's 640 × 480), scaled up by nearest neighbour to OpenH1's size |
| OpenH1 | `HALO_GRAPHICS=enhanced` (SMAA, Enhanced lights and the rest), `HALO_INTERPOLATION=true` (frames between ticks) |

Both: the HUD hidden (`show_hud 0` on the console, below), the same map,
input and seed, and fixed timing.

## The clips

| Name | Section | Scene | Length | Files the page expects |
| --- | --- | --- | --- | --- |
| lights | 01, the wipe | A frag grenade at a spot already scorched by one thrown before the clip (a loop without a fade): the spot is to be chosen from the survey in `capture/README.md` (Rat Race, yaw 90, first) | 4 s | `lights-original.mp4`, `.webm`, `-poster.webp`; the same for `lights-openh1` |
| edges | 02, the lens | Blood Gulch, a slow pan or strafe across the cliffs against the sky (the spawn of seed 7 faces them): the Original's edges crawl, SMAA's stay clean | 4-6 s | `edges-original.*`, `edges-openh1.*` |
| glow | 03, the flip | A plasma rifle or pistol fired straight down a genuinely dark hallway, still camera, steady shots, each bolt lighting the walls, floor and ceiling as it travels (a30's or a50's interiors, Truth and Reconciliation, or 343 Guilty Spark) | 4-6 s | `glow-original.*`, `glow-openh1.*` |

The stills now in their place: `lights-*.webp` (Derelict, the frag
grenade, Dynamic Lights only), `edge-*.webp` (Blood Gulch, anti-aliasing
only) and `glow-*.webp` (a 640 × 480 plasma pistol shot, Dynamic Lights
only). With the clips, the captions should say both sides' settings as in
the table above.

The pair in section 04 (`strafe-original.*`, `strafe-openh1.*`) and the
hero (`hero.*`) are done.

## Encoding

- Both clips of a pair: the same length and frame count, at 60 fps (the
  Original's 30 frames a second each shown twice), no audio. To be captured
  at 4K: OpenH1 at 3840 × 2880, the Original's 640 × 480 six times (or both
  cropped to 3840 × 2160); the sizes below to be settled after.
- A loop that cuts back to its first frame where the last matches it
  (`capture/README.md`, "Loops without a fade"); else the last 0.4 s
  crossfaded into the first.
- H.264 MP4 (`-crf 26 -profile:v high -pix_fmt yuv420p -movflags
  +faststart`) and VP9 WebM (`-crf 38 -b:v 0`), each at most 1 MB; the
  poster, the first frame, as WebP (quality 75).
- In the edges pair, keep the pixels: scale by whole numbers only, or crop.

## Capturing

On Linux, the scripts in `capture/` (its README): offscreen, without the
two problems below. What worked on a Mac (one game at a time, with a timeout that kills it):

- A data folder of its own for each run, with `maps` linked in and an
  `init.txt` of `map_name <scenario>` (no menus), and the environment:
  `HALO_DATA_ROOT`, `HALO_SAVE_ROOT`, `HALO_NET_ONLINE=false`,
  `HALO_HIDDEN_WINDOW=1`, `HALO_DISPLAY_MODE=windowed`,
  `HALO_WINDOW_SIZE=960x540` (drawn at 1920 × 1080 on a Retina display),
  `HALO_NO_AUDIO=1`, `HALO_UPDATE_ANSWER=no`, `HALO_EXIT_AFTER`.
- **Fixed timing**: `HALO_FRAME_TIME=0.0166667` for 60 frames a second of
  game time (`0.0333333` for 30), so the frames do not depend on how long
  each took to draw. `HALO_RANDOM_SEED` fixes a multiplayer map's spawn.
- **Scripted input**: `HALO_TEST_INPUT="script:<event>; ..."` on the
  simulation's clock, from the scenario's start: `aim=<yaw>,<pitch>`,
  `move=<x>,<y>` and `look=<x>,<y>` (sticks, from start to end), `fire`,
  `throw`, `action` and so on (`port/linux/src/xinput_sdl.c`). At most 64
  events. A turn with `look` is the stick's rate over each frame's time,
  so it differs a little between 30 and 60 frames a second: for a pair,
  move (strafe) rather than turn, or aim at times on the 30 Hz grid.
- **Frames**: `HALO_SCREENSHOT_DIR` and `HALO_SCREENSHOT_EVERY` (BMP, the
  window's pixels; 1920 × 1080 is 6 MB a frame, so delete the frames once
  encoded).
- **The HUD**: `HALO_TELNET_CONSOLE=true` and a port of its own
  (`HALO_TELNET_CONSOLE_PORT`), then, once the map is playing, send
  `(begin (show_hud 0) (sleep 1))` to it with `nc`. Sent bare, `show_hud 0`
  prints its result on the screen for a few seconds. Sent while the map is
  still loading, a command can stop the game.
- A level that opens with a cinematic (b30, a50) runs it at sixteen times
  its speed with `HALO_SKIP_CINEMATICS=1`, so a script's early times pass
  during it.
- OpenH1's frames are drawn a tick behind the simulation; the Original's
  are not. Line the two up by a frame or two (by eye, on a moving object).

### Two problems on macOS, and the local workarounds used

Neither is fixed in the game. The workarounds below were local edits to
`port/linux/src/d3d8_device.c`, for capturing only, and are not committed.

1. **Interpolation off hangs on the loading screen with a hidden window.**
   With `display.interpolation = false`, `D3DDevice_Present` waits for the
   display's vertical blank (`pending_flips`, at most two frames queued),
   which a hidden window never signals, so the game stops on its first
   frames. The workaround skipped that wait when an environment variable
   was set:

   ```c
   while (pending_flips >= 2 && !getenv("HALO_CAPTURE_NO_VBLANK"))
       pthread_cond_wait(&vertical_blank_condition, &vertical_blank_lock);
   if (!getenv("HALO_CAPTURE_NO_VBLANK"))
       pending_flips++;
   ```

   With it and `HALO_FRAME_TIME=0.0333333`, the Original draws one frame a
   tick, deterministically. (The main loop's throttle still waits up to a
   second for the vertical blank once, then turns itself off: "stuck
   waiting for VBLANK callback".)

2. **A screenshot of every frame or two hangs some loads.** Writing BMPs
   from the first frame, through the loading screen, stopped some runs on
   the loading screen. The workaround started the screenshots at a frame
   number, so loading went untouched:

   ```c
   if (screenshot_every > 0 && device.frame % (unsigned long)screenshot_every == 0
       && device.frame >= (unsigned long)(getenv("HALO_SCREENSHOT_FROM") ? atol(getenv("HALO_SCREENSHOT_FROM")) : 0))
       write_screenshot(back_buffer);
   ```

A machine with more headroom (or a capture path that records the window,
such as OBS at a fixed frame rate) avoids both.
