#!/usr/bin/env bash
# Fork-only: merges Snapmaker/OrcaSlicer main into the fork branch.
# Replaces CI for this fork (GitHub Actions is disabled on it). See FORK.md.
set -euo pipefail

UPSTREAM_URL="git@github.com:Snapmaker/OrcaSlicer.git"
FORK_BRANCH="ai-assistant"
PUSH=0
BUILD=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [-p] [-b] [-h]
  -p  push main and $FORK_BRANCH to origin after a successful merge
  -b  build the slicer after merging (./build_linux.sh -s, dependencies must be built)
  -h  show this help
EOF
}

while getopts "pbh" opt; do
    case $opt in
        p) PUSH=1 ;;
        b) BUILD=1 ;;
        h) usage; exit 0 ;;
        *) usage; exit 1 ;;
    esac
done

cd "$(git rev-parse --show-toplevel)"

if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "Working tree has uncommitted changes, commit or stash them first." >&2
    exit 1
fi

git remote get-url upstream >/dev/null 2>&1 || git remote add upstream "$UPSTREAM_URL"
git fetch upstream main
git fetch origin

git switch "$FORK_BRANCH"

# main is a pure mirror of upstream: it never carries fork commits.
git branch -f --no-track main upstream/main

if ! git merge --ff-only "origin/$FORK_BRANCH" 2>/dev/null; then
    echo "Local $FORK_BRANCH has diverged from origin/$FORK_BRANCH, reconcile it first." >&2
    exit 1
fi

if git merge-base --is-ancestor upstream/main HEAD; then
    echo "$FORK_BRANCH already contains upstream/main ($(git rev-parse --short upstream/main))."
else
    echo "Merging $(git rev-list --count HEAD..upstream/main) upstream commits into $FORK_BRANCH..."
    if ! git merge --no-edit upstream/main; then
        echo
        echo "Conflicts to resolve (then 'git commit' and re-run this script):"
        git diff --name-only --diff-filter=U | sed 's/^/  /'
        exit 2
    fi
fi

# Keep the documentation served by the MCP server in step with the merged code.
python3 -I scripts/fork/build-setting-docs.py
if [ -n "$(git status --porcelain -- resources/ai_assistant)" ]; then
    git add resources/ai_assistant
    git commit -m "Regenerate MCP setting documentation"
fi

echo
echo "Fork changes to upstream files (should stay limited to the hook points listed in FORK.md):"
git diff --stat upstream/main...HEAD -- . \
    ':!src/slic3r/AIAssistant' ':!resources/ai_assistant' ':!scripts/fork' ':!FORK.md'

if [ "$BUILD" = 1 ]; then
    ./build_linux.sh -s
fi

if [ "$PUSH" = 1 ]; then
    git push origin main "$FORK_BRANCH"
fi
