#!/usr/bin/env bash
#
# new-worktree.sh -- create a ready-to-build git worktree for a parallel agent.
#
# Encodes the setup that AIRogue needs and that `git worktree add -b br main`
# gets wrong:
#
#   * the worktree path is a SIBLING of the repo, never inside it. A nested
#     worktree is an embedded git repo: `git add -A` stages it as a broken
#     gitlink, and a directory named after a branch makes `git log main`
#     ambiguous ("both revision and filename").
#   * the start point is explicit and LAST on the command line. With
#     `git worktree add -b br main`, `main` is consumed as the PATH and the
#     branch silently starts from HEAD.
#   * a fresh worktree only contains a COMMIT, so uncommitted build plumbing
#     does not travel. HEAD's CMakeLists.txt in this repo is stale (its SOURCES
#     list omits files that exist at HEAD), so a worktree without main's
#     build files compiles and then fails to link. Those paths are carried by
#     default; feature files are carried with --carry.
#   * staging matters: `git diff` alone misses staged work, so everything is
#     read as `git diff HEAD`.
#   * ~760 MB of dependency sources live in git history under the primary
#     worktree's _deps/. They are reused through FETCHCONTENT_SOURCE_DIR_*
#     instead of being cloned again.
#
# Usage:
#   scripts/new-worktree.sh <name> [options]
#
set -euo pipefail

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
NAME=""
BRANCH=""
FROM="main"
CARRY=()
COPY_DATA=0
DO_BUILD=1
DO_CONFIGURE=1
DRY_RUN=0

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
info() { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

usage() {
    cat <<'EOF'
Create a sibling git worktree for a parallel agent, carrying uncommitted
build plumbing and reusing the existing dependency sources.

Usage:
  new-worktree.sh <name> [options]

Arguments:
  <name>              Short name, e.g. settings-manager.
                      Branch defaults to feature/<name>,
                      path defaults to ../<repo>-<name>.

Options:
  -b, --branch <br>   Branch to create (default: feature/<name>).
  -f, --from <ref>    Start point (default: main). Must be a commit.
  -C, --carry <path>  Also transplant uncommitted changes of <path> from the
                      primary worktree. Repeatable. Globs allowed.
      --data          Copy untracked runtime assets (data/style,
                      data/generated/{objects,tiles}, data/prompts).
      --configure-only  Create + configure, do not compile.
      --no-build      Create the worktree only; no CMake, no compile.
  -n, --dry-run       Print the plan; change nothing.
  -h, --help          This help.

Build plumbing carried automatically (when dirty):
  CMakeLists.txt shell.nix compile_and_run.sh bench/ patches/

Exit codes:
  0 success   1 bad usage / refused   2 configure or build failed
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)        usage; exit 0 ;;
        -n|--dry-run)     DRY_RUN=1; shift ;;
        -b|--branch)      BRANCH="${2:?--branch needs a value}"; shift 2 ;;
        -f|--from)        FROM="${2:?--from needs a value}"; shift 2 ;;
        -C|--carry)       CARRY+=("${2:?--carry needs a value}"); shift 2 ;;
        --data)           COPY_DATA=1; shift ;;
        --configure-only) DO_BUILD=0; shift ;;
        --no-build)       DO_BUILD=0; DO_CONFIGURE=0; shift ;;
        -*)               die "unknown option: $1 (see --help)" ;;
        *)                [[ -z "$NAME" ]] || die "unexpected argument: $1"
                          NAME="$1"; shift ;;
    esac
done

[[ -n "$NAME" ]] || { usage >&2; exit 1; }
[[ "$NAME" =~ ^[A-Za-z0-9][A-Za-z0-9._/-]*$ ]] \
    || die "name must match [A-Za-z0-9][A-Za-z0-9._/-]* (got: $NAME)"

# ---------------------------------------------------------------------------
# Locate the repo and its PRIMARY worktree
# ---------------------------------------------------------------------------
REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null) \
    || die "not inside a git repository"

# The primary worktree is the first entry of `git worktree list`, and it is the
# only one whose dependencies were (probably) already fetched.
PRIMARY=$(git worktree list --porcelain | sed -n 's/^worktree //p' | head -1)
[[ -n "$PRIMARY" ]] || die "could not determine the primary worktree"

REPO_BASE=$(basename "$REPO_ROOT")
# Slashes are legal in branch names but not useful in a directory name.
SAFE_NAME=${NAME//\//-}
WT_PATH="$(dirname "$REPO_ROOT")/$REPO_BASE-$SAFE_NAME"

: "${BRANCH:=feature/$NAME}"

# ---------------------------------------------------------------------------
# Pre-flight checks -- fail before creating anything
# ---------------------------------------------------------------------------
git rev-parse --verify --quiet "$FROM^{commit}" >/dev/null \
    || die "start point '$FROM' is not a commit (branches: $(git for-each-ref --format='%(refname:short)' refs/heads | tr '\n' ' '))"

git rev-parse --verify --quiet "refs/heads/$BRANCH" >/dev/null \
    && die "branch '$BRANCH' already exists (pick another with --branch)"

case "$WT_PATH" in
    "$REPO_ROOT"|"$REPO_ROOT"/*)
        die "refusing to create '$WT_PATH' inside the repository" ;;
esac

[[ -e "$WT_PATH" ]] && die "'$WT_PATH' already exists (remove it or pick another name)"

# Refuse upfront if any existing worktree is already on this branch, so git's
# own error is not the first thing the user sees.
if git worktree list --porcelain | grep -qx "branch refs/heads/$BRANCH"; then
    die "branch '$BRANCH' is already checked out in another worktree"
fi

START_SHA=$(git rev-parse --short "$FROM^{commit}")

# ---------------------------------------------------------------------------
# Plan summary
# ---------------------------------------------------------------------------
echo "worktree : $WT_PATH"
echo "branch   : $BRANCH"
echo "start    : $FROM @ $START_SHA"

# ---------------------------------------------------------------------------
# Carry-over set: build plumbing (always) + --carry paths
# ---------------------------------------------------------------------------
PLUMBING=(CMakeLists.txt shell.nix compile_and_run.sh bench patches)
CARRY_PATHS=()

# Only keep paths that actually exist in the primary worktree.
for p in "${PLUMBING[@]}" "${CARRY[@]+"${CARRY[@]}"}"; do
    [[ -e "$PRIMARY/$p" ]] && CARRY_PATHS+=("$p")
done

# Untracked-only data is copied separately with --data.
DATA_PATHS=(data/style data/generated/objects data/generated/tiles data/prompts)

# ---------------------------------------------------------------------------
# 1. Create the worktree
# ---------------------------------------------------------------------------
step "creating worktree"
if (( DRY_RUN )); then
    printf '  [dry-run] git worktree add -b %s %s %s\n' \
        "$BRANCH" "$WT_PATH" "$FROM"
else
    git worktree add -b "$BRANCH" "$WT_PATH" "$FROM"
fi

# ---------------------------------------------------------------------------
# 2. Submodules -- a worktree NEVER populates them
# ---------------------------------------------------------------------------
if [[ -f "$REPO_ROOT/.gitmodules" ]]; then
    step "initialising submodules"
    if (( DRY_RUN )); then
        printf '  [dry-run] (in %s) git submodule update --init --recursive\n' "$WT_PATH"
    else
        git -C "$WT_PATH" submodule update --init --recursive >/dev/null
    fi
fi

# ---------------------------------------------------------------------------
# 3. Transplant uncommitted changes
# ---------------------------------------------------------------------------
step "carrying uncommitted changes from $PRIMARY"

carried_tracked=()
carried_untracked=()

if (( ${#CARRY_PATHS[@]} )); then
    # `git diff HEAD` (not `git diff`): staged work counts too.
    mapfile -t changed < <(git -C "$PRIMARY" diff HEAD --name-only -- "${CARRY_PATHS[@]}")
    if (( ${#changed[@]} )); then
        # Never hold the patch in a shell variable: command substitution strips
        # the trailing newline (git apply calls that a corrupt patch) and cannot
        # store the NUL bytes that --binary emits for binary files.
        patch_file=$(mktemp "${TMPDIR:-/tmp}/new-worktree-patch.XXXXXX")
        git -C "$PRIMARY" diff HEAD --binary -- "${changed[@]}" > "$patch_file"
        if (( DRY_RUN )); then
            info "[dry-run] apply tracked changes for: ${changed[*]}"
        else
            if ! git -C "$WT_PATH" apply --whitespace=nowarn "$patch_file"; then
                rm -f "$patch_file"
                die "failed to apply carried changes; inspect $WT_PATH"
            fi
        fi
        rm -f "$patch_file"
        carried_tracked=("${changed[@]}")
    fi

    # Truly untracked files (not staged, so absent from `git diff HEAD`).
    while IFS= read -r -d '' f; do
        carried_untracked+=("$f")
        if (( DRY_RUN )); then
            info "[dry-run] copy untracked $f"
        else
            ( cd "$PRIMARY" && cp -a --parents -- "$f" "$WT_PATH/" )
        fi
    done < <(git -C "$PRIMARY" ls-files --others --exclude-standard -z -- "${CARRY_PATHS[@]}")
fi

if (( COPY_DATA )); then
    for d in "${DATA_PATHS[@]}"; do
        [[ -e "$PRIMARY/$d" ]] || continue
        carried_untracked+=("$d/")
        if (( DRY_RUN )); then
            info "[dry-run] copy $d"
        else
            ( cd "$PRIMARY" && cp -a --parents -- "$d" "$WT_PATH/" )
        fi
    done
fi

if (( ! ${#carried_tracked[@]} && ! ${#carried_untracked[@]} )); then
    info "nothing to carry (start point already contains everything)"
else
    (( ${#carried_tracked[@]} ))   && info "modified:  ${carried_tracked[*]}"
    (( ${#carried_untracked[@]} )) && info "untracked: ${carried_untracked[*]}"
fi

if (( ! DO_CONFIGURE )); then
    step "done (no build requested)"
    printf '\n  cd %s\n\n' "$WT_PATH"
    exit 0
fi

# ---------------------------------------------------------------------------
# 4. Configure against the shared dependency sources
# ---------------------------------------------------------------------------
# FetchContent defaults to full clones (no GIT_SHALLOW), so raylib+flecs+imgui+
# rlimgui cost ~760 MB of history per worktree. Pointing at the primary
# worktree's already-populated *-src dirs skips download/patch/update entirely.
DEPS_ARGS=()
if [[ -d "$PRIMARY/_deps" ]]; then
    for dep in raylib imgui rlimgui flecs; do
        src="$PRIMARY/_deps/$dep-src"
        [[ -d "$src" ]] && DEPS_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_${dep^^}=$src")
    done
fi

step "configuring"
if (( DRY_RUN )); then
    printf '  [dry-run] cmake -S . -B build %s\n' "${DEPS_ARGS[*]:-}"
    exit 0
fi

cmake_args=(-S . -B build)
(( ${#DEPS_ARGS[@]} )) && cmake_args+=("${DEPS_ARGS[@]}")

# The project must be configured and built inside nix-shell: nlohmann_json and
# curl arrive through NIX_CFLAGS_COMPILE from the Nix gcc wrapper, never through
# CMake's include directories. ccache's default temp dir is outside the sandbox.
CCACHE_BASEDIR="${CCACHE_BASEDIR:-$(dirname "$REPO_ROOT")}"
CCACHE_TEMPDIR="${CCACHE_TEMPDIR:-${TMPDIR:-/tmp}/ccache-tmp}"
mkdir -p "$CCACHE_TEMPDIR"
export CCACHE_BASEDIR CCACHE_TEMPDIR

in_shell() {
    # $1 is a shell command string, run with the worktree as cwd.
    if [[ -f "$WT_PATH/shell.nix" ]] && command -v nix-shell >/dev/null 2>&1; then
        ( cd "$WT_PATH" && nix-shell --run "$1" )
    else
        ( cd "$WT_PATH" && bash -c "$1" )
    fi
}

printf -v configure_cmd '%q ' cmake "${cmake_args[@]}"
if ! in_shell "$configure_cmd" >/tmp/new-worktree-configure.log 2>&1; then
    printf 'configure failed; see /tmp/new-worktree-configure.log\n' >&2
    sed -n '$p' /tmp/new-worktree-configure.log >&2
    exit 2
fi
info "configured"

# ---------------------------------------------------------------------------
# 5. Build
# ---------------------------------------------------------------------------
if (( DO_BUILD )); then
    step "building (this is the slow part)"
    jobs=$(nproc 2>/dev/null || echo 4)
    if ! in_shell "cmake --build build -j$jobs"; then
        printf 'build failed; the worktree is ready to debug in place:\n  cd %s\n' "$WT_PATH" >&2
        exit 2
    fi
    [[ -x "$WT_PATH/build/AIRogue" ]] && info "built: $WT_PATH/build/AIRogue"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
step "ready"
cat <<EOF

  path    : $WT_PATH
  branch  : $BRANCH (from $FROM @ $START_SHA)
  build   : $( [[ -x "$WT_PATH/build/AIRogue" ]] && echo "build/AIRogue" || echo "not built" )

  cd $WT_PATH

  Run (from inside the worktree):
    LD_LIBRARY_PATH=/run/opengl-driver/lib \\
    ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=0 ./build/AIRogue

  Remove when merged:
    git worktree remove $WT_PATH

EOF

if [[ -n "$(git -C "$WT_PATH" status --porcelain)" ]]; then
    printf '  carried into the worktree (uncommitted):\n'
    git -C "$WT_PATH" status --porcelain | sed 's/^/    /'
    printf '\n'
fi
