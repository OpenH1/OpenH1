# Halo VR for the Steam Frame

The VR build of the Linux port (`configure.py --vr`, `ninja linux_arm64`:
64-bit ARM, drawn with Vulkan), packaged
by `tools/frame_package.py` for a Steam Frame. Each release has it as
[OpenH1-Steam-Frame.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/OpenH1-Steam-Frame.zip)
(the package's `halo-vr-frame` folder). Refer to "VR" and "Steam Frame" in
`port/linux/README.md` of the repository.

| File | Is |
| --- | --- |
| `halo` | The game |
| `libSDL3.so.0` | SDL3, which the game finds beside it |
| `halo-vr.sh` | The launcher: the target of the Steam shortcut. Its comments list the environment it gives the game. |
| `install.sh` | The installer (`tools/frame_install.sh`): opened on the Frame, it installs the package it is in |
| `brokers.txt` | Internet play's MQTT brokers (`network.brokers_file`): without it, internet play finds no games |
| `VERSION` | The commit the game was built from |
| `SHA256SUMS` | The files' SHA-256, which the install checks |
| `*-LICENSE.txt`, ... | The licenses of the libraries in the game |

## The game's data

The game plays the maps of an Xbox disc of the game: the `maps` folder,
in `~/apps/halo-vr/maps` (beside `halo`), or the folder that contains
`maps/` named by `paths.data` in `config.toml`. The package does not
contain them, and the install does not touch them.

## Install, or update

On the Frame, in its desktop mode: download the latest release's
[OpenH1-Steam-Frame.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/OpenH1-Steam-Frame.zip)
(or an older one from the
[Releases](https://github.com/OpenH1/OpenH1/releases) page), unzip it, and
open `install.sh` in its `halo-vr-frame` folder. It installs the package
into `~/apps/halo-vr/` (as below), in a terminal window that shows what it
did. At the first install it asks for the maps folder of the game's Xbox
disc (or the folder that contains it) and copies the maps, and it adds
the game to Steam (SteamOS's `steamos-add-to-steam`) as `halo-vr.sh`.
Start SteamVR, then the game from the library. To update, do the same
with a newer zip.

Or from a
computer with the repository's `tools/frame_install.sh` and `ssh` to the
Frame (in its desktop mode, with SSH on):

```sh
tools/frame_install.sh --package OpenH1-Steam-Frame.zip <frame>
tools/frame_install.sh <frame>          # dist/halo-vr-frame, built from the repository
```

It copies the package to `~/apps/halo-vr/` on the Frame. It refuses while
the game runs, and never starts it. Each file goes in as `<name>.new`, is
checked against `SHA256SUMS`, then takes the old one's name at once. The
replaced `halo` is kept as `halo.prev` (only the last one), as are
`libSDL3.so.0` and `halo-vr.sh` when they change. `brokers.txt` is
replaced. It prints what changed. It never touches `config.toml`,
`halo-vr.env`, the maps, the logs or the saved games.

From a computer, add the game to Steam once, on the Frame: in Steam's
desktop mode, Games > Add a Non-Steam Game to My Library > Browse, select
`~/apps/halo-vr/halo-vr.sh`, then Add Selected Programs. The install
prints the same until Steam has such a shortcut. (It does not add it
itself: Steam keeps its shortcuts in a file it rewrites while it runs.)

Or by hand: copy the files of the zip's `halo-vr-frame` folder into
`~/apps/halo-vr/`, and add the shortcut as above.

To go back to the previous build: in `~/apps/halo-vr`, `mv halo.prev halo`
(and `libSDL3.so.0.prev`, `halo-vr.sh.prev` likewise, where they are).

## Uninstall

Remove the shortcut in Steam (its Properties > Manage > Remove
non-Steam game from your library), then the program's files:

```sh
cd ~/apps/halo-vr && cut -c 67- SHA256SUMS | xargs rm -f -- && rm -f SHA256SUMS halo.prev libSDL3.so.0.prev halo-vr.sh.prev
```

That keeps the maps, `config.toml`, `halo-vr.env` and the logs; remove
`~/apps/halo-vr` to remove those too. The saved games are in `~/.local/share/halo-linux`.

## Settings and logs

| Where | What |
| --- | --- |
| `~/apps/halo-vr/config.toml` | The settings (written at the first start) |
| `~/apps/halo-vr/halo-vr.env` | Your own environment for the game (`HALO_*=value` lines), read by `halo-vr.sh`. Updates leave it. Steam's launch options for the shortcut (`HALO_VR=0 %command%`) work too. |
| `~/apps/halo-vr/halo-vr.log` | The game's output when started from Steam, the previous run's in `halo-vr.log.prev`. A crash's backtrace is here. |
| `debug.txt` in the data folder | The game's log (`~/apps/halo-vr/debug.txt` with the maps beside `halo`) |
| `~/.local/share/halo-linux` | The saved games and the map cache |

Start SteamVR before the game. Without a headset the game plays flat, in
its window.

## Measuring performance

- Where a frame's time goes: put `HALO_GPU_STATS=1` and `HALO_VR_GPU_TIME=1`
  in `halo-vr.env` (or `gpu_stats = true` and `vr_gpu_time = true` under
  `[debug]` in `config.toml`). Every 300 frames `halo-vr.log` then has
  `vr: a frame's draws: ...` (the render thread's time before the eyes, on
  the eyes, and on the HUD and the present) and
  `vr: elapsed render GPU timeline per frame: ...` (the GPU's, from the
  frame's first commands to its last, gaps included); every 600 frames,
  `vulkan: ... frames: ... passes, ... draws`.
- The GPU's busy time alone, from the kernel, with nothing turned on in the
  game: copy `tools/gpu_busy.py` here and, over ssh while the game plays,
  `python3 gpu_busy.py --hz 120` (the headset's refresh rate). Near the
  frame's budget (8.33 ms at 120 Hz), the GPU holds the frame rate; well
  under it while the frame rate is still short, the CPU does.
