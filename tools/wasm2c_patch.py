#!/usr/bin/env python3
"""Rewrites wasm2c's plain memory accesses for the macOS build's guest.

The guest's memory is shared between its threads, and wasm2c gives every
plain load and store of a shared memory the treatment of an atomic one: a
relaxed atomic access through a volatile pointer, re-read from the memory's
descriptor each time. That is what the WebAssembly specification requires
of racing accesses, and it keeps the C compiler from optimising the game's
memory traffic at all.

The game was written for, and the other ports compile it as, plain C, where
racing accesses are the program's own business: its threads synchronise
with locks and interlocked operations, which here stay real atomics. This
rewrites DEFINE_SHARED_LOAD and DEFINE_SHARED_STORE, the plain accesses,
into ordinary ones at the fixed address where the host maps the memory
(HALO_LINEAR_MEMORY_BASE, port/macos/host/host.h), as a native build would
address its data.

Usage: wasm2c_patch.py <wasm2c output folder>
"""

import re
import sys
from pathlib import Path

LOAD = r"""#define DEFINE_SHARED_LOAD(name, t1, t2, t3, force_read)                      \
  static inline t3 name##_unchecked(uint8_t* const wasm_rt_local_memory_base, \
                                    wasm_rt_shared_memory_t* mem, u64 addr) { \
    t1 result;                                                                \
    wasm_rt_memcpy(&result, (uint8_t*)HALO_LINEAR_MEMORY_BASE + addr,         \
                   sizeof(t1));                                               \
    t3 ret = (t3)(t2)result;                                                  \
    force_read(ret);                                                          \
    return ret;                                                               \
  }                                                                           \
  DEF_MEM_CHECKS0(name, _shared_, t1, return, t3)
"""

STORE = r"""#define DEFINE_SHARED_STORE(name, t1, t2)                                     \
  static inline void name##_unchecked(                                        \
      uint8_t* const wasm_rt_local_memory_base, wasm_rt_shared_memory_t* mem, \
      u64 addr, t2 value) {                                                   \
    t1 wrapped = (t1)value;                                                   \
    wasm_rt_memcpy((uint8_t*)HALO_LINEAR_MEMORY_BASE + addr, &wrapped,        \
                   sizeof(t1));                                               \
  }                                                                           \
  DEF_MEM_CHECKS1(name, _shared_, t1, , void, t2)
"""

# a macro definition: its first line and every continued line after it
MACRO = r"#define {name}\(.*?\n(?:.*\\\n)*.*\n"


def replace_macro(text: str, name: str, replacement: str) -> str:
    pattern = re.compile(MACRO.format(name=name))
    text, count = pattern.subn(lambda _: replacement, text)
    if count != 1:
        sys.exit(f"wasm2c_patch.py: expected one definition of {name}, found {count}")
    return text


def main() -> None:
    folder = Path(sys.argv[1])
    header = folder / "game-impl.h"
    text = header.read_text(encoding="utf-8")
    text = replace_macro(text, "DEFINE_SHARED_LOAD", LOAD)
    text = replace_macro(text, "DEFINE_SHARED_STORE", STORE)
    text = ("#ifndef HALO_LINEAR_MEMORY_BASE\n#error HALO_LINEAR_MEMORY_BASE: the host's fixed address of the linear "
            "memory (port/macos/host/host.h)\n#endif\n" + text)
    header.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
