"""Ninja rules for the iOS build (``ninja ios``).

The iOS port is the macOS port (tools/macos_build.py) on UIKit: the same
guest, build/macos/halo.wasm, and its translation to C, build/macos/wasm2c,
compiled with the same host (port/macos/host, which takes the iOS paths
where HALO_IOS is defined) for iOS: ``ninja ios`` for devices
(build/ios/iphoneos/Halo.app), ``ninja ios_simulator`` for the simulator
(build/ios/iphonesimulator/Halo.app). iOS has no JIT to forbid here:
wasm2c's code is compiled ahead of time like any other.

OpenGL ES is the system's (OpenGLES.framework, ES 3.0, through SDL's UIKit
video), not ANGLE: the renderer makes do with ES 3.0 and decodes the
textures the GPU cannot sample (xbox_textures.c). SDL3 is linked
statically. The game's folder is the application's Documents folder, which
the Files app shows (copy the maps folder there).

The simulator's build is signed ad hoc; a device's by tools/ios_sign.py,
with an installed provisioning profile that covers the bundle identifier
(HALO_IOS_BUNDLE_ID, com.halo.decomp by default) or HALO_IOS_PROFILE.
These settings come from the environment or the repository's .env file
(tools/apple_env.py, .env.example).
"""

import os
import subprocess
import sys
from pathlib import Path
from typing import Any, List

from . import apple_env, macos_build
from .linux_build import MINIUPNPC_DEFINES, MINIUPNPC_DIR, miniupnpc_sources
from .ninja_syntax import Writer

PORT_DIR = Path("port/ios")
MACOS_HOST_DIR = Path("port/macos/host")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/ios")

IOS_MINIMUM = os.environ.get("HALO_IOS_MINIMUM", "16.0")
BUNDLE_ID = os.environ.get("HALO_IOS_BUNDLE_ID", "com.halo.decomp")
# the SDKs, and the ninja target of each
SDKS = {"iphoneos": "ios", "iphonesimulator": "ios_simulator"}

# SDL3's static library's dependencies
FRAMEWORKS = [
    "AVFoundation", "AudioToolbox", "CoreAudio", "CoreBluetooth", "CoreFoundation", "CoreGraphics", "CoreHaptics",
    "CoreMedia", "CoreMotion", "CoreVideo", "Foundation", "GameController", "Metal", "OpenGLES", "QuartzCore", "UIKit",
    "UniformTypeIdentifiers",
]


def ios_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR,
            *([Path(os.path.relpath(apple_env.ENV_FILE))] if apple_env.ENV_FILE.is_file() else [])]


def _sdk_path(sdk: str) -> str:
    return subprocess.run(["xcrun", "--sdk", sdk, "--show-sdk-path"], check=True, capture_output=True,
                          text=True).stdout.strip()


def generate_ios_build(n: Writer, sln: Any) -> None:
    if sys.platform != "darwin" or not PORT_DIR.is_dir():
        return
    wabt = macos_build._wabt()
    if not wabt or not macos_build.SDL_DIR.is_dir():
        n.comment("iOS build: needs the macOS build's tools (wasm2c, build/macos/third_party)")
        return
    n.comment("iOS build (ninja ios, ninja ios_simulator); see tools/ios_build.py")
    n.rule(
        name="ios_sdl3",
        command=(f"cmake -S {macos_build.SDL_DIR} -B $sdl_build -G Ninja -DCMAKE_BUILD_TYPE=Release "
                 "-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$sdk -DCMAKE_OSX_ARCHITECTURES=arm64 "
                 f"-DCMAKE_OSX_DEPLOYMENT_TARGET={IOS_MINIMUM} "
                 "-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "
                 "> $sdl_build-configure.log && ninja -C $sdl_build > $sdl_build-build.log"),
        description="IOS SDL3 ($sdk)",
        pool="console",
    )
    n.rule(
        name="ios_cc",
        command="clang -MMD -MF $out.d $cflags -c $in -o $out",
        description="IOS CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="ios_link",
        command="clang $ldflags -o $out @$out.rsp",
        description="IOS LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name="ios_bundle",
        command=f"$python tools/ios_bundle.py $app $sdk {IOS_MINIMUM} {BUNDLE_ID}",
        description="IOS BUNDLE $app",
    )
    # (the simulator runs on the Mac's kernel: no entitlements to ask for)
    n.rule(name="ios_sign_simulator", command="codesign --force --sign - $app > /dev/null && touch $out",
           description="IOS SIGN $app")
    n.rule(name="ios_sign_device",
           command=f"$python tools/ios_sign.py $app {BUNDLE_ID} {PORT_DIR / 'Halo.entitlements'} && touch $out",
           description="IOS SIGN $app")
    for sdk, target in SDKS.items():
        try:
            sysroot = _sdk_path(sdk)
        except (subprocess.CalledProcessError, OSError):
            n.comment(f"iOS build: no {sdk} SDK (Xcode)")
            continue
        _generate_sdk(n, sln, wabt, sdk, sysroot, target)
    n.newline()


def _generate_sdk(n: Writer, sln: Any, wabt: Path, sdk: str, sysroot: str, phony: str) -> None:
    simulator = sdk == "iphonesimulator"
    target = f"arm64-apple-ios{IOS_MINIMUM}" + ("-simulator" if simulator else "")
    release = getattr(sln, "port_release", False)

    wasm2c_dir = macos_build.BUILD / "wasm2c"
    wasm2c_outputs = [wasm2c_dir / f"game_{index}.c" for index in range(macos_build.WASM2C_OUTPUTS)]
    wasm2c_header = wasm2c_dir / "game.h"
    macos_gen_dir = macos_build.BUILD / "host" / "gen"
    host_gl_c = macos_gen_dir / "host_gl_bridge.c"
    host_gl_h = macos_gen_dir / "host_gl_functions.h"
    host_posix_c = macos_gen_dir / "host_posix_bridge.c"
    build = BUILD / sdk
    sdl_build = build / "sdl3"
    libsdl = sdl_build / "libSDL3.a"
    obj_dir = build / "obj"
    app = build / "Halo.app"
    executable = app / "halo"

    # ---------- SDL3, static

    n.build(outputs=libsdl, rule="ios_sdl3", implicit=[macos_build.SDL_DIR / "CMakeLists.txt"],
            variables={"sdl_build": str(sdl_build), "sdk": sdk})

    # ---------- the host

    common = [
        f"-target {target}", f"-isysroot {sysroot}", "-O2", "-g",
        f"-I{wabt}/include", f"-I{wasm2c_dir}",
        # (a variable: port/ios/halo_ios_memory.h)
        "-DHALO_LINEAR_MEMORY_BASE=halo_linear_memory_base", f"-include {PORT_DIR}/halo_ios_memory.h",
    ]
    host_cflags = " ".join([
        *common, "-Wall", "-Wno-unused-function", "-D_DARWIN_C_SOURCE", "-DHALO_IOS=1", "-DHALO_IOS_MEMORY_WRITER",
        f"-I{MACOS_HOST_DIR}", f"-I{PORT_DIR}", f"-I{macos_gen_dir}", f"-I{macos_build.SDL_DIR}/include",
        f"-I{macos_build.KHRONOS_DIR}", f"-I{LINUX_DIR}/src", f"-I{macos_build.TOML_DIR}",
        *(["-DHALO_RELEASE"] if release else []),
    ])
    wasm2c_cflags = " ".join([*common, "-w", "-fno-strict-aliasing"])
    objects: List[Path] = []
    implicit = [wasm2c_header, host_gl_h, libsdl]

    def host_object(source: Path, cflags: str, prefix: str = "") -> None:
        obj = obj_dir / (prefix + source.name + ".o")
        n.build(outputs=obj, rule="ios_cc", inputs=source, implicit=implicit, variables={"cflags": cflags})
        objects.append(obj)

    for source in wasm2c_outputs:
        host_object(source, wasm2c_cflags)
    runtime_dir = wabt / "share" / "wabt" / "wasm2c"
    for name in ("wasm-rt-impl.c", "wasm-rt-mem-impl.c"):
        host_object(runtime_dir / name, wasm2c_cflags)
    for source in sorted(MACOS_HOST_DIR.glob("*.c")):
        host_object(source, host_cflags)
    # (host_menu.m is the macOS menu bar's)
    for source in sorted(PORT_DIR.glob("*.c")):
        host_object(source, host_cflags)
    for source in sorted(PORT_DIR.glob("*.m")):
        host_object(source, host_cflags + " -fobjc-arc")
    # the platform layer's disc image extraction, for importing the maps from
    # the settings (port/ios/host_import.c): with the little of platform.h it
    # takes (port/ios/xiso), in place of the guest's
    host_object(LINUX_DIR / "src" / "xiso.c",
                host_cflags + f" -D__HALO_LINUX_PLATFORM_H -include {PORT_DIR}/xiso/platform.h")
    host_object(host_gl_c, host_cflags)
    host_object(host_posix_c, host_cflags)
    for name in ("posix_files.c", "posix_net.c"):
        host_object(LINUX_DIR / "src" / name, host_cflags)
    miniupnpc_cflags = " ".join([host_cflags, f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}",
                                 *MINIUPNPC_DEFINES])
    host_object(LINUX_DIR / "src" / "posix_upnp.c", miniupnpc_cflags)
    for source in miniupnpc_sources():
        host_object(source, miniupnpc_cflags + " -w", "miniupnpc_")

    frameworks = " ".join(f"-framework {name}" for name in FRAMEWORKS)
    n.build(outputs=executable, rule="ios_link", inputs=objects, implicit=[libsdl],
            variables={"ldflags": f"-target {target} -isysroot {sysroot} {libsdl} {frameworks}"})

    # ---------- the app bundle

    plist = app / "Info.plist"
    n.build(outputs=plist, rule="ios_bundle", inputs=PORT_DIR / "Info.plist",
            implicit=[Path("tools/ios_bundle.py"), PORT_DIR / "PrivacyInfo.xcprivacy",
                      PORT_DIR / "Assets.xcassets" / "AppIcon.appiconset" / "AppIcon.png", executable],
            variables={"app": str(app), "sdk": sdk})
    signature = build / "Halo.app.signed"
    n.build(outputs=signature, rule="ios_sign_simulator" if simulator else "ios_sign_device",
            inputs=[executable, plist], implicit=[PORT_DIR / "Halo.entitlements", Path("tools/ios_sign.py")],
            variables={"app": str(app)})
    n.build(outputs=phony, rule="phony", inputs=signature)
