# Windows

`ninja windows` compiles the game with clang for 32-bit x86 Windows. The
result is a native executable, `build/windows/halo.exe`, with `SDL3.dll`
next to it. You must build on Windows.

The Windows build uses the platform layer of the Linux build
(`port/linux/src`). The graphics, the sound, the input, the settings and
the multiplayer functions are the same as on Linux. Refer to
[port/linux/README.md](../linux/README.md).

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

`configure.py` generates the Windows build only on Windows.

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

The game is 32-bit code, as on Linux, because its data contains 32-bit
pointers. The clang target `i686-pc-windows-msvc` gives the ABI of MSVC:
the structure layout, the 16-bit `wchar_t` and the calling conventions.
Thus the Windows build needs fewer changes than the Linux build.

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

Each release also has the PDBs as `halo-windows-<configuration>-symbols.zip`.
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

## Limits

- Bink video is not available. The game skips the movies.
- The game uses only the ANSI file functions. A path to the data or the
  saved games with characters that are not in the code page of the system
  does not always operate.

## 64-bit Windows

The 64-bit Windows build is in development. It is to replace the 32-bit
build. `ninja windows64` makes `build/windows64/halo.exe`, with
`SDL3.dll` next to it. It is the 64-bit x86 Linux build
([port/linux/README.md](../linux/README.md), "64-bit x86") with a Windows
host: the game is an x32 guest image, inside the executable, in a 64-bit
process. It has no renderer yet: use it with `debug.null_renderer`, or as a
server.

### Build it

1. Go to the root folder of the repository.
2. Enter `python configure.py`.
3. Enter `ninja windows64`.

The requirements are the same as for the 32-bit build, with the 64-bit
(x64) MSVC libraries, and Git and `tar` (both in Windows 10 and 11) for the
guest's sources. The first `configure.py` also downloads LLVM 22 for the
guest (`tools/guest_toolchain.py`, about 900 MB, once) and the sources of
musl and SDL3 to `build/windows64/third_party`. With
`--guest-llvm=system`, the build uses the clang, `ld.lld`, `llvm-ar` and
`llvm-objdump` in the `PATH` for the guest instead.

On another computer, `python configure.py --windows64` and
`ninja windows64_guest` make the guest image alone,
`build/windows64/halo_guest.elf`, to check that it builds.

### How it operates

The guest image is the Linux Vulkan build's guest (`ninja linux_vulkan`)
with the code paths of the 32-bit Windows build (`HALO_WINDOWS`): the saved
games in `%APPDATA%\halo`, `config.toml` next to `halo.exe`, the Windows
self-updater. It is compiled without the red zone, because Windows writes
the records of an exception directly below the stack pointer, and with frame
pointers.

The host is `port/windows/host`, with the files of `port/android/host` that
do not depend on the operating system:

- `host_main_win.c`: the entry point. At the start, before it loads
  `SDL3.dll` (a delay-loaded DLL), the host reserves the memory of the guest
  below 4 GB. If another module already uses it, the game starts a copy of
  itself, suspended, and reserves the memory in the copy before it runs.
  Set `HALO_MEMORY_MAP=1` to log the map of the low 4 GB at the start.
- `host_memory_win.c`: the memory of the guest, as `host_memory.c` on
  Linux: the Xbox memory window at 0x80000000, the range of the image at
  0x70000000, and pools of 256 MB, from the top of the low 4 GB downward.
- `host_thread_win.c` and `host_enter_win.S`: each thread that runs guest
  code moves to a stack in the memory of the guest, and the limits of the
  stack in its TEB move with it.
- `host_syscall_win.c`: the Linux system calls of the guest, with the
  functions of Windows. The file descriptors are those of the C runtime.
- `host_memory_watch_win.c`: a vectored exception handler for the write
  tracking, the crash report in the log, and a function table that lets the
  unwinder of Windows walk the frames of the guest.
- The functions of `posix.h` are those of the 32-bit build
  (`port/windows/src`: `win32_files.c`, `win32_net.c`, `win32_p2p.c`,
  `win32_update.c`, `win32_upnp.c`).

The guest calls the host through thunks (`tools/android_imports.py
--host-abi sysv`): the guest uses the System V calling convention, and the
host uses the Microsoft x64 calling convention. The manifest
(`port/windows/host/halo.manifest`) sets UTF-8 as the code page of the
process.

The log is the standard error stream. A release build has no console: the
log is `halo.log` next to `halo.exe`.
