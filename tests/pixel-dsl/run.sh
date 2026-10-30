#!/usr/bin/env bash
# Regression harness for the sprite DSL compiler (src/PixelDSL.cpp).
#
# The repo has no test framework, so this is the same shape as the other
# headless verifiers: drive the CLI, assert on its verdict, and assert on the
# bytes it wrote. Run it after any change to the grammar, the renderer or the
# ramps:
#
#   tests/pixel-dsl/run.sh                 # uses ./build/AIRogue
#   tests/pixel-dsl/run.sh ./AIRogue       # or the in-source tree
#
# The binary is ASan-instrumented and linked against raylib, so it needs the
# NixOS run environment even for a headless mode that never opens a window.
set -u

cd "$(dirname "$0")/../.." || exit 1
BIN=${1:-./build/AIRogue}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-/run/opengl-driver/lib}"
export ASAN_OPTIONS="protect_shadow_gap=0:detect_leaks=0"

if [ ! -x "$BIN" ]; then
  echo "no binary at $BIN (build first: nix-shell --run 'cmake --build build -j\$(nproc)')"
  exit 1
fi

pass=0
fail=0
report() {
  if [ "$1" -eq 0 ]; then
    pass=$((pass + 1))
    printf '  ok    %s\n' "$2"
  else
    fail=$((fail + 1))
    printf '  FAIL  %s\n' "$2"
    if [ -n "${3:-}" ]; then printf '%s\n' "$3" | sed 's/^/        /'; fi
  fi
}

# compile <doc> [extra args...] -> sets OUT and CODE
compile() {
  local doc=$1
  shift
  OUT=$("$BIN" --sprite-compile "$doc" "$@" 2>&1)
  CODE=$?
}

expect_ok() {
  local doc=$1
  shift
  compile "$doc" "$@"
  report "$CODE" "compiles: $doc" "$OUT"
}

expect_refused() {
  local doc=$1
  local needle=$2
  compile "$doc"
  if [ "$CODE" -eq 0 ]; then
    report 1 "refuses: $doc" "expected a non-zero exit, got 0"
    return
  fi
  case "$OUT" in
    *"$needle"*) report 0 "refuses: $doc ($needle)" ;;
    *) report 1 "refuses: $doc" "exit was non-zero but the verdict does not contain '$needle':
$OUT" ;;
  esac
}

echo "sprite DSL: $BIN"

# --- the documents that must compile ---------------------------------------
expect_ok tests/pixel-dsl/grass.sprite --sprite-compile-out "$WORK/grass.png"
report $([ -s "$WORK/grass.png" ] && echo 0 || echo 1) "writes grass.png"

expect_ok tests/pixel-dsl/flagstone.sprite --sprite-compile-out "$WORK/flagstone.png"
report $([ -s "$WORK/flagstone.png" ] && echo 0 || echo 1) "writes flagstone.png"

expect_ok tests/pixel-dsl/crate.sprite --sprite-compile-out "$WORK/crate.png"
report $([ -s "$WORK/crate.png" ] && echo 0 || echo 1) "writes crate.png"

expect_ok tests/pixel-dsl/frames.sprite --sprite-compile-out "$WORK/wheat.png"
report $([ -s "$WORK/wheat_sprout.png" ] && [ -s "$WORK/wheat_mature.png" ] && echo 0 || echo 1) \
  "writes one PNG per frame"

# --- the invariants the compiler claims -------------------------------------
compile tests/pixel-dsl/grass.sprite
case "$OUT" in
  *"opaque: 100.0%"*) report 0 "a tile is fully opaque" ;;
  *) report 1 "a tile is fully opaque" "$OUT" ;;
esac
case "$OUT" in
  *"0 literal"*) report 0 "every colour comes from a ramp" ;;
  *) report 1 "every colour comes from a ramp" "$OUT" ;;
esac

# Determinism: same document, same seed, same bytes. If this ever fails, seeded
# scatter is not reproducible and the whole review story falls apart.
"$BIN" --sprite-compile tests/pixel-dsl/grass.sprite --sprite-compile-out "$WORK/a.png" >/dev/null 2>&1
"$BIN" --sprite-compile tests/pixel-dsl/grass.sprite --sprite-compile-out "$WORK/b.png" >/dev/null 2>&1
cmp -s "$WORK/a.png" "$WORK/b.png"
report $? "same document compiles to identical bytes"

# Wrap: the same geometry wraps with `wrap on` and warns with `wrap off`.
cat > "$WORK/wrap_on.sprite" <<'EOF'
sprite 32x32
kind tile
wrap on
fill stone.2
rect (30,30) (34,34) color=stone.0
pixel (-2,5) color=stone.0
EOF
cat > "$WORK/wrap_off.sprite" <<'EOF'
sprite 32x32
kind object
fill stone.2
rect (30,30) (34,34) color=stone.0
EOF
compile "$WORK/wrap_on.sprite"
if [ "$CODE" -eq 0 ]; then
  case "$OUT" in
    *clipped*) report 1 "wrap on clips nothing" "$OUT" ;;
    *) report 0 "wrap on clips nothing" ;;
  esac
else
  report 1 "wrap on clips nothing" "$OUT"
fi
compile "$WORK/wrap_off.sprite"
case "$OUT" in
  *clipped*) report 0 "wrap off reports what it clipped" ;;
  *) report 1 "wrap off reports what it clipped" "$OUT" ;;
esac

# --- the documents that must be refused, with the reason --------------------
expect_refused tests/pixel-dsl/bad_unknown_colour.sprite "out of range"
expect_refused tests/pixel-dsl/bad_transparent_tile.sprite "not full-bleed"
expect_refused tests/pixel-dsl/bad_mixed_frames.sprite "not both"
expect_refused tests/pixel-dsl/bad_scatter_seed.sprite "seed"
expect_refused tests/pixel-dsl/bad_ramp_missing.sprite "no material ramp"
expect_refused tests/pixel-dsl/bad_parse.sprite "fill needs a colour"

# --- the document turn's pure half: reply in, verdict out -------------------
"$BIN" --sprite-doc-check tests/pixel-dsl/reply_fenced.txt >"$WORK/doc1.out" 2>&1
report $? "document turn: a fenced reply compiles" "$(cat "$WORK/doc1.out")"
case "$(cat "$WORK/doc1.out")" in
  *"document:   397 bytes extracted"*) report 0 "document turn: the fence is extracted, not the prose" ;;
  *) report 1 "document turn: the fence is extracted, not the prose" "$(cat "$WORK/doc1.out")" ;;
esac

"$BIN" --sprite-doc-check tests/pixel-dsl/reply_bare.txt >"$WORK/doc2.out" 2>&1
report $? "document turn: a bare reply with <think> compiles" "$(cat "$WORK/doc2.out")"

"$BIN" --sprite-doc-check tests/pixel-dsl/reply_refused.txt >"$WORK/doc3.out" 2>&1
REFUSED_CODE=$?
case "$(cat "$WORK/doc3.out")" in
  *"not full-bleed"*) report $([ "$REFUSED_CODE" -ne 0 ] && echo 0 || echo 1) \
    "document turn: a refused reply reports the compiler's reason" ;;
  *) report 1 "document turn: a refused reply reports the compiler's reason" "$(cat "$WORK/doc3.out")" ;;
esac

"$BIN" --sprite-doc-prompt >"$WORK/prompt.out" 2>&1
PROMPT_CODE=$?
if [ "$PROMPT_CODE" -eq 0 ] && ! grep -q UNSUBSTITUTED "$WORK/prompt.out"; then
  report 0 "document turn: the prompt substitutes the grammar and the ramps"
else
  report 1 "document turn: the prompt substitutes the grammar and the ramps" "$(cat "$WORK/prompt.out")"
fi

# --- the DSL producer: art.generation.source = "dsl" ------------------------
# A pack with one hand-authored document, then the producer the queue runs. The
# contracts: the PNG lands in the generated layer, the document is not touched,
# and a missing or refused document fails with the path or the reason.
PACK="$WORK/packs/art/testpack"
mkdir -p "$PACK/sprites"
cp tests/pixel-dsl/flagstone.sprite "$PACK/sprites/tile_mossy_floor.sprite"

"$BIN" --art-pack "$PACK" --tile-art "mossy floor" --tile-art-dsl >"$WORK/dsl.out" 2>&1
report $? "dsl producer: compiles and places" "$(cat "$WORK/dsl.out")"
PLACED="$WORK/packs/generated/testpack/sprites/tile_mossy_floor.png"
report $([ -s "$PLACED" ] && echo 0 || echo 1) "dsl producer: sprite in the generated layer"
report $([ ! -f "$PACK/sprites/tile_mossy_floor.png" ] && echo 0 || echo 1)   "dsl producer: never writes into the art pack"
case "$(cat "$WORK/dsl.out")" in
  *"resolves:   $PLACED"*) report 0 "dsl producer: the placed sprite resolves" ;;
  *) report 1 "dsl producer: the placed sprite resolves" "$(cat "$WORK/dsl.out")" ;;
esac

"$BIN" --art-pack "$PACK" --tile-art "no such tile" --tile-art-dsl >"$WORK/dsl_missing.out" 2>&1
MISSING=$?
case "$(cat "$WORK/dsl_missing.out")" in
  *tile_no_such_tile.sprite*) report $([ "$MISSING" -ne 0 ] && echo 0 || echo 1)     "dsl producer: a missing document names the path to write" ;;
  *) report 1 "dsl producer: a missing document names the path to write" "$(cat "$WORK/dsl_missing.out")" ;;
esac

cp tests/pixel-dsl/bad_transparent_tile.sprite "$PACK/sprites/tile_broken.sprite"
"$BIN" --art-pack "$PACK" --tile-art "broken" --tile-art-dsl >"$WORK/dsl_bad.out" 2>&1
BROKEN=$?
case "$(cat "$WORK/dsl_bad.out")" in
  *"not full-bleed"*) report $([ "$BROKEN" -ne 0 ] && echo 0 || echo 1)     "dsl producer: a refused document reports the compiler's reason" ;;
  *) report 1 "dsl producer: a refused document reports the compiler's reason" "$(cat "$WORK/dsl_bad.out")" ;;
esac

# --- an object document goes through the same producer ----------------------
# The object convention is the bare key (`red_bloom.png`), not `tile_<slug>`, so
# this is also the assertion that the producer is kind-aware.
cp tests/pixel-dsl/bloom.sprite "$PACK/sprites/red_bloom.sprite"
"$BIN" --art-pack "$PACK" --tile-art "red bloom" --tile-art-dsl >"$WORK/obj.out" 2>&1
OBJECT_CODE=$?
case "$(cat "$WORK/obj.out")" in
  *"kind object"*) report 0 "dsl producer: object documents are recognised" ;;
  *) report 1 "dsl producer: object documents are recognised" "$(cat "$WORK/obj.out")" ;;
esac
OBJECT_PNG="$WORK/packs/generated/testpack/sprites/red_bloom.png"
report $([ "$OBJECT_CODE" -eq 0 ] && [ -s "$OBJECT_PNG" ] && echo 0 || echo 1) \
  "dsl producer: an object document compiles and places"

echo
echo "sprite DSL: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
