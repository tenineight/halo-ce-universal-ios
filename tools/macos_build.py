"""Ninja rules for the macOS build (``ninja macos``).

macOS on Apple Silicon kills any process that maps memory below 4 GB, so
the guest/host split of the Android port (an ILP32 image in the low 4 GB)
cannot work there. The macOS port runs the game as WebAssembly instead,
which has 32-bit pointers of its own (port/wasm/README.md), translated
ahead of time into native code:

- the guest, build/macos/halo.wasm: the game sources, the platform layer
  shared with the Linux port (port/linux/src) and the guest runtime
  (port/wasm/guest), compiled by wasi-sdk's clang for wasm32-wasip1-threads
  with wasi-libc as the C library;
- the guest as C, build/macos/wasm2c: wasm2c's translation of halo.wasm,
  whose plain memory accesses tools/wasm2c_patch.py frees from the atomics
  wasm2c gives a shared memory (the guest's atomics stay atomic);
- the host, build/macos/Halo.app: that C, wasm2c's runtime, and the host
  (port/macos/host) that runs it over SDL3 and OpenGL ES (ANGLE, on Metal),
  with the posix_*.c of the platform layer, as one native arm64
  executable in an app bundle.

configure.py downloads wasi-sdk, SDL3, the Khronos OpenGL ES headers and
ANGLE (from an Electron release) to build/macos/third_party; wasm2c is
wabt's (``brew install wabt``).
"""

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, List, Optional

from . import apple_env
from .android_build import GUEST_CODE_FLAGS, VARIADIC_PROTOTYPE_FILES, SDL_TAG, SDL_URL
from .embed_assets import hud_assets_build, hud_configure_inputs
from .linux_build import (MINIUPNPC_DEFINES, MINIUPNPC_DIR, MUSL_MATH_DIR, XDK_INCLUDE, compile_launcher,
                          game_defines_and_includes, game_sources, miniupnpc_sources, musl_math_sources,
                          xdk_headers)
from .ninja_syntax import Writer

PORT_DIR = Path("port/macos")
WASM_DIR = Path("port/wasm")
LINUX_DIR = Path("port/linux")
ANDROID_DIR = Path("port/android")
BUILD = Path("build/macos")
THIRD_PARTY = BUILD / "third_party"
TOML_DIR = Path("port/third_party/tomlc17")
EXPAT_DIR = Path("port/third_party/expat")
EXPAT_SOURCES = ("xmlparse.c", "xmlrole.c", "xmltok.c")
KCP_DIR = Path("port/third_party/kcp")

WASI_SDK_VERSION = "34"
WASI_SDK_DIR = THIRD_PARTY / f"wasi-sdk-{WASI_SDK_VERSION}.0-arm64-macos"
WASI_SDK_URL = (f"https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-{WASI_SDK_VERSION}/"
                f"wasi-sdk-{WASI_SDK_VERSION}.0-arm64-macos.tar.gz")
SDL_DIR = THIRD_PARTY / "SDL3"
KHRONOS_DIR = THIRD_PARTY / "khronos"
KHRONOS_HEADERS = {
    "GLES3/gl32.h": "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api/GLES3/gl32.h",
    "GLES3/gl3platform.h": "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api/GLES3/gl3platform.h",
    "GLES2/gl2ext.h": "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api/GLES2/gl2ext.h",
    "GLES2/gl2platform.h": "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/api/GLES2/gl2platform.h",
    "KHR/khrplatform.h": "https://raw.githubusercontent.com/KhronosGroup/EGL-Registry/main/api/KHR/khrplatform.h",
}
# ANGLE (OpenGL ES on Metal), as Electron ships it
ANGLE_DIR = THIRD_PARTY / "angle"
ELECTRON_VERSION = "v38.2.0"
ELECTRON_URL = (f"https://github.com/electron/electron/releases/download/{ELECTRON_VERSION}/"
                f"electron-{ELECTRON_VERSION}-darwin-arm64.zip")
ELECTRON_LIBRARIES = "Electron.app/Contents/Frameworks/Electron Framework.framework/Versions/A/Libraries"

MACOS_MINIMUM = os.environ.get("HALO_MACOS_MINIMUM", "13.0")
# the app's identity and signature (from the environment or .env: tools/apple_env.py);
# "-" signs ad hoc, which is enough to run on this Mac
MACOS_BUNDLE_ID = os.environ.get("HALO_MACOS_BUNDLE_ID", "com.halo.decomp")
MACOS_SIGN_IDENTITY = os.environ.get("HALO_MACOS_SIGN_IDENTITY", "-")

# The linear memory: the guest's data, stack and heap below the Xbox window
# at 0x80000000 (wasi-libc's malloc takes the initial memory up to its end),
# which xbox_memory.c grows the memory over at start-up
INITIAL_MEMORY = 0x80000000
MAXIMUM_MEMORY = 0x100000000
MAIN_STACK_SIZE = 16 * 1024 * 1024
# where the host maps the linear memory (and its guard pages): a constant,
# so that wasm2c's code addresses guest memory as a native build would
# (tools/wasm2c_patch.py, port/macos/host/host_memory.c)
# (1 TB: macOS reserves 0x1000000000-0x7000000000 for its shared region)
LINEAR_MEMORY_BASE = 0x10000000000

# The guest ABI: wasm32 is ILP32 and aligns 64-bit types to 8 bytes as MSVC
# does; the platform layer takes the Linux paths, with the OpenGL ES
# renderer of the Android port
GUEST_ABI_FLAGS = [
    "-pthread",
    "-D__linux__=1",
    "-D__unix__=1",
    "-DHALO_WASM=1",
    "-DHALO_GLES=1",
    "-fshort-wchar",
    "-fno-common",
    # the same floating point results on every port
    "-ffp-contract=off",
    "-O2",
    "-g0",
]

GAME_FLAGS = [
    "-std=gnu89", "-D__STRICT_ANSI__", "-w",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=incompatible-function-pointer-types",
    "-Wno-error=int-conversion",
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=return-type",
]

# the platform layer's units the guest leaves out: the host's (posix_*.c),
# the write tracking the host does (guest_memory_watch.c), and the
# desktop self-updater, which the macOS build does not have yet
GUEST_EXCLUDED = {"memory_watch.c", "updater.c"}

# the game units that call a function through a type other than its own,
# and the section of port/wasm/include/halo_wasm_prototypes.h that fixes
# each (a WebAssembly call through the wrong type traps)
SOURCE_FIXUP_DEFINES = {
    "source/cache/cache_files_windows.c": "HALO_WASM_VOID_THREAD_PROCEDURE",
    "source/rasterizer/xbox/rasterizer_xbox_motion_sensor.c": "HALO_WASM_VOID_RESULT_FIXUPS",
    "source/rasterizer/xbox/rasterizer_xbox_plasma_energy.c": "HALO_WASM_VOID_RESULT_FIXUPS",
    "source/rasterizer/xbox/rasterizer_xbox_text.c": "HALO_WASM_VOID_RESULT_FIXUPS",
}


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def _download(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    print(f"Downloading {url}")
    subprocess.run(["curl", "-sSfL", "-o", str(destination), url], check=True)


def fetch_third_party() -> None:
    """Download wasi-sdk, SDL3, the GLES headers and ANGLE (configure time, once)."""
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    if not WASI_SDK_DIR.is_dir():
        archive = THIRD_PARTY / "wasi-sdk.tar.gz"
        _download(WASI_SDK_URL, archive)
        subprocess.run(["tar", "xzf", archive.name], cwd=THIRD_PARTY, check=True)
        archive.unlink()
    if not SDL_DIR.is_dir():
        print(f"Cloning SDL3 {SDL_TAG}")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL_DIR)], check=True)
    for name, url in KHRONOS_HEADERS.items():
        if not (KHRONOS_DIR / name).is_file():
            _download(url, KHRONOS_DIR / name)
    if not (ANGLE_DIR / "libGLESv2.dylib").is_file():
        archive = THIRD_PARTY / "electron.zip"
        _download(ELECTRON_URL, archive)
        ANGLE_DIR.mkdir(parents=True, exist_ok=True)
        subprocess.run(["unzip", "-o", "-j", "-q", str(archive), f"{ELECTRON_LIBRARIES}/libEGL.dylib",
                        f"{ELECTRON_LIBRARIES}/libGLESv2.dylib", "LICENSES.chromium.html", "-d", str(ANGLE_DIR)],
                       check=True)
        archive.unlink()


def _wabt() -> Optional[Path]:
    """wabt's installation (the prefix of its wasm2c), or None"""
    wasm2c = shutil.which("wasm2c")
    if not wasm2c:
        return None
    prefix = Path(wasm2c).resolve().parent.parent
    if not (prefix / "include" / "wasm-rt.h").is_file():
        return None
    return prefix


def macos_configure_inputs() -> List[Path]:
    return [Path(__file__), WASM_DIR / "guest", PORT_DIR / "host", LINUX_DIR / "src", *hud_configure_inputs(),
            *([Path(os.path.relpath(apple_env.ENV_FILE))] if apple_env.ENV_FILE.is_file() else [])]


def generate_macos_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if sys.platform != "darwin" or platform.machine() != "arm64" or not (PORT_DIR / "host").is_dir():
        return
    wabt = _wabt()
    if not wabt:
        n.comment("macOS build: wasm2c not found (brew install wabt)")
        return
    try:
        fetch_third_party()
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"macOS build disabled: cannot fetch its third-party code ({error})", file=sys.stderr)
        return
    import json
    config = json.loads(config_path.read_text(encoding="utf-8"))
    release = getattr(sln, "port_release", False)

    guest_dir = BUILD / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    host_dir = BUILD / "host"
    host_obj_dir = host_dir / "obj"
    host_gen_dir = host_dir / "gen"
    wasm2c_dir = BUILD / "wasm2c"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    wasm_prefix_header = WASM_DIR / "include" / "halo_wasm_prefix.h"
    module = BUILD / "halo.wasm"
    sdl_build = BUILD / "sdl3-build"
    libsdl = sdl_build / "libSDL3.0.dylib"
    app = BUILD / "Halo.app"
    executable = app / "Contents" / "MacOS" / "halo"
    python = "$python"

    n.comment("macOS build (ninja macos); see port/macos/README.md")
    n.variable("macos_guest_cc", str(WASI_SDK_DIR / "bin" / "clang"))
    n.variable("macos_host_cc", "clang")

    # ---------- generated sources

    guest_gl_c = gen_dir / "guest_gl.c"
    host_gl_c = host_gen_dir / "host_gl_bridge.c"
    host_gl_h = host_gen_dir / "host_gl_functions.h"
    n.rule(
        name="macos_gl_stubs",
        command=(f"{python} tools/wasm_gl_stubs.py {LINUX_DIR}/src/gl.h {KHRONOS_DIR}/GLES3/gl32.h "
                 f"{KHRONOS_DIR}/GLES2/gl2ext.h {guest_gl_c} {host_gl_c} {host_gl_h}"),
        description="MACOS GL STUBS",
    )
    n.build(outputs=[guest_gl_c, host_gl_c, host_gl_h], rule="macos_gl_stubs",
            implicit=[Path("tools/wasm_gl_stubs.py"), LINUX_DIR / "src" / "gl.h"])

    guest_posix_c = gen_dir / "guest_posix.c"
    host_posix_c = host_gen_dir / "host_posix_bridge.c"
    n.rule(
        name="macos_posix_stubs",
        command=f"{python} tools/wasm_posix_stubs.py {LINUX_DIR}/src/posix.h {guest_posix_c} {host_posix_c}",
        description="MACOS POSIX STUBS",
    )
    n.build(outputs=[guest_posix_c, host_posix_c], rule="macos_posix_stubs",
            implicit=[Path("tools/wasm_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"])

    # ---------- the guest: C -> wasm32

    n.rule(
        name="macos_guest_cc",
        command=f"{compile_launcher(sln)}$macos_guest_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="MACOS WASM CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    guest_abi = " ".join([
        f"--target=wasm32-wasip1-threads --sysroot={WASI_SDK_DIR}/share/wasi-sysroot",
        *GUEST_ABI_FLAGS,
        *(["-DHALO_RELEASE"] if release else []),
    ])
    # (the Android flags, but tentative definitions cannot be common symbols in wasm)
    guest_code = " ".join(flag for flag in GUEST_CODE_FLAGS if flag != "-fcommon")
    prefixes = f"-include {wasm_prefix_header} -include {prefix_header}"
    # (every call must see its callee's real type: a mismatch traps in wasm)
    prototypes_header = WASM_DIR / "include" / "halo_wasm_prototypes.h"
    tool_implicit = [*xdk_headers(), semantics_header, platform_semantics_header, wasm_prefix_header,
                     prototypes_header]

    def guest_object(source: Path, cflags: str) -> Path:
        name = Path(str(source).replace(" ", "_"))
        obj = obj_dir / (name.relative_to(BUILD) if str(name).startswith(str(BUILD)) else name).with_suffix(".o")
        n.build(outputs=obj, rule="macos_guest_cc", inputs=source, implicit=tool_implicit,
                variables={"cflags": cflags})
        return obj

    objects: List[Path] = []
    game_cflags = " ".join([
        guest_abi, guest_code, " ".join(GAME_FLAGS), prefixes, f"-include {semantics_header}",
        f"-include {prototypes_header}", f"-I{LINUX_DIR}/include", game_defines_and_includes(config),
        f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        cflags = game_cflags
        # (wasm passes a variadic function's arguments in a buffer, so such
        # a call must see the real prototype, as on Android)
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {ANDROID_DIR}/include/halo_android_variadic_prototypes.h"
        if source.as_posix() in SOURCE_FIXUP_DEFINES:
            cflags += f" -D{SOURCE_FIXUP_DEFINES[source.as_posix()]}"
        if source.as_posix().startswith("source/bitmaps/libtiff/"):
            cflags += " -DHALO_WASM_LIBTIFF"
        objects.append(guest_object(source, cflags))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags))

    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w", prefixes,
        f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{WASM_DIR}/guest", f"-I{WASM_DIR}/include",
        f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", f"-I{KHRONOS_DIR}", f"-idirafter {XDK_INCLUDE}",
    ])
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in GUEST_EXCLUDED:
            continue
        objects.append(guest_object(source, platform_cflags))
    for source in hud_assets_build(n, "macos", gen_dir / "hud_hires_assets.c"):
        objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    for name in EXPAT_SOURCES:
        objects.append(guest_object(EXPAT_DIR / name, platform_cflags))
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    musl_math_cflags = " ".join([
        guest_abi, "-std=gnu11", "-w", f"-I{MUSL_MATH_DIR}/include", f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    for source in musl_math_sources():
        objects.append(guest_object(source, musl_math_cflags))
    for source in sorted((WASM_DIR / "guest").glob("*.c")):
        objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(guest_gl_c, platform_cflags))
    objects.append(guest_object(guest_posix_c, platform_cflags))

    n.rule(
        name="macos_guest_link",
        command=(f"$macos_guest_cc --target=wasm32-wasip1-threads --sysroot={WASI_SDK_DIR}/share/wasi-sysroot "
                 "-pthread -Wl,--import-memory -Wl,--shared-memory -Wl,--stack-first "
                 f"-Wl,--initial-memory={INITIAL_MEMORY} -Wl,--max-memory={MAXIMUM_MEMORY} "
                 f"-Wl,-z,stack-size={MAIN_STACK_SIZE} -Wl,--fatal-warnings -o $out @$out.rsp"),
        description="MACOS WASM LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=module, rule="macos_guest_link", inputs=objects)
    n.build(outputs="macos_wasm", rule="phony", inputs=module)

    # ---------- the guest as C

    wasm2c_outputs = [wasm2c_dir / f"game_{index}.c" for index in range(WASM2C_OUTPUTS)]
    wasm2c_header = wasm2c_dir / "game.h"
    wasm2c_impl_header = wasm2c_dir / "game-impl.h"
    # wasm2c cannot translate the atomic waits: they become host calls first
    module_for_wasm2c = BUILD / "halo.wasm2c.wasm"
    n.rule(
        name="macos_atomic_imports",
        command=f"{python} tools/wasm_atomic_imports.py $in $out > /dev/null",
        description="MACOS ATOMIC WAITS $out",
    )
    n.build(outputs=module_for_wasm2c, rule="macos_atomic_imports", inputs=module,
            implicit=[Path("tools/wasm_atomic_imports.py")])
    n.rule(
        name="macos_wasm2c",
        command=(f"rm -rf {wasm2c_dir} && mkdir -p {wasm2c_dir} && "
                 f"wasm2c --enable-threads --module-name=game --num-outputs={WASM2C_OUTPUTS} "
                 f"-o {wasm2c_dir}/game.c $in && {python} tools/wasm2c_patch.py {wasm2c_dir}"),
        description="MACOS WASM2C $in",
        restat=True,
    )
    n.build(outputs=[*wasm2c_outputs, wasm2c_header, wasm2c_impl_header], rule="macos_wasm2c", inputs=module_for_wasm2c,
            implicit=[Path("tools/wasm2c_patch.py")])

    # ---------- SDL3

    n.rule(
        name="macos_sdl3",
        command=(f"cmake -S {SDL_DIR} -B {sdl_build} -G Ninja -DCMAKE_BUILD_TYPE=Release "
                 f"-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_MINIMUM} "
                 "-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "
                 f"> {BUILD}/sdl3-configure.log && ninja -C {sdl_build} > {BUILD}/sdl3-build.log"),
        description="MACOS SDL3",
        pool="console",
    )
    n.build(outputs=libsdl, rule="macos_sdl3", implicit=[SDL_DIR / "CMakeLists.txt"])

    # ---------- the host

    n.rule(
        name="macos_host_cc",
        command="$macos_host_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="MACOS CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    common_host = [
        "-arch arm64", f"-mmacosx-version-min={MACOS_MINIMUM}", "-O2", "-g",
        f"-I{wabt}/include", f"-I{wasm2c_dir}", f"-DHALO_LINEAR_MEMORY_BASE={LINEAR_MEMORY_BASE:#x}ULL",
    ]
    host_cflags = " ".join([
        *common_host, "-Wall", "-Wno-unused-function", "-D_DARWIN_C_SOURCE",
        f"-I{PORT_DIR}/host", f"-I{host_gen_dir}", f"-I{SDL_DIR}/include", f"-I{KHRONOS_DIR}",
        f"-I{LINUX_DIR}/src", f"-I{TOML_DIR}",
        *(["-DHALO_RELEASE"] if release else []),
    ])
    # wasm2c's output: the guest's code. Its memory accesses are checked by
    # the guard pages around the linear memory (wasm-rt.h's default here)
    wasm2c_cflags = " ".join([*common_host, "-w", "-fno-strict-aliasing"])
    host_objects: List[Path] = []
    host_implicit = [wasm2c_header, host_gl_h]

    def host_object(source: Path, cflags: str, prefix: str = "") -> Path:
        obj = host_obj_dir / (prefix + source.name + ".o")
        n.build(outputs=obj, rule="macos_host_cc", inputs=source, implicit=host_implicit,
                variables={"cflags": cflags})
        host_objects.append(obj)
        return obj

    for source in wasm2c_outputs:
        host_object(source, wasm2c_cflags)
    runtime_dir = wabt / "share" / "wabt" / "wasm2c"
    for name in ("wasm-rt-impl.c", "wasm-rt-mem-impl.c"):
        host_object(runtime_dir / name, wasm2c_cflags)
    for source in sorted((PORT_DIR / "host").glob("*.c")):
        host_object(source, host_cflags)
    for source in sorted((PORT_DIR / "host").glob("*.m")):
        host_object(source, host_cflags + " -fobjc-arc")
    host_object(host_gl_c, host_cflags)
    host_object(host_posix_c, host_cflags)
    for name in ("posix_files.c", "posix_net.c"):
        host_object(LINUX_DIR / "src" / name, host_cflags)
    miniupnpc_cflags = " ".join([host_cflags, f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}",
                                 *MINIUPNPC_DEFINES])
    host_object(LINUX_DIR / "src" / "posix_upnp.c", miniupnpc_cflags)
    for source in miniupnpc_sources():
        host_object(source, miniupnpc_cflags + " -w", "miniupnpc_")

    n.rule(
        name="macos_host_link",
        command=(f"$macos_host_cc -arch arm64 -mmacosx-version-min={MACOS_MINIMUM} -o $out @$out.rsp "
                 f"-L{sdl_build} -lSDL3 -framework AppKit -Wl,-rpath,@executable_path/../Frameworks"),
        description="MACOS LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=executable, rule="macos_host_link", inputs=host_objects, implicit=[libsdl])

    # ---------- the app bundle

    frameworks = app / "Contents" / "Frameworks"
    bundled = [
        (libsdl, frameworks / "libSDL3.0.dylib"),
        (ANGLE_DIR / "libEGL.dylib", frameworks / "libEGL.dylib"),
        (ANGLE_DIR / "libGLESv2.dylib", frameworks / "libGLESv2.dylib"),
    ]
    n.rule(name="macos_copy", command="cp $in $out", description="MACOS STAGE $out")
    for source, destination in bundled:
        n.build(outputs=destination, rule="macos_copy", inputs=source)
    info_plist = app / "Contents" / "Info.plist"
    n.rule(
        name="macos_info_plist",
        command=(f"cp $in $out && plutil -replace CFBundleIdentifier -string {MACOS_BUNDLE_ID} $out "
                 f"&& plutil -replace LSMinimumSystemVersion -string {MACOS_MINIMUM} $out"),
        description="MACOS STAGE $out",
    )
    n.build(outputs=info_plist, rule="macos_info_plist", inputs=PORT_DIR / "Info.plist")
    bundled.append((PORT_DIR / "Info.plist", info_plist))
    signature = BUILD / "Halo.app.signed"
    n.rule(
        name="macos_sign",
        # Apple Silicon runs only signed code: ad hoc unless HALO_MACOS_SIGN_IDENTITY names one
        command=f"codesign --force --deep --sign '{MACOS_SIGN_IDENTITY}' {app} > /dev/null && touch $out",
        description="MACOS SIGN Halo.app",
    )
    n.build(outputs=signature, rule="macos_sign", inputs=[executable, *(d for _, d in bundled)])
    n.build(outputs="macos", rule="phony", inputs=signature)
    n.newline()


# wasm2c's output files: the guest's code is compiled in this many parallel units
WASM2C_OUTPUTS = 32
