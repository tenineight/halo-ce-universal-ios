#!/usr/bin/env python3
"""Builds the iOS app and installs it on an attached device, and starts it.

  1. builds the device app (ninja ios), unless --no-build;
  2. picks the device: the only one attached, or the one chosen from a list
     when there are several (or --device, a name or identifier);
  3. installs build/ios/iphoneos/Halo.app on it (xcrun devicectl);
  4. copies the maps folder to the game's folder on the device (the app's
     Documents/maps), if asked to (--maps, or yes to the question): the
     macOS game's (~/Library/Application Support/Halo/maps) unless
     --maps-folder names another. Only the files that changed are sent;
  5. starts the game, unless --no-launch.

Usage: ios_deploy.py [--device <name or identifier>] [--maps | --no-maps] [--maps-folder <folder>]
                     [--no-build] [--no-launch]
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Dict, List

import apple_env  # noqa: F401  (the .env file's settings)

APP = Path("build/ios/iphoneos/Halo.app")
BUNDLE_ID = os.environ.get("HALO_IOS_BUNDLE_ID", "com.halo.decomp")
ATTEMPTS = 3
MAPS = Path.home() / "Library" / "Application Support" / "Halo" / "maps"


def run(*arguments: str) -> None:
    print("+ " + " ".join(arguments))
    subprocess.run(arguments, check=True)


def run_on_device(*arguments: str) -> None:
    """a devicectl command, tried again when the connection drops (as it
    does now and then over Wi-Fi)"""
    for attempt in range(1, ATTEMPTS + 1):
        try:
            run(*arguments)
            return
        except subprocess.CalledProcessError:
            if attempt == ATTEMPTS:
                raise
            print(f"ios_deploy.py: failed, trying again ({attempt + 1} of {ATTEMPTS})")
            time.sleep(2)


def attached_devices() -> List[Dict[str, Any]]:
    """the iOS devices devicectl can reach now"""
    with tempfile.TemporaryDirectory() as folder:
        output = Path(folder) / "devices.json"
        subprocess.run(["xcrun", "devicectl", "list", "devices", "--quiet", "--json-output", str(output)], check=True,
                       stdout=subprocess.DEVNULL)
        devices = json.loads(output.read_text())["result"]["devices"]
    devices = [device for device in devices
               if device["hardwareProperties"].get("platform") == "iOS"
               and device["connectionProperties"].get("tunnelState") != "unavailable"]
    # (the ones on a cable first: quicker, and steadier than Wi-Fi)
    return sorted(devices, key=lambda device: device["connectionProperties"].get("transportType") != "wired")


def describe(device: Dict[str, Any]) -> str:
    return "{} ({}, {})".format(device["deviceProperties"].get("name", "?"),
                                device["hardwareProperties"].get("marketingName", "?"),
                                "cable" if device["connectionProperties"].get("transportType") == "wired" else "Wi-Fi")


def choose_device(devices: List[Dict[str, Any]], wanted: str) -> Dict[str, Any]:
    if wanted:
        for device in devices:
            if wanted in (device["identifier"], device["deviceProperties"].get("name")):
                return device
        sys.exit(f"ios_deploy.py: no device {wanted!r} attached")
    if not devices:
        sys.exit("ios_deploy.py: no iOS device attached")
    if len(devices) == 1:
        return devices[0]
    if not sys.stdin.isatty():
        sys.exit("ios_deploy.py: several devices attached: name one with --device")
    for number, device in enumerate(devices, 1):
        print(f"  {number}. {describe(device)}")
    while True:
        answer = input(f"Device [1-{len(devices)}]: ").strip()
        if answer.isdigit() and 1 <= int(answer) <= len(devices):
            return devices[int(answer) - 1]


def ask_yes_no(question: str) -> bool:
    if not sys.stdin.isatty():
        return False
    return input(f"{question} [y/N]: ").strip().lower() in ("y", "yes")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default="", help="the device's name or identifier (default: ask if several)")
    maps = parser.add_mutually_exclusive_group()
    maps.add_argument("--maps", dest="maps", action="store_true", default=None, help="copy the maps too")
    maps.add_argument("--no-maps", dest="maps", action="store_false", help="do not copy the maps (or ask)")
    parser.add_argument("--maps-folder", type=Path, default=MAPS, help=f"the maps to copy (default: {MAPS})")
    parser.add_argument("--no-build", action="store_true", help="install the app as last built")
    parser.add_argument("--no-launch", action="store_true", help="do not start the game")
    arguments = parser.parse_args()

    if not arguments.no_build:
        run("ninja", "ios")
    if not APP.is_dir():
        sys.exit(f"ios_deploy.py: {APP} is not built (ninja ios)")

    device = choose_device(attached_devices(), arguments.device)
    identifier = device["identifier"]
    print(f"Device: {describe(device)}")

    copy_maps = arguments.maps
    if copy_maps is None:
        copy_maps = ask_yes_no(f"Copy the maps ({arguments.maps_folder}) too?")
    if copy_maps and not arguments.maps_folder.is_dir():
        sys.exit(f"ios_deploy.py: no maps folder {arguments.maps_folder}")

    run_on_device("xcrun", "devicectl", "device", "install", "app", "--device", identifier, str(APP))
    if copy_maps:
        # (the folder's contents, into Documents/maps)
        run_on_device("xcrun", "devicectl", "device", "copy", "to", "--device", identifier,
            "--domain-type", "appDataContainer", "--domain-identifier", BUNDLE_ID,
            "--source", str(arguments.maps_folder) + "/", "--destination", "Documents/maps")
    if not arguments.no_launch:
        try:
            run("xcrun", "devicectl", "device", "process", "launch", "--terminate-existing", "--device", identifier,
                BUNDLE_ID)
        except subprocess.CalledProcessError:
            sys.exit("ios_deploy.py: installed, but the game did not start: is the device locked?")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
    except KeyboardInterrupt:
        sys.exit(130)
