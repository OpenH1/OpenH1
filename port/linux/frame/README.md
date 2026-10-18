# Halo VR for the Steam Frame

The VR build of the Linux port (`configure.py --vr`, `ninja linux_vulkan`:
64-bit ARM, drawn with Vulkan), packaged
by `tools/frame_package.py` for a Steam Frame. Refer to "VR" and "Steam
Frame" in `port/linux/README.md` of the repository.

| File | Is |
| --- | --- |
| `halo` | The game |
| `libSDL3.so.0` | SDL3, which the game finds beside it |
| `halo-vr.sh` | The launcher: the target of the Steam shortcut. Its comments list the environment it gives the game. |
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

From a computer with the package and `ssh` to the Frame (in its desktop
mode, with SSH on):

```sh
tools/frame_install.sh <frame>          # dist/halo-vr-frame, from the repository
tools/frame_install.sh --package halo-vr-frame.tar.gz <frame>
```

It copies the package to `~/apps/halo-vr/` on the Frame. It refuses while
the game runs, and never starts it. Each file goes in as `<name>.new`, is
checked against `SHA256SUMS`, then takes the old one's name at once. The
replaced `halo` is kept as `halo.prev` (only the last one), as are
`libSDL3.so.0` and `halo-vr.sh` when they change. `brokers.txt` is
replaced. It prints what changed. It never touches `config.toml`,
`halo-vr.env`, the maps, the logs or the saved games.

At the first install, add the game to Steam once, on the Frame: in Steam's
desktop mode, Games > Add a Non-Steam Game to My Library > Browse, select
`~/apps/halo-vr/halo-vr.sh`, then Add Selected Programs. The install
prints the same until Steam has such a shortcut. (It does not add it
itself: Steam keeps its shortcuts in a file it rewrites while it runs.)

Without a computer: unpack the package on the Frame, and copy its files
into `~/apps/halo-vr/`.

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
