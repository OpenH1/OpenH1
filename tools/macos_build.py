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
  to the executable, which finds it there (an rpath of @executable_path);
- build/macos/Halo.app: the app, the executable and SDL in a bundle (where
  the executable finds SDL in Contents/Frameworks, its other rpath), signed
  ad hoc with the hardened runtime (tools/macos_app.py); build/macos/Halo.dmg
  (ninja macos_dmg), a disk image of it;
- build/macos/xgpu_metal_test (ninja macos_metal_test): the Metal backend's
  tests (port/macos/tests).

The renderer is Metal's (port/macos/xgpu_metal.m), in the host: the guest
writes its frames as the renderer's command stream (HALO_RENDER_STREAM).
Refer to port/macos/README.md.
"""

import hashlib
import json
import os
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
# guest's platform code that names OpenGL is compiled as on Linux arm64, but
# the macOS host gives it no OpenGL (port/macos/host_renderer.c): the
# renderer is Metal's, in the host.
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
MACOS_GUEST_ABI_FLAGS = [*LINUX_ARM64_GUEST_ABI_FLAGS, "-Wno-incompatible-sysroot", "-DHALO_METAL",
                         "-DHALO_RENDER_STREAM"]
# (the renderer's backend is the host's, through the command stream:
# port/linux/src/xgpu_stream_encode.c in place of the OpenGL backend and its
# anti-aliasing passes)
MACOS_GUEST_EXCLUDED = ["xgpu_gl.c", "xgpu_post.c"]

# the app's bundle identifier (configure.py --macos-bundle-id): the
# project's GitHub organisation's pages, the reverse-DNS name of a domain it
# has (as Flathub names apps without a domain of their own)
MACOS_BUNDLE_ID = "io.github.opencommunityedition.opence"

# the host's frameworks: Metal and its layer (port/macos/host_metal.m,
# xgpu_metal.m)
MACOS_FRAMEWORKS = "-framework Metal -framework QuartzCore -framework Foundation -framework AppKit"

# (port/android/host's: those of the Android app alone, the fixed low
# memory and Linux's system calls, which port/macos and host_memory_base.c
# replace, and the OpenGL ES services)
HOST_EXCLUDED = {"host_main.c", "host_touch.c", "host_memory.c", "host_syscall.c", "host_gl.c"}

# the renderer's parts the host builds from port/linux/src (the MSL
# emitter, the Xbox's texture decoding), and port/macos's files that use
# them
SHARED_SOURCES = ["xgpu.h", "xgpu_backend.h", "xgpu_shader.h", "xgpu_msl.h", "xgpu_msl.c", "xgpu_text.c",
                  "xbox_texture_decode.c"]
SHARED_USERS = {"xgpu_metal_shaders.c", "xgpu_metal_textures.c"}
# the app's start (its folders, maps and window), which the Metal tests do
# without
APP_SOURCES = {"host_app.m"}


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
        abi_flags=MACOS_GUEST_ABI_FLAGS, extra_runtime=[ARM64_DIR / "guest_desktop.c", PORT_DIR / "guest_metal.c"],
        extra_imports=[ARM64_DIR / "host_imports.list", PORT_DIR / "host_imports.list"],
        extra_prototypes=[ARM64_DIR / "guest_desktop.c", PORT_DIR / "guest_metal.c"],
        base_register=True, excluded_platform_sources=MACOS_GUEST_EXCLUDED)

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
        f"-I{TOML_DIR}", f"-I{ANDROID_DIR}/guest/libc/arch/arm64_32", f"-I{PORT_DIR}",
    ])
    objc_cflags = f"{host_cflags} -fobjc-arc"
    host_sources = [source for source in sorted((ANDROID_DIR / "host").glob("*.[cS]"))
                    if source.name not in HOST_EXCLUDED]
    host_sources += [
        ARM64_DIR / "host_main.c", ARM64_DIR / "host_desktop.c",
        *sorted(source for source in PORT_DIR.glob("*.c")
                if not source.name.startswith("guest_") and source.name not in SHARED_USERS),
        LINUX_DIR / "src" / "posix_files.c", LINUX_DIR / "src" / "posix_net.c",
        TOML_DIR / "tomlc17.c",
    ]
    host_objects: List[Path] = []
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source, variables={"cflags": host_cflags})
        host_objects.append(obj)
    # the renderer's shared parts in the host, copied beside a stand-in for
    # the guest's platform.h (xgpu_metal_platform.h), which their quoted
    # includes would otherwise find in port/linux/src, and the files of
    # port/macos that use them; the XDK's enumerations and texture format
    # words for the stand-in (xdk_enums.h)
    shared_dir = BUILD / "host" / "shared"
    n.rule(name="macos_shared_copy", command="cp $in $out", description="MACOS SHARED $out")
    xdk = Path("port/include/xdk")
    n.rule(name="macos_xdk_enums",
           command=(f"(sed -n '/^\\/\\* ---------- enumerations/,/^\\/\\* ---------- structures and unions/p' "
                    f"{xdk / 'xdk_pdb.h'} | sed '$$d' && "
                    f"grep -E '^#define D3D(FORMAT|SIZE|TEXTURE)_' {xdk / 'xdk_d3d8.h'}) > $out"),
           description="MACOS SHARED $out")
    shared_copies: List[Path] = []
    for name in SHARED_SOURCES:
        n.build(outputs=shared_dir / name, rule="macos_shared_copy", inputs=LINUX_DIR / "src" / name)
        shared_copies.append(shared_dir / name)
    n.build(outputs=shared_dir / "platform.h", rule="macos_shared_copy", inputs=PORT_DIR / "xgpu_metal_platform.h")
    n.build(outputs=shared_dir / "xdk_enums.h", rule="macos_xdk_enums", inputs=[xdk / "xdk_pdb.h", xdk / "xdk_d3d8.h"])
    shared_copies += [shared_dir / "platform.h", shared_dir / "xdk_enums.h"]
    shared_cflags = f"-I{shared_dir} {host_cflags}"
    shared_objects: List[Path] = []
    for source in [shared_dir / name for name in SHARED_SOURCES if name.endswith(".c")] + \
            [PORT_DIR / name for name in sorted(SHARED_USERS)]:
        obj = host_obj_dir / "shared" / (source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source, order_only=shared_copies,
                variables={"cflags": shared_cflags})
        shared_objects.append(obj)
    host_objects += shared_objects
    # the Metal renderer and the window's presentation (Objective-C)
    metal_objects: List[Path] = []
    for source in sorted(PORT_DIR.glob("*.m")):
        obj = host_obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source, variables={"cflags": objc_cflags})
        if source.name in APP_SOURCES:
            host_objects.append(obj)
        else:
            metal_objects.append(obj)
    host_objects += metal_objects
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
                 f"{MACOS_FRAMEWORKS} -Wl,-rpath,@executable_path -Wl,-rpath,@executable_path/../Frameworks"),
        description="MACOS LINK $out",
    )
    n.build(outputs=output, rule="macos_host_link", inputs=host_objects, implicit=[libsdl])
    n.rule(name="macos_copy", command="cp $in $out", description="MACOS STAGE $out")
    n.build(outputs=staged_sdl, rule="macos_copy", inputs=libsdl)
    # internet play's MQTT brokers, a file beside the game (network.brokers_file)
    brokers = BUILD / "brokers.txt"
    n.build(outputs=brokers, rule="macos_copy", inputs=Path("port/assets/network/brokers.txt"))

    # ---------- the app (tools/macos_app.py), and its disk image

    app = BUILD / "Halo.app"
    app_executable = app / "Contents" / "MacOS" / "halo"
    icon = PORT_DIR / "opence-icon.icns"
    build_number = os.environ.get("HALO_BUILD_NUMBER", "0")
    bundle_id = getattr(sln, "macos_bundle_id", None) or MACOS_BUNDLE_ID
    n.rule(
        name="macos_app",
        command=(f"$python tools/macos_app.py bundle --app {app} --executable {output} --sdl {libsdl} "
                 f"--icon {icon} --resource {brokers} --bundle-id {bundle_id} "
                 f"--build {build_number if build_number.isdigit() else 0} "
                 f"--flavor {'release' if getattr(sln, 'port_release', False) else 'debug'} "
                 f"--minimum {MACOS_MINIMUM} > {BUILD}/app.log"),
        description=f"MACOS APP {app}",
    )
    n.build(outputs=app_executable, rule="macos_app", inputs=[output, libsdl, brokers],
            implicit=[icon, PORT_DIR / "halo.entitlements", Path("tools/macos_app.py")])
    n.build(outputs="macos_app", rule="phony", inputs=[app_executable])
    dmg = BUILD / "Halo.dmg"
    n.rule(name="macos_dmg", command=f"$python tools/macos_app.py dmg --app {app} --output $out > {BUILD}/dmg.log",
           description=f"MACOS DMG $out")
    n.build(outputs=dmg, rule="macos_dmg", inputs=[app_executable])
    n.build(outputs="macos_dmg", rule="phony", inputs=[dmg])
    n.build(outputs="macos", rule="phony", inputs=[output, staged_sdl, brokers, app_executable])

    # ---------- the Metal backend's tests (python3 tools/test_macos_metal.py)

    test_obj = host_obj_dir / "tests" / "xgpu_metal_test.m.o"
    n.build(outputs=test_obj, rule="macos_host_cc", inputs=PORT_DIR / "tests" / "xgpu_metal_test.m",
            variables={"cflags": objc_cflags})
    test = BUILD / "xgpu_metal_test"
    n.build(outputs=test, rule="macos_host_link", inputs=[test_obj, *metal_objects, *shared_objects], implicit=[libsdl])
    n.build(outputs="macos_metal_test", rule="phony", inputs=[test, staged_sdl])
    n.newline()
