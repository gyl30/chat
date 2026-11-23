#!/usr/bin/env bash
# Linux production build and isolated functional acceptance, from any working directory.
set -euo pipefail
build=$(realpath -m "${1:?usage: linux.sh RELEASE_BUILD_DIRECTORY}")
cd "$(dirname "$0")/../.."
if [[ -f "$build/CMakeCache.txt" ]] && ! grep -Fxq 'CMAKE_BUILD_TYPE:STRING=Release' "$build/CMakeCache.txt"; then
    echo '请使用独立的 Release 构建目录' >&2
    exit 2
fi
export CC=${CC:-gcc-16} CXX=${CXX:-g++-16} PYTHONDONTWRITEBYTECODE=1
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCHAT_BUILD_QT_CLIENT=ON
cmake --build "$build" --target chat_server chat_client chat_qt -j "${JOBS:-8}"
export CHAT_TEST_SERVER="$build/chat_server"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
python3 ops/acceptance/build_probe.py "$build" "$work/probe"
python3 ops/acceptance/login_sessions.py "$work/probe"
python3 ops/acceptance/group_moderation.py "$work/probe"
python3 ops/acceptance/clear.py
python3 ops/acceptance/search_filters.py
python3 ops/acceptance/reading_navigation.py "$work/probe"
python3 ops/acceptance/history.py "$work/probe"
python3 ops/acceptance/live_history.py "$work/probe"
python3 ops/acceptance/drafts.py "$work/probe"
python3 ops/acceptance/deep_reply.py "$work/probe"
python3 ops/acceptance/downloads.py "$work/probe"
python3 ops/acceptance/downloads.py "$work/probe" --stalls
dbus-run-session -- python3 ops/acceptance/recovery.py "$work/probe"
python3 ops/acceptance/desktop.py "$work/probe"
