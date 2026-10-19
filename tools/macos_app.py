#!/usr/bin/env python3
"""The macOS app, Halo.app, its signature, its disk image and notarization
(port/macos/README.md, "The app").

    python3 tools/macos_app.py bundle ...   # ninja macos does it: build/macos/Halo.app, signed ad hoc
    python3 tools/macos_app.py dmg ...      # ninja macos_dmg: build/macos/Halo.dmg
    python3 tools/macos_app.py sign --identity "Developer ID Application: <name> (<team>)"
    python3 tools/macos_app.py notarize --identity "Developer ID Application: <name> (<team>)" \\
        --keychain-profile <profile>

- bundle: Halo.app from the build: Contents/MacOS/halo (the executable, with
  an rpath of @executable_path/../Frameworks), Contents/Frameworks/
  libSDL3.0.dylib, Contents/Resources (the icon, brokers.txt, the About
  panel's credits) and Info.plist, signed with the hardened runtime and
  port/macos/halo.entitlements: ad hoc ("-") unless an identity is given.
- dmg: a disk image of the app, with a link to Applications to drag it to.
- sign: signs the app again with a Developer ID (and a secure timestamp,
  which notarization needs), and checks the signature.
- notarize: sign, then the app sent to Apple's notary service (xcrun
  notarytool) and its ticket stapled to it, then the disk image made of it,
  signed, notarized and stapled too. The credentials are notarytool's: a
  keychain profile (xcrun notarytool store-credentials <profile>), or an
  Apple ID, its team and an app-specific password.
"""

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENTITLEMENTS = ROOT / "port/macos/halo.entitlements"
APP_NAME = "Halo"
EXECUTABLE = "halo"
SDL_LIBRARY = "libSDL3.0.dylib"
ICON = "openh1-icon"
# what the web page and the README say of the project (port/web/site/index.html)
COPYRIGHT = ("OpenH1 is an unofficial fan project. It is not endorsed by or affiliated with Microsoft, Xbox "
             "Game Studios, Halo Studios or Bungie. Halo and Halo: Combat Evolved are trademarks of Microsoft "
             "Corporation. No game data is included: you need your own copy of the game.")


def run(*command: str) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def info_plist(bundle_id: str, build: int, minimum: str) -> dict:
    """the app's Info.plist. The version is the build's number (that of the
    release it is, HALO_BUILD_NUMBER; 0 for a local build), as the game
    names itself ("OpenH1 macOS | build <n>"). The games category and Game
    Mode's key make macOS turn Game Mode on while the game is fullscreen."""
    version = str(build)
    return {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": APP_NAME,
        "CFBundleExecutable": EXECUTABLE,
        "CFBundleIconFile": ICON,
        "CFBundleIdentifier": bundle_id,
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": APP_NAME,
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "LSApplicationCategoryType": "public.app-category.games",
        "LSSupportsGameMode": True,
        "LSMinimumSystemVersion": minimum,
        "GCSupportsControllerUserInteraction": True,
        "NSHighResolutionCapable": True,
        "NSHumanReadableCopyright": COPYRIGHT,
        "NSMicrophoneUsageDescription": "Voice chat in network games uses the microphone while you talk.",
        "NSLocalNetworkUsageDescription": "System link finds and plays games on your local network.",
    }


def credits_html(build: int, flavor: str) -> str:
    """the About panel's text: the build, as the game names it
    (source/main/main.c)"""
    label = f"build {build} ({flavor})" if build > 0 else f"local build ({flavor})"
    return ("<html><body style=\"font: 11px -apple-system; text-align: center\">"
            f"<p>OpenH1 macOS | {label}</p>"
            "<p>The Halo: Combat Evolved decompilation (Xbox build 01.01.14.2342), ported.<br>"
            "<a href=\"https://github.com/OpenH1/OpenH1\">github.com/OpenH1/OpenH1</a></p>"
            "</body></html>\n")


def codesign(path: Path, identity: str, entitlements: Path = None) -> None:
    command = ["codesign", "--force", "--sign", identity, "--options", "runtime",
               "--timestamp" if identity != "-" else "--timestamp=none"]
    if entitlements:
        command += ["--entitlements", str(entitlements)]
    run(*command, str(path))


def sign_app(app: Path, identity: str) -> None:
    """the library, then the app (whose signature covers it), and checks.
    Signed ad hoc, the app also has disable-library-validation: the hardened
    runtime loads only libraries of the app's team, and ad hoc signatures
    have none ("mapping process and mapped file (non-platform) have
    different Team IDs"); a Developer ID's are the same team."""
    codesign(app / "Contents/Frameworks" / SDL_LIBRARY, identity)
    if identity != "-":
        codesign(app, identity, ENTITLEMENTS)
    else:
        with open(ENTITLEMENTS, "rb") as file:
            entitlements = plistlib.load(file)
        entitlements["com.apple.security.cs.disable-library-validation"] = True
        with tempfile.TemporaryDirectory() as temporary:
            ad_hoc = Path(temporary) / "ad_hoc.entitlements"
            with open(ad_hoc, "wb") as file:
                plistlib.dump(entitlements, file)
            codesign(app, identity, ad_hoc)
    run("codesign", "--verify", "--strict", "--deep", str(app))


def bundle(args: argparse.Namespace) -> None:
    app = Path(args.app)
    staging = app.with_name(app.name + ".partial")
    shutil.rmtree(staging, ignore_errors=True)
    contents = staging / "Contents"
    for folder in ("MacOS", "Frameworks", "Resources"):
        (contents / folder).mkdir(parents=True)
    shutil.copy2(args.executable, contents / "MacOS" / EXECUTABLE)
    shutil.copy2(args.sdl, contents / "Frameworks" / SDL_LIBRARY)
    shutil.copy2(args.icon, contents / "Resources" / (ICON + ".icns"))
    for resource in args.resource or []:
        shutil.copy2(resource, contents / "Resources" / Path(resource).name)
    plist = info_plist(args.bundle_id, args.build, args.minimum)
    with open(contents / "Info.plist", "wb") as file:
        plistlib.dump(plist, file)
    (contents / "PkgInfo").write_text("APPL????")
    (contents / "Resources" / "Credits.html").write_text(credits_html(args.build, args.flavor), encoding="utf-8")
    sign_app(staging, args.identity)
    shutil.rmtree(app, ignore_errors=True)
    staging.rename(app)


def dmg(args: argparse.Namespace) -> None:
    app = Path(args.app)
    output = Path(args.output)
    with tempfile.TemporaryDirectory() as temporary:
        folder = Path(temporary) / APP_NAME
        folder.mkdir()
        # (ditto keeps the signature's extended attributes and links)
        run("ditto", str(app), str(folder / app.name))
        (folder / "Applications").symlink_to("/Applications")
        partial = output.with_name(output.name + ".partial.dmg")
        partial.unlink(missing_ok=True)
        run("hdiutil", "create", "-quiet", "-volname", APP_NAME, "-srcfolder", str(folder), "-fs", "APFS",
            "-format", "ULFO", "-ov", str(partial))
        partial.replace(output)
    if args.identity and args.identity != "-":
        run("codesign", "--force", "--sign", args.identity, "--timestamp", str(output))


def notary_credentials(args: argparse.Namespace) -> list:
    if args.keychain_profile:
        return ["--keychain-profile", args.keychain_profile]
    if args.apple_id and args.team_id and args.password:
        return ["--apple-id", args.apple_id, "--team-id", args.team_id, "--password", args.password]
    sys.exit("notarize: give --keychain-profile, or --apple-id, --team-id and --password")


def notarize_file(path: Path, credentials: list) -> None:
    run("xcrun", "notarytool", "submit", str(path), *credentials, "--wait")


def notarize(args: argparse.Namespace) -> None:
    app = Path(args.app)
    credentials = notary_credentials(args)
    sign_app(app, args.identity)
    with tempfile.TemporaryDirectory() as temporary:
        archive = Path(temporary) / (APP_NAME + ".zip")
        run("ditto", "-c", "-k", "--keepParent", str(app), str(archive))
        notarize_file(archive, credentials)
    run("xcrun", "stapler", "staple", str(app))
    dmg(argparse.Namespace(app=str(app), output=args.output, identity=args.identity))
    notarize_file(Path(args.output), credentials)
    run("xcrun", "stapler", "staple", args.output)
    run("spctl", "--assess", "--type", "execute", "--verbose=2", str(app))
    run("spctl", "--assess", "--type", "open", "--context", "context:primary-signature", "--verbose=2", args.output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    default_app = str(ROOT / "build/macos" / (APP_NAME + ".app"))
    default_dmg = str(ROOT / "build/macos" / (APP_NAME + ".dmg"))

    command = commands.add_parser("bundle", help="make Halo.app from the build")
    command.add_argument("--app", required=True)
    command.add_argument("--executable", required=True)
    command.add_argument("--sdl", required=True)
    command.add_argument("--icon", required=True)
    command.add_argument("--resource", action="append")
    command.add_argument("--bundle-id", required=True)
    command.add_argument("--build", type=int, default=0)
    command.add_argument("--flavor", default="debug")
    command.add_argument("--minimum", required=True)
    command.add_argument("--identity", default="-")
    command.set_defaults(function=bundle)

    command = commands.add_parser("dmg", help="a disk image of the app")
    command.add_argument("--app", default=default_app)
    command.add_argument("--output", default=default_dmg)
    command.add_argument("--identity", default="-")
    command.set_defaults(function=dmg)

    command = commands.add_parser("sign", help="sign the app with a Developer ID")
    command.add_argument("--app", default=default_app)
    command.add_argument("--identity", required=True, help='"Developer ID Application: <name> (<team>)"')
    command.set_defaults(function=lambda args: sign_app(Path(args.app), args.identity))

    command = commands.add_parser("notarize", help="sign, notarize and staple the app and its disk image")
    command.add_argument("--app", default=default_app)
    command.add_argument("--output", default=default_dmg)
    command.add_argument("--identity", required=True, help='"Developer ID Application: <name> (<team>)"')
    command.add_argument("--keychain-profile", help="notarytool's stored credentials")
    command.add_argument("--apple-id")
    command.add_argument("--team-id")
    command.add_argument("--password", help="an app-specific password")
    command.set_defaults(function=notarize)

    args = parser.parse_args()
    os.chdir(ROOT)
    args.function(args)


if __name__ == "__main__":
    main()
