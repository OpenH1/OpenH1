"""The macOS app (tools/macos_app.py, port/macos/README.md, "The app"): its
Info.plist and entitlements anywhere; on a Mac with Apple silicon, the app
that ninja macos built (build/macos/Halo.app): its contents, the
executable's rpath to its Frameworks, and its ad hoc signature with the
hardened runtime."""
import platform
import plistlib
import subprocess
import sys
from pathlib import Path

import pytest

from tools import macos_app
from tools.macos_build import MACOS_BUNDLE_ID, MACOS_MINIMUM

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "build" / "macos" / "Halo.app"

built = pytest.mark.skipif(
    sys.platform != "darwin" or platform.machine().lower() != "arm64" or not APP.is_dir(),
    reason="needs a Mac with Apple silicon and build/macos/Halo.app (ninja macos)")


def test_info_plist_asks_for_game_mode_and_names_the_build():
    plist = macos_app.info_plist(MACOS_BUNDLE_ID, 1234, MACOS_MINIMUM)
    assert plist["LSApplicationCategoryType"] == "public.app-category.games"
    assert plist["LSSupportsGameMode"] is True
    assert plist["GCSupportsControllerUserInteraction"] is True
    assert plist["NSHighResolutionCapable"] is True
    assert plist["LSMinimumSystemVersion"] == MACOS_MINIMUM
    assert plist["CFBundleShortVersionString"] == plist["CFBundleVersion"] == "1234"
    assert plist["CFBundleExecutable"] == "halo" and plist["CFBundleName"] == "Halo"
    assert plist["NSMicrophoneUsageDescription"] and plist["NSLocalNetworkUsageDescription"]
    assert "not endorsed" in plist["NSHumanReadableCopyright"]


def test_entitlements_are_the_narrowest():
    """MAP_JIT's entitlement, not unsigned executable memory, and library
    validation kept (only the ad hoc signature goes without it)"""
    with open(macos_app.ENTITLEMENTS, "rb") as file:
        entitlements = plistlib.load(file)
    assert entitlements == {"com.apple.security.cs.allow-jit": True,
                            "com.apple.security.device.audio-input": True}


@built
def test_app_contents():
    contents = APP / "Contents"
    for path in ("MacOS/halo", "Frameworks/libSDL3.0.dylib", "Resources/openh1-icon.icns",
                 "Resources/brokers.txt", "Info.plist"):
        assert (contents / path).is_file(), path
    with open(contents / "Info.plist", "rb") as file:
        assert plistlib.load(file)["LSSupportsGameMode"] is True
    commands = subprocess.run(["otool", "-l", str(contents / "MacOS/halo")], capture_output=True, text=True,
                              check=True).stdout
    assert "path @executable_path/../Frameworks " in commands


@built
def test_app_is_signed_with_the_hardened_runtime():
    subprocess.run(["codesign", "--verify", "--strict", "--deep", str(APP)], check=True)
    details = subprocess.run(["codesign", "-d", "--verbose=2", str(APP)], capture_output=True, text=True).stderr
    assert "runtime" in details
    entitlements = subprocess.run(["codesign", "-d", "--entitlements", "-", "--xml", str(APP)],
                                  capture_output=True).stdout
    granted = plistlib.loads(entitlements[entitlements.index(b"<?xml"):])
    assert granted["com.apple.security.cs.allow-jit"] is True
    assert "com.apple.security.cs.allow-unsigned-executable-memory" not in granted
