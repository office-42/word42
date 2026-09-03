# Word42

![The Word42 logo: a sheet of paper marked W42, beside the name in blue underlined serif](data/icons/about.svg)

A classic word processor, written from scratch in C on GTK 4, Pango and
Cairo: a menu bar, two toolbars, a ruler, a status bar and a page.

word42.org

[![Linux](https://github.com/office-42/word42/actions/workflows/linux.yml/badge.svg)](https://github.com/office-42/word42/actions/workflows/linux.yml)
[![macOS](https://github.com/office-42/word42/actions/workflows/macos.yml/badge.svg)](https://github.com/office-42/word42/actions/workflows/macos.yml)
[![Windows](https://github.com/office-42/word42/actions/workflows/windows.yml/badge.svg)](https://github.com/office-42/word42/actions/workflows/windows.yml)

![Word42 in Page Layout view: a document with a title, headings, a bulleted list, a dropped capital, a footnote, a line of text in eight scripts, a right-to-left Arabic paragraph, a table, a contents block with dotted leaders and a picture](docs/images/screenshot.png)

The [user guide](docs/GUIDE.md) describes every command in the program.

## Goals

Word42 measures itself against two yardsticks:

- the feature set of **[AbiWord](https://github.com/AbiWord/abiword/)**, a
  free word processor of long standing;
- the shape and restraint of the word processors of 1993 — Word 6 among
  them — which fitted on a handful of floppy disks and had a menu bar, two
  toolbars, a ruler, a status bar and a white galley you typed into, and were
  genuinely capable.

Word42 aims at that shape on a modern text stack that does Unicode, OpenType
and complex scripts properly. Its look is its own work: a navy title bar
drawn by the program itself, silver chrome with a four-tone bevel on every
button and the bevel inverted on every field, and a Normal view that is a
plain white galley, and a Page Layout view that opens by default: white
sheets centred on a light grey desk, each with a hairline round it, the
text smoothed and lightly hinted.

## Status

A working word processor. Typing, formatting, styles and numbered
headings, headers and footers, footnotes and endnotes, tables, pictures,
lists, columns, find and replace, spelling, hyphenation, mail merge, a
split window, a page-layout view and print preview — and it reads and
writes RTF, OpenDocument .odt, Word .docx and .doc, AbiWord .abw, HTML,
PDF and plain text.

The full list of what it does is in [docs/STATUS.md](docs/STATUS.md).
What it does not do yet is listed in [Word42.md](Word42.md), with the
internals worth doing regardless in [docs/ROADMAP.md](docs/ROADMAP.md).

## Getting it

Every push to `main` builds a Windows bundle — `word42.exe` with the GTK,
Pango, Cairo, pixbuf and spelling libraries it needs, about 80 MB
unpacked — as the `word42-windows` artifact of the
[Windows workflow](https://github.com/office-42/word42/actions/workflows/windows.yml).
Unzip it anywhere and run `bin\word42.exe`; nothing is installed. The same
workflow also makes a Windows installer, `word42-<version>-setup.exe`, as
the `word42-windows-installer` artifact: it puts Word42 on the Start menu
and can, if asked, take over `.rtf` files. The macOS workflow makes a
`Word42.app` (as `word42-macos-app`, a zip): unsigned, so the first launch
is a right-click ▸ Open. Linux builds from source (below); a Flatpak
manifest is in `build-aux/`.

## Building

Word42 needs a C11 compiler, Meson, Ninja, and GTK 4.10 or newer with Pango,
Cairo and gdk-pixbuf, plus [Lexbor](https://lexbor.com/), the HTML5 parser
that reads HTML for it. Reading PDF needs poppler-glib; without it Word42
still builds and still writes PDF, and simply cannot open one.

```sh
meson setup builddir
meson compile -C builddir
./builddir/src/word42
```

Per-platform dependency lists are in [docs/BUILD.md](docs/BUILD.md).

## How it works

The interesting part of a word processor is not the widgets, it is the data
structure underneath them. Word42 uses a **piece table**, the same design
several word processors have used since the 1980s.

The text of a file as loaded goes into an immutable *initial buffer*.
Everything typed since goes into an append-only *change buffer*. The document
proper is a linked list of **pieces**, each of which names a stretch of one
buffer or the other:

```
initial buffer:  "The quick brown fox"
change buffer:   "very "

pieces:  [strux SECTION]
         [strux BLOCK]
         [text initial 0..4  ap=0]     -> "The "
         [text change  0..5  ap=0]     -> "very "
         [text initial 4..19 ap=1]     -> "quick brown fox"  (bold)
```

Nothing is ever overwritten and nothing is ever removed from a buffer. A
deletion just drops the pieces that referred to the text; the characters stay
where they were. That is what makes unlimited undo cost almost nothing, and it
is why the bytes you loaded are still intact if the program falls over.

Formatting does not live in the pieces. Each piece holds an **AP index** into
an interned attribute/property table, so a hundred-page document set in three
fonts costs three formatting records, and comparing formatting is an integer
compare.

Undo records are symmetric: applying one performs the *opposite* of the edit it
describes and hands back the record that would redo it. One routine drives both
directions, and a stack entry simply flips each time it is used.

Structure is expressed with **struxes** — marks that occupy a document
position of their own, like characters do. A paragraph is a `BLOCK` strux
followed by the characters belonging to it. Pressing Enter inserts a `BLOCK`
strux; pressing Backspace at the start of a paragraph deletes the one in front
of the caret. Splitting and merging paragraphs need no special cases at all.

Layout happens at a fixed 96 dpi reference resolution, in Word's own unit of
**twips** (1/1440 inch). Zoom is a Cairo scale applied at paint time, so a
page breaks in exactly the same place at 75% as at 200%, and on a HiDPI screen
as on an old one. Pango does line breaking, shaping and bidi; Word42's
pagination places the resulting lines onto numbered pages.

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) goes into detail, and
[docs/GUIDE.md](docs/GUIDE.md) is the user guide.

## Layout of the source

```
src/util/      shared types and unit conversions
src/model/     the piece table, the AP table, the document object
src/layout/    pagination, and mapping between positions and the page
src/io/        importers and exporters
src/ui/        the view, the ruler, the window, the application
data/          icons, menus, stylesheet, desktop entry
docs/          design notes
```

## Prior art

Word42 studies two projects and copies neither's code:

- **[AbiWord](https://github.com/AbiWord/abiword)** for the document model.
  Its `src/README.TXT` is the clearest description of a piece table in a real
  word processor that exists, and Word42's model is a deliberate C rendering of
  the design it describes. AbiWord is GPL-2.0; Word42 is an independent
  implementation and shares no code with it.
- **[northstar-browser](https://github.com/nordstjernen-web/northstar-browser)**
  for how to ship a large C/GTK 4 program: Meson, a flat `src/` tree, and CI
  that builds on Linux, macOS and Windows on every push.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).

    Word42 is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

Word42 is not affiliated with, endorsed by, or derived from any Microsoft
product or from AbiWord. "Microsoft Word" is a trademark of Microsoft
Corporation and "AbiWord" is the name of a separate free software project;
both are named here only to describe the programs Word42 measures itself
against and the file formats it reads and writes. Word42 contains no code,
artwork, fonts or text from either; its icons, logo and stylesheet are its
own work.
