#!/usr/bin/env python3
"""Fills the iOS build's application bundle around its executable (tools/ios_build.py).

- the app icon, compiled from port/ios/Assets.xcassets by actool (Assets.car,
  and the Info.plist entries that name it);
- Info.plist, from port/ios/Info.plist: the bundle identifier, the platform,
  the minimum iOS, the version and build number (HALO_IOS_VERSION and
  HALO_IOS_BUILD, else 1.0 and 1; tools/ios_testflight.py sets the build
  number of an upload), the icon's entries, and the build's SDK and Xcode,
  which App Store Connect checks an upload for, as Xcode records them;
- the privacy manifest, port/ios/PrivacyInfo.xcprivacy.

Usage: ios_bundle.py <app folder> <sdk> <minimum iOS> <bundle identifier>
"""

import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import apple_env  # noqa: F401  (the .env file's settings)

PORT_DIR = Path("port/ios")


def run(*arguments: str) -> str:
    return subprocess.run(arguments, check=True, capture_output=True, text=True).stdout.strip()


def main() -> None:
    app, sdk, minimum, bundle_id = Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
    simulator = sdk == "iphonesimulator"

    # the icon
    with tempfile.TemporaryDirectory() as temporary:
        partial = Path(temporary) / "assets.plist"
        run("xcrun", "actool", str(PORT_DIR / "Assets.xcassets"), "--compile", str(app), "--platform", sdk,
            "--minimum-deployment-target", minimum, "--app-icon", "AppIcon", "--target-device", "iphone",
            "--target-device", "ipad", "--output-partial-info-plist", str(partial), "--output-format",
            "human-readable-text", "--notices", "--warnings")
        with open(partial, "rb") as f:
            icon_entries = plistlib.load(f)

    text = (PORT_DIR / "Info.plist").read_text(encoding="utf-8")
    for key, value in {
        "@BUNDLE_ID@": bundle_id,
        "@PLATFORM@": "iPhoneSimulator" if simulator else "iPhoneOS",
        "@MINIMUM@": minimum,
        "@VERSION@": os.environ.get("HALO_IOS_VERSION", "1.0"),
        "@BUILD@": os.environ.get("HALO_IOS_BUILD", "1"),
    }.items():
        text = text.replace(key, value)
    info = plistlib.loads(text.encode("utf-8"))
    info.update(icon_entries)

    # what Xcode records of the build
    sdk_version = run("xcrun", "--sdk", sdk, "--show-sdk-version")
    developer = Path(run("xcode-select", "-p"))
    with open(developer.parent / "Info.plist", "rb") as f:
        xcode = plistlib.load(f)
    with open(developer.parent / "version.plist", "rb") as f:
        xcode_build = plistlib.load(f).get("ProductBuildVersion", "")
    info.update({
        "DTPlatformName": sdk,
        "DTPlatformVersion": sdk_version,
        "DTPlatformBuild": run("xcrun", "--sdk", sdk, "--show-sdk-build-version"),
        "DTSDKName": f"{sdk}{sdk_version}",
        "DTSDKBuild": run("xcrun", "--sdk", sdk, "--show-sdk-build-version"),
        "DTXcode": xcode.get("DTXcode", ""),
        "DTXcodeBuild": xcode_build,
        "DTCompiler": "com.apple.compilers.llvm.clang.1_0",
        "BuildMachineOSBuild": run("sw_vers", "-buildVersion"),
    })
    with open(app / "Info.plist", "wb") as f:
        plistlib.dump(info, f, fmt=plistlib.FMT_BINARY)
    (app / "PkgInfo").write_text("APPL????", encoding="ascii")
    shutil.copyfile(PORT_DIR / "PrivacyInfo.xcprivacy", app / "PrivacyInfo.xcprivacy")


if __name__ == "__main__":
    main()
