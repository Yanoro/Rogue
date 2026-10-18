#!/usr/bin/env bash
# Build llama-server with decision-model support (/v1/systemone).
#
# Why this is needed at all: the `llama-cpp` package in this machine's nixpkgs
# (25.11pre-git, llama.cpp build 6981) predates llama.cpp's decision-model merge
# of 2026-10-02. Its llama-server serves /v1/chat/completions, /v1/embeddings and
# /v1/rerank but has no /v1/systemone at all, so it cannot load a model like
# Kev-4B in decision mode. Verified by grepping the binary, not assumed: the
# endpoint strings for the other routes are present and this one is absent.
#
# So the server is built from upstream master instead. That is a one-off, ~2
# minutes on this machine, and the result is reused by
# scripts/run_decision_server.sh.
#
# Usage:
#   scripts/build_llama_server.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="${LLAMA_CPP_DIR:-$REPO_ROOT/.deps/llama.cpp}"

echo "source: $SOURCE_DIR"
mkdir -p "$(dirname "$SOURCE_DIR")"

if [[ ! -d "$SOURCE_DIR/.git" ]]; then
  # Shallow: only the current tree is used, and the full history is large.
  git clone --depth 1 https://github.com/ggml-org/llama.cpp.git "$SOURCE_DIR"
else
  echo "(reusing existing checkout; delete it to force a fresh clone)"
fi

# The endpoint this whole exercise depends on, checked before spending two
# minutes compiling a server that cannot answer the request.
if ! grep -rlq "systemone" "$SOURCE_DIR/tools/server"; then
  echo "ERROR: this checkout has no /v1/systemone -- it predates the decision-"
  echo "       model merge. Delete $SOURCE_DIR and re-run." >&2
  exit 1
fi
echo "endpoint: /v1/systemone present in tools/server"

# CPU + OpenBLAS. A CUDA build is possible on this machine (nixpkgs carries CUDA
# 12.8) but needs CUDAToolkit_ROOT pointed at it, and it is not required to test
# whether a model answers correctly -- only to make it fast. See the note at the
# end of this script.
echo
echo "configuring (CPU + OpenBLAS) ..."
cd "$SOURCE_DIR"
mkdir -p /tmp/ccache-tmp
CCACHE_TEMPDIR=/tmp/ccache-tmp nix-shell -p cmake ninja ccache openblas --run '
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DGGML_CUDA=OFF -DLLAMA_CURL=OFF \
        -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF > /tmp/llama-cmake.log 2>&1 \
  && cmake --build build --target llama-server -j4 > /tmp/llama-build.log 2>&1
'
echo "built: $SOURCE_DIR/build/bin/llama-server"
"$SOURCE_DIR/build/bin/llama-server" --version 2>/dev/null | head -2 || true

cat <<'NOTE'

CPU-only build. On this machine a single three-question decision took ~10.7s,
against ~0.6s for hosted Jev -- usable for a comparison, too slow for a game
loop. For GPU offload, reconfigure with CUDA:

  nix-shell -p cmake ninja ccache cudaPackages.cuda_nvcc --run '
    cmake -S . -B build-cuda -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DGGML_CUDA=ON -DLLAMA_CURL=OFF \
          -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF'

The bare `cudaPackages.cuda_nvcc` shell does not put the toolkit anywhere CMake
finds on its own (ggml/src/ggml-cuda/CMakeLists.txt reports "CUDA Toolkit not
found"), so it also needs -DCUDAToolkit_ROOT=$(dirname $(dirname $(command -v
nvcc))) or an explicit CUDACXX. Not automated here because it is a real
detour and the CPU build already answers the correctness question.
NOTE
