#!/usr/bin/env bash
# Serve a local System 1 decision model for AIRogue.
#
# Why a script: the server is built from upstream llama.cpp (the nixpkgs package
# predates decision-model support -- see scripts/build_llama_server.sh), it needs
# its own shared libraries on LD_LIBRARY_PATH, and its flags differ from the
# hosted setup in ways that are easy to get wrong. One place that gets it right
# beats remembering it.
#
# Usage:
#   scripts/run_decision_server.sh                      # Kev-4B, GPU if available
#   scripts/run_decision_server.sh models/Laya-Q8_0.gguf
#   NGLAYERS=0 scripts/run_decision_server.sh           # CPU
#   PORT=8081 scripts/run_decision_server.sh            # if 9931 is taken
#   LLAMA_BUILD=build scripts/run_decision_server.sh    # force the CPU build tree
#
# Then, in another terminal:
#   scripts/test_decision_server.sh                     # raw curl, no engine
#   ./build/AIRogue --settings /tmp/kev-settings.json --systemone-test
set -euo pipefail

MODEL="${1:-models/Kev-4B-Q4_K_M.gguf}"
# llama.cpp moved its default from 8080 to 9931 in build b11521. Pinned here so
# the URL in this comment, in the logs and in any curl you copy all agree.
PORT="${PORT:-9931}"
# Offload every layer. Harmless on a CPU-only build (the flag exists, there are
# simply no GPU layers to move), so it does not need to be conditional.
NGLAYERS="${NGLAYERS:-99}"

# Context size, set explicitly rather than left to the model.
#
# This is the difference between a server that starts in seconds and one that
# never finishes. With `-c 0` llama-server adopts the model's own context from its
# metadata, and Kev's is large: on this machine that produced an 8 GB RSS, filled
# RAM and pushed the box into heavy swap -- observed as a server stuck at
# "Loading model" for three minutes with only 1.8 GiB available.
#
# A decision request is a short state plus a handful of questions. Kev's own
# tokenizer used 143 tokens for the three-question probe, and even a site-
# selection state with ten candidate descriptions is a few hundred. 2048 is
# generous, keeps the KV cache small, and makes memory use predictable.
CTX="${CTX:-2048}"
# Physical cores, not threads: 6 on this Ryzen 5 5500 (6c/12t), which is also
# what llama.cpp chose on its own. Exposed so it can be lowered when the game is
# competing for the CPU.
THREADS="${THREADS:-$(nproc --all 2>/dev/null | awk '{print ($1>6)?6:$1}')}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLAMA_ROOT="${LLAMA_CPP_DIR:-$REPO_ROOT/.deps/llama.cpp}"
# Prefer a CUDA build tree when one exists: CPU Kev takes ~41 s for a
# capability-list route, which is too slow to use interactively. LLAMA_BUILD=build
# forces the CPU tree when comparing the two.
LLAMA_BUILD_TREE="${LLAMA_BUILD:-}"
if [[ -z "$LLAMA_BUILD_TREE" ]]; then
  if [[ -x "$LLAMA_ROOT/build-cuda/bin/llama-server" ]]; then
    LLAMA_BUILD_TREE=build-cuda
  else
    LLAMA_BUILD_TREE=build
  fi
fi
BIN_DIR="$LLAMA_ROOT/$LLAMA_BUILD_TREE/bin"
cd "$REPO_ROOT"

if [[ ! -x "$BIN_DIR/llama-server" ]]; then
  cat >&2 <<EOF
no llama-server at $BIN_DIR/llama-server

The llama-cpp package in this machine's nixpkgs predates the decision-model
merge and has no /v1/systemone, so the server has to be built from upstream:

  scripts/build_llama_server.sh      # CPU (always works)
  build-cuda/bin/llama-server        # preferred automatically when present

Set LLAMA_BUILD=build to force the CPU tree.
EOF
  exit 1
fi

if [[ ! -f "$MODEL" ]]; then
  echo "no model at '$MODEL'" >&2
  echo "known models:" >&2
  ls -1 models/*.gguf 2>/dev/null >&2 || echo "  (none downloaded yet)" >&2
  cat >&2 <<'EOF'

download one with, for example:
  curl -L -C - -o models/Kev-4B-Q4_K_M.gguf \
    https://huggingface.co/ggml-org/Kev-4B-GGUF/resolve/main/Kev-4B-Q4_K_M.gguf
EOF
  exit 1
fi

echo "model:  $MODEL ($(du -h "$MODEL" | cut -f1))"
echo "port:   $PORT"
echo "gpu:    -ngl $NGLAYERS"
echo "ctx:    $CTX   threads: $THREADS"
echo "url:    http://127.0.0.1:$PORT/v1/systemone"
echo

# The binary links libllama/libggml from its own bin directory, and they are not
# installed anywhere, so the loader needs telling. A CUDA build additionally needs
# libcudart/libcublas; those store paths are in the binary's RPATH, not here.
export LD_LIBRARY_PATH="$BIN_DIR:${LD_LIBRARY_PATH:-}"
if [[ "$LLAMA_BUILD_TREE" == "build-cuda" ]]; then
  # libcuda.so.1 is the driver, not the stub the build linked against, and NixOS
  # keeps it in the OpenGL driver tree rather than on the default loader path.
  export LD_LIBRARY_PATH="/run/opengl-driver/lib:$LD_LIBRARY_PATH"
fi

# 127.0.0.1 only, which is llama-server's default and what we want: nothing but
# this machine can reach it, so no API key is needed.
exec "$BIN_DIR/llama-server" -m "$MODEL" --port "$PORT" -ngl "$NGLAYERS" \
     --host 127.0.0.1 -c "$CTX" -t "$THREADS"
