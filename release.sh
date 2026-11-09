#!/bin/bash
#
# frank-ecat — eCat3 (retro computers emulator) on RP2350 (FRANK M2)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# https://github.com/rh1tech/frank-ecat
# SPDX-License-Identifier: GPL-3.0-or-later
#
# release.sh — the release files of frank-ecat
#
# Usage: ./release.sh VERSION [--publish]
#   VERSION    e.g. 1.00 (written to version.txt, shown in Emulator info)
#   --publish  also tag vVERSION, push the tag and create the GitHub release
#              (gh CLI) with the files and release/notes_A_BB.md
#
# Output, in release/:
#   m2p2_frank-ecat-<family>_A_BB.uf2   one per family: USB keyboard host,
#                                       debug console on UART0 (CONSOLE=uart)
#   frank-ecat-sdcard_A_BB.zip          /ecat/software/<machine type>/ for the card
#
# Needs the host console build of eCat3 in build-host (see README).

set -e
cd "$(dirname "$0")"

FAMILIES="rk86 agat bk dvk uknc e85 poisk orion irisha unior pravetz"

VERSION_DOT="${1:?usage: ./release.sh VERSION [--publish]}"
PUBLISH=0
[ "${2:-}" = "--publish" ] && PUBLISH=1
MAJOR=$((10#${VERSION_DOT%%.*}))
MINOR=$((10#${VERSION_DOT##*.}))
VERSION_DOT="${MAJOR}.$(printf '%02d' $MINOR)"
VERSION="${MAJOR}_$(printf '%02d' $MINOR)"

[ -x build-host/eCat3-headless ] || { echo "no build-host/eCat3-headless: build eCat3 for the host first (README)" >&2; exit 1; }

echo "$MAJOR $(printf '%02d' $MINOR)" > version.txt
echo "frank-ecat $VERSION_DOT"

mkdir -p release
# the core first: in a fresh build directory the families' hot-function
# lists (tools/hot_sections.py) would ask for its objects before they exist
BUILD_DIR=build-release CONSOLE=uart ./build.sh ecat_core
BUILD_DIR=build-release CONSOLE=uart ./build.sh $(for f in $FAMILIES; do printf 'frank-ecat-%s ' "$f"; done)
for f in $FAMILIES; do
    cp "build-release/frank-ecat-$f.uf2" "release/m2p2_frank-ecat-${f}_${VERSION}.uf2"
done

ZIP="$PWD/release/frank-ecat-sdcard_${VERSION}.zip"
STAGE=$(mktemp -d)
mkdir -p "$STAGE/ecat"
cp -R ecat3/deploy/software "$STAGE/ecat/"
cat > "$STAGE/README.txt" <<EOF
frank-ecat $VERSION_DOT, SD card
================================

Copy the "ecat" folder to the root of a FAT32 microSD card:

  /ecat/software/<machine type>/   disk and tape images, opened with F11

The machines' ROMs and configurations are in the firmware (UF2); the card
holds software only. Your own images go into the same folders. The firmware
writes its own files there too: /ecat/frank.ini (F12, "Save settings to the
card") and /ecat/<family>.last (the last model used).

https://github.com/rh1tech/frank-ecat
EOF
find "$STAGE" -name .DS_Store -delete
rm -f "$ZIP"
(cd "$STAGE" && COPYFILE_DISABLE=1 zip -rqX "$ZIP" README.txt ecat)
rm -rf "$STAGE"

ls -l release/*_"${VERSION}".*

if [ "$PUBLISH" = 1 ]; then
    NOTES="release/notes_${VERSION}.md"
    [ -f "$NOTES" ] || { echo "write $NOTES first" >&2; exit 1; }
    git tag -a "v$VERSION_DOT" -m "frank-ecat $VERSION_DOT"
    git push origin "v$VERSION_DOT"
    gh release create "v$VERSION_DOT" --title "frank-ecat $VERSION_DOT" --notes-file "$NOTES" \
        release/*_"${VERSION}".uf2 "$ZIP"
fi
