#!/usr/bin/env bash
# Run the PickScore tools with a working torch on NixOS.
#
# Three NixOS-specific problems are solved here, all of which cost time to find:
#
#  1. There is no system torch, and pip cannot target the read-only system Python.
#     The venv lives in /tmp -- see pickscore_eval.py's docstring for the one-off
#     setup command.
#  2. The stock `nix-shell` python has no libstdc++ on its library path, so
#     `import torch` fails with "cannot open shared object file". Worse, the
#     obvious candidate in the Nix store is a *32-bit* build, which fails with
#     "wrong ELF class: ELFCLASS32" -- so the 64-bit one is selected explicitly.
#  3. ~/.cache is not writable under the sandbox, so the HuggingFace cache is
#     redirected to a workspace-local directory that persists across runs.
#
# Usage:
#     sprite_preview/run_pickscore.sh pickscore_eval.py --images DIR --prop fireplace
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
VENV="${PICKSCORE_VENV:-/tmp/ps-venv}"

if [ ! -x "$VENV/bin/python" ]; then
    echo "no PickScore venv at $VENV" >&2
    echo "create it once with:" >&2
    echo "  nix-shell -p python3Packages.python python3Packages.pip --run \\" >&2
    echo "    'python3 -m venv $VENV && $VENV/bin/python -m pip install torch \\" >&2
    echo "     --index-url https://download.pytorch.org/whl/cpu && \\" >&2
    echo "     $VENV/bin/python -m pip install transformers Pillow'" >&2
    exit 2
fi

# A 64-bit libstdc++ from the Nix store. 32-bit builds are present too and are
# indistinguishable by filename.
LIBSTDCPP=""
for candidate in /nix/store/*gcc-*-lib/lib/libstdc++.so.6; do
    if file -L -b "$candidate" 2>/dev/null | grep -q "64-bit"; then
        LIBSTDCPP="$(dirname "$candidate")"
        break
    fi
done
if [ -z "$LIBSTDCPP" ]; then
    echo "could not find a 64-bit libstdc++ in /nix/store" >&2
    exit 2
fi

ZLIB="$(dirname "$(ls /nix/store/*zlib-1*/lib/libz.so.1 2>/dev/null | head -1)")"
export LD_LIBRARY_PATH="$LIBSTDCPP:$ZLIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Persistent (not /tmp) so a 1.7 GB model download happens once. Gitignored.
export HF_HOME="${HF_HOME:-$HERE/.hf-cache}"
mkdir -p "$HF_HOME"

if [ $# -lt 1 ]; then
    echo "usage: $(basename "$0") <script.py> [args...]" >&2
    exit 2
fi

tool="$1"; shift
case "$tool" in
    /*) script="$tool" ;;
    *)  script="$HERE/$tool" ;;
esac
if [ ! -f "$script" ]; then
    echo "no such script: $script" >&2
    exit 2
fi

cmd=("$VENV/bin/python" "$script")
[ "$#" -gt 0 ] && cmd+=("$@")

exec "${cmd[@]}"
