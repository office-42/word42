#!/usr/bin/env bash
# pack-msix.sh - make a Microsoft Store MSIX package out of the Windows
# bundle, from an MSYS2 MINGW64 shell.
#
# Copyright (C) 2026 Andreas Røsdal
# SPDX-License-Identifier: GPL-3.0-or-later
#
#     build-aux/pack-msix.sh [builddir] [dist] [outdir]
#
# It takes the tree bundle-windows.sh leaves (bin/word42.exe and the lib
# and share directories beside it), adds the Store's tile images rendered
# from the application icon and an AppxManifest filled in from
# data/msix/AppxManifest.xml.in, and packs the lot with makeappx.exe from
# the Windows 10/11 SDK.  Run bundle-windows.sh first; this script does
# not build.
#
# The package comes out unsigned, which is what the Store wants -- it
# signs the package itself once it has passed certification.  To install
# it on this machine instead, set W42_MSIX_CERT_PFX to a self-signed
# .pfx whose subject is exactly the Publisher below and it is signed with
# signtool.  docs/WINDOWS-STORE.md has the whole procedure.
#
# The identity below is a placeholder.  A real submission uses the values
# Partner Center shows under Product identity once the name is reserved:
#
#   W42_MSIX_IDENTITY_NAME      Package/Identity/Name
#   W42_MSIX_PUBLISHER          Package/Identity/Publisher (CN=<GUID>)
#   W42_MSIX_PUBLISHER_DISPLAY  PublisherDisplayName
#   W42_MSIX_PHONE_PRODUCT_ID   mp:PhoneIdentity/PhoneProductId
#   W42_MSIX_PHONE_PUBLISHER_ID mp:PhoneIdentity/PhonePublisherId
#
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
builddir=${1:-builddir}
dist=${2:-dist}
outdir=${3:-msix}

[ -d "$dist/bin" ] || {
  echo "pack-msix: no $dist/bin -- run build-aux/bundle-windows.sh first" >&2
  exit 1
}

version=$(sed -n "s/^  version: '\([^']*\)',/\1/p" "$root/meson.build" | head -1)
[ -n "$version" ] || { echo "pack-msix: no version in meson.build" >&2; exit 1; }

# The Store keeps the fourth field of the version for itself: it must be 0
# on submission, so the project's Major.Minor.Patch becomes X.Y.Z.0.
msix_version=${W42_MSIX_VERSION:-$(awk -F. '{printf "%d.%d.%d.0", $1, $2, $3}' <<<"${version%%-*}")}

identity_name=${W42_MSIX_IDENTITY_NAME:-AndreasRosdal.Word42}
publisher=${W42_MSIX_PUBLISHER:-CN=Andreas Rosdal}
publisher_display=${W42_MSIX_PUBLISHER_DISPLAY:-Andreas Røsdal}
display_name=${W42_MSIX_DISPLAY_NAME:-Word42}
phone_product_id=${W42_MSIX_PHONE_PRODUCT_ID:-6a1f9c34-4b2e-42f0-9f77-1b5c0a2d4e42}
phone_publisher_id=${W42_MSIX_PHONE_PUBLISHER_ID:-00000000-0000-0000-0000-000000000000}

template=$root/data/msix/AppxManifest.xml.in
svg=$root/data/icons/scalable/apps/org.word42.word42.svg
stage=$root/$outdir/stage
msix=$root/$outdir/word42-$version-win64.msix

command -v rsvg-convert >/dev/null 2>&1 || {
  echo "pack-msix: rsvg-convert not found (pacman -S mingw-w64-x86_64-librsvg)" >&2
  exit 1
}

rm -rf "$stage"
mkdir -p "$stage/Assets" "$root/$outdir"
cp -r "$dist"/. "$stage/"
# The licence and readme ride along in the bundle; the Store shows its own.
rm -f "$stage/README.md" "$stage/LICENSE"

# The tiles.  Windows picks the variant that fits the display, so each
# logo is rendered at its nominal size and at the five scale factors; the
# 44x44 one also gets the target sizes the taskbar and the Start list ask
# for, plated and not.
render () { rsvg-convert -w "$2" -h "$2" -f png -o "$stage/Assets/$1" "$svg"; }

render_scaled () {
  render "$1.png" "$2"
  for scale in 100 125 150 200 400; do
    render "$1.scale-$scale.png" $(( ($2 * scale + 50) / 100 ))
  done
}

# The wide tile is the square icon centred on a transparent strip.
render_wide () {
  local out=$1 w=$2 h=$3 b64 x
  b64=$(rsvg-convert -w "$h" -h "$h" -f png "$svg" | base64 -w0)
  x=$(( (w - h) / 2 ))
  printf '<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="%s" height="%s"><image x="%s" y="0" width="%s" height="%s" xlink:href="data:image/png;base64,%s"/></svg>' \
    "$w" "$h" "$x" "$h" "$h" "$b64" \
    | rsvg-convert -f png -o "$stage/Assets/$out"
}

render_wide_scaled () {
  render_wide "$1.png" "$2" "$3"
  for scale in 100 125 150 200 400; do
    render_wide "$1.scale-$scale.png" \
      $(( ($2 * scale + 50) / 100 )) $(( ($3 * scale + 50) / 100 ))
  done
}

render_scaled StoreLogo 50
render_scaled Square44x44Logo 44
render_scaled Square71x71Logo 71
render_scaled Square150x150Logo 150
render_scaled Square310x310Logo 310
render_wide_scaled Wide310x150Logo 310 150
for ts in 16 24 32 48 256; do
  render "Square44x44Logo.targetsize-$ts.png" "$ts"
  render "Square44x44Logo.targetsize-${ts}_altform-unplated.png" "$ts"
done

sed -e "s|@MSIX_VERSION@|$msix_version|g" \
    -e "s|@IDENTITY_NAME@|$identity_name|g" \
    -e "s|@PUBLISHER@|$publisher|g" \
    -e "s|@PUBLISHER_DISPLAY_NAME@|$publisher_display|g" \
    -e "s|@DISPLAY_NAME@|$display_name|g" \
    -e "s|@PHONE_PRODUCT_ID@|$phone_product_id|g" \
    -e "s|@PHONE_PUBLISHER_ID@|$phone_publisher_id|g" \
    "$template" > "$stage/AppxManifest.xml"

winpath () { command -v cygpath >/dev/null 2>&1 && cygpath -w "$1" || printf '%s' "$1"; }

# The SDK tools are not on PATH; they live under the kit, one directory
# per SDK version, and the newest will do.
kit_tool () {
  command -v "$1" >/dev/null 2>&1 && { command -v "$1"; return 0; }
  local dir found
  for dir in "/c/Program Files (x86)/Windows Kits/10/bin" \
             "/c/Program Files/Windows Kits/10/bin"; do
    [ -d "$dir" ] || continue
    found=$(find "$dir" -maxdepth 3 -name "$1" -path '*/x64/*' 2>/dev/null | sort -V | tail -1)
    [ -n "$found" ] && { printf '%s' "$found"; return 0; }
  done
  return 1
}

# resources.pri is the index that tells Windows which of those scale
# variants to use.  Without it only the base-named images are looked at:
# the package still installs, but Partner Center complains about the
# missing scale-200 assets.
#
# It is built from a directory holding nothing but the assets and the
# manifest.  Pointed at the whole bundle, makepri reads every filename in
# the GTK icon theme as a qualifier and buries the real work in warnings;
# and anything else left in that directory -- its own config, its log,
# the .pri it is writing -- it indexes as a resource too, so those live
# one level up.  The <packaging> block createconfig writes has to go as
# well: it splits the scales into separate resource packs, and this is
# one package.
if makepri=$(kit_tool makepri.exe); then
  pri=$root/$outdir/pri
  rm -rf "$pri"
  mkdir -p "$pri/index"
  cp -r "$stage/Assets" "$pri/index/"
  cp "$stage/AppxManifest.xml" "$pri/index/"
  # makepri says what went wrong on its standard output, in UTF-16, so
  # the log is kept and shown -- readably -- only when it fails.
  log=$root/$outdir/makepri.log
  run_makepri () {
    local status=0
    ( cd "$pri/index" && MSYS2_ARG_CONV_EXCL='*' "$makepri" "$@" ) \
      > "$log" 2>&1 || status=$?
    if [ "$status" -ne 0 ]; then
      echo "pack-msix: makepri $1 failed (status $status):" >&2
      { iconv -f UTF-16LE -t UTF-8 < "$log" 2>/dev/null || cat "$log"; } >&2
      return 1
    fi
  }
  run_makepri createconfig /cf ../priconfig-full.xml /dq en-US /o
  sed '/<packaging>/,/<\/packaging>/d' "$pri/priconfig-full.xml" \
    > "$pri/priconfig.xml"
  run_makepri new /pr . /cf ../priconfig.xml /of ../resources.pri \
              /mn AppxManifest.xml /o
  cp "$pri/resources.pri" "$stage/"
  rm -rf "$pri" "$log"
else
  echo "pack-msix: makepri.exe not found; the scaled tiles will go unused" >&2
fi

rm -f "$msix"
if makeappx=$(kit_tool makeappx.exe); then
  MSYS2_ARG_CONV_EXCL='*' "$makeappx" pack /o /d "$(winpath "$stage")" /p "$(winpath "$msix")"
elif command -v makemsix >/dev/null 2>&1; then
  makemsix pack -d "$stage" -p "$msix"
else
  echo "pack-msix: neither makeappx.exe (Windows SDK) nor makemsix was found." >&2
  echo "pack-msix: the staged package is in $stage; pack it by hand." >&2
  exit 1
fi

# Signing is for installing it here.  A Store submission goes up unsigned.
if [ -n "${W42_MSIX_CERT_PFX:-}" ]; then
  signtool=$(kit_tool signtool.exe) || {
    echo "pack-msix: W42_MSIX_CERT_PFX is set but signtool.exe was not found" >&2
    exit 1
  }
  MSYS2_ARG_CONV_EXCL='*' "$signtool" sign /fd SHA256 /a \
    /f "$(winpath "$W42_MSIX_CERT_PFX")" \
    ${W42_MSIX_CERT_PASS:+/p "$W42_MSIX_CERT_PASS"} "$(winpath "$msix")"
fi

printf 'packed %s (%s, identity %s, version %s)\n' \
  "$msix" "$(du -h "$msix" | cut -f1)" "$identity_name" "$msix_version"
