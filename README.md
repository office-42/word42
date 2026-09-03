# Word42

<img src="data/icons/about.svg" width="315"
     alt="The Word42 logo: a sheet of paper marked W42, beside the name in blue underlined serif">

A classic word processor, written from scratch in C on GTK 4, Pango and
Cairo: a menu bar, two toolbars, a ruler, a status bar and a page. It aims
at the shape and restraint of the word processors of 1993 — and at the
feature set of [AbiWord](https://github.com/AbiWord/abiword/) — on a modern
text stack that does Unicode, OpenType and complex scripts properly.

word42.org

[![Linux](https://github.com/office-42/word42/actions/workflows/linux.yml/badge.svg)](https://github.com/office-42/word42/actions/workflows/linux.yml)
[![macOS](https://github.com/office-42/word42/actions/workflows/macos.yml/badge.svg)](https://github.com/office-42/word42/actions/workflows/macos.yml)
[![Windows](https://github.com/office-42/word42/actions/workflows/windows.yml/badge.svg)](https://github.com/office-42/word42/actions/workflows/windows.yml)

![Word42 in Page Layout view: the opening of Genesis — "In the beginning God created the heaven and the earth" — set as justified verses with superscript numbers under a centred title and a picture of light breaking over the waters, a dove descending](docs/images/screenshot.png)

A working word processor: typing, formatting, styles and numbered headings,
headers and footers, footnotes and endnotes, tables, pictures, lists,
columns, find and replace, spelling, hyphenation, mail merge, a split
window, a page-layout view and print preview — reading and writing RTF,
OpenDocument .odt, Word .docx and .doc, AbiWord .abw, HTML, PDF and plain
text. [docs/STATUS.md](docs/STATUS.md) lists what it does,
[Word42.md](Word42.md) what it does not do yet, and the
[user guide](docs/GUIDE.md) describes every command in the program.

## Getting it

Every push to `main` builds a Windows bundle and installer (artifacts of
the [Windows workflow](https://github.com/office-42/word42/actions/workflows/windows.yml))
and an unsigned `Word42.app` for macOS (the macOS workflow's
`word42-macos-app`; first launch is a right-click ▸ Open). Linux builds
from source; a Flatpak manifest is in `build-aux/`.

## Building

Word42 needs a C11 compiler, Meson, Ninja, and GTK 4.10 or newer with
Pango, Cairo and gdk-pixbuf, plus [Lexbor](https://lexbor.com/) for HTML.
Reading PDF needs poppler-glib; without it Word42 still builds, writes PDF,
and simply cannot open one. Per-platform dependency lists are in
[docs/BUILD.md](docs/BUILD.md).

```sh
meson setup builddir
meson compile -C builddir
./builddir/src/word42
```

## How it works

The document is a **piece table** — the design several word processors have
used since the 1980s — with formatting interned in an attribute table,
symmetric undo records, structural marks for paragraphs and sections, and
pagination in twips at a fixed reference resolution, so a page breaks in
the same place at every zoom and on every screen.
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) tells the whole story.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).

Word42 is not affiliated with, endorsed by, or derived from any Microsoft
product or from AbiWord; both are named only as the yardsticks Word42
measures itself against and for the file formats it reads and writes. It
contains no code, artwork, fonts or text from either; its icons, logo and
stylesheet are its own work.
