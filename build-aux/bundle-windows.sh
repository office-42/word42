#!/usr/bin/env bash
# bundle-windows.sh - gather word42.exe and everything it needs to run on a
# Windows machine without MSYS2, from an MSYS2 MINGW64 shell.
#
# Copyright (C) 2026 Andreas Røsdal
# SPDX-License-Identifier: GPL-3.0-or-later
#
#     build-aux/bundle-windows.sh builddir dist
#
# leaves a runnable tree in dist/ (dist/bin/word42.exe), which the CI zips.
# The DLLs come from ldd, followed to a fixed point so that the pixbuf
# loaders' and spelling providers' own dependencies come too.

# No -e: an optional file missing from the image must not stop the bundle.
set -uo pipefail

builddir=${1:-builddir}
dist=${2:-dist}
prefix=${MINGW_PREFIX:-/mingw64}

rm -rf "$dist"
mkdir -p "$dist/bin" "$dist/lib" "$dist/share"

cp "$builddir/src/word42.exe" "$dist/bin/" || { echo "no word42.exe in $builddir/src" >&2; exit 1; }

# The GDK pixbuf loaders for the everyday formats, and a cache naming just
# those, with paths relative to the bundle as MSYS2's own are.  The AVIF,
# HEIF, JPEG XL and TIFF loaders are left out: each drags in a video codec
# the size of everything else put together.
loaders="$dist/lib/gdk-pixbuf-2.0/2.10.0/loaders"
mkdir -p "$loaders"
for l in png jpeg gif bmp webp ico; do
  f="$prefix/lib/gdk-pixbuf-2.0/2.10.0/loaders/libpixbufloader-$l.dll"
  [ -f "$f" ] && cp "$f" "$loaders/"
done
[ -f "$prefix/lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll" ] &&
  cp "$prefix/lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll" "$loaders/"
( cd "$dist" && gdk-pixbuf-query-loaders lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll ) \
  | sed 's#^"[^"]*/lib/gdk-pixbuf-2.0/#"lib/gdk-pixbuf-2.0/#' > "$dist/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache" || true

# Enchant's Hunspell provider and the English dictionaries.
if [ -d "$prefix/lib/enchant-2" ]; then
  mkdir -p "$dist/lib/enchant-2"
  cp "$prefix"/lib/enchant-2/enchant_hunspell.dll "$dist/lib/enchant-2/" 2>/dev/null || true
fi
[ -d "$prefix/share/enchant" ] && cp -r "$prefix/share/enchant" "$dist/share/" || true
if [ -d "$prefix/share/hunspell" ]; then
  mkdir -p "$dist/share/hunspell"
  cp "$prefix"/share/hunspell/en_US.* "$prefix"/share/hunspell/en_GB.* "$dist/share/hunspell/" 2>/dev/null || true
fi

# Hyphenation patterns for English; libhyphen itself is static.
if [ -d "$prefix/share/hyphen" ]; then
  mkdir -p "$dist/share/hyphen"
  cp "$prefix"/share/hyphen/hyph_en_US.dic "$prefix"/share/hyphen/hyph_en_GB.dic "$dist/share/hyphen/" 2>/dev/null || true
fi

# GLib schemas (GTK needs its own), compiled.
mkdir -p "$dist/share/glib-2.0/schemas"
cp "$prefix"/share/glib-2.0/schemas/org.gtk.gtk4.Settings.*.xml "$dist/share/glib-2.0/schemas/" 2>/dev/null || true
cp "$prefix"/share/glib-2.0/schemas/gschema.dtd "$dist/share/glib-2.0/schemas/" 2>/dev/null || true
glib-compile-schemas "$dist/share/glib-2.0/schemas" || true

# Icons GTK's own widgets ask for (dialogs, spinners), and word42's.
mkdir -p "$dist/share/icons"
cp -r "$prefix/share/icons/hicolor" "$dist/share/icons/" 2>/dev/null || true
if [ -d "$prefix/share/icons/Adwaita" ]; then
  mkdir -p "$dist/share/icons/Adwaita"
  cp "$prefix/share/icons/Adwaita/index.theme" "$dist/share/icons/Adwaita/" 2>/dev/null || true
  for sub in scalable symbolic 16x16 24x24 32x32; do
    [ -d "$prefix/share/icons/Adwaita/$sub" ] && cp -r "$prefix/share/icons/Adwaita/$sub" "$dist/share/icons/Adwaita/" || true
  done
fi
gtk4-update-icon-cache -q -t -f "$dist/share/icons/Adwaita" 2>/dev/null || true

# Fontconfig and the GTK settings that make it look like Windows.
[ -d "$prefix/etc/fonts" ] && { mkdir -p "$dist/etc"; cp -r "$prefix/etc/fonts" "$dist/etc/"; } || true
mkdir -p "$dist/etc/gtk-4.0"
cat > "$dist/etc/gtk-4.0/settings.ini" <<'EOF'
[Settings]
gtk-font-name=Segoe UI 9
EOF

# The DLLs: everything ldd finds under the prefix, for the exe and for
# every DLL copied so far, until nothing new turns up.
copy_deps () {
  local target=$1
  ldd "$target" 2>/dev/null | awk '{print $3}' | grep -i "^$prefix/" | while read -r dll; do
    local base
    base=$(basename "$dll")
    if [ ! -f "$dist/bin/$base" ]; then
      cp "$dll" "$dist/bin/"
      echo "$dist/bin/$base"
    fi
  done
}

queue=("$dist/bin/word42.exe")
for f in "$dist"/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.dll "$dist"/lib/enchant-2/*.dll; do
  [ -f "$f" ] && queue+=("$f")
done
while [ ${#queue[@]} -gt 0 ]; do
  next=()
  for f in "${queue[@]}"; do
    while read -r added; do
      [ -n "$added" ] && next+=("$added")
    done < <(copy_deps "$f")
  done
  queue=("${next[@]}")
done

# Not a dependency ldd sees: GLib loads it for TLS, GTK for input methods.
for extra in gspawn-win64-helper.exe gspawn-win64-helper-console.exe; do
  [ -f "$prefix/bin/$extra" ] && cp "$prefix/bin/$extra" "$dist/bin/" || true
done

echo "bundled $(ls "$dist/bin" | wc -l) files into $dist/bin"
[ -f "$dist/bin/libgtk-4-1.dll" ] || { echo "GTK's DLL was not found by ldd; the bundle would not run" >&2; exit 1; }
