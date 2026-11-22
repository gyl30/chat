"""Check the native binary and scoped Linux Python SQLite independently."""

import os
from pathlib import Path
import re
import sqlite3
import subprocess


def pinned_identity():
    header = Path(__file__).resolve().parents[2] / "third/sqlite/sqlite3.h"
    text = header.read_text(encoding="utf-8")
    return tuple(
        re.search(rf'^#define SQLITE_{name}\s+"([^"]+)"$', text, re.M)[1]
        for name in ("VERSION", "SOURCE_ID")
    )


def server_sqlite(pid):
    executable = Path(f"/proc/{pid}/exe")
    version, source_id = subprocess.check_output(
        [executable, "--sqlite-version"], text=True
    ).splitlines()
    assert (version, source_id) == pinned_identity(), (version, source_id)
    return dict(
        version=version, source_id=source_id, executable=os.readlink(executable)
    )


def python_sqlite():
    with sqlite3.connect(":memory:") as conn:
        version, source_id = conn.execute(
            "SELECT sqlite_version(), sqlite_source_id()"
        ).fetchone()
    assert (version, source_id) == pinned_identity(), (version, source_id)
    expected = Path(os.environ["CHAT_TEST_SQLITE_LIB"]).resolve(strict=True)
    paths = {
        Path(fields[5]).resolve()
        for line in Path("/proc/self/maps").read_text().splitlines()
        if len(fields := line.split(maxsplit=5)) == 6
        and Path(fields[5]).name.startswith("libsqlite3.so")
    }
    assert paths == {expected}, (paths, expected)
    return dict(version=version, source_id=source_id, path=str(expected))
