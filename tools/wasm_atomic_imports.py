#!/usr/bin/env python3
"""Turns the guest's memory.atomic.wait and memory.atomic.notify into host imports.

wabt's wasm2c (1.0.42) cannot translate these instructions, which wasi-libc's
futex (and so every lock and condition variable of the guest's threads)
is made of: it stops with "unimplemented". This rewrites the module before
wasm2c sees it, through its text form (with generated names, so adding
imports renumbers nothing): each instruction becomes its memory offset
pushed as one more argument and a call to an import of module "halo",

    memory.atomic.wait32 offset=N  ->  i32.const N  call $host_atomic_wait32
    memory.atomic.wait64 offset=N  ->  i32.const N  call $host_atomic_wait64
    memory.atomic.notify offset=N  ->  i32.const N  call $host_atomic_notify

which the host implements with WebAssembly's semantics
(port/macos/host/host_memory.c).

Usage: wasm_atomic_imports.py input.wasm output.wasm
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

IMPORTS = {
    "wait32": '(import "halo" "host_atomic_wait32" (func $host_atomic_wait32 (param i32 i32 i64 i32) (result i32)))',
    "wait64": '(import "halo" "host_atomic_wait64" (func $host_atomic_wait64 (param i32 i64 i64 i32) (result i32)))',
    "notify": '(import "halo" "host_atomic_notify" (func $host_atomic_notify (param i32 i32 i32) (result i32)))',
}
INSTRUCTION = re.compile(r"memory\.atomic\.(wait32|wait64|notify)((?:\s+(?:offset|align)=\d+)*)")


def replace(match: re.Match) -> str:
    offset = re.search(r"offset=(\d+)", match.group(2))
    return f"i32.const {offset.group(1) if offset else 0}\n call $host_atomic_{match.group(1)}"


def main() -> None:
    source, output = sys.argv[1:3]
    with tempfile.TemporaryDirectory() as folder:
        text_path = Path(folder) / "module.wat"
        subprocess.run(["wasm2wat", "--enable-threads", "--generate-names", source, "-o", str(text_path)], check=True)
        text = text_path.read_text(encoding="utf-8")
        text, count = INSTRUCTION.subn(replace, text)
        # the imports go after the last one (imports come before any definition)
        lines = text.split("\n")
        last_import = max(index for index, line in enumerate(lines) if line.lstrip().startswith("(import "))
        lines[last_import + 1:last_import + 1] = [f"  {line}" for line in IMPORTS.values()]
        text_path.write_text("\n".join(lines), encoding="utf-8")
        subprocess.run(["wat2wasm", "--enable-threads", str(text_path), "-o", output], check=True)
    print(f"wasm_atomic_imports.py: {count} atomic waits and notifies made host calls")


if __name__ == "__main__":
    main()
