"""Ninja rules for the 64-bit Windows build (``ninja windows64``).

The game runs as the 64-bit x86 Linux build's does (tools/linux_guest_build.py,
port/linux/README.md "64-bit x86"): an x32 guest image (tools/guest_arch.py,
X32; tools/android_build.py, generate_guest_image) in a 64-bit host, here a
Windows executable (port/windows/host, port/windows/README.md "64-bit
Windows"):

- build/windows64/halo_guest.elf, ``ninja windows64_guest``: the guest image,
  the Vulkan desktop guest's (the Linux Vulkan build's, linux_vulkan) with the
  Windows code paths (HALO_WINDOWS: the saves in %APPDATA%\\halo, config.toml
  beside the executable, the Windows self-updater's release), no red zone
  (Windows writes an exception's records right below the stack pointer) and
  frame pointers everywhere (the host's function table walks the guest's
  frames with them, port/windows/host/host_memory_watch_win.c). It builds
  on any computer the guests build on (Linux, macOS, Windows), with the pinned
  LLVM (tools/guest_toolchain.py), so that the guest can be checked anywhere;
- the build is generated only when configure.py is given --windows64 (it
  downloads the pinned LLVM, musl, SDL3's source and the Khronos and Vulkan
  headers), or once those are there: while the 32-bit build is Windows's
  default, its configure stays as it was;
- build/windows64/halo.exe, ``ninja windows64``, on Windows only: the host,
  compiled by clang for x86_64-pc-windows-msvc and linked by lld with the
  Windows SDK, the guest image inside it; its import table's entries are
  thunks from the guest's calling convention (System V's) to the host's
  (Microsoft's), tools/android_imports.py --host-abi sysv;
- build/windows64/SDL3.dll beside it, from SDL3's Visual C++ package (as
  the 32-bit build's, tools/windows_build.py), which the host loads only
  once it has reserved the guest's memory (a delay-loaded DLL);
- build/windows64/brokers.txt, internet play's brokers.
"""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List

from .android_build import (MBEDTLS_DIR, TOML_DIR, fetch_third_party, generate_guest_image)
from .guest_arch import X32
from .guest_toolchain import LLVM_VERSION, GuestToolchain, guest_toolchain
from .host_gl_build import RENDER_STREAM_GUEST_EXCLUDED
from .linux_build import (MINIUPNPC_DIR, OPTIMISATION, compile_launcher, configuration_defines, miniupnpc_sources,
                          updater_defines)
from .macos_build import fetch_khronos_headers
from .ninja_syntax import Writer
from .vulkan_build import VULKAN_DIR, fetch_vulkan_headers
from .windows_build import SDL_DIR, fetch_sdl, windows_rc

BUILD = Path("build/windows64")
THIRD_PARTY = BUILD / "third_party"
KHRONOS_DIR = THIRD_PARTY / "khronos"
VULKAN_HEADERS_DIR = THIRD_PARTY / "vulkan-headers"
PORT_DIR = Path("port/windows/host")
WINDOWS_DIR = Path("port/windows")
ANDROID_DIR = Path("port/android")
LINUX_DIR = Path("port/linux")
LINUX_HOST_DIR = Path("port/linux/host")

# the guest's code paths beside the architecture's (generate_guest_image adds
# HALO_GUEST): the Linux Vulkan build's (tools/linux_guest_build.py: desktop
# OpenGL's declarations, the renderer's command stream, Vulkan's window) and
# Windows's; and its code generation: no red zone, as Windows's exception
# dispatch writes its records right below the interrupted stack pointer (the
# write tracking's page faults happen in guest code), and frame pointers in
# musl too, for the host's walk of guest frames
WINDOWS_GUEST_DEFINES = ["-DHALO_WINDOWS=1", "-DHALO_VULKAN=1", "-DHALO_RENDER_STREAM=1", "-mno-red-zone",
                         "-fno-omit-frame-pointer"]

# the host's files of port/android/host (the rest is port/windows/host's: the
# memory below 4 GB, threads on low stacks, Windows's system calls, the
# exception handlers, the entry point)
SHARED_HOST_SOURCES = ["host_common.c", "host_loader.c", "host_memory_watch.c", "host_render_record.c",
                       "host_render_stream.c", "host_sdl.c", "host_watch_hash.c"]
# the posix.h and update.h functions the guest's platform layer calls, the
# Windows versions the 32-bit build has (port/windows/src)
WIN32_HOST_SOURCES = ["win32_files.c", "win32_net.c", "win32_p2p.c", "win32_update.c", "win32_upnp.c"]
# the host's libraries (with SDL3.lib and delayimp.lib, for the delay-loaded
# SDL3.dll): Winsock and its helpers, the random number generator, timers,
# WaitOnAddress (synchronization), the window's and registry's functions,
# WinHTTP (the self-updater), the debugging helpers (crash reports)
HOST_LIBRARIES = ["ws2_32", "iphlpapi", "bcrypt", "winmm", "synchronization", "user32", "advapi32", "shell32",
                  "ole32", "winhttp", "dbghelp", "delayimp"]

HOST_FLAGS = [
    "--target=x86_64-pc-windows-msvc",
    OPTIMISATION,
    "-g",
    "-gcodeview",
    "-fno-omit-frame-pointer",
    # (the import thunks are System V functions, which may otherwise keep
    # data below the stack pointer, where Windows writes an exception's
    # records)
    "-mno-red-zone",
    # every x86-64 processor, as the guest
    "-march=x86-64",
    "-ffp-contract=off",
    "-std=gnu11",
    "-Wall",
    "-Wno-unused-function",
    "-D_CRT_SECURE_NO_WARNINGS",
    "-D_CRT_NONSTDC_NO_WARNINGS",
    "-D_WINSOCK_DEPRECATED_NO_WARNINGS",
    "-DWIN32_LEAN_AND_MEAN",
    "-DNOMINMAX",
    "-D_WIN32_WINNT=0x0A00",
]


def windows64_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR, WINDOWS_DIR / "src"]


def windows64_requested(sln: Any) -> bool:
    """whether to generate the build (on another computer than Windows, the
    guest alone): with configure.py --windows64, or once its sources are
    there"""
    return bool(getattr(sln, "windows64", False)) or (THIRD_PARTY / "SDL3").is_dir()


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def generate_windows64_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file() or not PORT_DIR.is_dir() or not windows64_requested(sln):
        return
    try:
        toolchain = guest_toolchain(sln, None)
    except (RuntimeError, OSError) as error:
        print(f"64-bit Windows build disabled: no LLVM {LLVM_VERSION} for the guest ({error})", file=sys.stderr)
        return
    if not toolchain:
        # (--guest-llvm=system: the PATH's)
        tools = {name: shutil.which(name) for name in ("clang", "ld.lld", "llvm-ar", "llvm-objdump")}
        if not all(tools.values()):
            print("64-bit Windows build disabled: --guest-llvm=system needs clang, ld.lld, llvm-ar and "
                  "llvm-objdump on the PATH", file=sys.stderr)
            return
        toolchain = GuestToolchain(cc=tools["clang"], ld=tools["ld.lld"], ar=tools["llvm-ar"],
                                   objdump=tools["llvm-objdump"])
    try:
        fetch_third_party(THIRD_PARTY)
        fetch_khronos_headers(KHRONOS_DIR)
        fetch_vulkan_headers(VULKAN_HEADERS_DIR)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"64-bit Windows build disabled: cannot fetch musl, SDL3 or the Khronos headers ({error})",
              file=sys.stderr)
        return
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))

    n.comment("64-bit Windows build (ninja windows64, ninja windows64_guest); see port/windows/README.md")
    n.variable("windows64_guest_cc", toolchain.cc)
    guest = generate_guest_image(
        n, sln, config, arch=X32, prefix="windows64", label="WINDOWS64", build=BUILD, third_party=THIRD_PARTY,
        guest_cc=toolchain.cc, gl_headers=KHRONOS_DIR.resolve(), ar=toolchain.ar, ld=toolchain.ld,
        objdump=toolchain.objdump, builtins_cc="", environment="gnu", platform_defines=WINDOWS_GUEST_DEFINES,
        extra_runtime=[LINUX_HOST_DIR / "guest_desktop.c", VULKAN_DIR / "guest_vulkan.c", PORT_DIR / "guest_windows.c"],
        extra_imports=[LINUX_HOST_DIR / "host_imports.list", VULKAN_DIR / "host_imports.list",
                       PORT_DIR / "host_imports.list"],
        extra_prototypes=[LINUX_HOST_DIR / "guest_desktop.c", VULKAN_DIR / "guest_vulkan.c",
                          PORT_DIR / "guest_windows.c"],
        updater_cflags=updater_defines(getattr(sln, "port_release", False)),
        excluded_platform_sources=RENDER_STREAM_GUEST_EXCLUDED, host_abi="sysv")
    n.build(outputs="windows64_guest", rule="phony", inputs=[guest["image"], guest["checked"]])
    n.newline()
    if sys.platform != "win32":
        return
    try:
        fetch_sdl()
    except OSError as error:
        print(f"64-bit Windows build: the host disabled, cannot fetch SDL3 ({error})", file=sys.stderr)
        return
    _generate_host(n, sln, guest)


def _generate_host(n: Writer, sln: Any, guest: Dict[str, Path]) -> None:
    """the host executable, SDL3.dll and brokers.txt (on Windows)"""
    cc = getattr(sln, "windows_cc", None) or "clang"
    host_obj_dir = BUILD / "host" / "obj"
    release = getattr(sln, "port_release", False)
    n.variable("windows64_cc", cc)
    n.rule(
        name="windows64_host_cc",
        command=f"{compile_launcher(sln)}$windows64_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="WINDOWS64 HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="windows64_link",
        command="$windows64_cc $ldflags -o $out --rsp-quoting=windows @$out.rsp $libs",
        description="WINDOWS64 LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name="windows64_copy",
        command="$python -c \"import shutil,sys; shutil.copyfile(sys.argv[1], sys.argv[2])\" $in $out",
        description="WINDOWS64 COPY $out",
    )
    n.variable("windows64_rc", windows_rc(cc))
    n.rule(
        name="windows64_rc",
        command="$windows64_rc /no-preprocess /FO $out $in",
        description="WINDOWS64 RC $out",
    )

    sdl_include = SDL_DIR / "include"
    base_cflags = [*HOST_FLAGS, *configuration_defines(sln),
                   *([] if release else ["-fstack-protector-strong"])]
    host_cflags = " ".join([
        *base_cflags,
        # (the POSIX names the shared host files use, over Windows's)
        f"-I{PORT_DIR / 'posix'}",
        f"-I{PORT_DIR}", f"-I{ANDROID_DIR / 'include'}", f"-I{ANDROID_DIR / 'host'}", f"-I{LINUX_HOST_DIR}",
        f"-I{LINUX_DIR / 'src'}", f"-I{TOML_DIR}", f"-I{_quote(sdl_include)}", f"-I{VULKAN_HEADERS_DIR}",
        "-DHALO_VULKAN=1",
    ])
    # (the files of port/windows/src talk to Windows alone, as in the 32-bit
    # build; and posix_dtls.c's Mbed TLS, and win32_upnp.c's miniupnpc)
    win32_cflags = " ".join([*base_cflags, f"-I{LINUX_DIR / 'src'}", f"-I{_quote(sdl_include)}"])
    mbedtls_include = f"-I{MBEDTLS_DIR / 'include'}"
    miniupnpc_include = f"-I{MINIUPNPC_DIR / 'include'} -DMINIUPNP_STATICLIB"

    objects: List[Path] = []

    def add(source: Path, cflags: str, name: str = "") -> None:
        obj = host_obj_dir / (name or source.name + ".o")
        n.build(outputs=obj, rule="windows64_host_cc", inputs=source, variables={"cflags": cflags})
        objects.append(obj)

    for source in sorted(PORT_DIR.glob("*.c")) + sorted(PORT_DIR.glob("*.S")):
        if source.name in ("guest_windows.c", "guest_image_win.S"):
            continue
        add(source, host_cflags)
    for name in SHARED_HOST_SOURCES:
        add(ANDROID_DIR / "host" / name, host_cflags)
    add(LINUX_HOST_DIR / "host_desktop.c", host_cflags)
    # (host_gl_window: the window has no OpenGL backend, as Android's)
    add(ANDROID_DIR / "host_renderer.c", host_cflags)
    add(TOML_DIR / "tomlc17.c", " ".join([*base_cflags, "-w"]))
    for name in WIN32_HOST_SOURCES:
        cflags = win32_cflags
        if name == "win32_upnp.c":
            cflags = f"{win32_cflags} {miniupnpc_include}"
        add(WINDOWS_DIR / "src" / name, cflags)
    for source in miniupnpc_sources():
        add(source, " ".join([*base_cflags, miniupnpc_include, f"-I{MINIUPNPC_DIR / 'src'}", "-w"]),
            name=f"miniupnpc_{source.name}.o")
    add(LINUX_DIR / "src" / "posix_dtls.c", f"{win32_cflags} {mbedtls_include}")
    for source in sorted((MBEDTLS_DIR / "library").glob("*.c")):
        add(source, " ".join([*base_cflags, mbedtls_include, f"-I{MBEDTLS_DIR / 'library'}", "-w"]),
            name=f"mbedtls/{source.name}.o")
    add(guest["host_table_c"], host_cflags)
    # the guest image, inside the executable (host_main_win.c)
    image_obj = host_obj_dir / "guest_image_win.S.o"
    n.build(outputs=image_obj, rule="windows64_host_cc", inputs=PORT_DIR / "guest_image_win.S",
            implicit=[guest["image"], guest["checked"]],
            variables={"cflags": f'--target=x86_64-pc-windows-msvc -DGUEST_IMAGE=\\"{guest["image"].as_posix()}\\"'})
    objects.append(image_obj)
    resources = BUILD / "halo.res"
    # (the icon, and the manifest: UTF-8 as the process's code page)
    n.build(outputs=resources, rule="windows64_rc", inputs=PORT_DIR / "halo.rc",
            implicit=[WINDOWS_DIR / "opence-icon.ico", PORT_DIR / "halo.manifest"])
    objects.append(resources)

    ldflags = [
        "--target=x86_64-pc-windows-msvc",
        "-fuse-ld=lld",
        "-g",
        # the guest's memory is reserved below 4 GB at start-up
        # (host_memory_win.c): the executable, its DLLs and their heaps go
        # above it (high-entropy ASLR), and SDL3.dll loads only once the
        # reservation is made
        "-Wl,/DYNAMICBASE",
        "-Wl,/HIGHENTROPYVA",
        "-Wl,/LARGEADDRESSAWARE",
        "-Wl,/DELAYLOAD:SDL3.dll",
        # (never CET's shadow stacks: the guest's threads move to stacks of
        # their own, port/windows/host/host_enter_win.S)
        "-Wl,/CETCOMPAT:NO",
        # (the manifest is the resources', halo.manifest)
        "-Wl,/MANIFEST:NO",
    ]
    if release:
        # (no console window: under Wine it can hide the game's window, as
        # in the 32-bit build)
        ldflags += ["-Wl,/SUBSYSTEM:WINDOWS", "-Wl,/ENTRY:mainCRTStartup"]
    else:
        ldflags += ["-Wl,/SUBSYSTEM:CONSOLE"]
    libs = " ".join([_quote(SDL_DIR / "lib" / "x64" / "SDL3.lib"), *(f"-l{lib}" for lib in HOST_LIBRARIES)])
    output = BUILD / "halo.exe"
    n.build(outputs=output, rule="windows64_link", inputs=objects,
            variables={"ldflags": " ".join(ldflags), "libs": libs})
    sdl_dll = BUILD / "SDL3.dll"
    n.build(outputs=sdl_dll, rule="windows64_copy", inputs=SDL_DIR / "lib" / "x64" / "SDL3.dll")
    brokers = BUILD / "brokers.txt"
    n.build(outputs=brokers, rule="windows64_copy", inputs=Path("port/assets/network/brokers.txt"))
    n.build(outputs="windows64", rule="phony", inputs=[output, sdl_dll, brokers])
    n.newline()
