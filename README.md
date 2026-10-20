<p align="center">
  <a href="https://openh1.com"><img src="website/public/media/og.jpg" alt="OpenH1: welcome back, soldier" width="100%"></a>
</p>

<h3 align="center">Halo: Combat Evolved, recompiled for Mac, Windows, Linux, Android and VR.</h3>

<p align="center">
  The 2001 game, compiled natively from its decompiled Xbox source. It plays
  exactly as it shipped, and new Vulkan and Metal renderers make it look the
  way you remember.
</p>

<p align="center">
  <a href="https://github.com/OpenH1/OpenH1/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/OpenH1/OpenH1?label=release&color=9dff3c&labelColor=0a0d09"></a>
  <a href="https://github.com/OpenH1/OpenH1/actions/workflows/build.yml"><img alt="Build" src="https://github.com/OpenH1/OpenH1/actions/workflows/build.yml/badge.svg?branch=master"></a>
  <a href="https://discord.gg/MJa327PSWt"><img alt="Discord" src="https://img.shields.io/discord/1561955817463222332?logo=discord&logoColor=white&label=Discord&color=5865F2&labelColor=0a0d09"></a>
  <a href="LICENSE"><img alt="Licence: AGPL-3.0-or-later" src="https://img.shields.io/badge/licence-AGPL--3.0--or--later-eef4ea?labelColor=0a0d09"></a>
</p>

<p align="center">
  <a href="#download"><b>Download</b></a> ·
  <a href="https://openh1.com">openh1.com</a> ·
  <a href="https://discord.gg/MJa327PSWt">Discord</a> ·
  <a href="#game-data">Bring your own game</a> ·
  <a href="#contributing">Contributing</a>
</p>

OpenH1 is a fork of [OpenCE](https://github.com/OpenCommunityEdition/OpenCE),
the community's port, and adds a Mac app with its own Metal renderer, a
Vulkan renderer, VR and more. It keeps OpenCE's netcode, so players of the
two can play together. OpenCE starts from the decompilation of
[bnunu/halo-1](https://github.com/bnunu/halo-1), a fork of
[punpckhdq/halo](https://github.com/punpckhdq/halo), of the Xbox build 2342
(`cachebeta.exe`, SHA-256
`4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`).

## Features

- **The original game, natively.** Not emulation: the game's own code, with
  the same 30 ticks a second on every machine, so it plays as it shipped.
- **New renderers.** Vulkan on Linux, Android and the Steam Frame, Metal on
  the Mac, with SMAA, per-pixel dynamic lights and frames drawn between
  ticks at your display's rate (4K, 120 Hz and up).
- **VR** on the Steam Frame and OpenXR headsets: reload by hand, draw from
  holsters, raise the scope to your eye.
- **128 players** online and on the LAN, up to 4 in split screen on each
  machine, and the campaign in co-op over the internet. Plays with OpenCE.
- **Mods and Custom Edition maps**, from the game's own menus.

| | Original | OpenH1 |
| --- | --- | --- |
| **Dynamic lights**<br><sub>Derelict, a frag grenade</sub> | <img src="website/public/media/lights-original.webp" alt="A frag grenade lit as the original drew it" width="360"> | <img src="website/public/media/lights-openh1.webp" alt="The same moment with OpenH1's per-pixel lights" width="360"> |
| **Anti-aliasing**<br><sub>Blood Gulch, enlarged 2×</sub> | <img src="website/public/media/edge-original.webp" alt="Blood Gulch's cliffs with no anti-aliasing" width="360"> | <img src="website/public/media/edge-openh1.webp" alt="The same cliffs with SMAA" width="360"> |
| **Plasma glow**<br><sub>Blood Gulch, a charged plasma pistol</sub> | <img src="website/public/media/glow-original.webp" alt="A plasma bolt as the original drew it" width="360"> | <img src="website/public/media/glow-openh1.webp" alt="The same bolt with OpenH1's glow" width="360"> |

<sub>Real captures, the same moment twice. [openh1.com](https://openh1.com) has them side by side, with clips.</sub>

## Under the hood

| | |
| --- | --- |
| **Code** | The Xbox game's decompiled C, compiled natively, with few and marked changes. Not emulation |
| **Simulation** | 30 ticks a second, as shipped, with the same calculations on every machine |
| **Frames** | One per refresh of your display, each drawn between the last two ticks; nothing the game calculates changes |
| **Architecture** | A 64-bit program, with the game compiled as a 32-bit-pointer guest inside it (`arm64_32` on ARM, `x32` on x86-64) |
| **Renderers** | One command stream into Vulkan 1.3 on Linux, Android and in VR, and into Metal on the Mac. Only the 32-bit Windows build still draws with OpenGL; a 64-bit Vulkan build is on the way |
| **Picture** | SMAA or FXAA, anisotropic filtering, sharper shadows, per-pixel lighting, full model detail. One switch: Video Setup › Graphics › Preset, Classic or Enhanced |
| **Display** | Any resolution from 640 × 480, a field of view up to 150°, a HUD redrawn at 8× |
| **Network** | Plays online with OpenCE players; internet play needs no servers to set up |
| **Mods** | Texture, sound, text and model packs from the game's Mods menu; Custom Edition maps that share themselves |
| **Releases** | Every commit is built for every platform, and each build of `master` that passes is a release |

Contributions are welcome: refer to "Contributing" below. Players and
contributors meet on the [OpenH1 Discord](https://discord.gg/MJa327PSWt).

## Download

GitHub Actions builds the game for each commit, and each build of `master`
that passes on all the platforms is a new release. These links download the
builds of the latest release:

| Platform | Release | Debug |
| --- | --- | --- |
| macOS (Apple silicon, macOS 14.4 or later) | [halo-macos-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-macos-release.zip) | [halo-macos-debug.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-macos-debug.zip) |
| Windows | [halo-windows-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-windows-release.zip) | [halo-windows-debug.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-windows-debug.zip) |
| Linux x86-64 (also the Steam Deck) | [halo-linux-x86_64-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-x86_64-release.zip) | [halo-linux-x86_64-debug.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-x86_64-debug.zip) |
| Linux arm64 | [halo-linux-arm64-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-arm64-release.zip) | [halo-linux-arm64-debug.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-arm64-debug.zip) |
| Linux x86-64 VR | [halo-linux-x86_64-vr-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-x86_64-vr-release.zip) | |
| Steam Frame VR ([install](port/linux/frame/README.md#install-or-update)) | [halo-steam-frame-vr-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-steam-frame-vr-release.zip) | |
| Android | [halo-android-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-android-release.zip) | [halo-android-debug.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-android-debug.zip) |

Use the release build to play. The debug build stops at the first failed
assertion and writes it to the log. Use the debug build to find and report
problems. The [Releases](https://github.com/OpenH1/OpenH1/releases) page
keeps the last five releases: if the latest build has a problem, get an
older build there.

- **macOS:** unzip, open `Halo.dmg` and drag Halo to Applications. The app
  is not notarized yet, so the first start is refused: open System
  Settings > Privacy & Security and choose **Open Anyway**.
- **Windows:** unzip into a folder and start `halo.exe`. The executable is
  not signed yet, so Windows may say that it protected your PC: choose
  **More info**, then **Run anyway**.
- **Linux:** unzip into a folder and start `bin/openh1` in it. The game
  needs a GPU and driver with Vulkan 1.3. The folders are laid out as a
  Unix prefix is (`bin/`, `lib/`, `share/`), for package managers too:
  refer to "Packaging" in
  [port/linux/README.md](port/linux/README.md#packaging).
- **Android:** unzip and install the APK. Refer to
  [port/android/README.md](port/android/README.md).

The Windows, Linux and Android builds update themselves. At start-up they
look for a newer release, and ask if you want to install it (refer to
"Updates" in [port/linux/README.md](port/linux/README.md#updates)). On
macOS, download the new release.

## Game data

The port does not include the game data. Download an Xbox disc image
(`.xiso` or `.iso`) of Halo: Combat Evolved. All versions of the game
operate. The maps of the European (PAL) version were made for a slower
console. The port changes them to play as the North American (NTSC) maps do,
so players of the two versions can play together.

1. Start the game.
2. At the first start, the game asks for the disc image. Select it in the
   file picker. On Linux and Windows, a disc image (`.iso` or `.xiso`) next
   to the executable is used without the file picker. Where no file picker
   can be shown (on Linux, without the desktop portal or `zenity`), the game
   says to put the disc image next to the executable, and stops.
3. The game extracts the `maps/` folder. Then the game starts.

The `maps/` folder holds the Xbox maps only. If it holds Halo PC's maps
instead (a Halo PC or Custom Edition install's `maps` folder), the game says
so and stops: Custom Edition maps go in `custom_maps/`, beside `maps/`.

On Linux, the game puts `maps/` in the folder of the release (next to
`bin/`). On Windows, the game puts `maps/` next to the executable. On
Android, copy the disc image to the phone first. The app puts `maps/` in its
data folder. Refer to [port/android/README.md](port/android/README.md). On
the web, the page copies `maps/` out of the disc image into the browser's
storage for the site; nothing is uploaded. Refer to
[port/web/README.md](port/web/README.md).

## Mods

Mods are folders of Halo PC tag files, as the Halo Editing Kit and Invader
write them, in `mods/` next to `maps/`: texture, sound, text and model packs
that play over the maps' on your machine only, in any game. Each has a
`mod.toml`. Every mod in the folder is on; the main menu's Mods turns them
off and on and sets the order they load in.
Refer to [docs/mods.md](docs/mods.md).

## Platforms

Each platform has its own instructions:

| Platform | Instructions |
| --- | --- |
| Linux x86-64 (x86-64 executable, Vulkan 1.3, SDL3) | [port/linux/README.md](port/linux/README.md#64-bit-x86) |
| Linux arm64 (arm64 executable, Vulkan 1.3, SDL3) | [port/linux/README.md](port/linux/README.md#64-bit-arm) |
| Linux VR (x86-64 and arm64, Vulkan 1.3; OpenXR headsets such as the Steam Frame) | [port/linux/README.md](port/linux/README.md#vr) |
| Windows (32-bit x86 executable, OpenGL 4.5, SDL3) | [port/windows/README.md](port/windows/README.md) |
| Android (arm64 app, Vulkan 1.3, SDL3) | [port/android/README.md](port/android/README.md) |
| macOS (Apple silicon: Halo.app, Metal) | [port/macos/README.md](port/macos/README.md) |
| Web (WebAssembly; not built for now: it returns with a WebGPU renderer) | [port/web/README.md](port/web/README.md) |

The Linux README also gives the controls, the settings and the multiplayer
functions. These are almost the same on all platforms.

## Multiplayer

The game can play system link games on a local network and on the internet:

- A system link game can have up to 128 players on up to 128 machines.
- Linux (x86-64 and arm64), Windows and Android machines and browsers can play
  in the same game. On a local network, the native builds play system link;
  browsers play each other in rooms, over WebRTC
  ([port/web/README.md](port/web/README.md)).
- An invite link, or the server browser, lets a machine join a game on the
  internet, browsers too: a native build takes a browser's WebRTC
  ([port/linux/README.md](port/linux/README.md), "Browsers"). No server of
  this project is necessary.
- The netcode is new. Each machine moves its own player at once,
  and the host makes the decisions for the game. Refer to
  [port/linux/NETCODE.md](port/linux/NETCODE.md).

To find people to play with, ask in #looking-for-game on the
[Discord](https://discord.gg/MJa327PSWt).

## Build the game

You do not need the Xbox SDK. The port supplies the SDK declarations that
the game uses. Refer to [port/include/xdk](port/include/xdk/README.md).

To build the game:

1. Install Python and [ninja](https://ninja-build.org/).
2. Install the tools for your platform. Refer to the README for the
   platform.
3. In the root folder of the repository, enter `python configure.py`.
4. Enter `ninja` with the target for the platform:

| Target | Result |
| --- | --- |
| `ninja linux_x86_64` (on 64-bit x86 Linux) | `build/linux_x86_64/halo` and `libSDL3.so.0` (with `python configure.py --vr`, its VR mode) |
| `ninja linux_arm64` (on 64-bit ARM Linux) | `build/linux_arm64/halo` and `libSDL3.so.0` (with `python configure.py --vr`, its VR mode) |
| `ninja windows` (on Windows, or on Linux after `configure.py --windows-cross`) | `build/windows/halo.exe` and `SDL3.dll` |
| `ninja android_apk` | `port/android/app/build/outputs/apk/debug/app-debug.apk` |
| `ninja macos` (on a Mac with Apple silicon) | `build/macos/Halo.app` (and `halo` with `libSDL3.0.dylib`, for development); `ninja macos_dmg`: `Halo.dmg` |
| `ninja web` | `build/web/site/`, the web site (`python tools/web_serve.py` serves it) |

If you enter `ninja` without a target, ninja builds the game for the
computer that you use: on 64-bit x86 Linux, `ninja linux_x86_64`.

The Linux builds are 64-bit, and draw with Vulkan 1.3: `configure.py`
downloads the Vulkan headers with musl and SDL3, so the build needs no
OpenGL or Vulkan development packages. Refer to
[port/linux/README.md](port/linux/README.md).

`tools/ci_build.py` makes the same builds as GitHub Actions. For example,
enter `python tools/ci_build.py linux_x86_64 release`.

### Build options

Give these options to `configure.py`:

| Option | Result |
| --- | --- |
| (none) | A debug build. A failed assertion stops the game. |
| `--release` | A release build. The game does not examine assertions, as in the retail game. |
| `--portable` | The Windows build operates on all x86-64 processors. Use this option for builds that you give to other persons. (The Linux, Android and macOS builds operate on all processors of their architecture without it.) |
| `--vr` | The VR mode (OpenXR) in the Linux build of the computer. Refer to "VR" in [port/linux/README.md](port/linux/README.md#vr). |
| `--lto=thin`, `--lto=off` | Less link-time optimization for the Windows build. The link is faster. |
| `--pgo=off` | No profile-guided optimization. |
| `--pgo=train` | Records a new optimization profile for the Windows build. Refer to "Optimization profiles". |
| `--profile` | A profiling build. It records CPU times. Refer to "Profiling builds". |
| `--guest-llvm=system`, `--guest-llvm=FOLDER` | The guest builds (Linux arm64 and x86-64, Android, macOS) compile the guest with clang and the LLVM tools on the PATH, or with the LLVM in FOLDER, not with the pinned LLVM. Refer to "The guests' compiler". |
| `--guest-llvm-cache FOLDER` | The folder for the pinned LLVM (else `~/.cache/openh1`, on macOS `~/Library/Caches/openh1`). |
| `--guest-base` | The Linux arm64 and Android builds put the guest's memory at a base that the host chooses, not in the low 4 GB (a base register: `tools/guest_base_register.py`). The 64-bit x86 build has none: with it, `ninja linux_x86_64` stops with the reason. |
| `--web-emcc PATH` | The Emscripten `emcc` for `ninja web` (else the one on the PATH, or `~/emsdk`'s). |

Without `--portable`, the Windows build uses all the instructions of the
processor that builds it (`-march=native`). Such a build does not always
start on a different computer.

### The guests' compiler

The Linux arm64 and x86-64, Android and macOS builds compile the game as a
guest image (`arm64_32` or `x32` code). On all computers, the guest is
compiled and linked by the same LLVM: release 22.1.8 from
[LLVM's releases](https://github.com/llvm/llvm-project/releases), for
64-bit x86 Linux, 64-bit ARM Linux and macOS on Apple silicon. The first
`configure.py` downloads it (approximately 1.5 GB, checked against its
SHA-256) and keeps clang, `ld.lld` and the LLVM tools that the builds use
(approximately 700 MB) in `~/.cache/openh1` (on macOS
`~/Library/Caches/openh1`, or the folder of `--guest-llvm-cache`). The
version and the checksums are in `tools/guest_toolchain.py`. The hosts
use the compiler of the system. On Linux, the LLVM tools need glibc 2.34
or later, libstdc++ and zlib; LLVM's `ld.lld` is also linked to
libxml2.so.2, for Windows links only, which some distributions (Arch,
CachyOS) no longer have, so a small stand-in for it is written beside the
tools (`tools/library_stand_in.py`).

A compiler that you give for a build (`--linux-arm64-cc`,
`--linux-x86-64-cc`, `--android-guest-cc`, `--macos-cc`,
`--macos-guest-cc`) is used instead, with the LLVM tools on the PATH, as
with `--guest-llvm=system`.

### Optimization profiles

The builds use profiles of the game to optimize the code:

- `pgo/halo_linux.profdata` for the guest builds: Linux, Android and macOS.
- `pgo/halo_windows.profdata` for Windows.

The profiles need clang 22 or later. With an older clang, the builds do not
use the profiles. The guest builds' pinned LLVM (refer to "The guests'
compiler") reads them on all platforms.

The Linux profile was recorded by the 32-bit Linux build, which is gone:
the guest builds cannot record one yet, and use it as it is (code changed
since then goes without). To record a new Windows profile:

1. Delete the profile.
2. Enter `python configure.py --pgo=train`.
3. Enter `ninja windows`.

The build then plays the main menu and the first minute of each campaign
level. This procedure continues for approximately 15 minutes. The game
data must be in `assets/`.

### Profiling builds

A profiling build records where the game spends its time.

1. Enter `python configure.py --profile`, then build as usual.
2. Start a recording. Enter `profile_record` in the console (or the telnet
   console), or set `debug.profile_record = true` in `config.toml`. To
   record each game of a session, also set
   `debug.profile_record_when = "game"`.
3. Stop the recording. Enter `profile_stop`, load another map, or quit.

The game writes numbered `.part<n>.json` files under `profiles/` in its data root
and logs their paths. Parts are retained after the recording stops. Logs go to
standard error; a Windows release build without standard error writes them to
`halo.log` next to `halo.exe`.

`tools/net_report.py` is a helper for developers. Enter
`python tools/net_report.py <recording.part1.json>` to read a recording
as tables. The command also writes `<recording>.summary.txt`, a short text
file.

| Command | Result |
| --- | --- |
| `profile_record [seconds]` | Starts a recording. With a number of seconds, the recording stops after that time. |
| `profile_stop` | Stops the recording. |

`debug.profile_memory` sets the memory of a recording in MB (default 256,
from 4 to 1024). A recording has no length limit:
the game writes a part each time the memory is full. Refer to "Settings" in
[port/linux/README.md](port/linux/README.md).

On Android, the files are in `/sdcard/Android/data/com.openh1.openh1/files/profiles/`.
Enter `adb pull` to copy the recording parts to a computer.

A profiling build plays with normal builds. It does not use
`--pgo=train`.

## Testing

The tests are listed in [AGENTS.md](AGENTS.md) ("Testing"). Most run
without the game data, in GitHub Actions too.

One check needs the retail maps, so it runs on your own computer (or on a
self-hosted runner), not in GitHub Actions: `tools/net_compare.py` plays
two builds together in a network game on one computer and checks that they
stay in step. Run it when a change can alter how a build calculates the
game, as a new platform, compiler or architecture does, before it is
merged. For example, to check your build against the latest release:

```sh
python tools/net_compare.py --a build/linux_x86_64/halo --b <release folder>/bin/openh1 \
    --data <folder with maps/> --maps bloodgulch,beavercreek,hangemhigh --swap
```

Each game prints PASS or FAIL. A FAIL names the first player, tick and
value that differ. Refer to "Comparing two builds" in
[port/linux/NETCODE.md](port/linux/NETCODE.md).

## Contributing

Pull requests are welcome, from small fixes to whole renderer features. We
review them promptly, and merge what is ready.

1. Fork the repository and branch from `master`.
2. Read [AGENTS.md](AGENTS.md): the layout, the build, the conventions (the
   game's sources are C89, `gnu89`), the tests, and what must stay
   deterministic.
3. Open a pull request. GitHub Actions builds it for Android, Windows,
   macOS and the Linux builds, and runs the tests (debug builds off
   `master`, so a check takes minutes).
4. Say what you tested and how, and what still needs a device to check.

**Fixes from OpenCE are welcome too.** If you have a pull request waiting in
[OpenCE](https://github.com/OpenCommunityEdition/OpenCE), open it here as
well, or tell us about it. We bring good ones in with their authorship kept
and credit given, as for OpenCE's #328, #369 and #392.

**Keep network play compatible.** A change to what the game sends, or to how
it calculates the game, needs `HALO_PORT_NETWORK_VERSION` and a
`tools/net_compare.py` run (refer to "Testing"), so that OpenH1 and OpenCE
players can still play together.

Report problems on the [issue tracker](https://github.com/OpenH1/OpenH1/issues),
with the build number (the main menu shows it) and `debug.txt`. For help,
or to talk about a change before you make it, ask on the
[Discord](https://discord.gg/MJa327PSWt): #help, and #dev-general.

## Licence

OpenH1 is free software: you can give it to others and change it under the
terms of the GNU Affero General Public License as published by the Free
Software Foundation, version 3 of the License or (at your option) any later
version (`AGPL-3.0-or-later`). It comes with no warranty. The full text is in
[LICENSE](LICENSE).

- **From the change.** The AGPL applies from the commit that made the change,
  and to each release built from that commit or a later one. Earlier
  releases, and the source at every earlier commit, were released under
  [CC0 1.0](LICENSES/CC0-1.0.txt) and stay that way. (OpenH1 took the
  change with its merge of OpenCE, which had made it.)
- **What OpenH1 builds on.** The decompilation of
  [punpckhdq/halo](https://github.com/punpckhdq/halo) is under the AGPL 3.0
  from 17 October 2026, and under CC0 1.0 before that date.
  [bnunu/halo-1](https://github.com/bnunu/halo-1) is under CC0 1.0.
- **Contributions** are under `AGPL-3.0-or-later`. A change taken from a
  project under GPL 3.0 keeps that licence and its notices; the GPL 3.0 and
  the AGPL 3.0 let the two be combined (section 13 of each).
- **Hosts and servers.** If you change the game and let other persons play
  with your version over a network (for example, as the host of an internet
  game, or on a web site), the AGPL asks you to offer them the source of
  your version (section 13). The game as released has its source here, and
  each release's source is its tag (`build-<n>`).
- **Third-party code, fonts and art** keep their own licences, which are
  next to them (`port/third_party`, `port/assets/fonts`,
  `port/assets/icons`, `port/assets/text_filter`). The release downloads and
  the web site's `licenses.txt` carry their notices.
- **Halo is not covered.** The licence covers the work of OpenH1's and
  OpenCE's authors. It gives no rights in Halo, its maps or anything else that Microsoft owns.

## Disclaimer

OpenH1 is an unofficial fan project, not endorsed by or affiliated with
Microsoft, Xbox Game Studios, Halo Studios or Bungie. Halo is a trademark of
Microsoft Corporation. No game data is included: you need your own copy of
the game.
