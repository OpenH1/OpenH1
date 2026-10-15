"""Ninja rules for the macOS build (``ninja macos``), for Apple Silicon.

The game runs as the Linux arm64 build runs it (tools/linux_arm64_build.py,
port/android/README.md, "How the port operates"): the guest image is the
game built as ILP32 AArch64 code, with the Linux desktop's code paths, and
the host is a native executable that loads it and serves its calls. macOS
keeps the low 4 GB of every process from it (__PAGEZERO), so the guest is
built with the base register (tools/guest_base_register.py): its 4 GB lie
wherever the host reserves them (port/android/host/host_memory_base.c).

- build/macos/halo: the host (port/android/host, with
  port/linux/arm64/host_main.c as its entry point and port/macos for
  Darwin's system calls), with the guest image inside it;
- build/macos/libSDL3.0.dylib: SDL3, built from source without OpenGL, next
  to the executable, which finds it there (an rpath of @executable_path).

There is no renderer yet (a Metal one comes later): run the game with the
null renderer. Refer to port/macos/README.md.
"""

import hashlib
import json
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List

from .android_build import SDL_TAG, TOML_DIR, fetch_third_party, generate_guest_image
from .linux_arm64_build import LINUX_ARM64_GUEST_ABI_FLAGS
from .linux_build import MBEDTLS_DIR, MINIUPNPC_DEFINES, MINIUPNPC_DIR, compile_launcher, miniupnpc_sources
from .linux_sysroot import download
from .ninja_syntax import Writer

PORT_DIR = Path("port/macos")
ARM64_DIR = Path("port/linux/arm64")
ANDROID_DIR = Path("port/android")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/macos")
THIRD_PARTY = BUILD / "third_party"
SDL_DIR = THIRD_PARTY / "SDL3"
# macOS 14.4: os_sync_wait_on_address (the guest's futexes, host_syscall.c)
MACOS_MINIMUM = "14.4"

# The Khronos OpenGL ES headers, for the guest's declarations only: the
# guest's renderer (port/linux/src/d3d8_gl.c) is compiled as on Linux arm64,
# but the macOS host gives it no OpenGL (port/macos/host_renderer.c), and
# the game runs with the null renderer until the Metal backend comes.
# (OpenGL-Registry and EGL-Registry, Apache-2.0 / MIT)
KHRONOS_DIR = THIRD_PARTY / "khronos"
KHRONOS_HEADERS = {
    "GLES2/gl2.h": ("OpenGL-Registry", "6af574a14089ccfee87efe230ebcdd8742859813",
                    "ba5e8e1755642efeb2d0f34b4cfe261cdbf6471e7763a40c7de10dff4c3c4921"),
    "GLES2/gl2ext.h": ("OpenGL-Registry", "6af574a14089ccfee87efe230ebcdd8742859813",
                       "9afc725e9dda7c8b476e7337e75b22fa660ed462c169e203ec28c10e62f91f4c"),
    "GLES2/gl2platform.h": ("OpenGL-Registry", "6af574a14089ccfee87efe230ebcdd8742859813",
                            "f5da0747540a50be5f44aad264aae45bdf157a192c40f17487dd9a2f99c71b6c"),
    "GLES3/gl3.h": ("OpenGL-Registry", "6af574a14089ccfee87efe230ebcdd8742859813",
                    "a0e4880142bd059bd4d7446f257920b5020b8bbb0a86eb0149cbd1ea2fcf8cb0"),
    "GLES3/gl32.h": ("OpenGL-Registry", "6af574a14089ccfee87efe230ebcdd8742859813",
                     "f203257863a7de3fce851efa61490907171db82082381d05f86f5d9bd39e8e17"),
    "GLES3/gl3platform.h": ("OpenGL-Registry", "6af574a14089ccfee87efe230ebcdd8742859813",
                            "a9e060dae5a2b11c5a889b679692b7089a10a7e03ebfbb6cf28217f6e322fb08"),
    "KHR/khrplatform.h": ("EGL-Registry", "db3425b8246136faccb5e2782b5694960bd6edf1",
                          "7b1e01aaa7ad8f6fc34b5c7bdf79ebf5189bb09e2c4d2e79fc5d350623d11e83"),
}
KHRONOS_URLS = [
    "https://raw.githubusercontent.com/KhronosGroup/{repository}/{commit}/api/{name}",
    "https://cdn.jsdelivr.net/gh/KhronosGroup/{repository}@{commit}/api/{name}",
]

# the guest of the Linux arm64 build (the base register is added by
# generate_guest_image); Apple's clang otherwise notes that its macOS
# sysroot is not the watchOS target's, which the guest does not use
MACOS_GUEST_ABI_FLAGS = [*LINUX_ARM64_GUEST_ABI_FLAGS, "-Wno-incompatible-sysroot"]

# (port/android/host's: those of the Android app alone, the fixed low
# memory and Linux's system calls, which port/macos and host_memory_base.c
# replace, and the OpenGL ES services)
HOST_EXCLUDED = {"host_main.c", "host_touch.c", "host_memory.c", "host_syscall.c", "host_gl.c"}


def is_macos_arm64() -> bool:
    return sys.platform == "darwin" and platform.machine().lower() == "arm64"


def macos_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR]


def fetch_khronos_headers(khronos_dir: Path = KHRONOS_DIR) -> None:
    """Download the OpenGL ES headers (configure time, once; a header whose
    SHA-256 is wrong is downloaded again)."""
    for name, (repository, commit, digest) in KHRONOS_HEADERS.items():
        header = khronos_dir / name
        if header.is_file() and hashlib.sha256(header.read_bytes()).hexdigest() == digest:
            continue
        print(f"Downloading the Khronos {name}")
        header.parent.mkdir(parents=True, exist_ok=True)
        data = download([url.format(repository=repository, commit=commit, name=name) for url in KHRONOS_URLS], digest)
        header.with_suffix(".tmp").write_bytes(data)
        header.with_suffix(".tmp").replace(header)


def _tool(name: str) -> str:
    """name on the PATH, else Homebrew's (LLVM's tools, and lld's)."""
    found = shutil.which(name)
    if found:
        return found
    for directory in ("/opt/homebrew/opt/llvm/bin", "/opt/homebrew/opt/lld/bin", "/opt/homebrew/bin"):
        candidate = Path(directory) / name
        if candidate.is_file():
            return str(candidate)
    return ""


def generate_macos_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file() or not PORT_DIR.is_dir():
        return
    if not is_macos_arm64():
        n.comment("macOS build: only on a Mac with Apple silicon")
        return
    ld = _tool("ld.lld")
    ar = _tool("llvm-ar")
    if not ld or not ar:
        n.comment("macOS build: needs ld.lld and llvm-ar (brew install llvm lld)")
        return
    try:
        fetch_third_party(THIRD_PARTY)
        fetch_khronos_headers()
    except (subprocess.CalledProcessError, OSError, RuntimeError) as error:
        print(f"macOS build disabled: cannot fetch musl, SDL3 or the OpenGL ES headers ({error})", file=sys.stderr)
        return
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))
    cc = getattr(sln, "macos_cc", None) or "clang"

    n.comment("macOS build (ninja macos); see port/macos/README.md")
    n.variable("macos_guest_cc", cc)
    n.variable("macos_cc", cc)

    guest = generate_guest_image(
        n, sln, config, prefix="macos", label="MACOS", build=BUILD, third_party=THIRD_PARTY,
        guest_cc=cc, gl_headers=KHRONOS_DIR.resolve(), ar=ar, ld=ld,
        builtins="", asm_target="aarch64-linux-gnu",
        abi_flags=MACOS_GUEST_ABI_FLAGS, extra_runtime=[ARM64_DIR / "guest_desktop.c"],
        extra_imports=[ARM64_DIR / "host_imports.list"], extra_prototypes=[ARM64_DIR / "guest_desktop.c"],
        base_register=True)

    # ---------- SDL3, built from the same source as the guest's headers

    sdl_build = BUILD / "sdl3-build"
    libsdl = sdl_build / "libSDL3.0.dylib"
    n.rule(
        name="macos_sdl3",
        command=(f"cmake -S {SDL_DIR} -B {sdl_build} -G Ninja -DCMAKE_C_COMPILER=$macos_cc "
                 f"-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 "
                 f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_MINIMUM} -DSDL_SHARED=ON -DSDL_STATIC=OFF "
                 f"-DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "
                 # (no OpenGL: the renderer will be Metal's)
                 f"-DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF > {BUILD}/sdl3-configure.log && "
                 f"ninja -C {sdl_build} > {BUILD}/sdl3-build.log"),
        description=f"MACOS SDL3 {SDL_TAG}",
        pool="console",
    )
    n.build(outputs=libsdl, rule="macos_sdl3", implicit=[SDL_DIR / "CMakeLists.txt"])

    # ---------- the host

    host_obj_dir = BUILD / "host" / "obj"
    n.rule(
        name="macos_host_cc",
        command=f"{compile_launcher(sln)}$macos_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="MACOS HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    host_cflags = " ".join([
        "-O2", "-g", "-Wall", "-Wno-unused-function", "-D_DARWIN_C_SOURCE", "-DHALO_GUEST_BASE=1",
        f"-mmacosx-version-min={MACOS_MINIMUM}",
        f"-I{ANDROID_DIR}/include", f"-I{ANDROID_DIR}/host", f"-I{SDL_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{TOML_DIR}", f"-I{ANDROID_DIR}/guest/libc/arch/arm64_32",
    ])
    host_sources = [source for source in sorted((ANDROID_DIR / "host").glob("*.[cS]"))
                    if source.name not in HOST_EXCLUDED]
    host_sources += [
        ARM64_DIR / "host_main.c", ARM64_DIR / "host_desktop.c",
        *sorted(PORT_DIR.glob("*.c")),
        LINUX_DIR / "src" / "posix_files.c", LINUX_DIR / "src" / "posix_net.c",
        TOML_DIR / "tomlc17.c",
    ]
    host_objects: List[Path] = []
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source, variables={"cflags": host_cflags})
        host_objects.append(obj)
    miniupnpc_cflags = " ".join([host_cflags, f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}",
                                 *MINIUPNPC_DEFINES])
    for source in [LINUX_DIR / "src" / "posix_upnp.c", *miniupnpc_sources()]:
        obj = host_obj_dir / ("miniupnpc_" + source.name + ".o" if source.parent.parent == MINIUPNPC_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source,
                variables={"cflags": miniupnpc_cflags + (" -w" if source.name != "posix_upnp.c" else "")})
        host_objects.append(obj)
    # the self-updater's download (posix_update.c) and internet play's DTLS
    # with browsers (posix_dtls.c), with port/third_party/mbedtls
    mbedtls_cflags = " ".join([host_cflags, f"-I{MBEDTLS_DIR / 'include'}"])
    for name in ("posix_update.c", "posix_dtls.c"):
        n.build(outputs=host_obj_dir / (name + ".o"), rule="macos_host_cc",
                inputs=LINUX_DIR / "src" / name, variables={"cflags": mbedtls_cflags})
        host_objects.append(host_obj_dir / (name + ".o"))
    for source in sorted((MBEDTLS_DIR / "library").glob("*.c")):
        obj = host_obj_dir / "mbedtls" / (source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source,
                variables={"cflags": f"{mbedtls_cflags} -I{MBEDTLS_DIR / 'library'} -w"})
        host_objects.append(obj)
    table_obj = host_obj_dir / "host_import_table.c.o"
    n.build(outputs=table_obj, rule="macos_host_cc", inputs=guest["host_table_c"],
            variables={"cflags": host_cflags})
    host_objects.append(table_obj)
    # the guest image, inside the executable (host_main.c)
    image_obj = host_obj_dir / "guest_image.S.o"
    n.build(outputs=image_obj, rule="macos_host_cc", inputs=PORT_DIR / "guest_image.S",
            implicit=[guest["image"]], variables={"cflags": f'-mmacosx-version-min={MACOS_MINIMUM} -DGUEST_IMAGE=\\"{guest["image"]}\\"'})
    host_objects.append(image_obj)

    output = BUILD / "halo"
    staged_sdl = BUILD / "libSDL3.0.dylib"
    n.rule(
        name="macos_host_link",
        command=(f"$macos_cc -mmacosx-version-min={MACOS_MINIMUM} -o $out $in -L{sdl_build} -lSDL3 "
                 "-Wl,-rpath,@executable_path"),
        description="MACOS LINK $out",
    )
    n.build(outputs=output, rule="macos_host_link", inputs=host_objects, implicit=[libsdl])
    n.rule(name="macos_copy", command="cp $in $out", description="MACOS STAGE $out")
    n.build(outputs=staged_sdl, rule="macos_copy", inputs=libsdl)
    # internet play's MQTT brokers, a file beside the game (network.brokers_file)
    brokers = BUILD / "brokers.txt"
    n.build(outputs=brokers, rule="macos_copy", inputs=Path("port/assets/network/brokers.txt"))
    n.build(outputs="macos", rule="phony", inputs=[output, staged_sdl, brokers])
    n.newline()
