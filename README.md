# OpenH1

Halo: Combat Evolved for Linux, Windows, macOS, Android, VR and the web.

OpenH1 is a port of the Halo: Combat Evolved decompilation. The
decompilation is of the Xbox build 2342 (`cachebeta.exe`, SHA-256
`4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`).

<img width="1289" height="995" alt="The game on Linux" src="https://github.com/user-attachments/assets/0d3ad50f-f8b8-46cf-aef8-e3661da2a7d7" />

OpenH1 is a fork of [OpenCE](https://github.com/OpenCommunityEdition/OpenCE),
the community's port, and adds a Mac app with its own Metal renderer, a
Vulkan renderer, VR and more. It keeps OpenCE's netcode, so players of the two can
play together. OpenCE starts from the decompilation of
[bnunu/halo-1](https://github.com/bnunu/halo-1), a fork of
[punpckhdq/halo](https://github.com/punpckhdq/halo).

## Download

GitHub Actions builds the game for each commit, and each build of `master`
that passes on all the platforms is a new release. These links download the
builds of the latest release:

| Platform | Release | Debug |
| --- | --- | --- |
| macOS (Apple silicon, macOS 14.4 or later) | [halo-macos-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-macos-release.zip) | [halo-macos-debug.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-macos-debug.zip) |
| Windows | [halo-windows-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-windows-release.zip) | [halo-windows-debug.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-windows-debug.zip) |
| Linux | [halo-linux-i686-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-i686-release.zip) | [halo-linux-i686-debug.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-i686-debug.zip) |
| Linux arm64 | [halo-linux-arm64-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-arm64-release.zip) | [halo-linux-arm64-debug.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-arm64-debug.zip) |
| Linux x86-64 | [halo-linux-x86_64-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-x86_64-release.zip) | [halo-linux-x86_64-debug.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-x86_64-debug.zip) |
| Linux x86-64 VR | [halo-linux-x86_64-vr-release.zip](https://github.com/OpenH1/OpenH1/releases/latest/download/halo-linux-x86_64-vr-release.zip) | |
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
- **Linux:** unzip into a folder and start `bin/opence` in it. The folders
  are laid out as a Unix prefix is (`bin/`, `lib/`, `share/`), for package
  managers too: refer to "Packaging" in
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
2. At the first start, the game asks for the disc image. Select it.
3. The game extracts the `maps/` folder. Then the game starts.

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
| Linux (32-bit x86 executable, OpenGL 4.5, SDL3) | [port/linux/README.md](port/linux/README.md) |
| Linux arm64 (arm64 executable, OpenGL ES 3, SDL3) | [port/linux/README.md](port/linux/README.md#64-bit-arm) |
| Linux x86-64 (x86-64 executable, OpenGL 4.5, SDL3; no 32-bit libraries) | [port/linux/README.md](port/linux/README.md#64-bit-x86) |
| Linux arm64 VR (Vulkan; OpenXR headsets such as the Steam Frame) | [port/linux/README.md](port/linux/README.md#vr) |
| Windows (64-bit executable, Vulkan 1.3, SDL3; Windows 10 1903 or later) | [port/windows/README.md](port/windows/README.md) |
| Android (arm64 app, Vulkan 1.3, SDL3) | [port/android/README.md](port/android/README.md) |
| macOS (Apple silicon: Halo.app, Metal) | [port/macos/README.md](port/macos/README.md) |
| Web (WebAssembly, WebGL 2, SDL3; desktop Chrome, Edge and Firefox) | [port/web/README.md](port/web/README.md) |

The Linux README also gives the controls, the settings and the multiplayer
functions. These are almost the same on all platforms.

## Multiplayer

The game can play system link games on a local network and on the internet:

- A system link game can have up to 128 players on up to 128 machines.
- Linux (x86, x86-64 and arm64), Windows and Android machines and browsers can play
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
| `ninja linux` | `build/linux/halo` |
| `ninja linux_arm64` (on 64-bit ARM Linux) | `build/linux_arm64/halo` and `libSDL3.so.0` |
| `ninja linux_x86_64` (on 64-bit x86 Linux, after `python configure.py --linux-x86-64`) | `build/linux_x86_64/halo` and `libSDL3.so.0` |
| `ninja linux_vulkan` (on 64-bit ARM or x86 Linux, after `python configure.py --linux-vulkan`) | `build/linux_vulkan/halo` and `libSDL3.so.0`; the Vulkan renderer (`--vr` also enables OpenXR) |
| `ninja windows` (on Windows) | `build/windows/halo.exe` and `SDL3.dll` |
| `ninja android_apk` | `port/android/app/build/outputs/apk/debug/app-debug.apk` |
| `ninja macos` (on a Mac with Apple silicon) | `build/macos/Halo.app` (and `halo` with `libSDL3.0.dylib`, for development); `ninja macos_dmg`: `Halo.dmg` |
| `ninja web` | `build/web/site/`, the web site (`python tools/web_serve.py` serves it) |

If you enter `ninja` without a target, ninja builds the game for the
computer that you use: on 64-bit x86 Linux, the x86 build (`ninja linux`).

`tools/ci_build.py` makes the same builds as GitHub Actions. For example,
enter `python tools/ci_build.py linux release`.

### Build options

Give these options to `configure.py`:

| Option | Result |
| --- | --- |
| (none) | A debug build. A failed assertion stops the game. |
| `--release` | A release build. The game does not examine assertions, as in the retail game. |
| `--portable` | The Linux build operates on all x86-64 processors (the Windows build always does). The Linux build also operates on older distributions and on SteamOS: refer to "Portable build" in [port/linux/README.md](port/linux/README.md#portable-build). Use this option for builds that you give to other persons. |
| `--lto=thin`, `--lto=off` | Less link-time optimization. The link is faster. |
| `--pgo=off` | No profile-guided optimization. |
| `--pgo=train` | Records a new optimization profile. Refer to "Optimization profiles". |
| `--profile` | A profiling build. It records CPU times. Refer to "Profiling builds". |
| `--guest-llvm=system`, `--guest-llvm=FOLDER` | The guest builds (Linux arm64 and x86-64, Android, macOS, Windows) compile the guest with clang and the LLVM tools on the PATH, or with the LLVM in FOLDER, not with the pinned LLVM. Refer to "The guests' compiler". |
| `--guest-llvm-cache FOLDER` | The folder for the pinned LLVM (else `~/.cache/opence`, on macOS `~/Library/Caches/opence`). |
| `--guest-base` | The Linux arm64 and Android builds put the guest's memory at a base that the host chooses, not in the low 4 GB (a base register: `tools/guest_base_register.py`). The 64-bit x86 build has none: with it, `ninja linux_x86_64` stops with the reason. |
| `--web-emcc PATH` | The Emscripten `emcc` for `ninja web` (else the one on the PATH, or `~/emsdk`'s). |

Without `--portable`, the Linux build uses all the instructions
of the processor that builds them (`-march=native`). Such a build does not
always start on a different computer.

### The guests' compiler

The Linux arm64 and x86-64, Android, macOS and Windows builds compile the
game as a guest image (`arm64_32` or `x32` code). On all computers, the
guest is compiled and linked by the same LLVM: release 22.1.8 from
[LLVM's releases](https://github.com/llvm/llvm-project/releases), for
64-bit x86 Linux, 64-bit ARM Linux, macOS on Apple silicon and 64-bit
Windows. The first
`configure.py` downloads it (approximately 1.5 GB, checked against its
SHA-256) and keeps clang, `ld.lld` and the LLVM tools that the builds use
(approximately 700 MB) in `~/.cache/opence` (on macOS
`~/Library/Caches/opence`, or the folder of `--guest-llvm-cache`). The
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

- `pgo/halo_linux.profdata` for Linux, Android and the other guest builds
  (macOS, Windows).

The profiles need clang 22 or later. With an older clang, the builds do not
use the profiles. The guest builds' pinned LLVM (refer to "The guests'
compiler") reads them on all platforms.

To record a new profile:

1. Delete the profile.
2. Enter `python configure.py --pgo=train`.
3. Enter `ninja linux`.

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

On Android, the files are in `/sdcard/Android/data/com.halo.decomp/files/profiles/`.
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
merged. For example, to check a 64-bit build against the 32-bit build:

```sh
python tools/net_compare.py --a build/linux/halo --b build/linux_x86_64/halo \
    --data <folder with maps/> --maps bloodgulch,beavercreek,hangemhigh --swap
```

Each game prints PASS or FAIL. A FAIL names the first player, tick and
value that differ. Refer to "Comparing two builds" in
[port/linux/NETCODE.md](port/linux/NETCODE.md).
