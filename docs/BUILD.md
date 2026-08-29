# Building Word42

Word42 needs a C11 compiler, Meson (>= 1.0), Ninja, and GTK 4.10 or newer
along with Pango, Cairo and GLib. GTK 4.10 is the floor because Word42 uses
`GtkFileDialog`, `GtkFontDialog` and `GtkAlertDialog`, which arrived in that
release.

```sh
meson setup builddir
meson compile -C builddir
./builddir/src/word42
```

Useful configure options:

```sh
meson setup builddir --buildtype=debug      # -O0, assertions live
meson setup builddir --buildtype=release    # optimised
meson setup builddir --werror               # what CI uses on Linux and Windows
meson configure builddir -Dprefix=$HOME/.local
meson install -C builddir
```

## Linux

```sh
# Debian / Ubuntu
sudo apt install build-essential meson ninja-build pkg-config \
     libgtk-4-dev libpango1.0-dev libcairo2-dev libglib2.0-dev

# Fedora
sudo dnf install gcc meson ninja-build pkgconf-pkg-config \
     gtk4-devel pango-devel cairo-devel glib2-devel

# Arch
sudo pacman -S base-devel meson ninja pkgconf gtk4 pango cairo glib2
```

## macOS

```sh
brew install meson ninja pkg-config gtk4 pango cairo glib
```

Homebrew builds GTK 4 against native Quartz, so no X server is involved. If
`pkg-config` cannot find GTK, put `$(brew --prefix)/lib/pkgconfig` on
`PKG_CONFIG_PATH`.

## Windows

Build in the **MINGW64** shell of [MSYS2](https://www.msys2.org/) — not in
`cmd`, PowerShell, or the plain MSYS shell, each of which has a different
toolchain on `PATH`.

```sh
pacman -S --needed \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-meson \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-gtk4 \
  mingw-w64-x86_64-pango \
  mingw-w64-x86_64-cairo \
  mingw-w64-x86_64-glib2

meson setup builddir
meson compile -C builddir
./builddir/src/word42.exe
```

The executable links against MSYS2 DLLs, so it has to run with
`C:\msys64\mingw64\bin` on `PATH`. Launching it from Explorer, or from a shell
without that directory, produces a Windows error dialog saying the code
execution cannot proceed because `libglib-2.0-0.dll` was not found. Running it
from the MINGW64 shell just works. To ship a standalone build, copy the
dependent DLLs next to the binary; `ldd builddir/src/word42.exe` lists them.

Word42 is built with `win_subsystem: 'windows'`, so it does not open a console
window. To see warnings from GLib and GTK while debugging, change that to
`'console'` in `src/meson.build` and rebuild.

## Continuous integration

Every push and pull request builds on Linux (`ubuntu-24.04`, GCC), macOS
(`macos-14`, clang) and Windows (`windows-2025`, MSYS2/MinGW64 GCC).
Linux additionally runs the built binary under Xvfb to prove it links and
finds its resources. Linux and Windows build with `--werror`; macOS does
not, because Apple clang warns about a different set of things and a warning
it invents on its own should not turn the tree red.
