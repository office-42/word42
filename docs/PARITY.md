# Feature parity: Word42 against Word 97 and AbiWord

A review of the sources as of version 1.0.1-dev (September 2026), area
by area, twelfth round.
"Full" means the feature works the way the other program's does for
ordinary documents; "partial" means it exists with named limits; "no"
means it is absent.  Word 97 is the target Word42 measures itself
against: the word processor whose menus, dialogs and file format the
ones since are built on, and the one this program imitates.  AbiWord is
the free word processor closest to it in spirit, and Word (modern) is
listed so the distance to the current product is honest.  Word 6 was
the yardstick until the eleventh round; the rounds below say what was
done against it.

## Editing and text

| Feature | Word42 | Word 97 | AbiWord | Word (modern) |
|---|---|---|---|---|
| Typing, selection (mouse, keyboard, word/paragraph clicks) | full | full | full | full |
| Undo/redo, unlimited, typing coalesced | full (saved-state aware) | full | full | full |
| Repeat the last action (F4) | yes (typing, formatting, style, case) | yes | no | yes |
| Cut/copy/paste | rich (RTF, HTML and text out; RTF, pictures and text in) | rich | rich (RTF/HTML/images) | rich |
| Spike (cut several things, paste them together) | no | yes | no | yes |
| Drag-and-drop text | yes (move; Ctrl copies; one undo step) | yes | yes | yes |
| Find/Replace (case, whole word, wrap, replace all) | full | full + formats and wildcards | full + regex | full + formats |
| Go To (page, line, bookmark) | full | full | full | full |
| Change Case | full | full | full | full |
| Spelling as you type + dialog (Enchant) | full, and only the script the dictionary is for | full | full | full |
| Grammar check | no | yes | no | yes |
| Thesaurus (Shift+F7) | full (MyThes files; meanings, synonyms, Look Up, Replace) | full | no | full |
| Hyphenation (libhyphen patterns) | full | full | no (stub) | full |
| Word count | full | full | full | full |
| AutoCorrect as you type | full (quotes, capitals, dashes, misspellings) | full, with exceptions | partial | full |
| AutoFormat a whole document | full (headings, lists, quotes, blanks) | full | no | full |
| AutoText | full (named entries, Ctrl+F3) | full | yes | yes |
| AutoComplete tips (an AutoText entry offered as its name is typed) | full (Enter takes it) | full, dates too | no | yes |
| Formatting marks (¶, ·, →) | full | full | full | full |

## Character and paragraph formatting

| Feature | Word42 | Word 97 | AbiWord | Word (modern) |
|---|---|---|---|---|
| Font, size, bold, italic, underline, strikeout, colour | full | full | full | full |
| Overline, small caps, all caps, super/subscript, highlight, letter spacing | full | full (no overline) | full | full |
| Underline styles (single, words only, double, dotted, dashed, thick, wave) | full | full, and more | single and double | full |
| Double strikethrough, shadow, outline, emboss, engrave | full | full | partial | full |
| Animated text (blinking, marching ants) | no, and not planned | yes | no | no (dropped) |
| Language per run | full | yes | yes | yes |
| Alignment, indents, spacing, line spacing (single, 1½, double, At Least, Exactly, multiple) | full | full | full | full |
| Tabs (left/centre/right/decimal), live ruler, leaders | full | full | full | full |
| Paragraph borders and shading | single/double/dashed/dotted, nine weights, sixteen colours per side | full, more styles | full | full |
| Page border | full (Borders and Shading; style, weight, colour, distance; docx, rtf, odt, html) | full, and art borders | no | full |
| Page background colour | full (screen and preview) | yes (Online Layout) | yes | yes |
| Keep with next/together, widows, page break before | full | full | full | full |
| Right-to-left paragraphs | full | (Middle East editions) | full | full |
| Styles: paragraph styles, character styles, user-defined, based on | full: a style keeps its own settings and follows its base for the rest | full | full | full |
| Lists: bullets, numbers, letters, roman, restart | nine levels | multi-level, outline numbered | multi-level | multi-level |
| Heading numbering (1, 1.1) | full | full | via lists | full |
| Drop caps | yes (Format ▸ Drop Cap; docx, odt, html) | yes | no | yes |
| Columns (per section, balanced) | full | full | full | full |
| Sections with own columns | full | full | full | full |
| Per-section page size/orientation | no | yes | yes | yes |
| Text direction (vertical text in cells) | no | yes | no | yes |

## Structures

| Feature | Word42 | Word 97 | AbiWord | Word (modern) |
|---|---|---|---|---|
| Tables: insert, rows, columns, column widths (drag), merge and split cells (across and down) | full | full | full | full |
| Draw Table (pencil and eraser) | no | yes | no | yes |
| A row taller than a page | broken between its lines, header rows repeated | breaks | breaks | breaks |
| Table properties: borders (table outside and inside, per cell side, each with its own line style, weight and colour), cell shading and fill, vertical alignment, row height, header rows repeated | full | full | full | full |
| Table menu: Insert/Delete/Select submenus, AutoFit (to contents, to window, distribute), Heading Rows Repeat, Formula (SUM, AVERAGE, COUNT, MAX, MIN, PRODUCT) | full | full | partial | full |
| Nested tables | no | no (Word 2000) | yes | yes |
| Footnotes and endnotes | full | full | full | full |
| Headers and footers with page fields | one line, with a different first page and different even pages | full, per section | full, per section | full |
| Page numbers | full | full | full | full |
| Pictures (inline, resize by handles) | full | full | full | full |
| Insert ▸ Picture ▸ From Scanner or Camera | yes (WIA on Windows, SANE on Linux) | yes | no | yes |
| Wrapped pictures (left/right, top and bottom, in front, behind; placed by offset) | yes | yes | yes | yes |
| Text frames (paragraphs framed at a side, text beside) | yes (Format ▸ Frame; docx, odt, html) | yes | yes | yes |
| Floating text boxes placed anywhere on the page | no | yes | yes | yes |
| Drawing shapes (line, arrow, rectangle, rounded rectangle, ellipse, with fill, outline and text) | as objects, drawn as vectors | AutoShapes and WordArt, editable | as objects | as objects |
| Table of contents (insert, update) | full | full | full | full |
| Table of figures, table of authorities | no | yes | no | yes |
| Index (mark entries, build, rebuild) | full | full | no | full |
| Bookmarks, hyperlinks, cross-references | full | full (Ctrl+K) | full | full |
| Fields in the body (page, pages, date, time, filename, word count) | full (F9 updates) | full, dozens | yes | yes |
| Captions | numbered text | yes | yes | yes |
| Comments | full | full | yes | yes |
| Track changes (mark, accept/reject all) | full, one author | full, coloured by author | full | full |
| Compare Documents | no | yes | no | yes |
| Versions kept inside the document | no | yes | no | no (dropped) |
| Mail merge (CSV) | full | full | yes | yes |
| Envelopes and labels | full (a document of their own) | full | no | full |
| Templates and wizards | built-in ones, and a folder of your own | full, with wizards | New from Template | full |
| Equations | no | Equation Editor 3.0 | yes (MathML) | yes |
| Macros | Word42 Basic: a VBA dialect on the MY-BASIC engine, with Selection, ActiveDocument, Application, Documents, MsgBox and InputBox | VBA | no (plugins) | VBA |

## Files

| Format | Word42 | Word 97 | AbiWord | Word (modern) |
|---|---|---|---|---|
| RTF read/write | full | full | full | full |
| Word .doc read | Word 97–2003; Word 6/95 text | native | yes | yes |
| Word .doc write | no | native | yes | yes |
| Word .docx read/write | full for the model above | — | yes | native |
| AbiWord .abw/.zabw | full for the model above | — | native | — |
| OpenDocument .odt | read and written | no | yes | yes |
| HTML read/write | full | read and write | full | yes |
| PDF write | full | no | yes | yes |
| PDF read (poppler) | text and pictures | no | no | yes |
| Plain text | full | full | full | full |

## Views and application

| Feature | Word42 | Word 97 | AbiWord | Word (modern) |
|---|---|---|---|---|
| Normal and Page Layout views | full | full | full | full |
| Online Layout (Web Layout) view | no | yes | yes | yes |
| Outline view, Master Document | no | yes | no | yes |
| Document Map (headings in a pane, click to go) | full | full | no | full (Navigation pane) |
| Print (range, current page, selection, copies, collate, odd/even, reverse, draft, print to file), Print Preview (multiple pages, magnifier, fit zooms, shrink to fit), Page Setup | full (no gutter, mirror margins or shrink to fit) | full | full | full |
| Zoom | free: any percentage, page width, whole page | free | free | free |
| Full Screen (chrome away, Escape back) | full | full | full | full |
| Multiple windows on one document | full | full | yes | full |
| Split window: two panes on one document | full | full | no | full |
| Typing in a long document (173 pages) | 12 ms a keystroke: the changed paragraph is shaped, the rest reused | incremental | incremental | incremental |
| Autosave and crash recovery | full | yes (AutoRecover) | yes | yes |
| Background save and print | no | yes | no | yes |
| Recent files, options, units | full | full | full | full |
| Message and confirmation boxes | the program's own chrome | own | system | own |
| Office Assistant | no, and not planned | yes | — | no (dropped) |
| Accessibility (screen reader) | no | partial | partial | full |
| Translations | no | many editions | 60+ | — |
| Slides: a presentation from the outline, shown full screen | full | via PowerPoint | no | via PowerPoint |
| Presentation files (.pptx) read and written | outline: titles and lines | no | no | native (PowerPoint) |
| Packaging | Windows installer, macOS .app, Flatpak manifest | — | all | — |

[COMPARISON.md](COMPARISON.md) sets the same programs, and LibreOffice
Writer, side by side on size, speed and files.

## Summary

Against **Word 97**, Word42 has everything typed and formatted in the
first week of use: fonts with all of Word 97's effects, paragraphs,
tabs, lists, tables and their properties, pictures, frames, drop caps,
page borders, headers and footers, notes, fields, a table of contents
and an index, comments and revisions, spelling and the thesaurus,
AutoCorrect, AutoText with its tips, the Document Map, mail merge,
macros in a VBA dialect, and Word 97's own .doc read.  It lacks
positioned text boxes and the Drawing toolbar's editable objects,
Online Layout and Outline views, Compare Documents, grammar, Equation
Editor, tables of figures, versions, and vertical text; it exceeds Word
97 in Unicode, right-to-left text, PDF and every file format made since.

Against **AbiWord**, Word42 covers the everyday word-processing set —
formatting, multi-level lists, tables and their properties, notes,
columns, sections, fields, comments, revisions, mail merge, hyphenation,
wrapped pictures, a rich clipboard, and its own file formats as well as
Word's and OpenDocument, paragraph and character styles of the
document's own, text frames and drop caps — and lacks free-floating text
boxes, equations, accessibility and translations.

The order of work that closes the most ground now: positioned text
boxes, Online Layout view, Compare Documents, editing the header and
footer on the page itself rather than in a box, then translations and
accessibility.

## Completeness, command by command

A menu-by-menu count (seventh review, August 2026).  P = present,
Pa = partial, M = missing.

| Menu | vs Word 6 (P/Pa/M) | Most missed there | vs AbiWord (P/Pa/M) | Most missed there |
|---|---|---|---|---|
| File | 14/0/1 | Find File | 14/0/0 | -- |
| Edit | 13/1/1 | Links | 13/0/1 | Paste Unformatted styles |
| View | 5/3/3 | Outline, Master Document, Footnotes pane | 7/1/2 | Web Layout |
| Insert | 8/6/2 | Object, Form Field | 12/2/2 | Text Box, Clip Art |
| Format | 7/8/1 | Style Gallery, one Font box | 17/4/2 | two the earlier review did not name |
| Tools | 6/3/5 | Thesaurus, Grammar, Protect Document | 6/2/2 | Plugins and Scripts |
| Table | 11/2/2 | Insert/Delete Cells, Select Column | 13/2/0 | -- |
| Window | 4/0/0 | -- | 2/0/0 | -- |
| Help | 5/1/3 | Quick Preview, Examples and Demos | 5/1/0 | -- |
| **Total** | **71/24/20** | | **87/12/11** | |

| Yardstick | Estimate | Why |
|---|---|---|
| Word 6 | about 65 % | everything typed and formatted in the first hour is there -- fonts, paragraphs, tabs, lists, tables, pictures, frames, drop caps, headers and footers, spelling, AutoText, find and replace, printing, a help window with contents, search and index, RTF/DOCX/ODT/ABW/DOC/PDF -- but Outline view, templates, a single tabbed Font box, thesaurus and grammar, and table selection are not |
| AbiWord | about 76 % | the same document model and daily editing set, ahead on mail merge, hyphenation, revisions, drop caps and period fidelity; behind on view modes, positioned text boxes, table selection |

Where the dialogs stop short of their Word 6 counterparts: Paragraph
(no preview or Tabs button), Font (no underline styles, hidden text,
raised and lowered by points, kerning), Page Setup (no gutter, mirror
margins, header distance, apply-to, first-page or odd/even layout),
Table Properties (no exact row height, indent, row alignment, rows
breaking across pages, border weights), Options (no view, edit, save,
print or spelling tabs beyond what is there).

Added in the eighth round, closing menu gaps: the Table menu's Select
Row and Select Table, Sort Ascending and Descending, Split Table,
Convert Text to Table and back, and Table Gridlines; the Window menu's
list of open documents and a working Arrange All; Edit's Paste Special
and Clear; File's Save All and Summary Info (kept in Word, OpenDocument
and RTF files); Insert's File; and View's Full Screen.  Fifteen
commands that were missing are there, and nothing is greyed as
"planned" any more.

Fixed in the seventh round after the reviews: names typed without an
extension are saved as Rich Text rather than silently as plain text;
Find and Replace pick up the selected word; Change Case works on the
word at the caret; Caption starts a real paragraph; pasted Windows text
keeps its line ends; Page Setup refuses margins that leave no room;
Options applies the view and zoom at once; every menu's mnemonics are
unique; table properties, row heights, header rows and a cell's own
borders are undo steps; annotations and revisions carry the name from
Options > User Info.

## The ninth round (August 2026)

Every script, in the program and out of it: the display falls back
through the fonts installed for Chinese, Japanese, Korean, Thai,
Devanagari, Arabic, Hebrew and emoji when the document's own font has no
glyph, and the spelling checker judges only words in the script its
dictionary is written for, so a page of Chinese or Russian is no longer
underlined from end to end.

View > Header and Footer sets the first page's and the even pages' header
and footer as well as the ordinary one, which Word, OpenDocument and RTF
files have been carrying since the eighth round with nothing in the
program able to set them.

Fixed against the eighth round's reviews:

  - HTML export closed a list inside a table row when a table followed a
    bulleted paragraph.
  - A style of one's own lost its tab stops, borders, shading, list kind,
    dropped capital, frame and direction when the style it was based on
    was edited.
  - Undo back to the text as it was saved never cleared the modified
    mark; it does now, and changes the undo history does not hold -- the
    stylesheet, the header and footer, the summary information, heading
    numbering -- keep the document unsaved until it is saved.
  - Format > Paragraph could not set Exactly or At Least line spacing.
  - Replace with an empty box left the match where it was.
  - Full Screen left the menu bar, toolbars, ruler and status bar on top
    of the page, and could not be left with Escape.
  - The Window menu numbered documents by the order they were last used,
    so the number in the menu could raise a different window.
  - Save All tried to write documents read from .doc, PDF and HTML.
  - Table Gridlines started out saying it was on while the page showed
    none, and never read its setting back.
  - Paste was offered with an empty clipboard.
  - Split Table on the first row, Accept and Reject Revisions, Update
    Fields, Remove Hyphenation and Go to Note did nothing and said
    nothing; each writes a line in the status bar now.
  - Insert > Drawing made a blank picture when the line was as thick as
    the shape.
  - Shift+F10 and the Menu key open the context menu at the caret.

The command counts are unchanged from the eighth round -- this round
made commands work rather than adding them -- except that the user guide
in docs/GUIDE.md now describes all of them.

## The tenth round (August 2026)

**A table row taller than a page.**  Until now such a row was placed
whole: it ran off the bottom of the sheet and what would not fit was
drawn outside the page, or lost in print.  A row that cannot fit a page
is now broken between its lines over as many pages as it takes, with the
table's header rows set again at the top of each and no rule drawn where
the row was cut, so the cell reads as one cell continued.  A row that
fits a page but not the rest of the current one still moves whole, which
is the prettier of Word's two behaviours and the one word processors
default to for short rows.

Also this round:

  - The first-line indent marker on the ruler could be dragged off the
    page: it is bounded by the column now, as the left and right markers
    already were.
  - A run of text cannot hold a control character any more.  A newline
    that reached a run -- from a file reader, say -- was invisible on the
    page and yet saved with the document; it becomes the line break it
    means (U+2028), and the other control characters go.
  - Message boxes and the "Save changes?" box are the program's own now,
    in its own chrome: the system's alert put a button labelled in the
    desktop's language into a program that is in English and drawn as
    1993.
  - The About box says that Word42 is an independent program, not
    affiliated with or endorsed by the makers of any other word
    processor, and that format names appear only to say which format is
    meant.

**Tab leaders.**  A tab stop can now be filled with dots, dashes or a
rule -- Word 6's four choices, on the same dialog -- and Insert > Table of
Contents uses the dots, so a contents page reads as one has always read.
The leader lives in the same byte as the stop's kind, and RTF
(`\\tldot`), Word (`w:leader`), OpenDocument (`style:leader-style`) and
AbiWord all carry it both ways.

Two more from the review of the interface:

  - Insert > Caption made three undo steps of one command: undoing it
    took the text back and left the paragraph restyled.  It is one step
    now, and the caption goes below the caret's paragraph wherever in it
    the caret was.
  - Insert > Table of Contents with the caret down in a footnote put its
    entries in the note.  It says where the caret has to be instead.

The command counts are unchanged from the eighth round.

## Since the tenth round

**Drag-and-drop of text.**  Press inside the selection and pull: a grey
drop caret follows the pointer, and the button going up moves the text
there, formatting and all, as one undo step; Ctrl held drops a copy
instead.  A press that never moves is a click, and places the caret as
it always did.  With revisions being marked, the move marks the old text
deleted and the new inserted, as Word did.

**Edit > Repeat (F4).**  Does the last thing again: the last run of
typing goes in at the caret, or the last character or paragraph
formatting, style, or change of case goes on to the selection.  The
record is stamped with where the undo history stands, so only the last
action is ever offered: any other edit since, and Repeat greys out
rather than repeating something older.

**Window > Split.**  Two panes on the same document, one above the
other, with a bar to drag between them.  Each pane scrolls and keeps a
caret of its own; the toolbars, the ruler and the status bar follow the
pane being edited, and Split again is one pane again.  The Window menu's
one missing command against Word 6 is there.

**AutoCorrect.**  Word 6 put four things right as you typed them, and so
does this: straight quotes take the shape that fits where they stand, two
capitals at the start of a word become one, the first word of a sentence
takes its capital, two hyphens become a dash, and a short list of
misspellings is corrected.  Tools > AutoCorrect shows the list and the
switch; Tools > Options has the switch too.  A correction and the
character that prompted it are one undo step.

**Underline styles.**  A run can be underlined in any of seven ways --
single, words only, double, dotted, dashed, thick and wave -- chosen in
Format > Font Effects.  RTF, Word and OpenDocument carry all of them both
ways; HTML carries what CSS can say; AbiWord's own format keeps the line
but not always its shape.

**A right-to-left paragraph sits where it should.**  Alignment in this
model is what the eye sees -- Left is the left margin, Right the right --
and the Word formats, which keep it relative to the paragraph's
direction, are turned round as they are read and written.  The Arabic
paragraph in the showcase now sits against the right margin, where
LibreOffice puts it.

**Cells merged down a column.**  Merge Cells took a selection across one
row; it takes a selection down one column too, and makes a cell as tall
as the rows it covers.  The rows it swallows keep their cells -- the
table's grid does not change shape -- and whatever was written in them is
kept and comes back when Split Cells gives the rows their cells again.
Word (`w:vMerge`), RTF (`\clvmgf` and `\clvmrg`) and OpenDocument
(`table:number-rows-spanned` with `<table:covered-table-cell/>`) carry
the merge both ways, and LibreOffice reads all three of ours as the one
tall cell they say.

**Table AutoFormat.**  Word 6's Table menu offered a list of ready-made
looks for a table and drew what each would do; so does this.  The looks
are Word42's own -- Plain, Grid, Ruled, Ruled Bands, Shaded Heading,
Columns, Report -- and each says how the cells are ruled, what shading
the heading row and every other row take, and what is set in bold.  The
preview draws the table small as the list is walked, the two switches say
whether the heading row and the first column are singled out, and putting
a look on is one undo step.

**The language of a run.**  A run of text can now be marked with the
language it is written in -- Tools > Language, with a tick beside the
languages a dictionary is installed for -- and the spelling checker uses
the dictionary for that language rather than the document's, so a
Norwegian sentence in an English document is checked in Norwegian and
suggested for in Norwegian.  "(no proofing)" marks text that is not
language at all and is never checked.  RTF (`\lang`, `\noproof`), Word
(`w:lang`, `w:noProof`), OpenDocument (`fo:language` and `fo:country`),
AbiWord (`lang`) and HTML (`lang=`) all carry the mark both ways, with
Word's language numbers turned into tags and back.

**AutoText, Revert, and a Word Count worth the name.**  Three of the
gaps the command-by-command count named, closed together.

  - **Edit > AutoText** keeps a piece of text under a name and puts it
    in at the caret; the box offers a name made from the selection's
    first words, as Word 6's did, and typing the name and pressing
    Ctrl+F3 puts the entry in without the box (F3 alone stays Find Next,
    which is what a program of this age is expected to do).  The entries
    live in the settings file and outlast the session.  What is kept is
    the text, not its formatting.
  - **File > Revert** goes back to the document as it was when it was
    last saved, asking first, and is greyed out when there is nothing to
    go back to.
  - **Tools > Word Count** was a message box with four numbers.  It is
    now the box Word 6 had: pages, words, characters with and without
    their spaces, paragraphs and lines, a switch for whether the notes
    are counted, and a column for the selection when there is one.

**A help window.**  Help was the weakest menu
against Word 6: one item where Word 6 had nine.  The user guide now
travels inside the program, and the help window reads it: **Contents**
lists the guide's sections and shows the one chosen, **Search for Help
on...** narrows that list to the sections that mention a word (title
matches first), and **Index** turns it into every sub-heading in the
guide, alphabetically.  The guide is the only copy -- the help cannot
drift from the documentation, because it is the documentation.

**Word42 on the Web** and **Report a Bug** open the project's pages,
which is what Word 6's Technical Support and AbiWord's bug report are
for.

**A colour behind the page.**  Format > Background sets the colour the
paper is, chosen from a list with a sample that shows it behind text.
It is what the eye sees and what the file carries: Word's
`w:background` (with the settings part that makes Word show it), RTF's
background shape, OpenDocument's `fo:background-color` on the page
layout, and HTML's body style, all both ways, and LibreOffice reads all
of ours.  Printing leaves the paper alone, as a word processor does
unless it is told otherwise.

**Templates, envelopes and labels.**  Two of the three commands the
count still named on the File menu, and one of the three on Tools.

  - **File > New from Template** lists six templates that travel with
    the program -- Blank, Letter, Memo, Fax Cover, Report, Meeting
    Notes -- and whatever is in the templates folder, and starts a
    document from the one chosen.  The built-in ones are built in the
    model rather than read from a file, so they cannot go missing, and
    they use the styles, tab stops, paragraph rules and date fields the
    program already has.  **File > Save as Template** puts a copy of the
    document in that folder; a document started from a template is
    untitled, so a template cannot be written over by accident.
  - **Tools > Envelopes and Labels** makes an envelope or a sheet of
    labels as a document of its own: the envelope's page is the
    envelope's size with the two addresses where the post expects them,
    and the sheet is a table with a cell per label, no rules, each cell
    the label's size.  The sizes are named by what they measure -- no
    maker's catalogue numbers.

**An index.**  Insert > Index > Mark Entry marks the selected words as an
index entry -- under the words themselves, or under a term of your own --
and Build the Index gathers every marked run into an alphabetical index
at the caret, each entry with the pages it is on and dots leading out to
the numbers.  Asking again replaces the index where it stands, as Update
Table of Contents does.

A mark is a field on the run (XE, or XE:term), so the words stay where
they are and read as they did, and every format that carries fields
carries the marks: RTF and Word as XE fields, OpenDocument as the pair of
alphabetical index marks it has for the purpose.  LibreOffice reads all
three of ours as its own index entries.  The gathering lives beside the
layout rather than in the window, since the page numbers are the
layout's -- which is also what lets it be checked without a window.

**AutoFormat.**  Format > AutoFormat the Whole Document does for a
finished document what AutoCorrect does as you type: a short line that
stands alone becomes a heading, a line that starts with a dash or a
number becomes an item of a list with the marker taken off, straight
quotes take a printer's shapes, two hyphens become a dash, and a run of
empty paragraphs becomes one.  The four are switches in the box, the
whole pass is one undo step, and a second pass over the same document
finds nothing left to do.  Tables and notes are left alone.

**Typing in a long document.**  Every keystroke laid the whole document
out again, shaping every paragraph: 336 ms in a 69-page document and
845 ms in a 173-page one, which is not a word processor one can write a
report in, let alone a book.  Shaping is now kept between passes under a
signature of everything that goes into it, so a keystroke shapes the
paragraph it changed and reuses the rest -- 4 999 of 5 000 in the
measurement -- and the same keystroke costs 20 ms.  The measured lines
of an unchanged paragraph are kept with it, so Pango's iterator is not
walked again either.

What the pass still does for every paragraph is take a snapshot out of
the piece table and work out where the lines fall; those are arithmetic
rather than shaping, and they are what is left to make incremental.

The cache changes nothing about what comes out: the checks lay a
document out twice and compare every line box of the cached pass with
one from a layout that has never seen the document, after typing, after
undo, after formatting, after the page narrows, after the spelling
checker is told to ignore a word, and after the caret moves.

**And the passes that remain.**  With the shaping reused, what was left
of a keystroke was three kinds of housekeeping over the whole document,
and each was made cheaper.  A paragraph's signature is now borrowed from
the buffer it is built in rather than allocated, and hashed as it is
built rather than in a second pass -- and the hash reads every byte,
which is not a detail: a version that read only the ends of a signature
put thousands of nearly identical paragraphs in one bucket and made a
keystroke five times slower than doing nothing at all.  The snapshot
fills the previous pass's paragraphs again instead of allocating three
things per paragraph.  And a piece's characters are converted to UTF-8
in one go rather than one at a time.

A keystroke in a 173-page document costs 12 ms now, against 845 ms
before any of this; in a 69-page one, 4 ms.  A hundred edits of every
kind and a hundred undo steps are checked paragraph by paragraph and
line by line against a layout that has never seen the document, which is
what says the reuse changes nothing.

## The eleventh round (September 2026)

A review of the whole source, layer by layer, and the fixes it called
for, with three commands that closed named gaps.

**View > Zoom.**  Word 6's box: 200%, 100%, 75%, Page Width, Whole
Page, or any percentage from 25 to 500; the Zoom box on the toolbar has
the two fits too and shows whatever figure is in force, spliced in as
an entry of its own when it is none of the steps.  The one row of the
table where Word42 said "presets" against everyone else's "free".

**Table > AutoFit > AutoFit to Contents.**  Each column as wide as its
widest cell wants to be on one line -- the layout measures every cell's
paragraphs unwrapped, in their own fonts -- and a table that would then
be wider than the text column gives way in proportion, as Word's did.

**The clipboard.**  Copy puts the selection out as HTML as well as RTF
and text, so a browser or a mail program pasting from Word42 keeps the
formatting; Paste takes a picture off the clipboard -- a screenshot, an
image copied from a browser -- and puts it in as a PNG at its pixel
size, and the Paste command lights up for one.

**Opening a long document.**  Interning a formatting record hashed it a
byte at a time, and a reader interns once a run or oftener: a quarter
of the time to open a 132-page report went there.  Hashed a word at a
time, the report opens in 0.47 s from RTF against 0.64 s before.

Fixed after the review:

  - A modeless box -- Find, Spelling, Symbol, Bookmark, Annotations,
    Mail Merge -- opened for the second pane of a split window kept
    pointing at that pane after Window > Split took it away.  The boxes
    follow the pane being edited now, and one built for a pane closes
    with it.
  - Up from the first line of a page landed on the first line of the
    page before, skipping the whole page, and Page Up did the same.
  - A footnote referenced just before a table that ran on to the next
    page was set at the foot of the page where the table ended.  The
    notes owed to a page are set before the table turns it, and the
    table's rows are fitted round them.
  - The caret could walk into a cell covered by a vertical merge --
    paragraphs the layout shows nowhere -- and vanish; Left and Right
    step over such cells.
  - Print > Current page printed page 1 from Normal view, whose layout
    is one galley; the page is found on a paginated layout now.
  - Tab at the end of a table added a row that the other pane of a
    split window and an open preview did not hear of.
  - Table > Delete Row took the wrong slot out of the table's record,
    so a table with set row heights came back with its rules and widths
    shifted along by one; and a deleted row's or column's footnotes
    were left orphaned in the notes section, numbered 0.
  - Undo of Insert or Delete Column across a merged cell left the cell
    the wrong number of columns wide, since the change of span was not
    recorded; Merge Cells lost the first cell's fill and sides; Split
    Table lost the moved rows' heights.
  - AutoFormat's printer's quotes replaced the whole paragraph's text
    with one run, flattening its bold and italic and turning its
    pictures into the placeholder character and deleting its footnotes.
    The quotes and dashes are changed one character at a time now, each
    keeping the formatting of the character it replaces.
  - Opening a file into a window that had one kept the old document's
    title-page and even-page headers and its tables' shapes.
  - RTF: a \'xx byte was always Windows-1252, so Russian, Greek, Turkish
    and Central European files from Word came in as mojibake; the code
    page \ansicpg declares is used, with the East Asian pages' two-byte
    characters gathered.  LibreOffice's font table, whose names carry a
    {\*\falt} alternative, lost every font name; Word's {\*\cs} and
    {\*\ts} stylesheet entries came in as paragraph styles; a table in
    a header or footer opened an empty table in the body; a paragraph
    marked \intbl before any \cellx crashed the reader; the first shape
    in a file lost its kind and so was dropped; a shape's text above
    U+7FFF was written wrongly.
  - AbiWord: a picture at the start of a second paragraph in a cell
    landed one position off.
  - Every reader's page geometry is clamped to a page that can be laid
    out, as the Word reader's already was.

**Macros.**  The roadmap said Word42 would never have a macro language;
it has one now, because a word processor that can be told to do the
same thing to fifty documents is worth more than one that cannot.
Word42 Basic is a dialect of VBA -- Sub and End Sub, Dim, If and Select
Case, Do and For, With, named arguments, `&` -- translated line by line
into the syntax of MY-BASIC, Tony Wang's MIT-licensed interpreter in one
C file, which is compiled into the program.  The object model is Word's
where Word42 has the thing behind it: Selection with its Font,
ParagraphFormat and Find, ActiveDocument, Application, Documents, and
MsgBox, InputBox and Debug.Print.  Tools > Macro > Macros (Alt+F8) is
Word 6's box -- Run, Create, Edit, Delete -- and the Macro Editor
(Alt+F11) runs a Sub with F5 and reports an error against the line of
the macro it was on.  A macro is one undo step; nothing runs unless it
is asked to; a loop that never ends is stopped.  What the dialect has
not got -- For Each, GoTo, error trapping, objects in variables, user
classes -- it says so about rather than running something else.

Not done, and worth knowing: a paragraph in an OpenDocument file that
sets an inherited indent or spacing back to nought gets the style's
value instead; Table > Sort drops the footnotes of the rows it moves;
DOCX and ODT do not carry a list's restart number; hidden text in RTF
is shown.

## The twelfth round (September 2026): Word 97

The target moved.  Word 6's menus are done as far as they are worth
doing, and the word processor the ones since are built on is Word 97;
Word42.md says so, the tables above are scored against it, and this
round took the first five of what it added.

**View > Document Map.**  A pane down the left of the window lists every
heading, indented by its level, with the one the caret is under shown
selected; a click puts the caret on the heading and scrolls it into
view.  The list is built from the same snapshot of the paragraphs the
layout uses and the same outline levels the table of contents and the
slide show use, so the three agree about what a heading is, and it
shows the section numbers when heading numbering is on.  It follows the
text as it is typed, a quarter of a second behind so that typing in a
heading does not remake it at every character, and is left alone while
hidden.  The ruler sits over the page rather than over the map, so its
zero stays the page's.  Full Screen puts it away with the rest of the
chrome, and the setting is remembered.

**Font effects.**  Word 97's Font box added double strikethrough,
shadow, outline, emboss and engrave to Word 6's, and Format > Font
Effects has them.  Pango has no attribute for any of them, so the one
function every painter draws a line through -- screen, print, preview
and PDF -- takes the glyph outlines of each such run from the line
again, clipped to the run's own width, and fills or strokes them as the
effect asks: a grey copy below and to the right for the shadow, a
stroke and no fill for the outline, two lines for the double
strikethrough, and for the relief a dark copy to one side, a light copy
to the other and the letter itself in the paper's white, lit from the
top left as Word drew it.  Pango's own ink for an outlined or relieved
run is made all but transparent (nought would mean "unset") so it does
not show through.  RTF (`\striked1`, `\shad`, `\outl`, `\embo`,
`\impr`), Word (`w:dstrike`, `w:shadow`, `w:outline`, `w:emboss`,
`w:imprint`), OpenDocument (`style:text-line-through-type`,
`fo:text-shadow`, `style:text-outline`, `style:font-relief`) and the
Word 97 binary format's sprms all carry the five both ways, and HTML
says what CSS can (a `text-shadow`, a stroke with no fill, a doubled
line-through) and reads it back.  The macro language's Font object has
them as Word's `Shadow`, `Outline`, `Emboss`, `Engrave` and
`DoubleStrikeThrough`.

**Page Border.**  Word 97's Borders and Shading box grew a Page Border
tab; here it is the last group of the same box: a line round every page
in a style, width and colour, at a distance from the edge of the paper
(24 points unless the document says otherwise).  It lives on the page
setup beside the background colour and is drawn by the layout's
backdrop, which every painter calls, so Page Layout view, the preview,
paper and PDF agree and Normal view, having no pages, shows none.  RTF
(`\pgbrdrt` and its siblings with `\pgbrdropt32`, from the edge), Word
(`w:pgBorders` with `w:offsetFrom="page"`, and the other measure turned
into ours on reading) and OpenDocument carry it both ways; in
OpenDocument the border stands inside the margins with the padding
between it and the text, so the margins written are the border's
distance and the rest of each margin is padding, which is how
LibreOffice reads a Word document's page border, and the reader adds
them back.  HTML writes it into the `@page` rule and reads it out.

**Thesaurus.**  Tools > Language > Thesaurus (Shift+F7) looks up the
word at the caret in a MyThes file -- `th_en_US_v2.dat` and its `.idx`,
the pair LibreOffice uses, in the mythes folder -- and shows Word 97's
box: the word looked up, its meanings with the part of speech beside
each, the synonyms of the meaning chosen, Replace, Look Up and
Previous.  The index is read into a table the first time the command is
used and the data file is mapped and read at the offset the index
gives, since it runs to tens of megabytes; no library is needed.  The
Tools menu's Language item is Word 97's submenu now -- Set Language,
Thesaurus, Hyphenate Document, Remove Hyphenation -- and the old
Hyphenation submenu is gone.

**AutoComplete.**  Word 97 offered an AutoText entry as its name was
typed: the first four letters and up came a tip over the caret with the
entry and "(Press ENTER to Insert)".  So here: the tip is a popover
that takes no focus, Enter puts the entry in place of the letters typed
as one undo step, Escape or any other key puts the tip away and does
what it always did, and moving the caret by any other means takes it
away too.

The command counts against Word 97, by the same method as the seventh
review's: P = present, Pa = partial, M = missing.

| Menu | vs Word 97 (P/Pa/M) | Most missed there |
|---|---|---|
| File | 15/1/3 | Versions, Send To, Web Page Preview |
| Edit | 14/1/2 | Paste as Hyperlink, Links |
| View | 8/2/3 | Online Layout, Outline, Master Document |
| Insert | 12/5/3 | Text Box, Object, Table of Figures |
| Format | 15/5/3 | Text Direction, Style Gallery, Theme |
| Tools | 10/3/6 | Grammar, Compare Documents, Protect Document, Customize |
| Table | 14/2/2 | Draw Table, Cell Height and Width's rows tab |
| Window | 3/0/0 | -- |
| Help | 5/1/2 | What's This?, WordPerfect Help |
| **Total** | **96/20/24** | |

| Yardstick | Estimate | Why |
|---|---|---|
| Word 97 | about 72 % | everything typed and formatted in the first week is there, Word 97's own file format is read, and the Document Map, page borders, the Font box's effects, the thesaurus and AutoComplete are done; positioned text boxes, the Drawing toolbar, Online Layout and Outline views, Compare Documents, grammar, Equation Editor and versions are not |
| AbiWord | about 78 % | as the eleventh round had it, plus the thesaurus and the Document Map, which AbiWord has not got |

