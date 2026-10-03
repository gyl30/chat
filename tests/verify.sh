#!/usr/bin/env bash
set -euo pipefail

chat_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

cmake -S "$chat_root" -B "$chat_root/build" \
    -DCHAT_BUILD_QT_CLIENT=ON -DCHAT_BUILD_TUI_CLIENT=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build "$chat_root/build" -j12
ctest --test-dir "$chat_root/build" --parallel 1 --output-on-failure

cmake -S "$chat_root" -B "$chat_root/build/asan" \
    -DCHAT_BUILD_QT_CLIENT=ON -DCHAT_BUILD_TUI_CLIENT=ON -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
    -DCMAKE_C_FLAGS_DEBUG=-g1 -DCMAKE_CXX_FLAGS_DEBUG=-g1 \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address
cmake --build "$chat_root/build/asan" -j12
ctest --test-dir "$chat_root/build/asan" --parallel 1 --output-on-failure

cmake -S "$chat_root" -B "$chat_root/build/ubsan" \
    -DCHAT_BUILD_QT_CLIENT=ON -DCHAT_BUILD_TUI_CLIENT=ON -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
    -DCMAKE_C_FLAGS_DEBUG=-g1 -DCMAKE_CXX_FLAGS_DEBUG=-g1 \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined
cmake --build "$chat_root/build/ubsan" -j12
ctest --test-dir "$chat_root/build/ubsan" --parallel 1 --output-on-failure

git -C "$chat_root" diff --check
