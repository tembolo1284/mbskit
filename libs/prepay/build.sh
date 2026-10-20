#!/usr/bin/env bash
#
# build.sh - build and run prepay one stage at a time, or all together.
# Run ./build.sh help for usage.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

VENV_DIR=".venv"
NATIVE_DIR="build-native"
DEPS="cmake ninja nanobind scikit-build-core numpy pytest"

log() { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
ok()  { printf '\033[1;32m    %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# Point PATH/env at the project venv so python, pip, cmake, ninja, pytest all
# resolve to it. Called at startup if .venv exists, and after the venv stage
# creates it (functions share this shell, so the export persists downstream).
activate_venv() {
    export VIRTUAL_ENV="$ROOT/$VENV_DIR"
    export PATH="$VIRTUAL_ENV/bin:$PATH"
    unset PYTHONHOME 2>/dev/null || true
}

usage() {
    cat <<'EOF'
prepay build script

Usage:
  ./build.sh [stage ...]   run the given stages, left to right
  ./build.sh               run the full pipeline (venv native csmoke py pytest)

Stages:
  venv     create ./.venv and install build/test deps into it
  deps     install build/test deps into the current environment
  native   configure + build libprepay.so (C ABI only)  -> build-native/
  csmoke   compile + run the C smoke test against libprepay
  py       build + editable-install the Python extension (scikit-build-core)
  pytest   run the Python test suite
  all      venv -> native -> csmoke -> py -> pytest
  clean    remove build artifacts and uninstall the package
  help     show this message

Notes:
  * If ./.venv exists it is used automatically by every stage.
  * Stages chain in order:   ./build.sh native csmoke
  * The venv is NOT left activated in your shell afterward; to enter it:
        source ./.venv/bin/activate
EOF
}

_install_deps() {
    pip install --upgrade pip >/dev/null
    pip install $DEPS
}

stage_venv() {
    log "venv: create $VENV_DIR and install build/test deps"
    if [ ! -d "$VENV_DIR" ]; then
        python3 -m venv "$VENV_DIR"
        ok "created $VENV_DIR"
    else
        ok "$VENV_DIR already exists"
    fi
    activate_venv
    _install_deps
    ok "deps installed into $VENV_DIR"
}

stage_deps() {
    log "deps: install build/test deps into the current environment"
    _install_deps
    ok "deps installed"
}

stage_native() {
    log "native: configure + build libprepay (C ABI only)"
    cmake -S . -B "$NATIVE_DIR" -DCMAKE_BUILD_TYPE=Release -DPREPAY_BUILD_PYTHON=OFF
    cmake --build "$NATIVE_DIR" -j
    ok "built $NATIVE_DIR/libprepay.so"
}

stage_csmoke() {
    log "csmoke: compile + run the C smoke against libprepay"
    [ -f tests/smoke.c ] || die "tests/smoke.c not found"
    [ -f "$NATIVE_DIR/libprepay.so" ] || stage_native
    cc -std=c11 -Wall -Wextra -Iinclude tests/smoke.c \
        -L"$NATIVE_DIR" -lprepay -Wl,-rpath,"$ROOT/$NATIVE_DIR" \
        -o "$NATIVE_DIR/smoke"
    "$NATIVE_DIR/smoke"
    ok "C smoke passed"
}

stage_py() {
    log "py: build + editable-install the extension (scikit-build-core + nanobind)"
    pip install -e . --no-build-isolation
    python -c "import prepay; print('    import prepay', prepay.__version__)"
}

stage_pytest() {
    log "pytest: run the Python test suite"
    python -m pytest -q tests
}

stage_clean() {
    log "clean: remove build artifacts"
    rm -rf build "$NATIVE_DIR" ./*.egg-info \
           python/prepay/__pycache__ tests/__pycache__ .pytest_cache
    pip uninstall -y prepay >/dev/null 2>&1 || true
    ok "cleaned (left $VENV_DIR in place; 'rm -rf $VENV_DIR' to drop it)"
}

run_stage() {
    case "$1" in
        venv)   stage_venv   ;;
        deps)   stage_deps   ;;
        native) stage_native ;;
        csmoke) stage_csmoke ;;
        py)     stage_py     ;;
        pytest) stage_pytest ;;
        clean)  stage_clean  ;;
        all)    stage_venv; stage_native; stage_csmoke; stage_py; stage_pytest ;;
        *)      die "unknown stage: $1  (run ./build.sh help)" ;;
    esac
}

# Use the venv automatically if it already exists.
[ -d "$VENV_DIR" ] && activate_venv

if [ $# -eq 0 ]; then
    run_stage all
    log "done."
    exit 0
fi

case "$1" in
    -h|--help|help) usage; exit 0 ;;
esac

for s in "$@"; do run_stage "$s"; done
log "done."
