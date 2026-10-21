#!/usr/bin/env bash
# mbskit superbuild driver.
#
#   ./build.sh                 configure + build + test (Release)
#   ./build.sh configure       cmake configure only
#   ./build.sh build           build (configures if needed)
#   ./build.sh test            ctest (builds if needed)
#   ./build.sh clean           rm -rf build dir
#   ./build.sh abi             dump exported symbols of each .so
#   ./build.sh abi-check       diff exports against abi/*.symbols (fails on removal/leak)
#   ./build.sh abi-freeze      rewrite abi/*.symbols from this build
#
# Flags (before or after the stage):
#   --debug        CMAKE_BUILD_TYPE=Debug
#   --prepay       -DMBSKIT_BUILD_PREPAY=ON
#   --no-werror    -DMBSKIT_WERROR=OFF
#   --examples     -DMBSKIT_BUILD_EXAMPLES=ON
#   -j N           parallel jobs (default: nproc)
#
# Env:
#   MBSKIT_BUILD_DIR   override build directory (default: ./build)
#   CC / CXX           honoured by cmake as usual

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${MBSKIT_BUILD_DIR:-$ROOT/build}"
BUILD_TYPE=Release
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
CMAKE_ARGS=()
STAGE=all

while [[ $# -gt 0 ]]; do
    case "$1" in
        configure|build|test|clean|abi|abi-check|abi-freeze|all) STAGE="$1" ;;
        --debug)      BUILD_TYPE=Debug ;;
        --prepay)     CMAKE_ARGS+=(-DMBSKIT_BUILD_PREPAY=ON) ;;
        --no-werror)  CMAKE_ARGS+=(-DMBSKIT_WERROR=OFF) ;;
        --examples)   CMAKE_ARGS+=(-DMBSKIT_BUILD_EXAMPLES=ON) ;;
        -j)           shift; JOBS="$1" ;;
        -j*)          JOBS="${1#-j}" ;;
        -h|--help)    sed -n '2,24p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

log() { printf '\033[1;34m[mbskit]\033[0m %s\n' "$*"; }

do_configure() {
    log "configure ($BUILD_TYPE) -> $BUILD_DIR"
    cmake -S "$ROOT" -B "$BUILD_DIR" \
          -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
          "${CMAKE_ARGS[@]+"${CMAKE_ARGS[@]}"}"
}

do_build() {
    [[ -f "$BUILD_DIR/CMakeCache.txt" ]] || do_configure
    log "build -j$JOBS"
    cmake --build "$BUILD_DIR" --parallel "$JOBS"
}

do_test() {
    do_build
    log "test"
    ctest --test-dir "$BUILD_DIR" --output-on-failure
}

do_clean() {
    log "clean $BUILD_DIR"
    rm -rf "$BUILD_DIR"
}

do_abi() {
    do_build
    for so in "$BUILD_DIR"/lib/*.so; do
        [[ -e "$so" ]] || continue
        log "exported symbols: $(basename "$so")"
        nm -D --defined-only "$so" | awk '$2 == "T" { print "  " $3 }' | sort
    done
}

case "$STAGE" in
    configure) do_configure ;;
    build)     do_build ;;
    test)      do_test ;;
    clean)     do_clean ;;
    abi)       do_abi ;;
    abi-check) do_build; MBSKIT_BUILD_DIR="$BUILD_DIR" "$ROOT/tools/check_abi.sh" ;;
    abi-freeze) do_build; MBSKIT_BUILD_DIR="$BUILD_DIR" "$ROOT/tools/check_abi.sh" --freeze ;;
    all)       do_configure; do_build; do_test; MBSKIT_BUILD_DIR="$BUILD_DIR" "$ROOT/tools/check_abi.sh" ;;
esac
