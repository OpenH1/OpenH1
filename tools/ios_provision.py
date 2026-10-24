#!/usr/bin/env python3
"""A development provisioning profile for the iOS app (port/ios/README.md,
"Signing"), made by Xcode's automatic signing.

    python3 tools/ios_provision.py --team <team ID> [--bundle-id com.openh1.openh1]

The app is built by ninja (tools/ios_build.py), not by Xcode, but only Xcode
makes provisioning profiles without an App Store Connect API key: this
writes a stand-in Xcode project (one empty app target, with the app's bundle
identifier and entitlements, port/ios/halo.entitlements) in
build/ios/provision and builds it for a device with
-allowProvisioningUpdates, which registers the bundle identifier, turns on
the capabilities the entitlements name, and makes a profile that holds the
team's registered devices. The profile is copied to
build/ios/embedded.mobileprovision, which tools/ios_app.py signs the app
with. Xcode must be signed in to the team's Apple account (Xcode, Settings,
Accounts); the device must be registered (Xcode registers a connected one).
"""

import argparse
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENTITLEMENTS = ROOT / "port/ios/halo.entitlements"
BUILD = ROOT / "build/ios"
WORK = BUILD / "provision"
PROFILE = BUILD / "embedded.mobileprovision"
DEFAULT_BUNDLE_ID = "com.openh1.openh1"

# the stand-in project: one target, one source, automatic signing; the IDs
# are any unique 24 hexadecimal digits
PROJECT = """// !$*UTF8*$!
{
	archiveVersion = 1;
	classes = {};
	objectVersion = 56;
	objects = {
		A0000000000000000000000A /* main.m in Sources */ = {isa = PBXBuildFile; fileRef = A0000000000000000000000B; };
		A0000000000000000000000B /* main.m */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.objc; path = main.m; sourceTree = "<group>"; };
		A0000000000000000000000C /* halo.entitlements */ = {isa = PBXFileReference; lastKnownFileType = text.plist.entitlements; path = halo.entitlements; sourceTree = "<group>"; };
		A0000000000000000000000D /* Info.plist */ = {isa = PBXFileReference; lastKnownFileType = text.plist.xml; path = Info.plist; sourceTree = "<group>"; };
		A0000000000000000000000E /* Stub.app */ = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = Stub.app; sourceTree = BUILT_PRODUCTS_DIR; };
		A0000000000000000000000F = {isa = PBXGroup; children = (A0000000000000000000000B, A0000000000000000000000C, A0000000000000000000000D, A00000000000000000000010); sourceTree = "<group>"; };
		A00000000000000000000010 /* Products */ = {isa = PBXGroup; children = (A0000000000000000000000E); name = Products; sourceTree = "<group>"; };
		A00000000000000000000011 /* Sources */ = {isa = PBXSourcesBuildPhase; buildActionMask = 2147483647; files = (A0000000000000000000000A); runOnlyForDeploymentPostprocessing = 0; };
		A00000000000000000000012 /* Stub */ = {
			isa = PBXNativeTarget;
			buildConfigurationList = A00000000000000000000013;
			buildPhases = (A00000000000000000000011);
			buildRules = ();
			dependencies = ();
			name = Stub;
			productName = Stub;
			productReference = A0000000000000000000000E;
			productType = "com.apple.product-type.application";
		};
		A00000000000000000000014 /* Project object */ = {
			isa = PBXProject;
			attributes = {LastUpgradeCheck = 1500; TargetAttributes = {A00000000000000000000012 = {DevelopmentTeam = @TEAM@; ProvisioningStyle = Automatic; };};};
			buildConfigurationList = A00000000000000000000015;
			compatibilityVersion = "Xcode 14.0";
			developmentRegion = en;
			hasScannedForEncodings = 0;
			knownRegions = (en, Base);
			mainGroup = A0000000000000000000000F;
			productRefGroup = A00000000000000000000010;
			projectDirPath = "";
			projectRoot = "";
			targets = (A00000000000000000000012);
		};
		A00000000000000000000016 /* Debug */ = {
			isa = XCBuildConfiguration;
			buildSettings = {
				CODE_SIGN_ENTITLEMENTS = halo.entitlements;
				CODE_SIGN_STYLE = Automatic;
				CODE_SIGN_IDENTITY = "Apple Development";
				DEVELOPMENT_TEAM = @TEAM@;
				INFOPLIST_FILE = Info.plist;
				IPHONEOS_DEPLOYMENT_TARGET = 17.0;
				PRODUCT_BUNDLE_IDENTIFIER = @BUNDLE@;
				PRODUCT_NAME = Stub;
				SDKROOT = iphoneos;
				TARGETED_DEVICE_FAMILY = "1,2";
			};
			name = Debug;
		};
		A00000000000000000000017 /* Debug */ = {isa = XCBuildConfiguration; buildSettings = {SDKROOT = iphoneos; }; name = Debug; };
		A00000000000000000000013 = {isa = XCConfigurationList; buildConfigurations = (A00000000000000000000016); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
		A00000000000000000000015 = {isa = XCConfigurationList; buildConfigurations = (A00000000000000000000017); defaultConfigurationIsVisible = 0; defaultConfigurationName = Debug; };
	};
	rootObject = A00000000000000000000014;
}
"""

MAIN = "int main(void) { return 0; }\n"


def provision(team: str, bundle_id: str) -> Path:
    shutil.rmtree(WORK, ignore_errors=True)
    project = WORK / "Stub.xcodeproj"
    project.mkdir(parents=True)
    (project / "project.pbxproj").write_text(PROJECT.replace("@TEAM@", team).replace("@BUNDLE@", bundle_id))
    (WORK / "main.m").write_text(MAIN)
    shutil.copy2(ENTITLEMENTS, WORK / "halo.entitlements")
    with open(WORK / "Info.plist", "wb") as file:
        plistlib.dump({"CFBundleExecutable": "Stub", "CFBundleIdentifier": bundle_id, "CFBundleName": "Stub",
                       "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1", "CFBundleVersion": "1"},
                      file)
    command = ["xcodebuild", "-project", str(project), "-target", "Stub", "-configuration", "Debug", "-sdk",
               "iphoneos", "-allowProvisioningUpdates", f"SYMROOT={WORK / 'out'}", "build"]
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, cwd=WORK, capture_output=True, text=True)
    (WORK / "xcodebuild.log").write_text(result.stdout + result.stderr)
    if result.returncode != 0:
        for line in (result.stdout + result.stderr).splitlines():
            if "error" in line.lower():
                print(line, file=sys.stderr)
        sys.exit(f"xcodebuild could not provision the app (its log: {WORK / 'xcodebuild.log'})")
    made = next((WORK / "out").glob("**/Stub.app/embedded.mobileprovision"), None)
    if not made:
        sys.exit("xcodebuild made no provisioning profile")
    shutil.copy2(made, PROFILE)
    return PROFILE


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--team", required=True, help="the Apple developer team's ID")
    parser.add_argument("--bundle-id", default=DEFAULT_BUNDLE_ID)
    args = parser.parse_args()
    print(provision(args.team, args.bundle_id))


if __name__ == "__main__":
    main()
