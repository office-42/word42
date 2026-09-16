# Word42 against Word, AbiWord and LibreOffice Writer

The yardsticks Word42 measures itself against, on the figures that
decide whether a word processor is worth installing: how big it is, how
fast it is, what it reads and writes, and what it can do.  Word42's
numbers were measured on this tree (version 1.0.1, September 2026,
a Linux build with every optional dependency) and say so; the other
columns are the programs' public figures and typical experience, and
are marked approximate where they are.  [PARITY.md](PARITY.md) has the
feature-by-feature account against Word 97 and AbiWord; this is the
short table.

## Size and speed

| Metric | Word42 | Microsoft Word (365) | AbiWord 3.0 | LibreOffice Writer 25.x |
|---|---|---|---|---|
| Licence | GPL-3.0-or-later | proprietary, subscription | GPL-2.0-or-later | MPL-2.0 |
| Written in | C (GTK 4, Pango, Cairo) | C++ (closed) | C++ (GTK 3) | C++ (its own VCL toolkit) |
| Source size | 60 k lines (measured) | not public | ~0.6 M lines (approx.) | ~10 M lines, whole suite (approx.) |
| Program on disk | 1.2 MB stripped binary, plus the GTK runtime | ~1–2 GB with Office (approx.) | ~30 MB (approx.) | ~800 MB installed (approx.) |
| Start to first window | 0.5 s (measured, Xvfb) | 2–4 s cold (typical) | ~1 s (typical) | 3–6 s cold, ~1 s warm (typical) |
| Open a 132-page, 160 000-word report | 0.35 s (.docx), 0.47 s (.rtf), then 0.3 s to lay out (measured) | a second or two (typical) | a few seconds (typical) | a few seconds (typical) |
| A keystroke in that report | 9 ms (measured) | instant | instant | instant |
| Memory, empty document | 247 MB resident (measured under Xvfb's software renderer; a GPU desktop is lower) | 200–400 MB (typical) | 50–100 MB (typical) | 250–500 MB (typical) |
| Platforms | Linux, Windows, macOS | Windows, macOS, web, iOS, Android | Linux (the Windows and macOS ports are dormant) | Linux, Windows, macOS |
| Translations | none | 100+ | 60+ | 100+ |
| Screen-reader accessibility | none | full | partial | full |
| Maintenance | active | monthly releases | 3.0.x; sparse since 2021 | two releases a year |

## Files

| Format | Word42 | Word | AbiWord | Writer |
|---|---|---|---|---|
| Native format | none of its own: RTF, .docx, .odt and .abw are all written in full | .docx | .abw | .odt |
| Word .docx | read and write | native | read and write (plugin) | read and write |
| Word .doc (Word 97's format) | read | read and write | read and write | read and write |
| RTF | read and write | read and write | read and write | read and write |
| OpenDocument .odt | read and write | read and write | read and write | native |
| AbiWord .abw | read and write | no | native | read (import filter) |
| HTML | read and write | read and write | read and write | read and write |
| PDF | write; read text and pictures | write; read (converted) | write | write; read (as drawing) |
| Plain text | read and write | read and write | read and write | read and write |
| WordPerfect, and the rest | no | .wpd | .wpd, .kwd, and others | dozens |
| Presentations | .pptx as an outline, and a slide show | via PowerPoint | no | via Impress |

## What it can do

| Feature | Word42 | Word | AbiWord | Writer |
|---|---|---|---|---|
| Styles: paragraph and character, based on, following their base | yes | yes | yes | yes |
| Lists: bullets, numbers, nine levels, restart | yes | yes | yes | yes |
| Heading numbering, table of contents, index | yes, yes, yes | yes, yes, yes | via lists, yes, no | yes, yes, yes |
| Tables: merge across and down, borders per side, AutoFit (window, contents), AutoFormat, sort, formula | yes | yes | most | yes |
| Nested tables | no | yes | yes | yes |
| Footnotes and endnotes | yes | yes | yes | yes |
| Headers and footers | one line each, with a different first and even page | full, per section | full, per section | full, per section |
| Pictures: inline, wrapped, positioned | yes | yes | yes | yes |
| Text frames, drop caps | yes | yes | frames | yes |
| Floating text boxes | no | yes | yes | yes |
| Drawing | five shapes, kept as vectors | full drawing tools | basic | full Draw tools |
| Equations | no | yes | MathML | Math |
| Columns and sections | yes | yes | yes | yes |
| Per-section page size and orientation | no | yes | yes | yes |
| Find and replace | text, case, whole word | with formats and wildcards | with regular expressions | with regular expressions |
| Spelling as you type | yes (Enchant) | yes | yes | yes |
| Grammar, thesaurus | no, yes (MyThes) | yes, yes | no, no | extension, yes |
| Hyphenation | yes | yes | stub | yes |
| AutoCorrect, AutoFormat, AutoText | yes | yes | partial | yes |
| Revision marks, comments | yes, one author | full | yes | full |
| Mail merge | CSV | full | yes | full |
| Envelopes and labels | yes | yes | no | yes |
| Templates | built-in and a folder | full | new from template | full |
| Clipboard | RTF, HTML and text out; RTF, pictures and text in | rich | RTF, HTML, images | rich |
| Zoom | any percentage, page width, whole page | free | free | free |
| Split window, several windows | yes, yes | yes, yes | no, yes | yes, yes |
| Print preview, print to file | yes | yes | yes | yes |
| Autosave and crash recovery | yes | yes | yes | yes |
| Macros, scripting | Word42 Basic, a VBA dialect (Selection, ActiveDocument, MsgBox...) | VBA | plugins | Basic, Python |
| Real-time collaboration | no | yes | no | no |
| Slide show from the outline | yes | via PowerPoint | no | via Impress |

## Reading the table

Word42 is two orders of magnitude smaller than either free competitor
and three smaller than Word, opens and lays out a long document in
under a second, and covers the word-processing set that a report, a
letter, a thesis or a newsletter needs, including several things
AbiWord lacks (an index, hyphenation, mail merge from CSV, envelopes,
a split window, AutoFormat).  What it lacks against all three is the
same short list: text boxes placed on the page, headers and footers
richer than one line, per-section page orientation, nested tables,
equations, grammar, translations and a screen reader's view of the
page.  [Word42.md](../Word42.md) is the order they will be taken in.
