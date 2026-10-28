#!/usr/bin/env bash
# Fork-only: builds the pacman package of the fork (Arch Linux, CachyOS) from the slicer built
# by ./build_linux.sh -s. The package lands in build/; install it with sudo pacman -U <file>.
set -euo pipefail

REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
WORK="$REPO/build/arch-package"

# The Snapmaker Connect web pages must be the ones of the bundled gateway (staged by
# build_linux.sh into resources/web/flutter_web): the copy tracked by git is older and its
# login no longer works with the slicer.
want=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["releaseTag"].lstrip("v"))' "$REPO/.github/connect-bundle.lock.json")
have=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$REPO/build/package/resources/web/flutter_web/version.json")
if [ "$want" != "$have" ]; then
    echo "build/package has the Snapmaker Connect pages $have instead of $want: run" >&2
    echo "  python3 scripts/packaging/stage_connect_bundle.py --platform linux-x64 then rebuild." >&2
    exit 1
fi

rm -rf "$WORK"
mkdir -p "$WORK"
cp "$REPO/scripts/fork/arch/PKGBUILD" "$WORK/"
cd "$WORK"
PKGDEST="$REPO/build" makepkg --force
ls -1 "$REPO"/build/snapmaker-orca-ai-*.pkg.tar.zst
