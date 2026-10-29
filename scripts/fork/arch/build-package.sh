#!/usr/bin/env bash
# Fork-only: builds the pacman package of the fork (Arch Linux, CachyOS) from the slicer built
# by ./build_linux.sh -s, or from an AppImage of the fork (release workflow). The package lands in
# build/ (or $PKGDEST); install it with sudo pacman -U <file>.
#   build-package.sh                 # packages build/package
#   build-package.sh <file.AppImage> # packages the contents of the AppImage
# MAKEPKG_ARGS adds makepkg options (e.g. --nodeps where the runtime libraries are not installed).
set -euo pipefail

REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
WORK="$REPO/build/arch-package"
STAGED="$REPO/build/package"

rm -rf "$WORK"
mkdir -p "$WORK"

if [ $# -ge 1 ]; then
    # The AppImage holds build/package with the launcher renamed AppRun, plus its own desktop
    # entry and icons, which the package installs from the sources instead.
    appimage="$(realpath "$1")"
    (cd "$WORK" && "$appimage" --appimage-extract >/dev/null)
    STAGED="$WORK/squashfs-root"
    mv "$STAGED/AppRun" "$STAGED/snapmaker-orca"
    rm -rf "$STAGED/usr" "$STAGED/.DirIcon" "$STAGED/Snapmaker_Orca.png" "$STAGED"/*.desktop
fi

# The Snapmaker Connect web pages must be the ones of the bundled gateway (staged by
# build_linux.sh into resources/web/flutter_web): the copy tracked by git is older and its
# login no longer works with the slicer.
want=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["releaseTag"].lstrip("v"))' "$REPO/.github/connect-bundle.lock.json")
have=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$STAGED/resources/web/flutter_web/version.json")
if [ "$want" != "$have" ]; then
    echo "$STAGED has the Snapmaker Connect pages $have instead of $want: run" >&2
    echo "  python3 scripts/packaging/stage_connect_bundle.py --platform linux-x64 then rebuild." >&2
    exit 1
fi

cp "$REPO/scripts/fork/arch/PKGBUILD" "$WORK/"
cd "$WORK"
export PKGDEST="${PKGDEST:-$REPO/build}"
# shellcheck disable=SC2086 # MAKEPKG_ARGS holds several options
SNAPMAKER_ORCA_STAGED="$STAGED" makepkg --force ${MAKEPKG_ARGS:-}
ls -1 "$PKGDEST"/snapmaker-orca-ai-*.pkg.tar.zst
