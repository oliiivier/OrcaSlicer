#!/usr/bin/env bash
# Fork-only: builds the slicer for Linux, Windows and macOS on GitHub Actions and publishes the
# packages as a pre-release (workflow .github/workflows/fork_release.yml). See FORK.md.
#
# Actions stays disabled on the fork so that the upstream workflows never run: it is enabled
# for the release only, with every other workflow disabled, then disabled again.
set -euo pipefail

REPO="oliiivier/OrcaSlicer"
FORK_BRANCH="ai-assistant"
WORKFLOW="fork_release.yml"

usage() {
    cat <<EOF
Usage: $(basename "$0") <tag>
  Publishes the pushed head of $FORK_BRANCH as the pre-release <tag> (e.g. v2.4.1-ai.1).
  The builds take several hours; the script waits for them.
EOF
}

[ $# -eq 1 ] || { usage; exit 1; }
TAG="$1"

cd "$(git rev-parse --show-toplevel)"
git fetch -q origin "$FORK_BRANCH"
if [ "$(git rev-parse HEAD)" != "$(git rev-parse "origin/$FORK_BRANCH")" ]; then
    echo "HEAD is not the pushed $FORK_BRANCH: push it first." >&2
    exit 1
fi

# The workflow token cannot create the tag when the commit brings changes to .github/workflows
# (upstream merges do): the tag is pushed over git and the pre-release created here, the
# workflow then only uploads the packages.
if tagged=$(git rev-parse -q --verify "refs/tags/$TAG^{commit}"); then
    [ "$tagged" = "$(git rev-parse HEAD)" ] || { echo "Tag $TAG already exists on another commit." >&2; exit 1; }
else
    git tag "$TAG" HEAD
fi
git push -q origin "refs/tags/$TAG"
if ! gh release view -R "$REPO" "$TAG" >/dev/null 2>&1; then
    gh release create -R "$REPO" "$TAG" --prerelease --verify-tag \
        --title "Snapmaker Orca + assistant IA $TAG" --notes-file scripts/fork/release-notes.md
fi

set_actions() {
    gh api -X PUT "repos/$REPO/actions/permissions" -F enabled="$1" >/dev/null
}

set_actions true
trap 'set_actions false; echo "GitHub Actions disabled again."' EXIT

# Workflows added by upstream since the last release become active with Actions.
gh workflow list -R "$REPO" --all --json path,state \
    -q ".[] | select(.state == \"active\" and .path != \".github/workflows/$WORKFLOW\") | .path" |
    while read -r path; do
        gh workflow disable -R "$REPO" "$(basename "$path")"
    done
gh workflow enable -R "$REPO" "$WORKFLOW" 2>/dev/null || true

gh workflow run -R "$REPO" "$WORKFLOW" --ref "$FORK_BRANCH" -f tag="$TAG"
# The run takes a moment to appear.
sleep 10
RUN_ID=$(gh run list -R "$REPO" --workflow "$WORKFLOW" --branch "$FORK_BRANCH" --limit 1 --json databaseId -q '.[0].databaseId')
echo "Run: https://github.com/$REPO/actions/runs/$RUN_ID"
gh run watch -R "$REPO" "$RUN_ID" --interval 120 --exit-status || true

gh release view -R "$REPO" "$TAG" --json url,assets -q '.url, (.assets[] | "  " + .name)' ||
    echo "No release $TAG: see the run above." >&2
