#!/usr/bin/env bash
# Scope the private SQLite dependency to this command and its children only.
set -euo pipefail
if [[ $# -lt 2 ]]; then
    echo "usage: $0 SQLITE_INSTALL_PREFIX COMMAND [ARGS...]" >&2
    exit 2
fi
prefix=$(realpath "$1")
shift
: "${prefix:?}"
if [[ ! -f "$prefix/lib/libsqlite3.so.0" ]]; then
    echo "未找到 SQLite 共享库：$prefix/lib/libsqlite3.so.0" >&2
    exit 2
fi
export LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$@"
