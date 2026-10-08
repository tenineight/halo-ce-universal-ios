"""Loads the Apple builds' settings (bundle identifiers, signing, versions)
from the repository's .env file into the environment, on import.

A variable already set in the environment wins over the file's. The file
holds KEY=value lines; blank lines and lines starting with # are skipped,
and quotes around a value are dropped. See .env.example for the settings.
"""

import os
from pathlib import Path

ENV_FILE = Path(__file__).resolve().parent.parent / ".env"


def load(path: Path = ENV_FILE) -> None:
    if not path.is_file():
        return
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip().removeprefix("export ").strip(), value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        os.environ.setdefault(key, value)


load()
