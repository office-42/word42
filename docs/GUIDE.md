# Word42 User Guide

Word42 is a word processor: you type into a page, format what you typed,
and save it in a format other programs can read. This guide describes
every command in the program, in the order you are likely to need them.

It assumes nothing beyond knowing how to start the program. If you are
looking for a single command, the [keyboard reference](#keyboard-reference)
at the end lists every shortcut, and the section headings follow the menus.

**Contents**

1. [The window](#1-the-window)
2. [Documents](#2-documents)
3. [Typing and moving about](#3-typing-and-moving-about)
4. [Editing](#4-editing)
5. [Formatting characters](#5-formatting-characters)
6. [Formatting paragraphs](#6-formatting-paragraphs)
7. [Styles](#7-styles)
8. [The page](#8-the-page)
9. [Tables](#9-tables)
10. [Pictures, drawings and frames](#10-pictures-drawings-and-frames)
11. [Notes, references and fields](#11-notes-references-and-fields)
12. [Tools](#12-tools)
13. [Views and windows](#13-views-and-windows)
14. [Printing and exporting](#14-printing-and-exporting)
15. [Languages and scripts](#15-languages-and-scripts)
16. [Help](#16-help)
17. [Keyboard reference](#17-keyboard-reference)
18. [Where Word42 keeps things](#18-where-word42-keeps-things)

---

## 1. The window

![The Word42 window](images/screenshot.png)

From the top down:

| Part | What it is |
| --- | --- |
| **Title bar** | The document's name and the program's version. Word42 draws it itself, so it looks the same on every system. |
| **Menu bar** | File, Edit, View, Insert, Format, Tools, Table, Window, Help. Every command in the program is on it. |
| **Standard toolbar** | New, Open, Save, Print, Print Preview, Spelling, Cut, Copy, Paste, Undo, Redo, Find, and the zoom box. |
| **Formatting toolbar** | The Style box, the font box, the size box, Bold, Italic, Underline, the four alignments, and the bullet and numbering buttons. |
| **Ruler** | Indent markers, tab stops, and the margins of the text. Drag on it to set indents and tabs; see [Tabs and the ruler](#tabs-and-the-ruler). |
| **The page** | What you type into. In Page Layout view it is a white sheet on a light grey desk, with a hairline round it. |
| **Status bar** | The page you are on, how far down the page the caret is, and its line and column. |

View ▸ Standard, View ▸ Formatting and View ▸ Ruler turn the two toolbars
and the ruler off and on; Word42 remembers the setting for the next time.

---

## 2. Documents

### New, Open, Close

**File ▸ New** (Ctrl+N) opens an empty document in a new window.
**File ▸ Open** (Ctrl+O) opens an existing one, and **File ▸ Close**
(Ctrl+W) closes the window, asking first if there is anything unsaved.

The File menu remembers the last eight files you opened or saved; pick one
from the bottom of the menu to open it again.

**File ▸ New from Template** starts a document from one of the templates
that travel with the program — Letter, Memo, Fax Cover, Report, Meeting
Notes — or from one of your own. **File ▸ Save as Template** puts a copy
of the document you are in into the templates folder, where the list
picks it up; the document itself is unchanged and keeps its own file. A
document started from a template is untitled: saving asks where to put
it, so a template is never written over by accident.

**File ▸ Revert** goes back to the document as it was when it was last
saved, throwing away the changes made since. It asks first, and it is
greyed out when the document has never been saved or has no changes to
throw away.

### The formats Word42 reads and writes

| Format | Open | Save |
| --- | --- | --- |
| Rich Text Format (`.rtf`) | yes | yes |
| Word document (`.docx`) | yes | yes |
| Word 97–2003 (`.doc`) | yes | — (save as `.rtf` or `.docx`) |
| OpenDocument Text (`.odt`) | yes | yes |
| AbiWord (`.abw`, `.zabw`) | yes | yes |
| Web page (`.html`, `.htm`) | yes | yes |
| Plain text (`.txt`) | yes | yes |
| Presentation (`.pptx`) | yes, as an outline | yes, from the outline |
| PDF (`.pdf`) | text only, where the PDF library is present | export only |

**File ▸ Save** (Ctrl+S) writes the document back in the format it came in.
**File ▸ Save As** (Ctrl+Shift+S or F12) writes it somewhere else, or in
another format: pick the format in the dialog's filter list, or simply type
the extension you want. **File ▸ Save All** saves every open document that
has been changed.

What survives a round trip depends on the format. RTF, `.docx`, `.odt` and
`.abw` carry the whole document — styles, tables, pictures, notes, fields,
headers and footers, revision marks. HTML carries the text and most of its
formatting. Plain text carries the text.

### Summary Info

**File ▸ Summary Info** records the document's title, subject, author,
keywords and comments. Word, OpenDocument and RTF files carry these, and
Word42 reads them back from a file that has them. Your name comes from
Tools ▸ Options the first time.

### Autosave and recovery

Every two minutes, a document with unsaved changes is copied into Word42's
data directory. Saving or closing cleanly removes the copy. If Word42 stops
without either — a power cut, a crash — the next start opens the copy as the
document it was, marked as changed, and tells you so. Save it, and the copy
goes.

### Insert one document into another

**Insert ▸ File** brings another document's text, with its formatting, in
at the caret. It reads the same formats File ▸ Open does.

---

## 3. Typing and moving about

Type, and the text goes in at the caret. Enter starts a new paragraph;
Backspace at the start of a paragraph joins it to the one before.

| Key | Moves |
| --- | --- |
| ← → ↑ ↓ | A character or a line |
| Ctrl+← / Ctrl+→ | A word |
| Home / End | Start or end of the line |
| Ctrl+Home / Ctrl+End | Start or end of the document |
| Page Up / Page Down | A screenful |
| Tab / Shift+Tab (in a table) | The next or previous cell |

Hold Shift with any of them to select as you move. With the mouse: click to
place the caret, drag to select, double-click for a word, triple-click for a
paragraph. **Edit ▸ Select All** (Ctrl+A) selects everything.

Selected text can be dragged: press inside the selection and pull, and a
grey caret shows where the text will land; let go and it moves there,
formatting and all, as one undo step. Hold **Ctrl** while letting go and a
copy lands instead, the original staying put. A click inside the selection
that never moves simply places the caret, as it always did.

**Edit ▸ Go To** (Ctrl+G or F5) jumps to a page or a line by number, or to a
bookmark by name.

---

## 4. Editing

| Command | Key | What it does |
| --- | --- | --- |
| Undo | Ctrl+Z | Steps back. There is no limit; a run of typing undoes as one step, and a compound change — a table property, a style edit — as one step. |
| Redo | Ctrl+Y or Ctrl+Shift+Z | Steps forward again. |
| Repeat | F4 | Does the last thing again: the last run of typing goes in at the caret, or the last character or paragraph formatting, style, or change of case goes on the selection. Only the last action can be repeated — edit anything else and Repeat greys out. |
| Cut | Ctrl+X | Removes the selection and puts it on the clipboard. |
| Copy | Ctrl+C | Puts the selection on the clipboard. |
| Paste | Ctrl+V | Puts the clipboard in at the caret, with its formatting. |
| Paste Special | — | Puts the clipboard in as plain text, taking the formatting of the text round it. |
| Clear | Del | Removes the selection without touching the clipboard. |
| AutoText... | — | Keeps the selected text under a name, and puts a kept piece of text in at the caret. |

### Edit ▸ AutoText

A piece of text you type often — a closing, an address, a company name —
can be kept under a name and put in whenever you want it. Select the
text, open **Edit ▸ AutoText**, and the box offers a name made from its
first words; **Add** keeps it. With nothing selected, the box lists what
is kept: pick one and **Insert** puts it in at the caret, and **Delete**
takes it out of the list for good.

Typing the name and pressing **Ctrl+F3** puts the entry in without the box,
the way Word 6's F3 did (F3 alone is Find Next here). Entries are kept
between runs, in the settings file; what is kept is the text, not its
formatting, so an entry takes the formatting of the place it lands in.

### Find and Replace

**Edit ▸ Find** (Ctrl+F) searches forward from the caret; F3 finds the next
one. **Edit ▸ Replace** (Ctrl+H) replaces one at a time or all of them. Both
pick up the word at the caret when you open them, so finding another
occurrence of the word you are looking at takes two keys.

Right-clicking in the text opens a menu of the commands you want most:
Cut, Copy, Paste, and the formatting of the paragraph you clicked in.
Shift+F10, or the Menu key, opens the same menu at the caret.

---

## 5. Formatting characters

Formatting belongs to the text, not to the caret: select the text, then
apply. With nothing selected, the setting applies to what you type next.

### The quick ones

| Command | Key |
| --- | --- |
| Bold | Ctrl+B |
| Italic | Ctrl+I |
| Underline | Ctrl+U |
| Grow the font | Ctrl+] |
| Shrink the font | Ctrl+[ |

The font and size boxes on the formatting toolbar set the family and the
size of the selection directly.

### Format ▸ Font (Ctrl+D)

The whole record in one box: family, style, size, colour, underline, and
the effects — strikeout, superscript, subscript — with a preview.

### Format ▸ Font Effects

The rest of them: strikethrough, overline, superscript, subscript, small
capitals, all capitals, a highlight colour, and character spacing (letters
pushed apart or drawn together, in points).

It also carries the **underline**, which has seven kinds: single, double,
words only (the spaces between words are left alone), dotted, dashed,
thick and wave. Ctrl+U is the single one; the others are chosen here. RTF,
Word and OpenDocument files carry all of them.

### Format ▸ Change Case

Sentence case, lowercase, UPPERCASE, Title Case and tOGGLE cASE
(Shift+F3). Each character keeps its own formatting through the change.
With nothing selected, it changes the word at the caret.

---

## 6. Formatting paragraphs

### Alignment

Ctrl+L, Ctrl+E, Ctrl+R and Ctrl+J are left, centre, right and justified;
so are the four buttons on the formatting toolbar and Format ▸ Alignment.

### Format ▸ Paragraph

- **Alignment** and **Direction** — two settings that mean two things.
  Alignment says which margin the lines sit against: Left is the left
  margin and Right the right one, whichever way the writing runs.
  Direction sets the paragraph right-to-left, for Arabic and Hebrew: the
  text shapes and orders itself that way, and a right-to-left paragraph is
  normally set flush right. Word's file formats keep alignment the other
  way round, relative to the direction; Word42 turns it round when it
  reads and writes them, so a document looks the same in both.
- **Indents** — left, right, and a special first line: none, first line
  indented **By** so much, or a hanging indent.
- **Spacing** — before and after the paragraph, in points.
- **Line spacing** — single, 1½, double, At Least, Exactly or a multiple.

### Tabs and the ruler

There is a tab stop every half inch until you set one of your own.

- **Click** the ruler to put a tab stop where you click.
- **Drag** a stop along the ruler to move it, or off the ruler to remove it.
- The **box at the ruler's left** cycles the kind of stop the next click
  makes: left, centre, right, decimal.
- **Format ▸ Tabs** sets them by number, clears them, and gives a stop a
  **leader**: the dots, dashes or rule that fill the gap in front of it.
  A table of contents is the reason leaders exist.

The three markers on the ruler are the first-line indent (top), the left
indent (bottom) and the right indent; drag them.

### Format ▸ AutoFormat

**Format ▸ AutoFormat the Whole Document** looks over a document that
was typed as though on a typewriter and makes it a word processor's:

- a short line that stands alone, with nothing before it and text after
  it, becomes a heading;
- a line that begins with a dash, a star or a bullet becomes an item of
  a bulleted list, and one that begins with "1." or "a)" an item of a
  numbered list, with the marker taken off;
- straight quotes take the shapes a printer would set, two hyphens
  become an en dash and three an em dash;
- a run of empty paragraphs becomes one.

The box says which of the four to apply and remembers what you chose.
The whole pass is one undo step, so Ctrl+Z takes it all back if you do
not like what it did. Text in tables and in notes is left alone.

### Format ▸ Background

The colour behind the page: pick one from the list and the sample shows
what it will look like, text and all. The colour is shown on the screen
and in Print Preview; printing leaves the paper as it is, which is what
a word processor does with a page colour unless told otherwise.

Word (`w:background`), RTF (a background shape), OpenDocument
(`fo:background-color` on the page layout) and HTML (the body's own
style) all carry the colour both ways, and LibreOffice reads all four of
ours.

### Format ▸ Borders and Shading

A line above, below, to the left or right of a paragraph, or round it:
single, double, dashed or dotted, in any of Word XP's nine weights from a
quarter point to six, in any of the sixteen colours, and a grey shading or
a colour behind it. Everything that draws the document draws them, so they
print and export as they appear.

In a table the dialog can apply to the paragraph, to the cell -- its
sides, their line, its background -- or to the whole table, where the
checked sides are the table's outside, "Inside" rules between its cells,
and the line is drawn round it all. Where two cells disagree about the
rule between them, the heavier line wins, as in Word.

### Bullets and numbering

The two buttons on the formatting toolbar, or Format ▸ Bullets
(Ctrl+Shift+L) and Format ▸ Numbering. An item gets a quarter-inch hanging
indent with the marker in it; Enter on an empty item ends the list.

**Format ▸ Bullets and Numbering** chooses the kind: 1. 2. 3., a. b. c.,
A. B. C., i. ii. iii., I. II. III., or round, open, square and dash
bullets — and restarts the numbering at the item you are on.

---

## 7. Styles

A style is a named set of formatting. Word42 starts with Normal,
Heading 1, Heading 2, Heading 3, Title and Caption.

- **Apply** one from the Style box at the left of the formatting toolbar,
  or with Ctrl+Shift+N (Normal) and Ctrl+Alt+1/2/3 (the headings).
- **Format ▸ Style** redefines one: change it, and every paragraph in that
  style changes with it.
- **New** in that dialog makes a style of your own. A new style is *based
  on* an existing one and keeps only what you change in it, so changing the
  base changes everything built on it.
- A **character style** carries font, size, weight, slant, underline,
  colour and case, and goes on to the selected text the way Bold does,
  leaving the paragraph alone.
- **Delete** removes a style; its paragraphs fall back to Normal.

**Format ▸ Heading Numbering** numbers the headings from their outline
levels — 1, 1.1, 1.2, 2 — and keeps the numbering right as you edit.

Styles travel in RTF, `.docx`, `.odt` and `.abw`, and styles those files
define are read in. Word's own `heading 1` arrives as Word42's Heading 1,
whatever language the file was written in.

---

## 8. The page

### File ▸ Page Setup

Paper size, orientation, and the four margins. The units are the ones set
in Tools ▸ Options.

### Breaks

| Command | Key | What it does |
| --- | --- | --- |
| Insert ▸ Page Break | Ctrl+Enter | Starts a new page. |
| Insert ▸ Column Break | — | Starts the next column. |
| Insert ▸ Section Break | — | Starts a new section on a new page. |

A section has its own column layout, so a two-column article can follow a
one-column title.

### Format ▸ Columns

One, two or three newspaper columns, with the spacing between them, applied
to the whole document, this section, or from this point on. The text runs
down one column and on to the next in Page Layout view; footnotes sit at
the foot of their own column, and the last page's columns are balanced so
they end level. Normal view shows a single column.

### View ▸ Header and Footer

A line of text at the top and bottom of every page, with its own alignment.
Three fields go in the text and are replaced when the page is drawn or
printed:

- `{PAGE}` — the page number
- `{NUMPAGES}` — how many pages there are
- `{DATE}` — today's date

**Different first page** gives the first page a header and footer of its
own — a title page with no running head, say. **Different odd and even
pages** gives the even-numbered pages theirs, so a page number can sit at
the outside edge of every spread. Each switch turns on the pair of rows
below it; leave it off and every page uses the same header and footer.
Word, OpenDocument and RTF files carry all three, both ways.

Headers and footers show in Page Layout view and in print, not in Normal
view.

**Insert ▸ Page Numbers** is the shortcut for the common case: it puts
`{PAGE}` in the footer with the alignment you pick.

---

## 9. Tables

**Table ▸ Insert Table** puts a grid of any size after the paragraph you
are in.

### Moving and typing

Tab and Shift+Tab move from cell to cell; Tab in the last cell adds a row.
Enter makes another paragraph inside the cell. A row is as tall as its
tallest cell. A row that will not fit the rest of a page moves to the next
page whole; a row with more in it than a page can hold is broken between
its lines, and carries on at the top of the next page under the table's
header rows.

### Changing the shape

| Command | What it does |
| --- | --- |
| Insert Rows / Delete Rows | Above the caret's row, or removes it. |
| Insert Columns / Delete Columns | The same across. A cell merged across the place grows or shrinks with it. |
| Merge Cells | Joins the selected cells of a row, keeping every cell's paragraphs; select down a column instead and the cells become one cell as tall as those rows. |
| Split Cells | Undoes a merge, sideways or downwards. What a downward merge hid is still there and comes back. |
| Split Table | Makes the caret's row the first row of a table of its own. |
| Table AutoFormat | A list of ready-made looks -- rules, shading, a bold heading -- with a preview; the one chosen goes on the caret's table in a single undo step. |
| Select Row / Select Table | Selects one or all of it. |

Drag a cell's right edge to change the column widths.

### Sorting

**Table ▸ Sort Ascending** and **Sort Descending** order the rows by the
text in their first column. A header row stays where it is, and each row's
formatting travels with it.

### Converting

**Table ▸ Convert** turns a table into paragraphs with tabs between what
were the cells — and turns tabbed paragraphs back into a table, one column
per tab.

### Table ▸ Table Properties

The rules round the cells on or off, the width of the lines, shading behind
the caret's cell, each of a cell's four sides ruled on its own, a least
height for a row, and whether the first row repeats at the top of every
page a long table runs on to. Every one of these is a single undo step.

**Table ▸ Table Gridlines** shows the cells of an unruled table faintly on
screen. It never prints.

The marks that hold a table together cannot be deleted by accident:
selecting across a table and pressing Delete empties the cells and leaves
the table standing. Delete the rows to delete the table.

---

## 10. Pictures, drawings and frames

### Insert ▸ Picture

Puts a picture into the text, where it takes one position exactly as a
character does: it wraps with the text, sits on the baseline, and deletes
and undoes as one thing. Any format the system's image library can read —
PNG, JPEG, GIF, BMP, TIFF, WebP, AVIF, HEIF, SVG.

Click a picture and eight handles appear: drag a corner to resize it
keeping its proportions, a side to stretch it. A dotted outline shows the
size it will be until you let go.

**Format ▸ Picture** sets the size by number and how the text treats it:
in the line, or set to one side with the text running round it.

### Insert ▸ Drawing

A line, arrow, rectangle, rounded rectangle or ellipse, with its line
width, colour and fill. Word42 draws them as pictures, so they resize with
the handles and travel through RTF, HTML and PDF like any picture.

### Format ▸ Frame

Sets a paragraph in a frame at the left or right of the column, with the
text after it running down the other side. Framed paragraphs one after
another share a frame.

### Format ▸ Drop Cap

Drops a paragraph's first letter over three lines, or as many as you ask
for. Beside a dropped letter, a frame or a picture, the text is set line by
line, and the rest of the paragraph returns to full width.

---

## 11. Notes, references and fields

### Footnotes and endnotes

**Insert ▸ Footnote** (Ctrl+Alt+F) puts a numbered mark at the caret and
takes you to the note at the foot of the page. **Insert ▸ Endnote**
(Ctrl+Alt+E) is the same, numbered i, ii, iii, with the notes after the
text under a rule. Ctrl+Alt+N jumps between a mark and its note.

Notes are numbered in order and move with their lines from page to page.
Deleting a mark deletes its note; undo brings back both.

### Bookmarks and hyperlinks

**Insert ▸ Bookmark** (Ctrl+Shift+F5) names the selection. Edit ▸ Go To
finds bookmarks by name.

**Insert ▸ Hyperlink** (Ctrl+K) makes the selection a link, or puts the
address in as one. Links are blue and underlined, the pointer says so over
them, and Ctrl+click follows one.

**Insert ▸ Cross-reference** puts in the page number a bookmark is on, or
the bookmarked text itself.

### Table of contents

**Insert ▸ Table of Contents** puts one paragraph per heading at the caret,
indented by level, with dots running out to the page number at a right tab
stop at the margin.
It is ordinary text, to edit or delete freely. **Insert ▸ Update Table of
Contents** rebuilds it in place from the headings and page numbers as they
now are.

### Index

An index is made the way a book's is: mark the words as you write, then
gather them.

Select a word or a phrase and **Insert ▸ Index ▸ Mark Entry** marks it.
The box offers the words themselves as the entry; type something else to
file it under that instead — "Cats" marked, but filed under "Animals".
The words on the page do not change and read as they did.

**Insert ▸ Index ▸ Build the Index** puts the index in at the caret: one
paragraph per entry, in alphabetical order, with the page numbers at a
right tab stop and dots leading out to them. Ask for it again and the
index already there is replaced where it stands, so it can be brought up
to date after the text has moved. The pages are the layout's, so ask for
the index in Page Layout view.

The marks travel with the document: RTF and Word carry them as XE
fields, OpenDocument as the pair of index marks it has for the purpose,
and LibreOffice reads all three as its own index entries.

### Captions and annotations

**Insert ▸ Caption** starts a "Figure N:" paragraph in the Caption style,
N counting on from the captions already there.

**Insert ▸ Annotation** (Ctrl+Alt+A) attaches a note to the selected text,
shown as a pale wash. The annotations box lists them, selects each one on a
click, and deletes them. They carry your name from Tools ▸ Options.

### Fields

**Insert ▸ Field** puts a page number, the number of pages, the date, the
time, the file name or the word count into the text as a field. A field
shows its result, shaded grey; **Insert ▸ Update Fields** (F9) renews every
one, and so does printing or exporting.

### Date, time and symbols

**Insert ▸ Date and Time** (Alt+Shift+D) offers today in a dozen formats.
**Insert ▸ Symbol** is a grid of the characters a keyboard has not got. It
stays open while you work, so you can pick, type, and pick again.

---

## 12. Tools

### Spelling

Words the dictionary does not know are underlined in red as you type;
**Tools ▸ Automatic Spell Checking** turns that off. **Tools ▸ Spelling**
(F7) opens the box: Not in Dictionary, Change To, Suggestions, and Ignore,
Ignore All, Change, Change All and Add.

Word42 uses whatever dictionary is installed for your language. Words in
another script than the dictionary's are left alone — an English dictionary
is not asked to judge Chinese, Russian or Arabic — and words with soft
hyphens in them are checked as though the hyphens were not there. Without
any dictionary at all, the box says so.

### Tools ▸ Language

**Tools ▸ Language** marks the selected text as written in a language of
its own: pick it from the list, and the spelling checker uses the
dictionary for it rather than the document's. A tick in the list marks the
languages a dictionary is installed for; the first entry leaves the text
in the document's own language, and "(no proofing)" says the text is not
language at all and is never checked — the right thing for a code
listing or a part number.

With nothing selected the language is set for what is typed next, the way
bold is. The mark travels with the text: RTF carries it as `\lang`, Word
as `w:lang`, OpenDocument as `fo:language` and `fo:country`, AbiWord as
its `lang` property and HTML as `lang=`.

### Tools ▸ AutoCorrect

What is put right as you type, as Word 6 did it:

- **Quotes** — a straight `"` or `'` becomes the opening or closing curly
  one, whichever fits where it stands.
- **TWo INitial CApitals** — the second capital goes.
- **The first word of a sentence** — takes its capital.
- **Two hyphens** — become a dash.
- **Misspellings** — a short list of the ones a hand makes rather than a
  head: teh, adn, thier, recieve, and a few more, with (c), (r) and (tm)
  becoming ©, ® and ™.

The dialog lists them and carries the switch; Tools ▸ Options has the
switch as well. A correction and the character that prompted it are one
undo step, so Ctrl+Z once puts back exactly what you typed.

### Tools ▸ Word Count

Pages, words, characters with and without their spaces, paragraphs and
lines. **Include footnotes and endnotes** counts what is down in the
notes as well, and with text selected the box counts the selection
beside the document.

### Tools ▸ Envelopes and Labels

**Tools ▸ Envelopes and Labels** makes an envelope or a sheet of labels
as a document of its own, in a new window. For an envelope, give the
delivery address and the return address and pick the size: the page
becomes the envelope's, the return address sits small in the top corner
and the delivery address a third of the way in and two fifths of the way
down, where a sorting machine looks for it. For labels, the delivery
address box is the label's text and the sheet is a table with a cell per
label, no rules, each cell the label's size — the same text on every
label, or on the first one only.

The delivery address starts as whatever is selected in the document, and
the return address as the name in Tools ▸ Options. Sizes are named by
what they measure rather than by any maker's catalogue number.

### Tools ▸ Hyphenation

**Hyphenate Document** puts soft hyphens into words by the language's
patterns, so lines may break inside words; **Remove Hyphenation** takes
them out again. The soft hyphens travel in RTF and HTML, and the spelling
checker looks past them.

### Tools ▸ Revisions

**Mark Revisions While Editing** (Ctrl+Shift+E): from then on, text you
type is underlined in red and text you delete is struck through in red
rather than removed. **Accept All Revisions** keeps the insertions and
drops the deletions; **Reject All Revisions** does the opposite. The marks
survive in RTF and `.docx`, and export to HTML as `<ins>` and `<del>`.

### Tools ▸ Mail Merge

1. **Get Data** opens a CSV file whose first row names the fields.
2. **Insert Merge Field** puts «Name» into the letter.
3. **Merge to New Document** writes one copy of the letter per record, each
   on its own page, and opens the result in a new window.

### Tools ▸ Options

- **Measurement Units** — inches or centimetres, used by every dialog and
  the ruler.
- **Default View** and **Default Zoom** — what a new window opens with.
- **User Info** — your name, which annotations and revisions carry into
  Word and OpenDocument files.
- **Spelling** — check spelling as you type, on or off.
- **Correct as you type** — the AutoCorrect switch, the same one the
  AutoCorrect dialog carries.

The settings live in a small file in your configuration directory, along
with the toolbar and ruler switches from the View menu.

---

## 13. Views and windows

### View ▸ Normal and View ▸ Page Layout

**Normal** is a continuous galley: one column, no page furniture, the
fastest way to write. **Page Layout** is the printed page — sheets with
their margins, headers, footers, columns and footnotes in place, centred on
a light grey desk. Both use the same layout engine, so what you see in one
is what the other will print.

The zoom box on the standard toolbar runs from 25% to 500%; the View menu
has 75%, 100%, 150% and 200%.

### View ▸ Show Formatting Marks (Ctrl+Shift+8)

A dot for every space, an arrow for a tab, a bent arrow for a line break
and a pilcrow at every paragraph end, in blue.

### View ▸ Full Screen

Gives the page the whole screen: the menu bar, both toolbars, the ruler
and the status bar go away. Escape brings them back, or View ▸ Full
Screen again.

### View ▸ Slide Show

The document's outline, presented. Each heading and the lines under it make
a slide; the paragraphs before the first heading make one of their own. The
show fills the screen, in type large enough for a room.

| Key | Does |
| --- | --- |
| Space, Enter, →, ↓, Page Down, click | The next slide |
| Backspace, ←, ↑, Page Up, right-click | The one before |
| Home / End | The first / the last |
| Escape, Q | End the show |

It starts on the slide the caret is in, so a talk can be picked up where it
was left. **File ▸ Export as Presentation** writes the same slides as a
`.pptx` file, and **File ▸ Open** reads one back: each slide's title becomes
a Heading 1 and its lines the paragraphs under it, which is the outline the
talk was made from.

### The Window menu

**New Window** opens a second window on the *same* document: type in one
and the other shows it. **Split** divides the window into two panes on the
same document, one above the other, with a bar to drag between them — read
one part while writing in another, each pane scrolling and keeping a caret
of its own, with the toolbars, the ruler and the status bar following the
pane you are editing; Split again puts the window back to one pane.
**Arrange All** puts the open windows side by side.
Below them, the menu lists and numbers every open document; pick one to
raise it.

---

## 14. Printing and exporting

**File ▸ Print** (Ctrl+P) prints through the system's print dialog.

**File ▸ Print Preview** (Ctrl+F2) opens Word42's own preview: the pages on
a grey ground, a zoom, a page counter and a Print button. It is the same
layout engine that draws the document, so the preview is the print.

**File ▸ Export as PDF** writes the document as a PDF, pictures, notes and
all.

**File ▸ Export as Web Page** writes one self-contained HTML file:
headings, paragraphs with their alignment and indents, runs with their
font, size, weight, colour and links, lists, tables, pictures embedded in
the file itself, footnotes as links to the notes at the end, annotations as
tooltips, and revision marks as `<ins>` and `<del>`. Nothing is left
pointing at another file, so the page can be sent as it is.

Fields are updated before printing and before either export.

---

## 15. Languages and scripts

Word42 is built on a modern text stack, and handles the scripts of the
world in one document:

- **Every script** — Latin, Cyrillic, Greek, Chinese, Japanese, Korean,
  Arabic, Hebrew, Thai, Devanagari and the rest. Where the font you have
  asked for has no glyph for a character, Word42 looks through the fonts
  installed for that script and uses the first that has one, so a paragraph
  in Times New Roman can still show 你好世界 or こんにちは. The document
  keeps the font you chose; only the drawing falls back.
- **Right to left** — Arabic and Hebrew shape and run right-to-left on
  their own. Format ▸ Paragraph ▸ Direction makes the whole paragraph
  right-to-left, so its first line starts at the right margin.
- **Combining marks** — accents and marks that sit on the letter before
  them are kept with it: the caret steps over the pair, and Backspace
  removes it whole.
- **Emoji** — colour emoji draw as colour emoji where a colour font is
  installed, including the ones built from several characters.
- **Files** — every format Word42 writes carries the full range of Unicode:
  RTF escapes what it must (surrogate pairs included), and `.docx`, `.odt`,
  `.abw` and HTML are UTF-8 throughout. Plain text is read as UTF-8, and
  falls back to Windows-1252 for older files; CRLF, CR and LF line endings
  all work.
- **Spelling** — the checker judges only the script its dictionary is
  written for, so text in other scripts is not marked wrong, and a run
  marked with a language of its own (Tools ▸ Language) is checked with
  that language's dictionary when one is installed.
- **Hyphenation** — uses the pattern dictionary for your language, if one
  is installed.

---

## 16. Help

**Help ▸ Contents** (F1) opens the help window: the sections of this
guide on the left, the one chosen on the right. The guide travels inside
the program, so the help is the same wherever Word42 is installed and
whatever is on the machine.

**Search for Help on...** puts the caret in the search box at the top:
type a word and the list narrows to the sections that mention it, the
ones with it in their title first. **Index** turns the list into every
sub-heading in the guide, in alphabetical order.

**Word42 on the Web** and **Report a Bug** open word42.org and the
project's issue list in whatever the desktop uses for the web. Nothing
about the document is sent anywhere.

---

## 17. Keyboard reference

### Files

| Key | Command |
| --- | --- |
| Ctrl+N | New |
| Ctrl+O | Open |
| Ctrl+S | Save |
| Ctrl+Shift+S, F12 | Save As |
| Ctrl+W | Close |
| Ctrl+P | Print |
| Ctrl+F2 | Print Preview |
| Ctrl+Q | Exit |

### Editing

| Key | Command |
| --- | --- |
| Ctrl+Z | Undo |
| Ctrl+Y, Ctrl+Shift+Z | Redo |
| F4 | Repeat |
| Ctrl+X / Ctrl+C / Ctrl+V | Cut / Copy / Paste |
| Ctrl+A | Select All |
| Ctrl+F | Find |
| F3 | Find Next |
| Ctrl+H | Replace |
| Ctrl+G, F5 | Go To |

### Formatting

| Key | Command |
| --- | --- |
| Ctrl+B / Ctrl+I / Ctrl+U | Bold / Italic / Underline |
| Ctrl+D | Font |
| Ctrl+] / Ctrl+[ | Grow / shrink the font |
| Ctrl+L / Ctrl+E / Ctrl+R / Ctrl+J | Left / Centre / Right / Justified |
| Ctrl+Shift+N | Normal style |
| Ctrl+Alt+1 / 2 / 3 | Heading 1 / 2 / 3 |
| Ctrl+Shift+L | Bullets |
| Shift+F3 | Change case |

### Inserting

| Key | Command |
| --- | --- |
| Ctrl+Enter | Page break |
| Ctrl+Alt+F | Footnote |
| Ctrl+Alt+E | Endnote |
| Ctrl+Alt+N | Go to note |
| Ctrl+K | Hyperlink |
| Ctrl+Shift+F5 | Bookmark |
| Ctrl+Alt+A | Annotation |
| Alt+Shift+D | Date and time |
| F9 | Update fields |

### Tools and view

| Key | Command |
| --- | --- |
| F7 | Spelling |
| Ctrl+Shift+E | Mark revisions while editing |
| Ctrl+Shift+8 | Show formatting marks |
| F1 | Help contents |
| Shift+F10 | The context menu, at the caret |
| Escape | Leave Full Screen |

---

## 18. Where Word42 keeps things

| What | Where |
| --- | --- |
| Settings — units, default view and zoom, your name, toolbar and ruler switches | The `word42` folder in your configuration directory (`%APPDATA%` on Windows, `~/.config` elsewhere) |
| Autosave copies | The `word42/autosave` folder in your data directory |
| Recent files | With the settings |

Word42 is free software under the GNU General Public License, version 3 or
later. It is not affiliated with, nor endorsed by, the makers of any other
word processor; where the names of file formats appear, they are there to
say which format is meant.

See also [BUILD.md](BUILD.md) for building it from source,
[ARCHITECTURE.md](ARCHITECTURE.md) for how it works inside,
[PARITY.md](PARITY.md) for how it compares with other word processors, and
[ROADMAP.md](ROADMAP.md) for what is coming.
