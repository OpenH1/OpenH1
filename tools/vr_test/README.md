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
