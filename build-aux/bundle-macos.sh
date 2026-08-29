#!/usr/bin/env bash
# bundle-macos.sh - make a Word42.app from a Homebrew build, on macOS.
#
# Copyright (C) 2026 Andreas Røsdal
# SPDX-License-Identifier: GPL-3.0-or-later
#
#     build-aux/bundle-macos.sh builddir Word42.app
#
# The app holds the binary, every dylib it needs (found and rewritten by
# dylibbundler), GTK's schemas, the pixbuf loaders, the icons GTK asks for,
# the hyphenation patterns, and a launcher that points GLib and GDK at
# them.  It is not signed or notarised: Gatekeeper will want a right-click
# Open the first time.

set -uo pipefail
[ -n "${W42_BUNDLE_VERBOSE:-}" ] && set -x

builddir=${1:-builddir}
app=${2:-Word42.app}
prefix=$(brew --prefix)

contents="$app/Contents"
res="$contents/Resources"
rm -rf "$app"
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$res"

cp "$builddir/src/word42" "$contents/MacOS/word42-bin" || { echo "no word42 in $builddir/src" >&2; exit 1; }

# --- the launcher --------------------------------------------------------
# The pixbuf loader cache names its modules by absolute path, and nobody
# knows where the app will sit, so the launcher fills a template in.
cat > "$contents/MacOS/word42" <<'EOF'
#!/bin/bash
here="$(cd "$(dirname "$0")" && pwd)"
res="$(cd "$here/../Resources" && pwd)"
export GSETTINGS_SCHEMA_DIR="$res/share/glib-2.0/schemas"
export XDG_DATA_DIRS="$res/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"
export W42_HYPHEN_DIR="$res/share/hyphen"
if [ -f "$res/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache.in" ]; then
  cache="${TMPDIR:-/tmp}/word42-loaders-$$.cache"
  sed "s|@RES@|$res|g" "$res/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache.in" > "$cache"
  export GDK_PIXBUF_MODULE_FILE="$cache"
fi
exec "$here/word42-bin" "$@"
EOF
chmod +x "$contents/MacOS/word42"

# --- resources ------------------------------------------------------------
mkdir -p "$res/share/glib-2.0/schemas"
cp "$prefix"/share/glib-2.0/schemas/org.gtk.gtk4.Settings.*.xml "$res/share/glib-2.0/schemas/" 2>/dev/null || true
glib-compile-schemas "$res/share/glib-2.0/schemas" || true

loaders="$res/lib/gdk-pixbuf-2.0/2.10.0/loaders"
mkdir -p "$loaders"
for l in png jpeg gif bmp webp ico svg; do
  for f in "$prefix"/lib/gdk-pixbuf-2.0/2.10.0/loaders/*"$l"*.so; do
    [ -f "$f" ] && cp "$f" "$loaders/"
  done
done

mkdir -p "$res/share/icons"
cp -R "$prefix/share/icons/hicolor" "$res/share/icons/" 2>/dev/null || true
if [ -d "$prefix/share/icons/Adwaita" ]; then
  mkdir -p "$res/share/icons/Adwaita"
  cp "$prefix/share/icons/Adwaita/index.theme" "$res/share/icons/Adwaita/" 2>/dev/null || true
  for sub in scalable symbolic 16x16 24x24 32x32; do
    [ -d "$prefix/share/icons/Adwaita/$sub" ] && cp -R "$prefix/share/icons/Adwaita/$sub" "$res/share/icons/Adwaita/"
  done
  gtk4-update-icon-cache -q -t -f "$res/share/icons/Adwaita" 2>/dev/null || true
fi

if [ -d "$prefix/share/hyphen" ]; then
  mkdir -p "$res/share/hyphen"
  cp "$prefix"/share/hyphen/hyph_en_US.dic "$prefix"/share/hyphen/hyph_en_GB.dic "$res/share/hyphen/" 2>/dev/null || true
fi

# --- the icon ----------------------------------------------------------
iconset=$(mktemp -d)/word42.iconset
mkdir -p "$iconset"
icons=data/icons
cp "$icons/16x16/apps/org.word42.word42.png"   "$iconset/icon_16x16.png"
cp "$icons/32x32/apps/org.word42.word42.png"   "$iconset/icon_16x16@2x.png"
cp "$icons/32x32/apps/org.word42.word42.png"   "$iconset/icon_32x32.png"
cp "$icons/64x64/apps/org.word42.word42.png"   "$iconset/icon_32x32@2x.png"
cp "$icons/128x128/apps/org.word42.word42.png" "$iconset/icon_128x128.png"
cp "$icons/256x256/apps/org.word42.word42.png" "$iconset/icon_128x128@2x.png"
cp "$icons/256x256/apps/org.word42.word42.png" "$iconset/icon_256x256.png"
iconutil -c icns "$iconset" -o "$res/word42.icns" || echo "iconutil failed; no icon" >&2

version=$(sed -n "s/^  version: '\([^']*\)',/\1/p" meson.build | head -1)
cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Word42</string>
  <key>CFBundleDisplayName</key><string>Word42</string>
  <key>CFBundleIdentifier</key><string>org.word42.word42</string>
  <key>CFBundleVersion</key><string>${version:-0.0.0}</string>
  <key>CFBundleShortVersionString</key><string>${version:-0.0.0}</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>word42</string>
  <key>CFBundleIconFile</key><string>word42</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>CFBundleDocumentTypes</key>
  <array>
    <dict>
      <key>CFBundleTypeName</key><string>Rich Text Document</string>
      <key>CFBundleTypeRole</key><string>Editor</string>
      <key>LSItemContentTypes</key><array><string>public.rtf</string></array>
    </dict>
    <dict>
      <key>CFBundleTypeName</key><string>Word Document</string>
      <key>CFBundleTypeRole</key><string>Viewer</string>
      <key>LSItemContentTypes</key><array><string>com.microsoft.word.doc</string></array>
    </dict>
    <dict>
      <key>CFBundleTypeName</key><string>Plain Text</string>
      <key>CFBundleTypeRole</key><string>Editor</string>
      <key>LSItemContentTypes</key><array><string>public.plain-text</string></array>
    </dict>
  </array>
</dict>
</plist>
EOF

# --- the libraries ---------------------------------------------------------
# dylibbundler copies every Homebrew dylib the binary (and the loaders)
# link to into Frameworks and rewrites the install names to find them
# there; -od overwrites, -b bundles, -s adds a search path.
targets=("$contents/MacOS/word42-bin")
for f in "$loaders"/*.so; do [ -f "$f" ] && targets+=("$f"); done
args=()
for t in "${targets[@]}"; do args+=(-x "$t"); done
dylibbundler -od -b -d "$contents/Frameworks" -p @executable_path/../Frameworks \
  -s "$prefix/lib" "${args[@]}" > /dev/null || { echo "dylibbundler failed" >&2; exit 1; }

# --- the check ----------------------------------------------------------------
# Nothing the app runs may still point at Homebrew.  A pixbuf loader that
# does (librsvg's pulls in a chain of its own) is left out rather than
# shipped broken; word42's own icons are resources and need no loader.
bad=0
for f in "$contents/MacOS/word42-bin" "$contents/Frameworks"/*.dylib; do
  [ -f "$f" ] || continue
  if otool -L "$f" | grep -q "$prefix"; then
    echo "still linked to $prefix: $f" >&2
    otool -L "$f" | grep "$prefix" >&2
    bad=1
  fi
done
[ "$bad" -eq 0 ] || exit 1
for f in "$loaders"/*.so; do
  [ -f "$f" ] || continue
  if otool -L "$f" | grep -q "$prefix"; then
    echo "leaving out $(basename "$f"): still linked to $prefix" >&2
    rm -f "$f"
  fi
done

# The loader cache, with the module paths made relative to Resources.
( cd "$res" && GDK_PIXBUF_MODULEDIR="$loaders" gdk-pixbuf-query-loaders "$loaders"/*.so 2>/dev/null ) \
  | sed "s|$(cd "$res" && pwd)|@RES@|g" > "$res/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache.in" || true

echo "bundled $(ls "$contents/Frameworks" | wc -l | tr -d ' ') libraries into $app"
