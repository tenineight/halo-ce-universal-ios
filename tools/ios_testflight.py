#!/usr/bin/env python3
"""Packs the iOS build for App Store Connect (TestFlight), without game data.

The app has none to leave out: the player imports the maps from their own
disc image on the first start (port/ios/host_import.c,
port/linux/src/sdl_platform.c's platform_offer_game_data).

  1. builds the device app (ninja ios);
  2. puts a copy of it in an Xcode archive (build/ios/testflight/Halo.xcarchive)
     with the bundle identifier, version and build number of this upload
     (the build number by default the time, so that each upload's is
     greater), signed with the entitlements the game asks for
     (port/ios/Halo.entitlements);
  3. has Xcode export it for App Store Connect (xcodebuild -exportArchive,
     automatic signing: with -allowProvisioningUpdates it registers the
     bundle identifier, and makes the distribution certificate and profile,
     for the team Xcode is signed in to), to build/ios/testflight/Halo.ipa,
     or with --upload straight to App Store Connect, whose app record for
     the bundle identifier must exist (My Apps, +, New App).

Usage: ios_testflight.py --bundle-id <identifier> [--team <id>] [--version 1.0] [--build <n>] [--upload]
"""

import argparse
import datetime
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import apple_env  # noqa: F401  (the .env file's settings)

BUILD = Path("build/ios")
APP = BUILD / "iphoneos" / "Halo.app"
OUTPUT = BUILD / "testflight"
ENTITLEMENTS = Path("port/ios/Halo.entitlements")


def run(*arguments: str, **options) -> None:
    print("+ " + " ".join(arguments))
    subprocess.run(arguments, check=True, **options)


def team_of_profile(app: Path) -> str:
    """the team of the profile the development build was signed with"""
    data = subprocess.run(["security", "cms", "-D", "-i", str(app / "embedded.mobileprovision")], check=True,
                          capture_output=True).stdout
    return plistlib.loads(data)["TeamIdentifier"][0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bundle-id", default=os.environ.get("HALO_IOS_BUNDLE_ID"), required=False)
    parser.add_argument("--team", default=os.environ.get("HALO_IOS_TEAM"),
                        help="the developer team (default: HALO_IOS_TEAM, or the development profile's)")
    parser.add_argument("--version", default=os.environ.get("HALO_IOS_VERSION", "1.0"))
    parser.add_argument("--build", default=datetime.datetime.now().strftime("%Y%m%d%H%M"))
    parser.add_argument("--upload", action="store_true", help="upload to App Store Connect, not only export")
    arguments = parser.parse_args()
    if not arguments.bundle_id:
        sys.exit("ios_testflight.py: --bundle-id (or HALO_IOS_BUNDLE_ID) is needed: App Store Connect's app's")

    run("ninja", "ios")
    team = arguments.team or team_of_profile(APP)

    # the archive, with this upload's identity
    archive = OUTPUT / "Halo.xcarchive"
    shutil.rmtree(OUTPUT, ignore_errors=True)
    app = archive / "Products" / "Applications" / "Halo.app"
    shutil.copytree(APP, app, symlinks=True)
    (app / "embedded.mobileprovision").unlink(missing_ok=True)
    shutil.rmtree(app / "_CodeSignature", ignore_errors=True)
    with open(app / "Info.plist", "rb") as f:
        info = plistlib.load(f)
    info["CFBundleIdentifier"] = arguments.bundle_id
    info["CFBundleShortVersionString"] = arguments.version
    info["CFBundleVersion"] = arguments.build
    with open(app / "Info.plist", "wb") as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_BINARY)

    # signed (ad hoc) with what the app is to be entitled to, which the
    # export's signing gets a profile for
    with open(ENTITLEMENTS, "rb") as f:
        entitlements = plistlib.load(f)
    entitlements.update({
        "application-identifier": f"{team}.{arguments.bundle_id}",
        "com.apple.developer.team-identifier": team,
        "get-task-allow": False,
    })
    with tempfile.NamedTemporaryFile(suffix=".plist", delete=False) as f:
        plistlib.dump(entitlements, f)
    try:
        run("codesign", "--force", "--sign", "-", "--entitlements", f.name, str(app))
    finally:
        os.unlink(f.name)

    with open(archive / "Info.plist", "wb") as f:
        plistlib.dump({
            "ApplicationProperties": {
                "ApplicationPath": "Applications/Halo.app",
                "Architectures": ["arm64"],
                "CFBundleIdentifier": arguments.bundle_id,
                "CFBundleShortVersionString": arguments.version,
                "CFBundleVersion": arguments.build,
                "Team": team,
            },
            "ArchiveVersion": 2,
            "CreationDate": datetime.datetime.now(datetime.timezone.utc),
            "Name": "Halo",
            "SchemeName": "Halo",
        }, f)

    options = OUTPUT / "ExportOptions.plist"
    with open(options, "wb") as f:
        plistlib.dump({
            "method": "app-store-connect",
            "destination": "upload" if arguments.upload else "export",
            "teamID": team,
            "signingStyle": "automatic",
            "uploadSymbols": False,
            "manageAppVersionAndBuildNumber": False,
        }, f)
    run("xcodebuild", "-exportArchive", "-archivePath", str(archive), "-exportPath", str(OUTPUT),
        "-exportOptionsPlist", str(options), "-allowProvisioningUpdates")
    print(f"ios_testflight.py: {arguments.bundle_id} {arguments.version} ({arguments.build}) "
          + ("uploaded to App Store Connect" if arguments.upload else f"exported to {OUTPUT}"))


if __name__ == "__main__":
    main()
