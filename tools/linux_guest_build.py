"""Ninja rules for the desktop guest builds: Linux on 64-bit ARM
(``ninja linux_arm64``) and on 64-bit x86 (``ninja linux_x86_64``).

The game's data needs 32-bit pointers. Recent 64-bit ARM processors have no
32-bit mode, and few distributions have 32-bit ARM libraries; 64-bit x86
processors do have one, which the x86 build (``ninja linux``, tools/linux_build.py)
uses, but it needs a distribution's 32-bit libraries, which some no longer
carry, and 32-bit drivers. So these builds run the game as the Android port
does (port/android/README.md, "How the port operates"): the
guest image is ILP32 code of the computer's architecture (tools/guest_arch.py:
arm64_32, or x32; tools/android_build.py, generate_guest_image), with the
Linux desktop's code paths instead of the app's, and the host is a 64-bit
Linux executable instead of an app's library:

- build/<name>/halo: the host (port/android/host, with
  port/linux/host/host_main.c as its entry point), with the guest image
  inside it;
- build/<name>/libSDL3.so.0: SDL3, next to the executable, which finds
  it there (an rpath of $ORIGIN), as few distributions have SDL3 yet, and
  its license, SDL3-LICENSE.txt.

Each build is a LinuxGuest in LINUX_GUESTS, generated on a computer of its
architecture. Its sources (musl and SDL3, and the OpenXR headers for VR) are
downloaded when configure.py generates it, as the Android build's are, and
only when it is asked for: the 64-bit ARM build, the default build of its
computers, always; the 64-bit x86 build, which is not, with configure.py
--linux-x86-64 (or --linux-x86-64-cc, or --vr), or once its sources are
there. A build that is not asked for, or cannot be generated, is a target
that stops with the reason. Refer to "64-bit ARM" and "64-bit x86" in
port/linux/README.md.
"""

import hashlib
import io
import json
import platform
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from .android_build import (GUEST_ARCH_SCRIPT, GUEST_TOOLCHAIN_SCRIPT, MUSL_VERSION, SDL_TAG, TOML_DIR, ZLIB_DEFINES,
                            ZLIB_DIR, ZLIB_SOURCES, fetch_third_party, generate_guest_image, host_memory_sources)
from .embed_assets import smaa_assets_build
from .guest_arch import ARM64_32, X32, GuestArch
from .guest_toolchain import LLVM_VERSION, guest_toolchain
from .linux_sysroot import download
from .linux_build import (MBEDTLS_DIR, MINIUPNPC_DEFINES, MINIUPNPC_DIR, SDL_RPATH_LDFLAGS, compile_launcher,
                          miniupnpc_sources, sdl3_rule, updater_defines)
from .ninja_syntax import Writer, escape

# the desktop host's own files (its entry point, SDL's desktop functions, VR)
PORT_DIR = Path("port/linux/host")
ANDROID_DIR = Path("port/android")
LINUX_DIR = Path("port/linux")
# the system's OpenGL ES, EGL and OpenGL headers (libgles-dev, libegl-dev and
# libgl-dev on Debian and Ubuntu, mesa on Arch Linux)
GL_HEADERS = Path("/usr/include")
# The Khronos OpenXR headers, for the VR build's host (port/linux/host/host_vr.c),
# downloaded by the VR build when it is configured, as the OpenXR SDK has them
# (Apache-2.0 OR MIT). The host loads the OpenXR runtime itself, so the build
# needs no OpenXR package.
OPENXR_TAG = "release-1.1.63"
OPENXR_URLS = [
    "https://raw.githubusercontent.com/KhronosGroup/OpenXR-SDK/{tag}/include/openxr/{name}",
    "https://cdn.jsdelivr.net/gh/KhronosGroup/OpenXR-SDK@{tag}/include/openxr/{name}",
]
OPENXR_HEADERS = {
    "openxr.h": "17e8e334a7ad0c6933b31143330d4771010e393df5d5d9d08566eb15bb171a15",
    "openxr_platform.h": "f17e6d752e0065ede9f2a4c8932155acff96462306a2468a67a6ad77484641d8",
    "openxr_platform_defines.h": "a458ba5777415f2de518fdcdcc87fcf8651ba0468c9f69a101378672aad80946",
    "openxr_loader_negotiation.h": "ebcca0946b4b738ebee41943b821abe4df518b3a37766e0ce6883bcefce65124",
}

# the OpenGL backend the host has, for the renderer's command stream
# (configure.py --render-stream; port/linux/host/host_gl_stream.c): the
# backend's files of port/linux/src, copied beside the host's stand-ins for
# the guest's headers (port/linux/host/gl), which their quoted includes then
# find in place of the guest's
HOST_GL_SHARED = ["xgpu.h", "xgpu_backend.h", "xgpu_shader.h", "xgpu_gl.h", "gl.h", "xgpu_gl.c", "xgpu_glsl.c",
                  "xgpu_post.c", "xgpu_text.c", "gl_functions.c", "xbox_texture_decode.c"]
HOST_GL_STAND_INS = ["platform.h", "sdl_platform.h", "port_config.h"]
HOST_GL_SOURCES = ["host_gl_stream.c", "host_gl_textures.c"]
# (the guest's renderer, then: none of its own)
RENDER_STREAM_GUEST_EXCLUDED = ["xgpu_gl.c", "xgpu_post.c"]

# (with the desktop renderer, GLVND's libOpenGL in place of libGLESv2: the
# host's own OpenGL calls, port/android/host/host_gl.c)
HOST_LIBRARIES = ["SDL3", "GLESv2", "EGL", "m", "dl", "pthread"]
DESKTOP_GL_HOST_LIBRARIES = ["SDL3", "OpenGL", "EGL", "m", "dl", "pthread"]


@dataclass(frozen=True)
class LinuxGuest:
    # the ninja target, the build's folder (build/<name>), the prefix of its
    # rules and the configure.py settings that are its own (<name>_cc, and
    # <name>_gl)
    name: str
    # what its rules' descriptions start with
    label: str
    # what it is, for the build file's comments and messages
    title: str
    # the guest's architecture
    arch: GuestArch
    # the processors (platform.machine()) of the computers that build it
    machines: Tuple[str, ...]
    # its renderer unless configure.py says otherwise (<name>_gl): OpenGL ES
    # 3 ("gles", HALO_GLES, as on Android) or desktop OpenGL 4.5 ("desktop",
    # as the x86 build's)
    renderer: str
    # whether it is the default target of those computers (configure.py):
    # not the x86-64 build, as a 64-bit x86 computer builds the x86 build
    # (ninja linux) by default
    default: bool


LINUX_GUESTS = (
    LinuxGuest(name="linux_arm64", label="LINUX ARM64", title="64-bit ARM Linux", arch=ARM64_32,
               machines=("aarch64", "arm64"), renderer="gles", default=True),
    LinuxGuest(name="linux_x86_64", label="LINUX X86_64", title="64-bit x86 Linux", arch=X32,
               machines=("x86_64", "amd64"), renderer="desktop", default=False),
)


def fetch_openxr_headers(openxr_dir: Path) -> None:
    """Download the OpenXR headers into openxr_dir/openxr (configure time,
    once; a header whose SHA-256 is wrong is downloaded again)."""
    include = openxr_dir / "openxr"
    include.mkdir(parents=True, exist_ok=True)
    for name, digest in OPENXR_HEADERS.items():
        header = include / name
        if header.is_file() and hashlib.sha256(header.read_bytes()).hexdigest() == digest:
            continue
        print(f"Downloading the OpenXR SDK's {name} ({OPENXR_TAG})")
        data = download([url.format(tag=OPENXR_TAG, name=name) for url in OPENXR_URLS], digest)
        header.with_suffix(".tmp").write_bytes(data)
        header.with_suffix(".tmp").replace(header)


def native_linux_guest() -> Optional[LinuxGuest]:
    """the desktop guest build of this computer, if there is one (whether it
    is the computer's default build is its own, LinuxGuest.default)"""
    if not sys.platform.startswith("linux"):
        return None
    machine = platform.machine().lower()
    return next((target for target in LINUX_GUESTS if machine in target.machines), None)


def linux_guest_configure_inputs() -> List[Path]:
    return [Path(__file__), GUEST_ARCH_SCRIPT, GUEST_TOOLCHAIN_SCRIPT, PORT_DIR]


def generate_linux_guest_builds(n: Writer, sln: Any) -> None:
    for target in LINUX_GUESTS:
        generate_linux_guest_build(n, sln, target)


def _unavailable(n: Writer, target: LinuxGuest, reason: str, quiet: bool = False) -> None:
    """a target of the build's name that stops with the reason it cannot be
    built, rather than none (ninja would then say only that it is unknown);
    configure.py says it too unless quiet"""
    message = f"{target.title} build ({target.name}) unavailable: {reason}"
    if not quiet:
        print(message, file=sys.stderr)
    n.comment(message)
    n.rule(
        name=f"{target.name}_unavailable",
        command=escape(f"echo {shlex.quote(message)} >&2 && exit 1"),
        description=f"{target.label} UNAVAILABLE",
    )
    n.build(outputs=target.name, rule=f"{target.name}_unavailable")
    n.newline()


def requested(sln: Any, target: LinuxGuest, third_party: Path) -> bool:
    """whether configure.py is to generate the build, and so download its
    sources: the computer's default build, one asked for (--<name>, its
    compiler, or VR), or one whose sources are there already"""
    return (target.default or bool(getattr(sln, target.name, False)) or bool(getattr(sln, f"{target.name}_cc", None))
            or bool(getattr(sln, "port_vr", False))
            or ((third_party / f"musl-{MUSL_VERSION}").is_dir() and (third_party / "SDL3").is_dir()))


def generate_linux_guest_build(n: Writer, sln: Any, target: LinuxGuest) -> None:
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file() or not PORT_DIR.is_dir():
        return
    if native_linux_guest() != target:
        n.comment(f"{target.title} build ({target.name}): only on a {target.title} computer")
        return
    third_party = Path("build") / target.name / "third_party"
    if not requested(sln, target, third_party):
        option = "--" + target.name.replace("_", "-")
        _unavailable(n, target, f"run python3 configure.py {option} first (it downloads musl and SDL3 for it)",
                     quiet=True)
        return
    # (written aside, so that a build that fails to generate halfway leaves
    # nothing of itself but its unavailable target)
    output = io.StringIO()
    try:
        _generate(Writer(output, n.width), sln, target, config_path, third_party)
    except (subprocess.CalledProcessError, OSError, RuntimeError) as error:
        # (the reasons of _generate's own, and those of what it calls: a
        # compiler that cannot run, android_build.compiler_include)
        _unavailable(n, target, str(error))
        return
    n.output.write(output.getvalue())


def _generate(n: Writer, sln: Any, target: LinuxGuest, config_path: Path, third_party: Path) -> None:
    """writes the build to n; raises RuntimeError with the reason it cannot
    be built"""
    prefix, label = target.name, target.label
    build = Path("build") / target.name
    sdl_dir = third_party / "SDL3"
    openxr_dir = third_party / "openxr"
    # the host's compiler: the build's own (--<name>-cc), or the system's; the
    # guest's: the build's own too, else the pinned LLVM's
    # (tools/guest_toolchain.py, configure.py --guest-llvm)
    explicit_cc = getattr(sln, f"{prefix}_cc", None)
    cc = explicit_cc or "clang"
    # the renderer: OpenGL ES 3 (HALO_GLES, as on Android), or desktop OpenGL
    # 4.5 (the x86-64 build's, --linux-arm64-gl=desktop, and the VR build's:
    # the OpenXR runtime takes a desktop GL context)
    vr = getattr(sln, "port_vr", False)
    desktop_gl = vr or (getattr(sln, f"{prefix}_gl", None) or target.renderer) == "desktop"
    # (the host's OpenGL ES and EGL, and the guest's entry points: OpenGL ES's,
    # or desktop OpenGL's from glcorearb.h)
    headers = ["GLES3/gl32.h", "GLES2/gl2ext.h", "EGL/egl.h", "KHR/khrplatform.h"]
    if desktop_gl:
        headers.append("GL/glcorearb.h")
    missing = [header for header in headers if not (GL_HEADERS / header).is_file()]
    if missing:
        raise RuntimeError(f"no {', '.join(missing)} in {GL_HEADERS} (libgles-dev, libegl-dev"
                           + (", libgl-dev" if desktop_gl else "") + ")")
    try:
        fetch_third_party(third_party)
    except (subprocess.CalledProcessError, OSError, RuntimeError) as error:
        raise RuntimeError(f"cannot fetch musl or SDL3 ({error})") from error
    if vr:
        try:
            fetch_openxr_headers(openxr_dir)
        except (RuntimeError, OSError) as error:
            raise RuntimeError(f"cannot fetch the OpenXR headers ({error})") from error
    try:
        toolchain = guest_toolchain(sln, explicit_cc)
    except (RuntimeError, OSError) as error:
        raise RuntimeError(f"no LLVM {LLVM_VERSION} for the guest ({error})") from error
    guest_cc = toolchain.cc if toolchain else cc
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))
    # the guest's and the host's code paths: the desktop's (no HALO_ANDROID),
    # the renderer's and VR's
    guest_defines = [] if desktop_gl else ["-DHALO_GLES=1"]
    host_defines = ["-DHALO_DESKTOP_GL=1"] if desktop_gl else []
    if vr:
        guest_defines.append("-DHALO_VR=1")
        host_defines.append("-DHALO_VR=1")
    # (configure.py --guest-base: the guest's memory at a base the host
    # chooses, tools/guest_base_register.py, instead of the low 4 GB; for an
    # architecture that has one, GuestArch.base_register_flags)
    base_register = bool(getattr(sln, "guest_base", False))
    if base_register:
        if target.arch.base_register_flags is None:
            raise RuntimeError(f"--guest-base: the {target.arch.name} guest has no base register")
        host_defines.append("-DHALO_GUEST_BASE=1")
    # (configure.py --render-stream: the renderer's backend is the host's,
    # the guest's frames its command stream, port/linux/src/xgpu_stream.h)
    render_stream = bool(getattr(sln, "render_stream", False))
    if render_stream:
        guest_defines.append("-DHALO_RENDER_STREAM=1")

    n.comment(f"{target.title} build (ninja {target.name}); see port/linux/README.md")
    n.variable(f"{prefix}_guest_cc", guest_cc)
    n.variable(f"{prefix}_cc", cc)

    guest = generate_guest_image(
        n, sln, config, arch=target.arch, prefix=prefix, label=label, build=build, third_party=third_party,
        guest_cc=guest_cc, gl_headers=GL_HEADERS, ar=toolchain.ar if toolchain else "llvm-ar",
        ld=toolchain.ld if toolchain else "ld.lld", objdump=toolchain.objdump if toolchain else "llvm-objdump",
        builtins_cc=f"${prefix}_cc",
        environment="gnu", platform_defines=guest_defines, extra_runtime=[PORT_DIR / "guest_desktop.c"],
        extra_imports=[PORT_DIR / "host_imports.list", *([PORT_DIR / "vr_imports.list"] if vr else [])],
        extra_prototypes=[PORT_DIR / "guest_desktop.c", *([LINUX_DIR / "src" / "vr_host.h"] if vr else [])],
        updater_cflags=updater_defines(getattr(sln, "port_release", False)), desktop_gl=desktop_gl,
        base_register=base_register, excluded_platform_sources=RENDER_STREAM_GUEST_EXCLUDED if render_stream else ())

    # ---------- SDL3, built from the same source as the guest's headers

    sdl_build = build / "sdl3-build"
    libsdl = sdl_build / "libSDL3.so.0"
    sdl3_rule(n, sln, name=f"{prefix}_sdl3", description=f"{label} SDL3 {SDL_TAG}", source=sdl_dir,
              sdl_build=sdl_build, log_dir=build, cc=f"${prefix}_cc")
    n.build(outputs=libsdl, rule=f"{prefix}_sdl3", implicit=[sdl_dir / "CMakeLists.txt"])

    # ---------- the host

    host_obj_dir = build / "host" / "obj"
    host_cc = f"{prefix}_host_cc"
    n.rule(
        name=host_cc,
        command=f"{compile_launcher(sln)}${prefix}_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description=f"{label} HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    # (frame pointers: the sampler and the crash report walk the host's
    # frames too, host_arch.c)
    host_cflags = " ".join([
        "-O2", "-g", "-fno-omit-frame-pointer", "-fPIC", "-Wall", "-Wno-unused-function", "-D_GNU_SOURCE",
        f"-I{ANDROID_DIR}/include", f"-I{ANDROID_DIR}/host", f"-I{sdl_dir}/include", f"-I{LINUX_DIR}/src",
        f"-I{TOML_DIR}", f"-I{openxr_dir}", *host_defines,
    ])
    # (the app's own: its entry point, and the touch controls through Java)
    android_only = {"host_main.c", "host_touch.c"}
    host_sources = [source for source in host_memory_sources(sorted((ANDROID_DIR / "host").glob("*.c")), base_register)
                    if source.name not in android_only]
    host_sources += [
        PORT_DIR / "host_main.c", PORT_DIR / "host_desktop.c",
        LINUX_DIR / "src" / "posix_files.c", LINUX_DIR / "src" / "posix_net.c",
        LINUX_DIR / "src" / "posix_trace_marker.c",
        # (where config.toml is, for the debugging settings host_main.c reads)
        LINUX_DIR / "src" / "install_layout.c",
        TOML_DIR / "tomlc17.c",
    ]
    if vr:
        # OpenXR (port/linux/src/vr_host.h), with the runtime loaded at run time
        host_sources.append(PORT_DIR / "host_vr.c")
    host_objects: List[Path] = []
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule=host_cc, inputs=source, variables={"cflags": host_cflags})
        host_objects.append(obj)
    miniupnpc_cflags = " ".join([host_cflags, f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}",
                                 *MINIUPNPC_DEFINES])
    for source in [LINUX_DIR / "src" / "posix_upnp.c", *miniupnpc_sources()]:
        obj = host_obj_dir / ("miniupnpc_" + source.name + ".o" if source.parent.parent == MINIUPNPC_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule=host_cc, inputs=source,
                variables={"cflags": miniupnpc_cflags + (" -w" if source.name != "posix_upnp.c" else "")})
        host_objects.append(obj)
    # the self-updater's download (posix_update.c) and internet play's DTLS
    # with browsers (posix_dtls.c), with port/third_party/mbedtls
    mbedtls_cflags = " ".join([host_cflags, f"-I{MBEDTLS_DIR / 'include'}"])
    for name in ("posix_update.c", "posix_dtls.c"):
        n.build(outputs=host_obj_dir / (name + ".o"), rule=host_cc,
                inputs=LINUX_DIR / "src" / name, variables={"cflags": mbedtls_cflags})
        host_objects.append(host_obj_dir / (name + ".o"))
    for source in sorted((MBEDTLS_DIR / "library").glob("*.c")):
        obj = host_obj_dir / "mbedtls" / (source.name + ".o")
        n.build(outputs=obj, rule=host_cc, inputs=source,
                variables={"cflags": f"{mbedtls_cflags} -I{MBEDTLS_DIR / 'library'} -w"})
        host_objects.append(obj)
    host_objects += host_gl_renderer(n, prefix, label, build, host_cc, host_cflags, desktop_gl)
    table_obj = host_obj_dir / "host_import_table.c.o"
    n.build(outputs=table_obj, rule=host_cc, inputs=guest["host_table_c"],
            variables={"cflags": host_cflags})
    host_objects.append(table_obj)
    # the guest image, inside the executable (host_main.c)
    image_obj = host_obj_dir / "guest_image.S.o"
    n.build(outputs=image_obj, rule=host_cc, inputs=PORT_DIR / "guest_image.S",
            implicit=[guest["image"], guest["checked"]], variables={"cflags": f'-DGUEST_IMAGE=\\"{guest["image"]}\\"'})
    host_objects.append(image_obj)

    output = build / "halo"
    staged_sdl = build / "libSDL3.so.0"
    n.rule(
        name=f"{prefix}_host_link",
        # (a position-independent executable, as the guest needs the low 4 GB
        # of the address space: a fixed one, and the heap after it, would be
        # loaded there)
        command=(f"${prefix}_cc -pie -o $out $in -L{sdl_build} "
                 + " ".join(f"-l{lib}" for lib in (DESKTOP_GL_HOST_LIBRARIES if desktop_gl else HOST_LIBRARIES))
                 # (the SDL beside the executable, as the portable x86 build's)
                 + " " + " ".join(SDL_RPATH_LDFLAGS) + " -Wl,--no-undefined"
                 # (posix_trace_marker.c's, which the GPU driver's calls must reach)
                 + "".join(f" -Wl,--export-dynamic-symbol={name}" for name in ("open", "open64", "openat", "openat64"))),
        description=f"{label} LINK $out",
    )
    n.build(outputs=output, rule=f"{prefix}_host_link", inputs=host_objects, implicit=[libsdl])
    n.rule(name=f"{prefix}_copy", command="cp -L $in $out", description=f"{label} STAGE $out")
    n.build(outputs=staged_sdl, rule=f"{prefix}_copy", inputs=libsdl)
    # (SDL's license, which the release carries beside it)
    sdl_license = build / "SDL3-LICENSE.txt"
    n.build(outputs=sdl_license, rule=f"{prefix}_copy", inputs=sdl_dir / "LICENSE.txt")
    # internet play's MQTT brokers, a file beside the game (network.brokers_file)
    brokers = build / "brokers.txt"
    n.build(outputs=brokers, rule=f"{prefix}_copy", inputs=Path("port/assets/network/brokers.txt"))
    n.build(outputs=target.name, rule="phony", inputs=[output, staged_sdl, sdl_license, brokers])
    n.newline()


def host_gl_renderer(n: Writer, prefix: str, label: str, build: Path, host_cc: str, host_cflags: str,
                     desktop_gl: bool) -> List[Path]:
    """The OpenGL backend in the host (port/linux/host/host_gl_stream.c), for
    the renderer's command stream: the backend's files of port/linux/src
    (HOST_GL_SHARED) copied beside the host's stand-ins for the guest's
    headers (port/linux/host/gl), and the XDK's enumerations and texture
    format words for them (xdk_enums.h, as tools/macos_build.py makes it);
    its anti-aliasing's SMAA data and zlib. Every desktop guest's host has
    it (its import, host_gl_window, is every desktop guest's); the guest
    uses it when built with --render-stream. Returns the objects."""
    shared_dir = build / "host" / "gl"
    obj_dir = build / "host" / "obj" / "gl"
    n.rule(name=f"{prefix}_gl_copy", command="cp $in $out", description=f"{label} HOST GL $out")
    xdk = Path("port/include/xdk")
    n.rule(name=f"{prefix}_xdk_enums",
           command=(f"(sed -n '/^\\/\\* ---------- enumerations/,/^\\/\\* ---------- structures and unions/p' "
                    f"{xdk / 'xdk_pdb.h'} | sed '$$d' && "
                    f"grep -E '^#define D3D(FORMAT|SIZE|TEXTURE)_|^#define D3DTSS_MAXSTAGES ' {xdk / 'xdk_d3d8.h'}) > $out"),
           description=f"{label} HOST GL $out")
    copies: List[Path] = []
    for name in HOST_GL_SHARED:
        n.build(outputs=shared_dir / name, rule=f"{prefix}_gl_copy", inputs=LINUX_DIR / "src" / name)
        copies.append(shared_dir / name)
    for name in HOST_GL_STAND_INS:
        n.build(outputs=shared_dir / name, rule=f"{prefix}_gl_copy", inputs=PORT_DIR / "gl" / name)
        copies.append(shared_dir / name)
    n.build(outputs=shared_dir / "xdk_enums.h", rule=f"{prefix}_xdk_enums",
            inputs=[xdk / "xdk_pdb.h", xdk / "xdk_d3d8.h"])
    copies.append(shared_dir / "xdk_enums.h")
    # (the renderer's own code paths: the host's, OpenGL ES's or desktop
    # OpenGL's, not yet the VR mode's)
    cflags = " ".join([f"-I{shared_dir}", host_cflags, f"-I{ZLIB_DIR}", "-DXGPU_GL_HOST=1", "-UHALO_VR",
                       *([] if desktop_gl else ["-DHALO_GLES=1"])])
    objects: List[Path] = []
    for source in [shared_dir / name for name in HOST_GL_SHARED if name.endswith(".c")] + \
            [PORT_DIR / name for name in HOST_GL_SOURCES]:
        obj = obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule=host_cc, inputs=source, order_only=copies, variables={"cflags": cflags})
        objects.append(obj)
    smaa = smaa_assets_build(n, f"{prefix}_host", build / "host" / "smaa_embedded.c")[0]
    n.build(outputs=obj_dir / "smaa_embedded.c.o", rule=host_cc, inputs=smaa, variables={"cflags": cflags})
    objects.append(obj_dir / "smaa_embedded.c.o")
    for name in ZLIB_SOURCES:
        obj = obj_dir / "zlib" / (name + ".o")
        n.build(outputs=obj, rule=host_cc, inputs=ZLIB_DIR / name,
                variables={"cflags": " ".join([host_cflags, *ZLIB_DEFINES, "-w"])})
        objects.append(obj)
    return objects
