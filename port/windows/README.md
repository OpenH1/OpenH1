# Windows

`ninja windows` compiles the game with clang for 32-bit x86 Windows. The
result is a native executable, `build/windows/halo.exe`, with `SDL3.dll`
next to it. You must build on Windows.

The Windows build uses the platform layer of the Linux build
(`port/linux/src`). The sound, the input, the settings and the multiplayer
functions are the same as on Linux. The graphics are drawn by the OpenGL
backend (`port/linux/src/xgpu_gl.c`, OpenGL 4.5) in the game itself, where
the Linux builds draw with Vulkan in their 64-bit host. Refer to
[port/linux/README.md](../linux/README.md).

## Install the release

The release is `OpenH1-Windows.zip`. Unzip it into a folder and start
`halo.exe`. The executable is not code-signed, so when it was downloaded
with a browser, Windows SmartScreen can say "Windows protected your PC". It
says that of any app that is not signed. Choose **More info**, then **Run
anyway**.

### Install with Scoop

[Scoop](https://scoop.sh) installs the same zip with no warning: its
downloads do not carry the browser's mark (the Mark of the Web) that
SmartScreen checks. Scoop checks each download against the hash in its
manifest.

```powershell
scoop bucket add openh1 https://github.com/OpenH1/scoop-bucket
scoop install openh1
```

The bucket is [OpenH1/scoop-bucket](https://github.com/OpenH1/scoop-bucket).
Its manifest follows the releases by itself (its Excavator workflow, every
6 hours).

- The game is in `~\scoop\apps\openh1\current`, with an **OpenH1** Start
  menu shortcut and an `openh1` command.
- The files of the player next to `halo.exe` are kept in
  `~\scoop\persist\openh1` and linked into each version: `config.toml`,
  `maps\`, `custom_maps\`, `mods\`, `screenshots\`, `crashes\`, `bans.txt`,
  `cheaters.txt` and `text_filter.txt`. At the first start the game copies
  `maps\` out of the disc image as usual; the manifest moves that folder
  into `persist` before an update or an uninstall. Saved games stay in
  `%APPDATA%\halo`.
- A first install writes `update.auto = false` in `config.toml`: Scoop
  updates the game (`scoop update openh1`), not the game's updater, which
  would replace the files of Scoop's version folder.

## Requirements

You do not need the Xbox SDK.

- Visual Studio or the Visual Studio Build Tools, with the C++ workload.
  The build uses the 32-bit (x86) MSVC libraries and a Windows 10 or 11
  SDK. It does not use the MSVC compiler.
- LLVM (clang and lld), Python 3 and ninja in the `PATH`. For example,
  enter `scoop install llvm python ninja`.
- A network connection for the first build. `configure.py` downloads the
  Visual C++ development package of SDL 3.4.16 to
  `build/windows/third_party`.

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja windows`.

`configure.py` generates the Windows build only on Windows, and with
`--windows-cross`, which cross-compiles it from Linux: the same clang and
lld build against an MSVC toolchain, whose directories
`--windows-vctools-dir` (the versioned folder under `VC/Tools/MSVC`, the
one with `include` and `lib`) and `--windows-sdk-dir` (the SDK folder with
`Include` and `Lib`) name, or `VCToolsInstallDir` and `WindowsSdkDir`. The
build passes them to clang in `INCLUDE` and `LIB`. `--windows-cc` names
the cross compiler (a run-of-the-mill clang by default).

    python configure.py --windows-cross \
        --windows-vctools-dir "$VCToolsInstallDir" \
        --windows-sdk-dir "$WindowsSdkDir"
    ninja windows

The toolchain can be an [msvc-wine](https://github.com/mstorsjo/msvc-wine)
install or an unmodified MSVC from a Windows install (from an NTFS volume,
for instance). The SDK must have a versioned folder in each of `Include`
and `Lib` (`Include/<version>/um`): a plain `xwin splat` does not, a splat
made with `xwin splat --use-winsysroot-style` does.

The build uses all the instructions of the processor of the computer that
builds it (`-march=native`). Such a build does not always start on a
different computer. To make a build for other computers, enter
`python configure.py --portable`. Refer to the build options in the main
[README](../../README.md#build-options).

For a new optimization profile (`--pgo=train`), the build compiles the
profile runtime of LLVM for 32-bit x86 (`pgo/halo_profile_runtime.c`).
LLVM for Windows supplies this runtime only for x86-64.

## Start the game

Enter `build\windows\halo.exe`.

The game finds the game data as on Linux. Refer to "Start the game" in
[port/linux/README.md](../linux/README.md#start-the-game).

| Item | Location |
| --- | --- |
| Settings | `config.toml` next to `halo.exe` |
| Saved games | `%APPDATA%\halo`, or `paths.saves` in `config.toml` |
| Log | `debug.txt` in the data root (the folder that contains `maps\`) |
| Log of the port | The console. The release build has no console: `halo.log` next to `halo.exe` |
| Crash reports | `crashes\` next to `halo.exe`, until the game sends them |

## How the port operates

The game is 32-bit code because its data contains 32-bit pointers. On
Windows the game runs as a 32-bit x86 process; on Linux it is 32-bit-pointer
code in a 64-bit process (port/linux/README.md, "64-bit x86"). The clang
target `i686-pc-windows-msvc` gives the ABI of MSVC: the structure layout,
the 16-bit `wchar_t` and the calling conventions. Thus the Windows build
needs fewer changes than the Linux builds.

The executable is large-address-aware, because the platform layer reserves
the Xbox memory at `0x80000000`.

### Headers

- The game and the platform layer use the Xbox SDK declarations of
  `port/include/xdk`, not the Windows SDK declarations with the same names.
  The compiler reads `port/include/xdk` before the Windows SDK.
- The C runtime is the static UCRT of Windows.
- `include/halo_windows_prefix.h` is the first header of each file. It gives
  Windows names to the Xbox SDK functions of the platform layer (for example
  `CreateFileA`, `ReadFile`, `Sleep`). Thus the code for Windows gets to
  Windows. The list of names is in `include/halo_windows_api_names.h`.
- `include/crt` changes some C runtime headers. The file functions of the
  game (`fopen`, `open`, `mkdir`) go through the translation of Xbox paths
  (`src/windows_crt.c`).
- `include/posix` declares the POSIX functions that the platform layer uses.

### Code for Windows

These files use only the Windows SDK:

| File | Contents |
| --- | --- |
| `src/win32_files.c`, `src/win32_net.c` | The file and socket functions of `port/linux/src/posix.h`. |
| `src/win32_posix.c` | The POSIX functions on Windows threads, critical sections, condition variables, `VirtualAlloc` and the performance counter. |
| `src/win32_memory_watch.c` | The write tracking of textures, with a vectored exception handler. |
| `src/win32_crash.c` | The crash reports. Refer to "Crash reports". |

`port.json` gives the Linux files that these files replace, and the Windows
libraries of the link.

### Inline functions

MSVC makes a COMDAT copy of an external `__inline` function where it does
not inline a call. Some files call such functions through a prototype.
clang inlines more calls. Thus a function can have no copy. The build
prevents this:

- `port/linux/game/msvc_comdat.c` gives one weak definition of each header
  inline function.
- A wrapper takes the address of each `__inline` function in a `.c` file.
  Thus the compiler makes an external copy (`inline_export_wrapper` in
  `tools/windows_build.py`).

MSVC gives file scope to a structure tag in a prototype. clang does not.
Thus the build also includes the declarations of the Linux build
(`build/windows/halo_msvc_tags.h`).

## Crash reports

A crash writes the faulting address and the calls that led to it to
`debug.txt` and to the log. The builds of the workflow (the releases)
also send a crash report to the Sentry project of the developers:

1. The game starts a second copy of `halo.exe` (`halo.exe --crash-report`).
   This copy writes a minidump of the game to `crashes\` next to
   `halo.exe`, and the game stops. A minidump contains the stacks and the
   registers of the threads, and the list of the modules. It does not
   contain the other memory of the game.
2. At the first crash, the copy asks the player whether to send crash
   reports. `crash_reports.upload` in `config.toml` keeps the answer:
   `"yes"` sends this report and all the reports after it, and `"no"` sends
   no reports and does not ask again. The answer `"no"` deletes the
   minidump.
3. The copy sends the minidump and a copy of `halo.log` to Sentry, then
   deletes them. If it cannot send them (for example, with no network
   connection), the game sends them when it starts the next time
   (`halo.exe --crash-upload`, in the background). `crashes\` keeps at
   most 8 reports.

Sentry finds the function names and the source lines of the minidump in
the PDBs (`halo.pdb`, `SDL3.pdb`). The `release` job of the workflow
uploads the PDBs and the executables of each build of main to Sentry. This
needs the `SENTRY_AUTH_TOKEN` secret of the repository: an organization
auth token from the settings of the Sentry organization (the token gives
the organization and its region). The `SENTRY_PROJECT` variable of the
repository can give the slug of the project; without it, the job uses the
id of the project in the DSN. For a personal token instead, also set the
`SENTRY_ORG` variable (the slug of the organization) and the `SENTRY_URL`
variable (`https://de.sentry.io`).

Each release also has the PDBs as `OpenH1-Windows-symbols.zip` (and `OpenH1-Windows-debug-symbols.zip`).
`tools/symbolize_crash.py` uses them to add the function names and the
source lines to the crash lines of a `debug.txt` or a `halo.log`:

```
python tools/symbolize_crash.py debug.txt halo.exe
```

Give the `halo.exe` of the build that crashed, with its `halo.pdb` next to
it. The tool needs `llvm-symbolizer` (LLVM).

Builds without a build number (a local build, or a build of a branch other
than main) send no crash reports. To test the crash reports with such a
build, set the `HALO_CRASH_REPORTS_ANY_BUILD` environment variable.

The crash reports also include the crashes of `abort()` and of an invalid
argument to a function of the C runtime.

## Testing under Wine on Linux

`tools/wine_bench.sh` runs a Windows build under Wine on a Linux computer
with a GPU. It can start the game and play a level, draw it with the GPU's
driver, collect the logs and take shots, all without a Windows machine. It
needs Wine (with winevulkan, as Wine has had for years), the GPU's OpenGL
and Vulkan drivers, gamescope or KWin for the headless display, `unzip`,
`curl`, `python3`, and `gh` for CI artifacts. It also needs game data: a
`maps` folder, `~/halo-x86-maps/maps` by default (`OPENH1_BENCH_MAPS`).

```sh
tools/wine_bench.sh setup               # the Wine prefix and folders, in ~/openh1-wine (OPENH1_BENCH)
tools/wine_bench.sh fetch               # the latest release's OpenH1-Windows.zip
tools/wine_bench.sh fetch build-510     # a release by its tag (--debug: OpenH1-Windows-debug.zip)
tools/wine_bench.sh fetch 199           # a pull request's halo-windows-debug artifact (gh)
tools/wine_bench.sh fetch run:<run id>  # a workflow run's; or a path to a zip or a folder
tools/wine_bench.sh run                 # 30 s of bots on Blood Gulch, headless; then the frame rate
tools/wine_bench.sh run --map ratrace --bots 3 --seconds 60 HALO_INTERPOLATION=false
tools/wine_bench.sh run --visible       # in a window on the computer's own display, at the menus
tools/wine_bench.sh shot --every 600 --max 6   # a frame every 600, to the run's shots/
tools/wine_bench.sh logs                # the last run's debug.txt, halo.log, frame rate and Wine's errors
tools/wine_bench.sh doctor              # Wine, the GPU, OpenGL in the headless display, the builds
```

- The last build fetched is the current one, and `run --build <name>`
  runs another. The builds are in `~/openh1-wine/builds`.
- Each run gets its own folder, `~/openh1-wine/runs/<date>-<build>`.
  The folder is the run's data root: `maps/` is linked to the game data,
  and the folder also holds the run's `debug.txt`, its `wine.log` (the
  game's output and Wine's, timestamped), `environment.txt`, `halo.log`
  (from a release build) and `shots/`. `runs/last` is the newest run.
- A run without `--visible` uses a display of its own that nobody sees:
  gamescope's headless backend (or KWin's virtual one, with
  `OPENH1_BENCH_DISPLAY=kwin`), drawn by the GPU. The display gets its own
  D-Bus and settings folder, so it does not touch the desktop session.
  The run plays a local game with bots (`debug.network_test`
  `local:<map>:slayer`, `bots.count`) with vsync off, silent sound,
  system link on the loopback, and no internet play, updates, crash
  reports or clipboard. When `debug.exit_after` ends it, the bench prints
  the renderer, the frame rate while playing (from `debug.gpu_stats`'
  line every 60 frames) and Wine's errors. With OpenGL, the headless
  display refreshes at `--refresh` Hz (1000 by default), and its refresh
  limits the frame rate even with vsync off.
- `VAR=value` gives a run a setting's variable (`HALO_*`, as in the
  settings table of [port/linux/README.md](../linux/README.md)) or one of
  Wine's (`WINEDEBUG=+wgl`), over the bench's own. The words after `--`
  go to `halo.exe` as its arguments.
- `shot` saves frames (`debug.screenshot_every`) and keeps at most
  `--max` of them, spread over the run. They show the game's content:
  keep them on the computer, out of the repository and out of public
  places, as with the maps.
- The prefix uses Wine's GLX (`UseEGL` = `N`). With Wine 11's EGL, its
  default on X11, SDL finds no pixel format for the window on NVIDIA's
  driver, and the game runs without a window.
- Only one run at a time: runs share the prefix, and each run stops the
  prefix's Wine processes when it ends.

Wine is not Windows. A run under Wine shows that the game starts, plays
and draws with a real GPU driver, and it gives the logs and shots to
compare with the Linux builds. It does not show these:

- The drivers on Windows. Wine passes OpenGL and Vulkan to the Linux
  driver, which differs from the same GPU's Windows driver in its
  behaviour, its bugs and its frame pacing. Measure frame rate and pacing
  on Windows.
- Windows itself: its memory layout (the DLLs, overlays and security
  programs in the low 4 GB), its exception dispatch and stack limits, and
  minidumps.
- Microsoft Defender, SmartScreen and the Mark of the Web, the unsigned
  executable, Scoop, and the updater replacing a real install.

Check these on a Windows computer before a release.

## Limits

- Bink video is not available. The game skips the movies.
- The game uses only the ANSI file functions. A path to the data or the
  saved games with characters that are not in the code page of the system
  does not always operate.
