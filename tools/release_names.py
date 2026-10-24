#!/usr/bin/env python3
"""The names the release publishes each build under, as players see them in
their downloads: OpenH1-<machine>[-debug] (.dmg for the Mac, .apk for
Android, .zip for the rest). The workflow's artifacts and dist/ keep their own names
(tools/ci_build.py dist_name: halo-<platform>-<config>), which this maps.

    python3 tools/release_names.py <artifact>   # its published name, without the extension

The self-updaters ask for these names (port/linux/src/updater.c's
UPDATE_NAME, and the Android app's Updater.java, with -debug for a debug
build).
"""

import sys

# the release builds' names; a debug build's is the same with -debug
NAMES = {
    "windows": "OpenH1-Windows",
    "linux-x86_64": "OpenH1-Linux",
    "linux-x86_64-vr": "OpenH1-Linux-VR",
    "linux-arm64": "OpenH1-Linux-ARM64",
    "steam-frame-vr": "OpenH1-Steam-Frame",
    "macos": "OpenH1",
    "android": "OpenH1-Android",
}


def published(artifact: str) -> str:
    """halo-<platform>-<config>[-symbols] -> OpenH1-<machine>[-debug][-symbols]; an artifact of no machine
    here keeps its name"""
    name, symbols = (artifact[:-len("-symbols")], "-symbols") if artifact.endswith("-symbols") else (artifact, "")
    for config in ("release", "debug"):
        if name.startswith("halo-") and name.endswith(f"-{config}"):
            platform = name[len("halo-"):-len(f"-{config}")]
            if platform in NAMES:
                return NAMES[platform] + ("-debug" if config == "debug" else "") + symbols
    return artifact


def main() -> int:
    arguments = sys.argv[1:]
    if len(arguments) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    print(published(arguments[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
