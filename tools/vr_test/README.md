# Headless VR tests

These tools run the VR build (`configure.py --vr`, "VR" in
[port/linux/README.md](../../port/linux/README.md#vr)) on a simulated
headset, with no headset and no display: Monado's simulated headset and its
null compositor, in Docker, on an NVIDIA GPU of a 64-bit ARM Linux machine
(an NVIDIA DGX Spark, for example). The headset's refresh rate and eye size
are settable, by default a Steam Frame's: 72 Hz and 1728x1728 eyes.

What they show and what they do not:

- The game's logic, its drawing and its pacing by the runtime, at the
  headset's refresh rate and resolution: a problem of the engine reproduces.
- Not the Steam Frame's GPU driver (Turnip and Zink on an Adreno 750) nor
  its performance: the GPU is NVIDIA's, with NVIDIA's driver. Confirm a
  problem of the driver, and any timing, on a Steam Frame.
- The simulated head does not move from frame to frame, as a worn headset
  always does a little: it is the control for flicker that depends on the
  pose (`debug.vr_test_jitter` adds a known movement). It does turn slowly,
  by about 10 degrees each way over a long run: do not assume the same view
  for minutes. `debug.vr_test_head = "0"` holds it still and level.

| File | Is |
| --- | --- |
| `Dockerfile.build` | `halo-arm64-build`: builds the game for 64-bit ARM Linux |
| `Dockerfile.monado`, `monado-null-compositor.patch` | `halo-xrtest-monado`: the simulated headset. The patch adds `XRT_COMPOSITOR_NULL_FPS` and `XRT_COMPOSITOR_NULL_VIEW_SIZE` to Monado's null compositor (stock: 20 Hz, 128 pixels). |
| `container_run.sh` | One run, inside `halo-xrtest-monado` |
| `vr_run.sh` | Starts one run |
| `flicker_scan.sh`, `flickscan.py` | Flicker scan of levels, at once |
| `montage.py`, `triplet.py` | Contact sheets of a scan |
| `openxr_probe.c` | What an OpenXR runtime offers the game: its extensions, the GL binding the game would take, its headset (port/linux/README.md, "SteamVR on a desktop PC") |
| `openxr_stub_runtime.c` | A stand-in runtime for a desktop without a headset or Monado: below |
| `steamvr_driver.sh`, `steamvr_driver/` | SteamVR without a headset: an OpenVR driver of the tests', and the script that runs SteamVR with it, below |

## Set up

Docker with NVIDIA's container toolkit (`--gpus all`), then, from the root
of the repository:

```sh
docker build -t halo-arm64-build -f tools/vr_test/Dockerfile.build tools/vr_test
docker build -t halo-xrtest-monado -f tools/vr_test/Dockerfile.monado tools/vr_test
python3 -m venv ~/.venv/halo-vr-test && ~/.venv/halo-vr-test/bin/pip install numpy
```

The maps: the `maps` folder of the game's data ("Start the game" in
port/linux/README.md). `HALO_MAPS` names it.

## Build

```sh
docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src halo-arm64-build \
	sh -c 'python3 configure.py --vr && ninja linux_arm64'
```

The game is `build/linux_arm64/halo`.

## Run

```sh
HALO_MAPS=~/maps tools/vr_test/vr_run.sh out/b30 b30 60 HALO_SCREENSHOT_DIR=/out/shots HALO_SCREENSHOT_EVERY=72
```

plays b30 for 60 seconds, then stops. `out/b30/game.log` is the game's log,
`out/b30/shots` both eyes (`eyes*.bmp`) and the HUD (`hud*.bmp`) once a
second. The map is a campaign level (`b30`), a map path
(`levels\b30\b30`), or `menu`: the main menu, starting from
`<out>/config.in.toml` and the saved games in `<out>/save` where they are;
after a run, `<out>` has the game's `config.toml` and saved games.

Settings follow as `HALO_*=value`, the environment variables of
port/linux/README.md. The headset:

- `XRT_COMPOSITOR_NULL_FPS=90 XRT_COMPOSITOR_NULL_VIEW_SIZE=2160 vr_run.sh ...`:
  its refresh rate and eye size (default 72 and 1728).
- `HALO_GAME`, `HALO_BIN`: another build's folder, or another executable in
  it (to compare two builds).
- `SLOT`: runs at once need different slots (by default, from the PID).

To act without hands (port/linux/README.md, "Settings" under "VR"):

| Setting | Does |
| --- | --- |
| `HALO_VR_TEST_HEAD="0"`, `"<degrees> <seconds>"` | Holds the head still and level, or turns it left and right smoothly |
| `HALO_VR_TEST_HANDS="..."` | Poses both controllers, the grip, the trigger, the buttons, the head's turn; poses separated by `;` are held 288 frames each |
| `HALO_VR_TEST_TURN=<degrees a second>` | Turns the player |
| `HALO_VR_TEST_JITTER=<mm>` | Moves the head at random each frame, as a worn headset does |
| `HALO_VR_AIM=gamepad HALO_TEST_INPUT=pad` | The gamepad mode: turns, looks, fires, throws, holds Back and walks, and logs each |
| `HALO_TEST_INPUT=look:<seed>` | Stands still, turning and looking about |
| `HALO_GPU_STATS=1`, `HALO_VR_GPU_TIME=1`, `HALO_VR_TIMING=1` | The frames' timing, the GPU's time, what the runtime's calls cost |

Runs of 5 at once are fine on a DGX Spark.

## Flicker scan

```sh
PYTHON=~/.venv/halo-vr-test/bin/python HALO_MAPS=~/maps \
	tools/vr_test/flicker_scan.sh out/scan 75 0 a10 a30 b30
```

plays each level at once for 75 seconds, the head still (jitter 0 mm),
standing and looking about (`look:1`), at 0.4 of the eyes' resolution,
with a screenshot each frame into `/dev/shm`. `flickscan.py` counts the
left eye's pixels that change and change back over three frames, and
writes for each level `stats.json` (the count of each frame), `heat.bmp`
(where), and the 8 worst frames (`worst_*.bmp`, the pixels red, with the
three frames as they were, `raw_*`). The last line of each is the mean.
`EXTRA` adds settings to each run. `FLICK_FULL=1 FLICK_PATTERN=frame`
scans the flat game's screenshots (`HALO_VR=0`).

`montage.py <level's dir> sheet.bmp` and `triplet.py <level's dir> <frame>
out.bmp` show them.

Baselines, the head still for 75 s, the mean flickering pixels each frame
(October 2026): a10 203, a30 277, a50 17, b30 305, b40 222, c10 55, c20
142, c40 171, d20 309, d40 391.

## Without Docker: the stub runtime

`openxr_stub_runtime.c` is an OpenXR runtime of a few hundred lines that the
game loads as it loads any (`XR_RUNTIME_JSON`), on the computer's own
desktop and GPU: a headset that is always worn and still, standing, with no
controllers, whose frames are paced at its refresh rate and dropped. It
offers what SteamVR 2.18 offers the game (EGL's binding,
`XR_KHR_composition_layer_depth`, no cylinder layers), so it checks the
game's side of a session where SteamVR cannot open one (with no headset:
port/linux/README.md, "SteamVR on a desktop PC"): its GL context's binding,
its swapchains, its drawing, its screenshots and its time a frame. It
composes nothing, so it says nothing of a compositor's time. Its comment says
how to build it; then, from the root of the repository:

```sh
XR_RUNTIME_JSON=$PWD/build/openxr_stub_runtime.json SDL_VIDEO_DRIVER=x11 \
	HALO_SCREENSHOT_DIR=/tmp/shots HALO_SCREENSHOT_EVERY=450 HALO_GPU_STATS=1 HALO_VR_TEST_HEAD=0 \
	HALO_VR_TEST_HANDS="-0.15 -0.3 0.35 0 0 0, 0.18 -0.25 0.35 0 0 0, 0.8" \
	HALO_EXIT_AFTER=60 build/linux_x86_64/halo
```

`XR_STUB_EYE_SIZE` (1512x1680, SteamVR's null driver's), `XR_STUB_FPS` (90)
and `XR_STUB_CYLINDER=1` (offer cylinder layers, as Monado does) change the
headset. The screenshots are each eye (`eyes*.bmp`), the HUD's image
(`hud*.bmp`) and the eyes with the HUD's layers over them (`view*.bmp`).

## SteamVR without a headset: the test driver

SteamVR on Linux shows a headset only in direct mode, on a display of its
own: its null driver does not open a session on a desktop
(port/linux/README.md, "SteamVR on a desktop PC"). `steamvr_driver/` is an
OpenVR driver whose headset takes the frames itself, in driver direct mode,
as a streamed headset's driver does, so SteamVR needs no display: the game
plays through SteamVR's own runtime and compositor. The headset is still,
standing, with 1512x1680 eyes at 90 Hz and no controllers (the test hooks
pose the head and hands, as with Monado).

```sh
SDL_VIDEO_DRIVER=x11 HALO_EXIT_AFTER=60 HALO_VR_TEST_HEAD=0 \
	tools/vr_test/steamvr_driver.sh /tmp/steamvr-test --capture /tmp/frames 450 build/linux_x86_64/halo
```

`steamvr_driver.sh` builds the driver (into `build/steamvr_driver`, or
`HALOTEST_BUILD`, with OpenVR's `openvr_driver.h` of its v2.15.6 release and
Khronos's Vulkan headers v1.4.309, each file downloaded from the release's
commit and checked against its SHA-256 on every run, so that one left
partial is downloaded again; the driver is built again when its source, the
headers or the compiler change; OpenVR's license is BSD's, Vulkan's headers
Apache's or MIT's; `steamvr_driver.sh --build` only builds it, without
SteamVR), makes the folder given SteamVR's configuration
(`xdg/openvr/openvrpaths.vrpath` naming the driver, `config/steamvr.vrsettings`
forcing it, `logs/`, and a `home` whose `.steam` links to the user's
`root`, `sdk32`, `sdk64`, `bin32` and `bin64`, which SteamVR's launchers
need), runs the command with `HOME`, `XDG_CONFIG_HOME` and `XR_RUNTIME_JSON`
set for it, and then stops SteamVR. SteamVR's settings and its logs are the
folder's: the user's SteamVR settings are neither read nor changed. It is
not a sandbox: SteamVR runs from the user's Steam folder, through those
links, and may write logs and caches there (Steam's own, its runtime's and
its web helper's). It refuses to start while SteamVR is running. However it
ends (the command's end, Ctrl-C, SIGTERM or SIGHUP, an error), it stops the
command and the SteamVR processes started for it (those whose `HOME` is the
folder's: never the user's own SteamVR).

- `--capture <dir> <every>` writes each layer of every `<every>`th frame,
  both eyes side by side, as `steamvr<frame>_layer<n>.pam` (RGBA): layer 0
  is the game's eyes, layer 1 SteamVR's system layer (the game's quad
  layers, as SteamVR draws them; transparent elsewhere). Only layers of
  RGBA8 or BGRA8 (sRGB or not) are captured, BGRA's swizzled; another
  format is skipped, with a line in `vrserver.txt`. The driver reads them
  back through a Vulkan device of its own, which must be the GPU SteamVR
  made the images on: OpenVR does not tell a driver which that is, so the
  driver logs each Vulkan device with its UUID and takes the one given as
  `driver_halotest`'s `vulkanDeviceUUID` setting (or
  `HALOTEST_VULKAN_DEVICE_UUID`), else the only one that can import the
  images, else the first discrete GPU. A captured frame holds SteamVR's
  server up for a fifth of a second (the game's next frames are late).
- Every 450 frames the driver logs SteamVR's frame timing in
  `<folder>/logs/vrserver.txt` (`halotest: timing`): its compositor's CPU
  time (in driver direct mode it measures no GPU time of its own), the
  GPU's for all of the frame, dropped and reprojected frames.
  `vrcompositor.txt` has SteamVR's own summary of the game's time
  (`Game Info`).
