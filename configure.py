#!/usr/bin/env python3

# root build script: writes build.ninja for the native ports (Linux, Windows,
# Android, macOS) and the web port

import argparse
import io
import os
import sys
from pathlib import Path
from types import SimpleNamespace

from tools import ninja_syntax
from tools.android_build import android_configure_inputs, generate_android_build
from tools.guest_toolchain import LLVM_VERSION, default_cache
from tools.linux_guest_build import generate_linux_guest_builds, linux_guest_configure_inputs, native_linux_guest
from tools.linux_build import check_profile_options, generate_linux_build, linux_configure_inputs
from tools.macos_build import generate_macos_build, is_macos_arm64, macos_configure_inputs
from tools.web_build import generate_web_build, web_configure_inputs
from tools.windows_build import generate_windows_build, windows_configure_inputs

# arguments
parser = argparse.ArgumentParser()
parser.add_argument(
    "--linux-cc",
    metavar="BINARY",
    help="compiler for the native Linux build, `ninja linux` (default: clang)",
)
parser.add_argument(
    "--compiler-launcher",
    metavar="BINARY",
    help="a program that runs each compile, such as ccache "
    "(links and the Android host's few units run the compiler directly)",
)
parser.add_argument(
    "--release",
    action="store_true",
    help="release builds (Linux, Windows, Android, web): assertions are not checked",
)
parser.add_argument(
    "--profile",
    action="store_true",
    help="profiling builds (Linux, Windows, Android): CPU scopes recorded on a "
    "console command or launch setting (README, \"Profiling builds\"); not with --pgo=train",
)
parser.add_argument(
    "--lto",
    choices=["full", "thin", "off"],
    default="full",
    help="link-time optimisation (Linux, Windows): full (the default: the whole game "
    "optimised as one module, the fastest code and the slowest links), thin (parallel and incremental) or off",
)
parser.add_argument(
    "--portable",
    action="store_true",
    help="x86 builds (Linux, Windows): code for any x86-64 processor (SSE2) rather than for this "
    "machine's (-march=native, the default); use it for builds that run on other computers. On Linux it also "
    "builds against Debian 11's glibc 2.31 rather than this machine's, and brings its own SDL 3 (libSDL3.so.0, "
    "beside the executable), so that it starts on SteamOS and older distributions: the first build downloads "
    "the Debian packages and SDL's source (build/linux/third_party, tools/linux_sysroot.py), and needs CMake, "
    "pkgconf and wayland-scanner, not the 32-bit SDL 3",
)
parser.add_argument(
    "--pgo",
    nargs="?",
    const="train",
    default="use",
    choices=["use", "train", "off"],
    help="profile-guided optimisation: use (the default) optimises with the profiles in pgo/, "
    "train (or plain --pgo) first records this platform's profile if it is missing, by letting an instrumented "
    "build play every campaign level (needs the game data in assets/ and a display; Linux and Windows), off does "
    "without",
)
parser.add_argument(
    "--pgo-profile",
    metavar="PROFDATA",
    type=Path,
    help="profile-guided optimisation from this profile instead",
)
parser.add_argument(
    "--guest-llvm",
    metavar="pinned|system|FOLDER",
    default="pinned",
    help="the guests' compiler, linker and LLVM tools (the Android, Linux arm64 and x86-64, and macOS builds' "
    f"guest images): pinned (the default) downloads LLVM {LLVM_VERSION}'s release for this computer once (about "
    "1.5 GB, checked against its SHA-256) into --guest-llvm-cache; system takes clang, ld.lld, llvm-ar and "
    "llvm-objdump from the PATH (and the NDK's, or Homebrew's on macOS); a FOLDER is an LLVM installation (its "
    "bin/). A build's own compiler (--linux-arm64-cc, --linux-x86-64-cc, --android-guest-cc, --macos-cc, "
    "--macos-guest-cc) is used instead. The hosts are built with the system's compiler",
)
parser.add_argument(
    "--guest-llvm-cache",
    metavar="FOLDER",
    help=f"where --guest-llvm=pinned keeps LLVM (default: {default_cache()})",
)
parser.add_argument(
    "--android-ndk",
    type=str,
    help="Android NDK for `ninja android` (default: ANDROID_NDK_HOME, or the newest under the Android SDK)",
)
parser.add_argument(
    "--android-guest-cc",
    type=str,
    help="clang with the arm64_32 target for the Android guest (default: --guest-llvm's)",
)
parser.add_argument(
    "--linux-arm64-cc",
    type=str,
    help="clang (with the arm64_32 target) for `ninja linux_arm64`, the 64-bit ARM Linux build, its guest and its "
    "host (default: --guest-llvm's for the guest, clang for the host)",
)
parser.add_argument(
    "--linux-arm64-gl",
    choices=["gles", "desktop"],
    default="gles",
    help="the renderer of `ninja linux_arm64`: OpenGL ES 3 (the default, as on Android) or desktop OpenGL 4.5",
)
parser.add_argument(
    "--linux-x86-64",
    action="store_true",
    help="generate `ninja linux_x86_64`, the 64-bit x86 Linux build, on a 64-bit x86 Linux computer, whose default "
    "build is the x86 one: configure.py then downloads its musl and SDL3 (once, to build/linux_x86_64/third_party). "
    "--linux-x86-64-cc and --vr ask for it too",
)
parser.add_argument(
    "--linux-x86-64-cc",
    type=str,
    help="clang (with the x32 target) for `ninja linux_x86_64`, the 64-bit x86 Linux build, its guest and its host "
    "(default: --guest-llvm's for the guest, clang for the host)",
)
parser.add_argument(
    "--linux-vulkan",
    action="store_true",
    help="generate `ninja linux_vulkan`, the 64-bit x86 Linux build with the Vulkan renderer (on the host, through "
    "the renderer's command stream), on a 64-bit x86 Linux computer: configure.py then downloads its musl, SDL3 and "
    "Vulkan headers (once, to build/linux_vulkan/third_party); refer to port/linux/vulkan/README.md",
)
parser.add_argument(
    "--vr",
    action="store_true",
    help="the VR mode (OpenXR) in `ninja linux_arm64` and `ninja linux_x86_64`, with the desktop OpenGL renderer; "
    "refer to \"VR\" in port/linux/README.md",
)
parser.add_argument(
    "--guest-base",
    action="store_true",
    help="the Linux arm64 and Android builds' guest memory at a base the host chooses (a base register, "
    "tools/guest_base_register.py) instead of the fixed low 4 GB; the macOS build always has it",
)
parser.add_argument(
    "--macos-cc",
    type=str,
    help="clang for `ninja macos`, the macOS build: its host, and its guest unless --macos-guest-cc (default: clang "
    "for the host, --guest-llvm's for the guest)",
)
parser.add_argument(
    "--macos-guest-cc",
    type=str,
    help="clang (with the arm64_32 target) for the macOS build's guest alone (default: --macos-cc's, else "
    "--guest-llvm's)",
)
parser.add_argument(
    "--macos-bundle-id",
    type=str,
    help="the macOS app's bundle identifier (default: io.github.opencommunityedition.opence, after the "
    "project's GitHub organisation); macOS keeps the app's permissions (the microphone, the local network) by it",
)
parser.add_argument(
    "--web-emcc",
    type=str,
    help="Emscripten's emcc for `ninja web` (default: emcc on the PATH, then EMSDK's or ~/emsdk's)",
)
args = parser.parse_args()
try:
    check_profile_options(args.profile, args.pgo)
except ValueError as error:
    parser.error(str(error))

# the settings the builds read
sln = SimpleNamespace(
    build_dir=Path("build"),
    linux_cc=args.linux_cc,
    compiler_launcher=args.compiler_launcher,
    port_release=args.release,
    port_profile=args.profile,
    port_lto=args.lto,
    port_portable=args.portable,
    port_pgo=args.pgo,
    port_pgo_profile=args.pgo_profile,
    android_ndk=args.android_ndk,
    guest_llvm=args.guest_llvm,
    guest_llvm_cache=args.guest_llvm_cache,
    android_guest_cc=args.android_guest_cc,
    linux_arm64_cc=args.linux_arm64_cc,
    linux_arm64_gl=args.linux_arm64_gl,
    linux_x86_64=args.linux_x86_64,
    linux_x86_64_cc=args.linux_x86_64_cc,
    linux_vulkan=args.linux_vulkan,
    port_vr=args.vr,
    macos_cc=args.macos_cc,
    macos_guest_cc=args.macos_guest_cc,
    macos_bundle_id=args.macos_bundle_id,
    guest_base=args.guest_base,
    web_emcc=args.web_emcc,
)


def is_windows() -> bool:
    return os.name == "nt"


# build.ninja
out = io.StringIO()
n = ninja_syntax.Writer(out)
n.variable("ninja_required_version", "1.3")
n.newline()

configure_script = Path(os.path.relpath(os.path.abspath(sys.argv[0])))
n.comment("The arguments passed to configure.py, for rerunning it.")
n.variable(
    "configure_args",
    [f'"{arg}"' if any(ch.isspace() for ch in arg) else arg for arg in sys.argv[1:]],
)
n.variable("python", f'"{sys.executable}"')
n.newline()

generate_linux_build(n, sln)
generate_android_build(n, sln)
generate_linux_guest_builds(n, sln)
generate_windows_build(n, sln)
generate_macos_build(n, sln)
generate_web_build(n, sln)

n.comment("Reconfigure on change")
n.rule(
    name="configure",
    command=f"$python {configure_script} $configure_args",
    generator=True,
    description=f"RUN {configure_script}",
)
n.build(
    outputs="build.ninja",
    rule="configure",
    implicit=[
        configure_script,
        Path("tools/ninja_syntax.py"),
        *linux_configure_inputs(),
        *android_configure_inputs(),
        *linux_guest_configure_inputs(),
        *windows_configure_inputs(),
        *macos_configure_inputs(),
        *web_configure_inputs(),
    ],
)
n.newline()

# the build for this computer, where it could be generated (the Windows
# build is left out when SDL cannot be fetched, for instance): on 64-bit x86
# Linux the x86 build, not the guest build (linux_x86_64); on a Mac with Apple
# silicon the macOS build
native_guest = native_linux_guest()
default = ("windows" if is_windows() else native_guest.name if native_guest and native_guest.default
           else "macos" if is_macos_arm64() else "linux")
if f"\nbuild {default}: " in out.getvalue():
    n.comment("Default rule: the build for this computer")
    n.default(default)

with open("build.ninja", "w", encoding="utf-8") as f:
    f.write(out.getvalue())
out.close()
