#!/usr/bin/env python3
"""Signs the iOS build's Halo.app for a device (tools/ios_build.py).

Picks a provisioning profile that covers the bundle identifier (HALO_IOS_PROFILE,
or the installed ones Xcode keeps: one for the identifier itself before a
wildcard), the signing identity in the keychain whose certificate the
profile lists, and the entitlements: what the profile grants, for this
identifier, with those of port/ios/Halo.entitlements the profile allows
(extended virtual addressing needs a profile made for it).

Usage: ios_sign.py <Halo.app> <bundle identifier> <wanted entitlements plist>
"""

import datetime
import hashlib
import os
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import apple_env  # noqa: F401  (the .env file's settings)

PROFILE_FOLDERS = [
    Path.home() / "Library/Developer/Xcode/UserData/Provisioning Profiles",
    Path.home() / "Library/MobileDevice/Provisioning Profiles",
]


def decode(path: Path) -> Dict[str, Any]:
    data = subprocess.run(["security", "cms", "-D", "-i", str(path)], check=True, capture_output=True).stdout
    return plistlib.loads(data)


def covers(application_identifier: str, team: str, bundle_identifier: str) -> int:
    """2 for the identifier itself, 1 for a wildcard that covers it, 0 otherwise"""
    if application_identifier == f"{team}.{bundle_identifier}":
        return 2
    if application_identifier.endswith("*") and f"{team}.{bundle_identifier}".startswith(application_identifier[:-1]):
        return 1
    return 0


def find_profile(bundle_identifier: str) -> Tuple[Path, Dict[str, Any]]:
    if os.environ.get("HALO_IOS_PROFILE"):
        path = Path(os.environ["HALO_IOS_PROFILE"])
        return path, decode(path)
    now = datetime.datetime.now(datetime.timezone.utc)
    best: Optional[Tuple[int, datetime.datetime, Path, Dict[str, Any]]] = None
    for folder in PROFILE_FOLDERS:
        for path in sorted(folder.glob("*.mobileprovision")) if folder.is_dir() else []:
            try:
                profile = decode(path)
            except subprocess.CalledProcessError:
                continue
            expiry = profile["ExpirationDate"].replace(tzinfo=datetime.timezone.utc)
            team = profile["TeamIdentifier"][0]
            score = covers(profile["Entitlements"].get("application-identifier", ""), team, bundle_identifier)
            if expiry <= now or not score or "iOS" not in profile.get("Platform", ["iOS"]):
                continue
            if not best or (score, expiry) > (best[0], best[1]):
                best = (score, expiry, path, profile)
    if not best:
        sys.exit(f"ios_sign.py: no provisioning profile covers {bundle_identifier} (set HALO_IOS_PROFILE)")
    return best[2], best[3]


def find_identity(profile: Dict[str, Any]) -> str:
    wanted = {hashlib.sha1(certificate).hexdigest().upper() for certificate in profile["DeveloperCertificates"]}
    output = subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"], check=True,
                            capture_output=True, text=True).stdout
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in wanted:
            return fields[1]
    sys.exit(f"ios_sign.py: no signing identity in the keychain for profile {profile['Name']}")


def main(arguments: List[str]) -> None:
    app, bundle_identifier, wanted_path = Path(arguments[0]), arguments[1], Path(arguments[2])
    path, profile = find_profile(bundle_identifier)
    team = profile["TeamIdentifier"][0]
    granted = profile["Entitlements"]
    entitlements: Dict[str, Any] = {
        "application-identifier": f"{team}.{bundle_identifier}",
        "com.apple.developer.team-identifier": team,
        "get-task-allow": granted.get("get-task-allow", False),
    }
    with open(wanted_path, "rb") as f:
        wanted = plistlib.load(f)
    for key, value in wanted.items():
        if key in granted:
            entitlements[key] = value
        else:
            print(f"ios_sign.py: {profile['Name']} does not grant {key}; signing without it")
    identity = find_identity(profile)
    print(f"ios_sign.py: {profile['Name']} ({path.name}), identity {identity}")
    (app / "embedded.mobileprovision").write_bytes(path.read_bytes())
    with tempfile.NamedTemporaryFile(suffix=".plist", delete=False) as f:
        plistlib.dump(entitlements, f)
    try:
        subprocess.run(["codesign", "--force", "--sign", identity, "--entitlements", f.name, str(app)], check=True)
    finally:
        os.unlink(f.name)


if __name__ == "__main__":
    main(sys.argv[1:])
