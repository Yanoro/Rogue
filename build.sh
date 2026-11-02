#!/usr/bin/env bash
#
# Memory-aware build driver for AIRogue.
#
# A plain `cmake --build build -j12` asks for 12 compiler processes that each
# peak at 1-3.5 GiB of resident memory. On a 15 GiB machine that is 20-40 GiB of
# demand: the kernel starts swapping and the OOM killer eventually takes a
# compiler down, which surfaces as a random, confusing build failure. This
# script instead:
#
#   * sizes the parallelism from MemAvailable, not from the core count;
#   * compiles the handful of very large translation units first, with little
#     parallelism, then lets the many small ones fill every core;
#   * points ccache at a writable temp directory (its default lives under
#     $XDG_RUNTIME_DIR, which a sandboxed build cannot write to);
#   * defaults to Debug (-O0), which compiles ~4x faster than the -O2 that Nix's
#     gcc wrapper injects when no build type is set.
#
# Usage:
#   ./build.sh                 # fast Debug build of the game
#   ./build.sh -j 8            # explicit parallelism, no memory heuristics
#   ./build.sh --release       # -O3 game build (slow to compile, fast to run)
#   ./build.sh --low-memory    # ~45% less RAM per compiler, ~6% slower
#   ./build.sh --no-asan       # drop AddressSanitizer
#   ./build.sh --clean         # wipe objects first
#
# See the airogue-build skill for the run environment (LD_LIBRARY_PATH, ASAN_OPTIONS).

set -euo pipefail

PROJECT_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

deps_available() {
  command -v pkg-config >/dev/null 2>&1 &&
    pkg-config --exists libcurl nlohmann_json 2>/dev/null
}

# --- Re-enter the Nix dev shell when its dependencies are not already usable.
if [[ -z "${IN_NIX_SHELL:-}" ]] && ! deps_available; then
  if command -v nix-shell >/dev/null 2>&1; then
    inner="$(printf '%q ' "$PROJECT_ROOT/build.sh" "$@")"
    exec nix-shell "$PROJECT_ROOT/shell.nix" --run "exec bash $inner"
  fi
  echo "error: libcurl/nlohmann_json not found and nix-shell is unavailable." >&2
  exit 1
fi

usage() {
  sed -n '/^# Usage:/,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

BUILD_TYPE=Debug
JOBS=""
LOW_MEMORY=OFF
ASAN=ON
CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    -j|--jobs)     JOBS="${2:?--jobs needs a number}"; shift 2 ;;
    -j*)           JOBS="${1#-j}"; shift ;;
    --release)     BUILD_TYPE=Release; shift ;;
    --debug)       BUILD_TYPE=Debug; shift ;;
    --low-memory)  LOW_MEMORY=ON; shift ;;
    --no-asan)     ASAN=OFF; shift ;;
    --clean)       CLEAN=1; shift ;;
    -h|--help)     usage; exit 0 ;;
    *) echo "build.sh: unknown argument '$1' (try --help)" >&2; exit 2 ;;
  esac
done

# ccache's default temp dir is $XDG_RUNTIME_DIR/ccache-tmp, which is outside the
# workspace and therefore unwritable in a sandboxed build ("ccache: error: failed
# to create temporary file ..." from every compile).
export CCACHE_TEMPDIR="${CCACHE_TEMPDIR:-/tmp/ccache-tmp}"
mkdir -p "$CCACHE_TEMPDIR" 2>/dev/null || true

# Its object store (~/.cache/ccache) is outside the workspace too, so a sandboxed
# build silently never stores anything and every build is a full rebuild. Fall
# back to a cache inside the untracked build tree when the usual one is not
# writable from here.
if command -v ccache >/dev/null 2>&1 && [[ -z "${CCACHE_DIR:-}" ]]; then
  default_cache="${XDG_CACHE_HOME:-$HOME/.cache}/ccache"
  if mkdir -p "$default_cache" 2>/dev/null &&
     touch "$default_cache/.build-sh-probe" 2>/dev/null; then
    rm -f "$default_cache/.build-sh-probe"
  else
    export CCACHE_DIR="$BUILD_DIR/.ccache"
    mkdir -p "$CCACHE_DIR"
  fi
fi

# --- Hardware sizing.
NCORES=$(nproc)
available_mib() { awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo; }

clamp() { # clamp <value> <lo> <hi>
  local v="$1" lo="$2" hi="$3"
  if (( v < lo )); then v=$lo; fi
  if (( v > hi )); then v=$hi; fi
  echo "$v"
}

# Peak RSS per compiler process, measured on this project at -O0 -g with ASan
# (after the GameECS split the worst TU is src/GameECSInit.cpp at 2.2 GiB;
# median ~1.0 GiB; a 2 KB file is 0.18 GiB). MiB.
if [[ "$LOW_MEMORY" == ON ]]; then
  HEAVY_BUDGET=1700; LIGHT_BUDGET=800
else
  HEAVY_BUDGET=2800; LIGHT_BUDGET=1250
fi

# MemAvailable already excludes what is in use; this is headroom for whatever
# else you start while the build runs.
budget_mib=$(( $(available_mib) - 1536 ))
if (( budget_mib < 1024 )); then budget_mib=1024; fi

JOBS_HEAVY=$(clamp $(( budget_mib / HEAVY_BUDGET )) 1 "$NCORES")
JOBS_LIGHT=$(clamp $(( budget_mib / LIGHT_BUDGET )) 1 "$NCORES")

# The largest translation units, by source size. They decide both peak memory
# and the critical path, so they go first with little parallelism; the remaining
# ~80 small files then fill every core.
heavy_objects() {
  local f
  while read -r f; do
    printf 'src/%s.o\n' "${f%.cpp}"
  done < <(find "$PROJECT_ROOT/src" -maxdepth 1 -name '*.cpp' -printf '%s %f\n' |
             sort -rn | head -n 6 | cut -d' ' -f2-)
}

# --- Configure (cheap; keeps compile_commands.json and flags in sync). It emits
# a wall of deprecation/dev warnings from raylib's and FetchContent's CMake code,
# so keep it quiet on success and print the whole log only when it fails.
configure_log="$(mktemp)"
if ! cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
      -DAIROGUE_ASAN="$ASAN" \
      -DAIROGUE_LOW_MEMORY="$LOW_MEMORY" >"$configure_log" 2>&1; then
  cat "$configure_log" >&2
  rm -f "$configure_log"
  echo "build.sh: cmake configure failed" >&2
  exit 1
fi
rm -f "$configure_log"

if (( CLEAN )); then
  cmake --build "$BUILD_DIR" --target clean >/dev/null
fi

started=$(date +%s)
echo "build.sh: $BUILD_TYPE  asan=$ASAN  low_memory=$LOW_MEMORY  cores=$NCORES" \
     "mem_available=$(( $(available_mib) )) MiB" >&2

mapfile -t heavy < <(heavy_objects)
if [[ -n "$JOBS" ]]; then
  echo "build.sh: explicit -j $JOBS (memory heuristics disabled)" >&2
  cmake --build "$BUILD_DIR" --target AIRogue --parallel "$JOBS"
elif [[ ${#heavy[@]} -gt 0 && -f "$BUILD_DIR/Makefile" ]] &&
     ( cd "$BUILD_DIR" && make help 2>/dev/null | grep -qx '\.\.\. '"${heavy[0]}" ); then
  echo "build.sh: heavy phase -j $JOBS_HEAVY, then light phase -j $JOBS_LIGHT" >&2
  ( cd "$BUILD_DIR" && make "${heavy[@]}" -j "$JOBS_HEAVY" )
  ( cd "$BUILD_DIR" && make AIRogue -j "$JOBS_LIGHT" )
else
  cd "$BUILD_DIR" && make AIRogue -j "$JOBS_LIGHT"
fi

echo "build.sh: done in $(( $(date +%s) - started ))s" >&2
