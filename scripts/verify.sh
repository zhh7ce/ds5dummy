#!/bin/bash
# DS5Dummy — local release verification
#
# Runs what a CI job would run: both BT backends build with -Werror and pass
# ctest, then HIDRAW is built again and retested under ASan+UBSan. No root,
# no controller and no PipeWire device are required.
#
# Usage: ./scripts/verify.sh
# Build trees: build/verify-hidraw, build/verify-l2cap, build/verify-asan

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="$ROOT/build"

log() { echo "[verify] $*"; }

# 配置并跑一套构建。flags 原样进入 CMAKE_CXX_FLAGS（编译与链接都用得上，
# sanitizer 的运行库因此不需要单独的 linker 参数）。
run_suite() {
    local name=$1 backend=$2 cxx_flags=$3
    local dir="$BUILD_ROOT/verify-$name"

    log "$name: configure + build"
    cmake -S "$ROOT" -B "$dir" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBT_BACKEND="$backend" \
        -DDS5DUMMY_BUILD_TESTS=ON \
        -DDS5DUMMY_BUILD_TOOLS=OFF \
        -DCMAKE_CXX_FLAGS="$cxx_flags"
    cmake --build "$dir" --parallel "$(nproc)"

    log "$name: ctest"
    ctest --test-dir "$dir" --output-on-failure
}

run_suite hidraw HIDRAW "-Werror"
run_suite l2cap L2CAP "-Werror"
run_suite asan HIDRAW \
    "-Werror -fsanitize=address,undefined -fno-omit-frame-pointer"

log "all suites passed"
