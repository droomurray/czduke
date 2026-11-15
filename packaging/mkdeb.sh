#!/bin/sh
# Usage: packaging/mkdeb.sh <binary> <output-dir>
# Builds an APPLaunch-compatible .deb for the CardputerZero.
set -eu

BIN="$1"
OUTDIR="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
VERSION="${VERSION:-0.1.0}"
PKG=czduke
PREFIX=/usr/share/APPLaunch
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
chmod 0755 "$STAGE"

install -D -m 0755 "$BIN" "$STAGE$PREFIX/apps/$PKG/czduke"
"${STRIP:-strip}" "$STAGE$PREFIX/apps/$PKG/czduke"
install -D -m 0644 "$HERE/duke3d.cfg" "$STAGE$PREFIX/apps/$PKG/duke3d.cfg"
install -D -m 0755 "$HERE/get-shareware" "$STAGE$PREFIX/apps/$PKG/get-shareware"
install -D -m 0755 "$HERE/czduke" "$STAGE$PREFIX/bin/czduke"
install -D -m 0644 "$HERE/czduke.desktop" "$STAGE$PREFIX/applications/$PKG.desktop"
install -D -m 0644 "$HERE/icon.png" "$STAGE$PREFIX/share/images/$PKG.png"

# The Build licence asks for BUILDLIC.TXT to travel with anything built from
# the engine, so both licences go into the package.
install -d -m 0755 "$STAGE/usr/share/doc/$PKG"
install -m 0644 "$HERE/../BUILDLIC.TXT" "$STAGE/usr/share/doc/$PKG/BUILDLIC.TXT"
{
    cat "$HERE/copyright"
    echo
    cat "$HERE/../LICENSE"
} >"$STAGE/usr/share/doc/$PKG/copyright"
chmod 0644 "$STAGE/usr/share/doc/$PKG/copyright"

mkdir -p "$STAGE/DEBIAN"
{
    echo "Package: $PKG"
    echo "Version: $VERSION"
    echo "Architecture: arm64"
    echo "Maintainer: czduke contributors"
    echo "Section: games"
    echo "Priority: optional"
    echo "Installed-Size: $(du -sk "$STAGE" | cut -f1)"
    echo "Depends: libc6, libsdl2-2.0-0"
    echo "Recommends: libfluidsynth3, timgm6mb-soundfont, python3"
    echo "Homepage: https://github.com/droomurray/czduke"
    echo "Description: Duke Nukem 3D for the M5Stack CardputerZero"
    echo " A JFDuke3D port that draws straight to the 320x170 panel and reads the"
    echo " built-in keyboard. Needs duke3d.grp from your own copy of the game"
    echo " (shareware or full) in ~/duke3d; no game data is included."
    echo " ."
    echo " Free of charge, non-commercial use only: the Build engine inside is under"
    echo " Ken Silverman's BUILD licence, the game code under the GNU GPL version 2."
} >"$STAGE/DEBIAN/control"

mkdir -p "$OUTDIR"
dpkg-deb --root-owner-group -Zxz -b "$STAGE" "$OUTDIR/${PKG}_${VERSION}_arm64.deb"
