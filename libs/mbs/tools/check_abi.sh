#!/usr/bin/env bash
# Compare the exported symbols of each built .so against the frozen list in
# abi/<lib>.symbols.  Symbols added are reported (fine: minor bump); symbols
# removed fail (major bump required).
#
#   tools/check_abi.sh            compare
#   tools/check_abi.sh --freeze   rewrite abi/*.symbols from the current build
#
# Env: MBSKIT_BUILD_DIR (default ./build)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${MBSKIT_BUILD_DIR:-$ROOT/build}"
ABI_DIR="$ROOT/abi"
FREEZE=0
[[ "${1:-}" == "--freeze" ]] && FREEZE=1

[[ -d "$BUILD_DIR/lib" ]] || { echo "no build at $BUILD_DIR; run ./build.sh first" >&2; exit 2; }
mkdir -p "$ABI_DIR"

rc=0
for so in "$BUILD_DIR"/lib/lib*.so; do
    [[ -e "$so" ]] || continue
    name="$(basename "$so" .so)"
    cur="$(mktemp)"
    nm -D --defined-only "$so" | awk '$2 == "T" || $2 == "W" { print $3 }' | sort > "$cur"
    frozen="$ABI_DIR/$name.symbols"

    if (( FREEZE )) || [[ ! -f "$frozen" ]]; then
        cp "$cur" "$frozen"
        echo "[abi] $name: froze $(wc -l < "$frozen") symbols -> abi/$name.symbols"
        rm -f "$cur"; continue
    fi

    removed="$(comm -23 "$frozen" "$cur" || true)"
    added="$(comm -13 "$frozen" "$cur" || true)"

    if [[ -n "$removed" ]]; then
        echo "[abi] $name: REMOVED symbols (ABI break, needs major bump):"
        echo "$removed" | sed 's/^/    - /'
        rc=1
    fi
    if [[ -n "$added" ]]; then
        echo "[abi] $name: added symbols (minor bump; re-freeze when released):"
        echo "$added" | sed 's/^/    + /'
    fi
    if [[ -z "$removed" && -z "$added" ]]; then
        echo "[abi] $name: unchanged ($(wc -l < "$frozen") symbols)"
    fi

    # Anything exported that is not lib-prefixed is a leak regardless.
    prefix="${name#lib}_"
    leaks="$(grep -v "^${prefix}" "$cur" || true)"
    if [[ -n "$leaks" ]]; then
        echo "[abi] $name: non-${prefix}* symbols exported (visibility leak):"
        echo "$leaks" | sed 's/^/    ! /'
        rc=1
    fi
    rm -f "$cur"
done
exit $rc
