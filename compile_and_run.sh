#!/usr/bin/env bash
#
# Configure, build and run AIRogue.
#
# The build dependencies (libcurl, nlohmann_json, raylib, ...) are declared in
# shell.nix, so when they are not available on the ambient PATH this script
# re-executes itself inside that Nix shell before building. The project is
# built out-of-source in ./build, and the game is launched from the project
# root because it loads its assets (maps/, data/, fonts/) with relative paths.

set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
BINARY="$BUILD_DIR/AIRogue"

# --- Re-enter the Nix dev shell when the dependencies are not already usable.
deps_available() {
  command -v pkg-config >/dev/null 2>&1 &&
    pkg-config --exists libcurl nlohmann_json 2>/dev/null
}

if [[ -z "${IN_NIX_SHELL:-}" ]] && ! deps_available; then
  if command -v nix-shell >/dev/null 2>&1; then
    printf -v inner_cmd 'exec bash %q' "$PROJECT_ROOT/compile_and_run.sh"
    for arg in "$@"; do
      printf -v quoted_arg ' %q' "$arg"
      inner_cmd+="$quoted_arg"
    done
    exec nix-shell "$PROJECT_ROOT/shell.nix" --run "$inner_cmd"
  fi
  echo "error: libcurl/nlohmann_json were not found and nix-shell is unavailable." >&2
  echo "       Install the dependencies or run this script from 'nix-shell'." >&2
  exit 1
fi

# --- Build (out-of-source). Assets are resolved relative to the project root.
cd "$PROJECT_ROOT"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR"
fi

cmake --build "$BUILD_DIR" --parallel "$(nproc)"

if [[ ! -x "$BINARY" ]]; then
  echo "error: the build finished but '$BINARY' does not exist." >&2
  exit 1
fi

# --- Run.
exec env \
  LD_LIBRARY_PATH="/run/opengl-driver/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0 \
  "$BINARY" "$@"
