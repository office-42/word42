# Word42

<img src="data/icons/about.svg" width="315"
     alt="The Word42 logo: a sheet of paper marked W42, beside the name in blue underlined serif">

A classic word processor, written from scratch in C on GTK 4, Pango and
Cairo: a menu bar, two toolbars, a ruler, a status bar and a page.

![Word42 in Page Layout view, open at the first chapter of Jonah in the King James Bible sample — "Now the word of the Lord came unto Jonah" — the verses set justified with superscript numbers under the book's heading, running down the left of a picture of the great fish in the deep, on page 1070 of 1464](docs/images/screenshot.png)

A working word processor: typing, formatting, styles and numbered headings,
headers and footers, footnotes and endnotes, tables, pictures, lists,
columns, page borders, find and replace, spelling, a thesaurus,
hyphenation, mail merge, macros in a dialect of VBA, a Document Map,
Compare Documents, a split window, Normal, Online Layout and Page Layout
views and print preview — reading and writing
RTF, OpenDocument .odt, Word .docx and .doc, AbiWord .abw, HTML, PDF and
plain text, and writing EPUB e-books. Word 97 is the yardstick: its menus
are the specification and [docs/PARITY.md](docs/PARITY.md) keeps the score against it.
[docs/STATUS.md](docs/STATUS.md) lists what it does,
[Word42.md](Word42.md) what it does not do yet,
[docs/COMPARISON.md](docs/COMPARISON.md) sets it beside Word, AbiWord
and LibreOffice Writer on size, speed, files and features, and the
[user guide](docs/GUIDE.md) describes every command in the program.

Word42 speaks the language of the desktop it runs on. With English, its
menus, dialogs and messages come in the ten most spoken languages in the
world: Chinese (Simplified), Hindi, Spanish, Arabic, French, Bengali,
Portuguese, Russian and Urdu, and for Arabic and Urdu the window is laid
out from the right. The user guide is in English.

## For writers

Word42 is made to write books in. AutoCorrect sets quotation marks and
dialogue dashes the way the text's language does — « » and – in
Norwegian — and the spelling, thesaurus and hyphenation follow the
document's language; the status bar counts the words toward a goal, the
Document Map weighs the chapters against each other, and a Novel
template, typewriter scrolling, backup copies, EPUB export and
`word42 --convert-to=pdf|epub|odt FILE` do the rest.
[docs/morild](docs/morild/) is *Morild*, a 150-page science-fiction novel
in Norwegian written in Word42, as its manuscript, a PDF and an e-book.

## Getting it

Every push to `main` builds a Windows bundle and installer (artifacts of
the [Windows workflow](https://github.com/office-42/word42/actions/workflows/windows.yml))
and an unsigned `Word42.app` for macOS (the macOS workflow's
`word42-macos-app`; first launch is a right-click ▸ Open). Linux builds
from source; a Flatpak manifest is in `build-aux/`.

## Building

Word42 needs a C11 compiler, Meson, Ninja, and GTK 4.10 or newer with
Pango, Cairo and gdk-pixbuf, plus [Lexbor](https://lexbor.com/) for HTML.
Reading PDF needs poppler-glib, spelling needs Enchant, hyphenation
libhyphen and the thesaurus a MyThes file; without any of them Word42
still builds and says which is missing. Per-platform dependency lists are
in [docs/BUILD.md](docs/BUILD.md).

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

## Build status 

[![Linux](https://github.com/office-42/word42/actions/workflows/linux.yml/badge.svg)](https://github.com/office-42/word42/actions/workflows/linux.yml)
[![macOS](https://github.com/office-42/word42/actions/workflows/macos.yml/badge.svg)](https://github.com/office-42/word42/actions/workflows/macos.yml)
[![Windows](https://github.com/office-42/word42/actions/workflows/windows.yml/badge.svg)](https://github.com/office-42/word42/actions/workflows/windows.yml)
## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).

Word42 is not affiliated with, endorsed by, or derived from any Microsoft
product or from AbiWord; both are named only as the yardsticks Word42
measures itself against and for the file formats it reads and writes. It
contains no code, artwork, fonts or text from either; its icons, logo and
stylesheet are its own work.
