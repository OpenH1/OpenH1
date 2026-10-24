# Windows

`ninja windows` makes the Windows build, `build/windows/halo.exe`, with
`SDL3.dll` next to it, on Windows or cross-compiled from Linux. The build is 64-bit: it
starts on 64-bit Windows 10 (version 1903 or later) and Windows 11, and
needs a graphics driver with Vulkan 1.3.

The game is the 64-bit x86 Linux build
([port/linux/README.md](../linux/README.md), "64-bit x86") with a Windows
host: an x32 guest image, inside the executable, in a 64-bit process. The
graphics are the Vulkan renderer of the Linux build
([port/linux/vulkan/README.md](../linux/vulkan/README.md)). The sound, the
input, the settings and the multiplayer functions are the same as on Linux.

The build replaces the 32-bit build of the releases before it. Its download
has the same name (`OpenH1-Windows.zip`), so their self-updaters update to
it.

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
  The build uses the 64-bit (x64) MSVC libraries and a Windows 10 or 11
  SDK. It does not use the MSVC compiler.
- LLVM (clang, lld and `llvm-rc`), Python 3, Git and ninja in the `PATH`.
  For example, enter `scoop install llvm python git ninja`.
- A network connection for the first build. `configure.py` downloads, to
  `build/windows/third_party`, the Visual C++ development package of SDL
  3.4.16, the sources of musl and SDL3 for the guest, and the Khronos and
  Vulkan headers. It also downloads LLVM 22 for the guest
  (`tools/guest_toolchain.py`, about 900 MB, once) to the cache folder of
  the user. With `--guest-llvm=system`, the build uses the clang, `ld.lld`,
  `llvm-ar` and `llvm-objdump` in the `PATH` for the guest instead.

## Build the game

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja windows`.

`configure.py` generates the Windows build on Windows, and with
`--windows-cross`, which cross-compiles it from Linux: the same clang and
lld build the host against an MSVC toolchain (its x64 libraries), whose
directories `--windows-vctools-dir` (the versioned folder under
`VC/Tools/MSVC`, the one with `include` and `lib`) and `--windows-sdk-dir`
(the SDK folder with `Include` and `Lib`) name, or `VCToolsInstallDir` and
`WindowsSdkDir`. The build passes them to clang in `INCLUDE` and `LIB`.
`--windows-cc` names the cross compiler (a run-of-the-mill clang by
default); the guest is built with the pinned LLVM, as on Windows.

    python configure.py --windows-cross \
        --windows-vctools-dir "$VCToolsInstallDir" \
        --windows-sdk-dir "$WindowsSdkDir"
    ninja windows

The toolchain can be an [msvc-wine](https://github.com/mstorsjo/msvc-wine)
install or an unmodified MSVC from a Windows install (from an NTFS volume,
for instance). The SDK must have a versioned folder in each of `Include`
and `Lib` (`Include/<version>/um`): a plain `xwin splat` does not, a splat
made with `xwin splat --use-winsysroot-style` does.

On another computer, `python configure.py --windows-guest` and
`ninja windows_guest` make the guest image alone,
`build/windows/halo_guest.elf`, to check that it builds.

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
| Vulkan pipeline cache | `%LOCALAPPDATA%\OpenH1\vulkan_pipelines.bin` |
| Crash reports | `crashes\` next to `halo.exe`, until the game sends them |

Without a graphics driver with Vulkan 1.3, the game says so and stops.

## How the port operates

The guest image is the guest of the 64-bit x86 Linux build (`ninja
linux_x86_64`) with the code paths of Windows (`HALO_WINDOWS`): the saved
games in `%APPDATA%\halo`, `config.toml` next to `halo.exe`, the
self-updater of Windows. It is compiled without the red zone, because
Windows writes the records of an exception directly below the stack
pointer, and with frame pointers.

The host is `host/`, with the files of `port/android/host` that do not
depend on the operating system (the loader, the write tracking, the
command stream of the renderer, the SDL functions), and
`port/linux/vulkan`:

| File | Contents |
| --- | --- |
| `host/host_main_win.c` | The entry point. Before it loads `SDL3.dll` (a delay-loaded DLL), the host reserves the memory of the guest below 4 GB. If another module already uses it, the game starts a copy of itself, suspended, and reserves the memory in the copy before it runs. Set `HALO_MEMORY_MAP=1` to log the map of the low 4 GB at the start. |
| `host/host_memory_win.c` | The memory of the guest, as `host_memory.c` on Linux: the Xbox memory window at 0x80000000, the range of the image at 0x70000000, and pools of 256 MB, from the top of the low 4 GB downward. |
| `host/host_thread_win.c`, `host/host_enter_win.S` | Each thread that runs guest code moves to a stack in the memory of the guest, and the limits of the stack in its TEB move with it. |
| `host/host_syscall_win.c` | The Linux system calls of the guest, with the functions of Windows. The file descriptors are those of the C runtime. |
| `host/host_memory_watch_win.c` | A vectored exception handler for the write tracking, the crash report in the log, and a function table that lets the unwinder of Windows walk the frames of the guest. |
| `host/host_crash_win.c` | The crash reports. Refer to "Crash reports". |
| `host/posix/`, `host/host_posix_win.c` | The POSIX functions that the shared host files use, with the functions of Windows. |
| `src/win32_files.c`, `src/win32_net.c`, `src/win32_p2p.c`, `src/win32_upnp.c` | The functions of `port/linux/src/posix.h`: files, sockets, the command line, the `halo://` links, Discord, UPnP. |
| `src/win32_update.c` | The download and the file replacement of the self-updater (`port/linux/src/update.h`), with WinHTTP. |

The guest calls the host through thunks (`tools/android_imports.py
--host-abi sysv`): the guest uses the System V calling convention, and the
host uses the Microsoft x64 calling convention. The manifest
(`host/halo.manifest`) sets UTF-8 as the code page of the process, so that
paths with any characters operate.

## Crash reports

A crash writes the faulting address, the registers and the calls that led
to it to the log, with the frames of the guest. The builds of the workflow
(the releases) also send a crash report to the Sentry project of the
developers:

1. The game starts a second copy of `halo.exe` (`halo.exe --crash-report`).
   This copy writes a minidump of the game to `crashes\` next to
   `halo.exe`, and the game stops. A minidump contains the stacks and the
   registers of the threads, and the list of the modules. It does not
   contain the other memory of the game. The stack of a thread that runs
   guest code is its stack in the memory of the guest.
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

Each release also has the PDBs and the guest image as
`OpenH1-Windows-symbols.zip` (and `OpenH1-Windows-debug-symbols.zip`).
`tools/symbolize_crash.py` uses them to add the function names and the
source lines to the crash lines of a `halo.log`:

```
python tools/symbolize_crash.py halo.log <folder>
```

Give the folder of the build that crashed: `halo.exe`, `halo.pdb` and
`halo_guest.elf`. The tool needs `llvm-symbolizer` (LLVM).

OpenH1 has no Sentry project yet: no build sends crash reports. To test the
crash reports, set the `HALO_CRASH_REPORTS_ANY_BUILD` environment
variable.

The crash reports also include the crashes of `abort()` and of an invalid
argument to a function of the C runtime.

## Limits

- Bink video is not available. The game skips the movies.
- Windows 10 before version 1903 does not take UTF-8 as the code page of
  the process: there, a path to the data or the saved games with
  characters that are not in the code page of the system does not always
  operate.
