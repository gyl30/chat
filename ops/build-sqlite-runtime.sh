#!/usr/bin/env bash
# Build the repository's SQLite for Linux Python without replacing the system library.
set -euo pipefail
if [[ $# -ne 1 ]]; then
    echo "usage: $0 NEW_INSTALL_PREFIX" >&2
    exit 2
fi
source_dir=$(cd -- "$(dirname -- "$0")/../third/sqlite" && pwd -P)
prefix=$(realpath -m -- "$1")
if [[ -e "$prefix" || -L "$prefix" ]]; then
    echo '请选择不存在的独立安装前缀' >&2
    exit 2
fi
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
"${CC:-cc}" -O2 -fPIC -shared -DSQLITE_THREADSAFE=1 -pthread \
    -Wl,-soname,libsqlite3.so.0 "$source_dir/sqlite3.c" -ldl -lm \
    -o "$work/libsqlite3.so.0"
mkdir -p -- "$(dirname -- "$prefix")"
mkdir -- "$prefix"
install -D -m 755 "$work/libsqlite3.so.0" "$prefix/lib/libsqlite3.so.0"
