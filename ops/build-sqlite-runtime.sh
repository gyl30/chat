#!/usr/bin/env bash
# Build the pinned SQLite library in a private Linux prefix; never installs system-wide.
set -euo pipefail
if [[ $# -ne 2 ]]; then
    echo "usage: $0 sqlite-autoconf-3510300.tar.gz NEW_INSTALL_PREFIX" >&2
    exit 2
fi
archive=$(realpath "$1")
prefix=$(realpath -m "$2")
if [[ -e "$prefix" ]]; then
    echo '请选择不存在的独立安装前缀' >&2
    exit 2
fi
printf '%s  %s\n' 81f5be397049b0cae1b167f2225af7646fc0f82e4a9b3c48c9ea3a533e21d77a "$archive" | sha256sum -c -
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
tar -xf "$archive" -C "$work" --strip-components=1
cd "$work"
[[ "$(openssl dgst -sha3-256 -r sqlite3.c | cut -d ' ' -f 1)" == 32d5424f97e0a7fc5ed2f6335afbb58be4e0298bd7117a34e39d345ff13d859e ]]
./configure --prefix="$prefix"
make -j "${JOBS:-8}"
make install
