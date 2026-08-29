# Word42 architecture

This document explains the shape of the program: what the layers are, what
each one is allowed to know about, and why the document model is built the way
it is. It is aimed at someone about to change the code.

## The layers

```
    ui/       W42Application -> W42Window -> W42View, W42Ruler
      |             GTK 4 widgets, actions, menus, input
      v
    layout/   W42Layout
      |             Pango line breaking, pagination, position <-> pixel
      v
    model/    W42Document -> W42PieceTable -> W42ApTable
      |             text, structure, formatting, undo
      v
    io/       importers and exporters
```

Dependencies point downwards only. The model knows nothing about GTK; it is
plain C over GLib. The layout engine knows about Pango and Cairo but not about
widgets or events. Everything that knows what a mouse is lives in `ui/`.

That split is why `libw42core` builds as its own static library: the model,
the layout engine and the importers link without GTK, and nothing graphical
can reach into them.

## The document model

### Two buffers

A document has two character buffers, both arrays of `gunichar` (UCS-4, so
indexing is O(1) and no offset is ever ambiguous):

- **initial** — the text of the file as it was loaded. Written once, never
  touched again.
- **change** — everything typed since. Appended to, never overwritten, never
  truncated.

Because neither buffer is ever modified in place, a deletion cannot destroy
data. It only removes the *references* to it.

### Pieces

The document is a doubly-linked list of pieces. Each names a contiguous run of
one buffer, plus the formatting that run carries:

```c
struct _W42Piece {
  W42Piece *prev, *next;
  guint8    type;       /* TEXT or STRUX */
  guint8    strux;      /* SECTION or BLOCK, when type is STRUX */
  guint8    in_change;  /* which buffer a TEXT piece points into */
  W42ApIdx  ap;         /* formatting */
  gsize     offset;     /* character offset into that buffer */
  gsize     length;     /* characters; a strux is always 1 */
};
```

Inserting in the middle of a run splits a piece in two. Deleting a range
splits at both ends and unlinks what is between. After every public operation
`pt_coalesce()` merges neighbouring pieces that came from the same run of the
same buffer with the same formatting, so ordinary editing does not fragment
the list without bound.

### Positions

A **document position** counts things, where a thing is one character or one
structural mark. A new document is:

| position | contents |
|---|---|
| 0 | `strux SECTION` |
| 1 | `strux BLOCK` — one empty paragraph |

so its length is 2, and position 2 is the only place the caret can sit.

A position is a legal caret spot when the thing immediately before it is text
or a paragraph mark. That single rule excludes the slot between the section
mark and the first paragraph mark, and needs no special cases anywhere else.

`w42_pt_first_caret_pos()` finds the first legal spot, and deletions are
clamped so the opening section mark and first paragraph mark can never be
removed. A document always has somewhere to put the caret.

### Struxes give you Enter and Backspace for free

Structure is not a tree and not a separate list; it is marks *in the same
sequence as the text*.

- **Enter** inserts a `BLOCK` strux at the caret. Everything after it is now
  in the new paragraph, automatically, because paragraph membership is defined
  by "which BLOCK strux comes before me".
- **Backspace at the start of a paragraph** deletes the `BLOCK` strux in front
  of the caret, and the two paragraphs are one.

Neither operation is written as a special case. They are `insert` and `delete`
on the same sequence as everything else.

### Formatting: the AP table

Pieces do not carry formatting, they carry a `W42ApIdx` — an index into an
interned table of `W42Fmt` records:

```c
typedef struct {
  W42CharFmt ch;   /* family, size, bold, italic, underline, colour, script */
  W42ParaFmt pa;   /* style, alignment, indents, spacing */
} W42Fmt;
```

Interning is by byte comparison, which is only sound because every `W42Fmt`
starts life zeroed by `w42_fmt_init_default()` — that defines the padding
bytes. **Always build a `W42Fmt` from that function**, never from an
uninitialised stack struct.

A text piece varies only the `ch` half; a `BLOCK` strux varies only the `pa`
half. That is why paragraph formatting survives deleting all the text in a
paragraph: it lives on the mark, not on the characters. It is also why an
empty paragraph knows how tall to be.

Applying formatting never touches the text. `pt_apply_fmt()` walks the range,
computes the AP each position should end up with, and swaps them in. Positions
the caller's mask does not apply to keep the AP they already had, so the
operation is uniform and its inverse is exact.

### Undo: one routine, two directions

A change record describes an edit that *has already happened*:

```c
CR_INSERT   len things appeared at pos
CR_DELETE   this content vanished from pos   (content saved in the record)
CR_FMT      these AP runs used to cover [pos, pos+len)
```

`pt_apply()` performs the **opposite** of what a record describes, and returns
a record describing what it just did. So:

- undoing an insert deletes the range — and the deletion captures the content,
  turning the record into a `CR_DELETE` that would redo it;
- redoing that `CR_DELETE` re-inserts the content, turning it back into a
  `CR_INSERT`.

The stack entry flips direction each time it is used. There is no separate
redo stack and no second code path.

Two conveniences sit on top:

- **Coalescing.** A run of single-character insertions merges into the
  preceding `CR_INSERT`, so a typed word undoes as a word. Anything that
  should end the run — a caret move, a click, a command — calls
  `w42_pt_break_undo_coalesce()`.
- **Grouping.** `w42_pt_begin_group()` / `w42_pt_end_group()` tag records with
  a shared id, and undo consumes a whole group at once. Replacing a selection
  is a delete plus an insert and undoes as one step.

## Layout

`w42_layout_build()` throws away the previous layout and reformats everything.
That is honest about what it is: correct, simple, and fast enough for
documents of the size Word 6 was built for. Incremental reformatting is the
obvious next optimisation and the interface does not need to change for it.

### Units

Measurements are stored in **twips** (1/1440 inch), which is what Word's own
file formats use. Layout converts to pixels at a fixed **96 dpi** reference
resolution — never the screen's actual DPI:

```c
#define W42_TWIPS_PER_PX (1440.0 / 96.0)   /* 15.0 */
```

Zoom is applied as a `cairo_scale()` at paint time, and the layout's
`PangoContext` has its resolution pinned to 96 with
`pango_cairo_context_set_resolution()`. The consequence matters: **a page
breaks in the same place at every zoom level, on every screen**. A word
processor whose pagination moved when you zoomed would be useless.

### Pagination

Blocks are snapshotted out of the piece table (`w42_pt_snapshot_blocks()`) as
UTF-8 text plus a list of runs. Each becomes a `PangoLayout` with a
`PangoAttrList` built from the runs. Pango does the line breaking, shaping and
bidi; Word42 then walks the resulting lines with a `PangoLayoutIter` and drops
each onto a page, starting a new one when a line will not fit.

The output is a flat array of `W42LineBox`, each recording its page, its
position in page-relative pixels, its byte range in the block, and a borrowed
pointer to the `PangoLayoutLine`.

### Positions to pixels and back

Because a block's characters occupy consecutive document positions right after
its paragraph mark, the mapping is arithmetic rather than a search:

```c
doc_pos = block->start_pos + 1 + character_index_of(byte)
```

From there `pango_layout_line_index_to_x()` gives the caret's x, and
`pango_layout_line_x_to_index()` turns a click back into a position. Selection
highlighting uses `pango_layout_line_get_x_ranges()`, which is bidi-correct —
a selection over mixed left-to-right and right-to-left text draws as the two
or three visual rectangles it actually occupies.

Note the coordinate subtlety, which the `W42LineBox` records separately:
`index_to_x` and `x_to_index` are relative to the **line's** start, while
`get_x_ranges` is relative to the **paragraph's text column** (`origin_x`).

## Styles and section numbers

A style is a name for a `W42CharFmt` and a `W42ParaFmt` together with an
outline level. The stylesheet lives beside the AP table in the piece table.
Applying a style writes the paragraph half onto the paragraph mark — which
is where `pa.style` lives — and the font, size, weight and slant onto the
paragraph's runs. Word keeps direct formatting on top of a style; Word42
replaces it, which is simpler and rarely what anyone notices.

Section numbers are not text. The layout engine walks the blocks with one
counter per outline level, resets the deeper counters whenever a shallower
heading comes along, and hangs the number on the heading's first line box as
a `PangoLayout` of its own, with the first line indented to clear it. Every
painter — screen, printer, preview, PDF — draws lines through
`w42_layout_draw_line`, which is where the number is painted, so the four
cannot disagree. Turn numbering off and the numbers vanish without the
document changing at all.

Enter after a heading gives a Normal paragraph: a heading's "next style" is
Normal, as it is in Word, since nobody wants two Heading 1s in a row.

## Word .doc

`w42-doc.c` reads Word 97-2003 files. An OLE2 walker (FAT, mini FAT,
directory) hands over the WordDocument and Table streams; the File
Information Block says where everything else is. Text comes through
Word's own piece table, each piece 8-bit or UTF-16; paragraph and
character properties come from the formatted disk pages the "bins" point
at, as sprms applied over the paragraph style's chain, which the
stylesheet supplies. Styles are matched by their built-in identity rather
than their name, since names are localized. Tables are the cell and
row-end marks in the text with the row's shape in the row-end
paragraph's properties, emitted the same way the RTF reader emits them.

## Spelling

`w42-spell.c` wraps Enchant behind a five-call interface — check, suggest,
ignore, add, and "what is the next word of this text" — so that nothing
else knows Enchant exists, and a build without it simply has no dictionary.
The layout underlines: given a dictionary, `build_block_layout` runs each
paragraph's words past it and adds Pango's error underline to the ones it
rejects, skipping the word the caret is in because that one is still being
typed. Printing and PDF build their own layouts and never set a dictionary,
so the underlines are a screen-only thing without anyone having to say so.
The Spelling box walks the document with the same word splitter, selects
each unknown word in the view and edits through the view's ordinary
insert-over-selection, so Change is undoable like any other typing.

## Lists

A list item is a paragraph property, `W42ParaFmt.list`, and nothing more in
the model: no list table, no item text. The marker is painted the way a
heading's section number is — a small Pango layout hung on the first line
box, drawn by every painter — and numbers are counted while laying out, over
consecutive numbered paragraphs. Joining a list gives the paragraph a
hanging indent and the marker sits in it, so wrapped lines line up under
the text, as Word's do.

## Tables

A table is three more kinds of strux in the same sequence as everything
else: `TABLE` opens it and carries the table's id, `CELL` opens a cell and
carries its row and column, `ENDTABLE` closes it. A `BLOCK` always follows
a `CELL` and always follows an `ENDTABLE`, so a cell is one or more ordinary
paragraphs and the paragraph after a table is an ordinary paragraph. That is
what makes typing, selection, undo and styles work inside cells without
knowing about them: a cell's paragraph is a paragraph.

A merged cell is a `CELL` whose payload says it spans more than one
column; the columns it covers have no `CELL` of their own. Merging takes
the covered cells' marks out and leaves their paragraphs behind, so the
merged cell has all of them, as Word's does.

What has to know is deletion. The marks that hold a table together — the
three strux kinds, the `BLOCK` that opens each cell, the `BLOCK` after the
table — may only go when the whole table goes, `TABLE` through `ENDTABLE`.
`w42_pt_delete` carves a range into the stretches that may be deleted and
deletes those; Backspace at the start of a cell does nothing, as in Word.
Rows are added and removed by the model with the cells renumbered
afterwards, and renumbered again after any undo, since a row coming back
through undo is a row coming back.

Layout takes a table whole. Each row's cells are laid out side by side in
their columns, the row is as tall as its tallest cell, and a row that will
not fit on the page moves to the next page with its lines. Cell borders are
rectangles the layout records and the painters draw with the page furniture.
Cells side by side broke two assumptions in the position mapping — that the
nearest line by height is the right one, and that the next line in the array
is the line below — so both are geometric now: the nearest line by height and
then by horizontal distance, and the nearest line in the direction of travel.

## Pictures

A picture is an `OBJECT` piece — the third piece type after text and struxes,
and AbiWord's `ObjectPiece` by another name. It occupies one document
position, so everything that works on positions works on pictures without
knowing: Backspace deletes it, undo brings it back, a selection across it
highlights it, find skips it.

The bytes of the file as loaded live in a `W42ObjectTable` beside the AP
table, referenced by index from the piece. The decoded surface is a cache
built on first draw.

In a block snapshot a picture is one U+FFFC — the Object Replacement
Character, which is what Pango expects — in a run of its own carrying the
object index. The layout engine turns that into a `PangoAttrShape` with the
picture's size, so Pango measures it and breaks lines around it like any
other glyph; when a line is painted Pango calls back into the layout's shape
renderer, which draws the surface at the current point on whatever cairo
context is being painted to. That is why pictures reach the screen, the
printer and the PDF export through one piece of code.

A picture wider than the text column is shown scaled to fit. The document
keeps the size that was asked for; only the display yields.

## PDF

Export is cairo's PDF surface fed by the same line boxes the screen paints.
Import is poppler, and is lossy by nature: a PDF holds characters with
positions, not paragraphs, so the paragraphs are guessed — a line ending in
sentence punctuation followed by one starting with a capital is a break; a
hyphen at a line end is a word the typesetter broke; anything else is a wrap.
The pictures come out as PNG and go in after the text, each in a paragraph of
its own, since a text flow cannot represent where on the page they were.

## Two views, one engine

`w42_layout_set_galley()` switches the layout engine between the two views the
View menu offers. Page Layout breaks lines onto sheets; Normal leaves the
breaks out, puts the whole document on one very tall page, and swaps the page
margins for a narrow inset — Word 6 sat the galley just inside the window with
a selection bar to its left and nothing above it.

Everything else is shared. The same blocks, the same Pango layouts, the same
line boxes, the same position-to-pixel mapping. The view decides where the
text column lands on screen and the ruler asks it rather than working the
answer out a second time from the page setup, which is how the two used to
disagree in Normal view.

## The look

Word 6 predates theming, so the stylesheet states its colours outright rather
than inheriting the desktop's: the Windows 3.1 palette, a silver face, white
and light grey for a control's lit edges and mid and black grey for its shaded
ones. Every raised control is that four-tone bevel and every field is the same
bevel turned inside out. Pressed and toggled-on invert it, which is the whole
of how a 1993 toolbar told you bold was on.

The title bar is Word42's own, drawn into a `GtkWindowHandle` so that dragging
and double-click-to-maximise keep working. The desktop's title bar cannot be
made navy with centred white text, and without that the window does not read
as the right program.

## The view

`W42View` is a `GtkWidget` subclass that paints through
`gtk_snapshot_append_cairo()`. It owns the caret, the selection anchor and the
"pending" formatting — what you get when you press Ctrl+B with nothing
selected and then start typing.

Input is deliberately explicit. The key controller handles navigation and
editing keys itself and hands everything else to a `GtkIMContext`, so dead
keys, compose sequences and CJK input methods work rather than being bypassed
by a `keyval`-to-character shortcut.

The widget is not a `GtkScrollable`; it reports its full size from `measure()`
and lets `GtkScrolledWindow` wrap it in a viewport. That keeps the scrolling
code to the few lines in `view_scroll_to_caret()`.

## Things to be careful about

- **`W42Fmt` must be zeroed before use** — interning compares bytes,
  padding included. Use `w42_fmt_init_default()`.
- **Piece pointers do not survive mutation.** `pt_coalesce()` frees pieces.
  Never hold a `W42Piece *` across a public operation.
- **`PangoLayoutLine` pointers in `W42LineBox` are borrowed** from the
  layouts `W42Layout` keeps alive. They die when the layout is rebuilt.
- **Positions are characters, not bytes.** The only place bytes appear is
  inside a block's UTF-8 text, for Pango's benefit.
