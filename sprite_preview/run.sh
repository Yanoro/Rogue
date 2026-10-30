#!/usr/bin/env bash
# Run the sprite_preview scratch tools with Pillow available, without nix-shell
# syntax leaking into every invocation.
#
# Why a wrapper at all: on NixOS the system python lives in the read-only store,
# so Pillow cannot be imported from it and there is no pip to add it. The
# established pattern in this repo is `nix-shell -p ... --run "..."` (see
# tools/README.md §3), which is correct but long enough that it discourages
# running the tools often. This wrapper is that command, once.
#
# It is deliberately *only* an environment wrapper: it adds no behaviour, so a
# tool run through it behaves exactly like the same tool run any other way.
#
# Usage:
#     sprite_preview/run.sh comfy_sheet.py --sheet
#     sprite_preview/run.sh comfy_props.py barrel well cart
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"

if [ $# -lt 1 ]; then
    echo "usage: $(basename "$0") <script.py> [args...]" >&2
    echo "       scripts live in sprite_preview/" >&2
    exit 2
fi

# Accept both `comfy_sheet.py` and a full/relative path.
tool="$1"
shift
case "$tool" in
    /*) script="$tool" ;;
    *)  script="$HERE/$tool" ;;
esac

if [ ! -f "$script" ]; then
    echo "no such script: $script" >&2
    exit 2
fi

# Build the command as an argv array, then quote it for nix-shell.
#
# The naive `"python3 $(printf '%q' "$script")$(printf ' %q' "$@")"` is wrong:
# with no extra arguments `printf ' %q' "$@"` still prints a single `''`, so the
# tool receives one empty argument. argparse rejects that as an unrecognised
# positional, which presents as a bug in the tool rather than in this wrapper.
cmd=(python3 "$script")
if [ "$#" -gt 0 ]; then
    cmd+=("$@")
fi
quoted=$(printf '%q ' "${cmd[@]}")

exec nix-shell -p python3Packages.pillow --run "$quoted"
