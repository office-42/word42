/* w42-piecetable.h - the document model: a piece table over two buffers
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The design follows AbiWord's: the text of the file as loaded lives in an
 * immutable "initial" buffer, everything typed since lives in an append-only
 * "change" buffer, and the document is a doubly-linked list of pieces that
 * point into one or the other.  Nothing is ever overwritten or removed from a
 * buffer, which is what makes full undo and crash recovery cheap.
 *
 * Positions
 * ---------
 * A document position is a count of "things", where a thing is either one
 * character or one structural marker (strux).  A brand new document is
 *
 *      pos 0: strux SECTION
 *      pos 1: strux BLOCK      <- one empty paragraph
 *
 * so its length is 2 and the only place the caret can sit is position 2.
 * Inserting a BLOCK strux in the middle of a paragraph splits it in two;
 * deleting one merges the paragraphs back together.  That single rule gives
 * you Enter and Backspace for free.
 */

#pragma once

#include "w42-attrs.h"
#include "w42-object.h"
#include "w42-style.h"

G_BEGIN_DECLS

typedef enum {
  W42_PIECE_TEXT = 0,
  W42_PIECE_STRUX,
  W42_PIECE_OBJECT      /* a picture; occupies one position like a character */
} W42PieceType;

typedef enum {
  W42_STRUX_SECTION = 0,
  W42_STRUX_BLOCK,
  W42_STRUX_TABLE,      /* a table starts; payload is its id */
  W42_STRUX_CELL,       /* a cell starts; payload packs row, column and
                         * the columns it spans.  A BLOCK always follows,
                         * so a cell is paragraphs. */
  W42_STRUX_ENDTABLE,   /* a table ends; a BLOCK always follows */
  W42_STRUX_FOOTNOTE,   /* a footnote's reference mark, in the text; payload
                         * is the note's id.  Takes one position, like a
                         * picture, and is drawn as the note's number. */
  W42_STRUX_NOTES,      /* the notes section: the last thing in the
                         * document, holding every footnote's paragraphs */
  W42_STRUX_NOTE        /* a footnote's paragraphs start; payload is its id.
                         * A BLOCK always follows. */
} W42StruxType;

/* A table's shape.  Column widths are in twips; 0 means share the column
 * equally, which is what Insert Table gives you. */
typedef struct {
  int     n_cols;
  GArray *widths;       /* int, n_cols of them */
  guint8  borders;      /* the cells are ruled; on unless turned off */
  guint8  header_rows;  /* rows repeated at the top of every page */
  GArray *row_heights;  /* int twips, the least height of each row; may be
                         * shorter than the table, and 0 means as tall as
                         * the text */
} W42TableProps;

typedef struct _W42PieceTable W42PieceTable;

/* ---- Formatting masks ------------------------------------------------- */

typedef enum {
  W42_CHAR_FAMILY    = 1 << 0,
  W42_CHAR_SIZE      = 1 << 1,
  W42_CHAR_BOLD      = 1 << 2,
  W42_CHAR_ITALIC    = 1 << 3,
  W42_CHAR_UNDERLINE = 1 << 4,
  W42_CHAR_STRIKEOUT = 1 << 5,
  W42_CHAR_SCRIPT    = 1 << 6,
  W42_CHAR_COLOR     = 1 << 7,
  W42_CHAR_LINK      = 1 << 8,
  W42_CHAR_BOOKMARK  = 1 << 9,
  W42_CHAR_SMALLCAPS = 1 << 10,
  W42_CHAR_ALLCAPS   = 1 << 11,
  W42_CHAR_HIGHLIGHT = 1 << 12,
  W42_CHAR_SPACING   = 1 << 13,
  W42_CHAR_COMMENT   = 1 << 14,
  W42_CHAR_REVISION  = 1 << 15,
  W42_CHAR_OVERLINE  = 1 << 16,
  W42_CHAR_FIELD     = 1 << 17
} W42CharMask;

typedef enum {
  W42_PARA_STYLE        = 1 << 0,
  W42_PARA_ALIGN        = 1 << 1,
  W42_PARA_INDENT_LEFT  = 1 << 2,
  W42_PARA_INDENT_RIGHT = 1 << 3,
  W42_PARA_INDENT_FIRST = 1 << 4,
  W42_PARA_SPACE_BEFORE = 1 << 5,
  W42_PARA_SPACE_AFTER  = 1 << 6,
  W42_PARA_LINE_SPACING = 1 << 7,
  W42_PARA_LINE_SPACING_PCT = 1 << 8,
  W42_PARA_PAGE_BREAK   = 1 << 9,
  W42_PARA_LIST         = 1 << 10,
  W42_PARA_TABS         = 1 << 11,
  W42_PARA_BORDER       = 1 << 12,
  W42_PARA_SHADING      = 1 << 13,
  W42_PARA_FLOW         = 1 << 14,   /* keep with next, keep together, widows */
  W42_PARA_SECTION      = 1 << 15,   /* section break and its columns */
  W42_PARA_FRAME        = 1 << 16,   /* drop cap, and the frame at the side */
  W42_PARA_CELL_SPAN    = 1 << 17,   /* a cell's vertical merge */
  W42_PARA_ALL          = (1 << 18) - 1
} W42ParaMask;

/* ---- Block snapshots -------------------------------------------------- */

/* A maximal span of characters sharing one formatting record, expressed in
 * the coordinates the layout engine wants: UTF-8 byte offsets. */
typedef struct {
  gsize        doc_pos;      /* document position of the run's first character */
  gsize        byte_offset;  /* offset into W42Block.text */
  gsize        n_bytes;
  gsize        n_chars;
  W42ApIdx     ap;
  W42ObjectIdx object;       /* W42_OBJECT_NONE for text.  An object run is
                              * one U+FFFC in the text, standing in for the
                              * picture the way Pango expects. */
  int          footnote;     /* 0, or the number of the footnote whose
                              * reference mark this run is: one U+FFFC too */
  int          footnote_id;
  gboolean     endnote;      /* the note goes at the end, numbered i, ii */
} W42Run;

/* One paragraph, flattened for layout. */
typedef struct {
  gsize    start_pos;    /* position of the BLOCK strux itself */
  W42ApIdx ap;           /* the strux's AP: the paragraph formatting */
  GString *text;         /* UTF-8, no trailing newline */
  GArray  *runs;         /* W42Run */
  int      table;        /* the table this paragraph is a cell of, or -1 */
  int      row;
  int      col;
  int      span;         /* columns the cell covers; 1 unless merged */
  W42ApIdx cell_ap;      /* the CELL mark's AP: its own borders, if any */
  int      note;         /* the footnote this paragraph belongs to, or -1 */
  int      note_number;  /* that footnote's number, by order of reference */
  gboolean note_end;     /* it is an endnote's paragraph */
} W42Block;

void w42_block_free (W42Block *block);

/* ---- Lifecycle -------------------------------------------------------- */

W42PieceTable *w42_pt_new           (void);
void           w42_pt_free          (W42PieceTable *pt);

/* Replaces the whole document with `text`, treating '\n' as a paragraph
 * break, and loads it into the immutable initial buffer.  Clears undo. */
void           w42_pt_load_text     (W42PieceTable *pt, const char *utf8);

W42ApTable    *w42_pt_ap_table      (W42PieceTable *pt);
W42ObjectTable *w42_pt_object_table (W42PieceTable *pt);
W42StyleSheet  *w42_pt_stylesheet   (W42PieceTable *pt);

/* ---- Headers and footers ---------------------------------------------- */

/* One line of text at the top of every page and one at the bottom, set in
 * the Normal style.  The text may carry fields, spelt the way Word showed
 * its field codes: {PAGE} for the page number, {NUMPAGES} for the count,
 * {DATE} for today.  Not part of the undo history. */
typedef struct {
  char     *text;      /* NULL or "" for none */
  W42Align  align;
} W42PageText;

/* File > Summary Info: what a document says about itself.  The strings
 * are interned or NULL; every format that has somewhere to put them
 * writes them, and reads them back. */
typedef struct {
  const char *title;
  const char *subject;
  const char *author;
  const char *keywords;
  const char *comments;
} W42DocInfo;

const W42DocInfo *w42_pt_get_info (W42PieceTable *pt);
void              w42_pt_set_info (W42PieceTable *pt, const W42DocInfo *info);

/* Who is writing: the name annotations and revisions carry in files.
 * Interned; NULL means unknown.  Not part of the undo history. */
void        w42_pt_set_author (W42PieceTable *pt, const char *name);
const char *w42_pt_get_author (W42PieceTable *pt);

/* A document can carry three headers and three footers: the one most
 * pages use, one for a title page, and one for even-numbered pages.
 * The plain get and set functions are the ordinary one. */
typedef enum {
  W42_PAGE_TEXT_DEFAULT = 0,
  W42_PAGE_TEXT_FIRST,
  W42_PAGE_TEXT_EVEN,
  W42_PAGE_TEXT_KINDS
} W42PageTextKind;

const W42PageText *w42_pt_get_header (W42PieceTable *pt);
const W42PageText *w42_pt_get_footer (W42PieceTable *pt);
void w42_pt_set_header (W42PieceTable *pt, const char *text, W42Align align);
void w42_pt_set_footer (W42PieceTable *pt, const char *text, W42Align align);

const W42PageText *w42_pt_get_header_kind (W42PieceTable *pt, W42PageTextKind kind);
const W42PageText *w42_pt_get_footer_kind (W42PieceTable *pt, W42PageTextKind kind);
void w42_pt_set_header_kind (W42PieceTable *pt, W42PageTextKind kind,
                             const char *text, W42Align align);
void w42_pt_set_footer_kind (W42PieceTable *pt, W42PageTextKind kind,
                             const char *text, W42Align align);

/* Whether the title page and the even-numbered pages use their own:
 * a document may want a blank first-page header, so the choice is kept
 * apart from whether the text is empty. */
gboolean w42_pt_get_title_page   (W42PieceTable *pt);
void     w42_pt_set_title_page   (W42PieceTable *pt, gboolean on);
gboolean w42_pt_get_facing_pages (W42PieceTable *pt);
void     w42_pt_set_facing_pages (W42PieceTable *pt, gboolean on);

/* The header or footer a page uses, counting pages from 0. */
const W42PageText *w42_pt_page_header (W42PieceTable *pt, int page);
const W42PageText *w42_pt_page_footer (W42PieceTable *pt, int page);
gsize          w42_pt_length        (W42PieceTable *pt);

/* ---- Queries ---------------------------------------------------------- */

gboolean  w42_pt_is_caret_pos   (W42PieceTable *pt, gsize pos);
gsize     w42_pt_first_caret_pos(W42PieceTable *pt);
gsize     w42_pt_clamp_pos      (W42PieceTable *pt, gsize pos);
gsize     w42_pt_next_pos       (W42PieceTable *pt, gsize pos);
gsize     w42_pt_prev_pos       (W42PieceTable *pt, gsize pos);

/* AP of the thing at `pos`, or of the thing before it when `pos` is the end
 * of the document.  Used to seed the caret's pending formatting. */
W42ApIdx  w42_pt_ap_at          (W42PieceTable *pt, gsize pos);
/* AP of the BLOCK strux governing `pos`. */
W42ApIdx  w42_pt_block_ap_at    (W42PieceTable *pt, gsize pos);
/* Whether a paragraph mark sits exactly at `pos`: the caret there is at
 * the end of the paragraph before, not the start of the one after. */
gboolean  w42_pt_is_block_mark  (W42PieceTable *pt, gsize pos);

GPtrArray *w42_pt_snapshot_blocks (W42PieceTable *pt);  /* of W42Block* */

/* ---- Mutation --------------------------------------------------------- */

void w42_pt_insert_text  (W42PieceTable *pt, gsize pos, const char *utf8, W42ApIdx ap);
void w42_pt_insert_block (W42PieceTable *pt, gsize pos, W42ApIdx ap);
void w42_pt_insert_object (W42PieceTable *pt, gsize pos, W42ObjectIdx object, W42ApIdx ap);

/* The picture at `pos`, or W42_OBJECT_NONE when there is none there. */
W42ObjectIdx w42_pt_object_at (W42PieceTable *pt, gsize pos);

/* Shows the picture at `pos` at a new size, in twips.  Pictures are never
 * changed in place -- the table only grows -- so this is a new entry with
 * the same bytes put where the old one was, as one undo step. */
void w42_pt_resize_object (W42PieceTable *pt, gsize pos, int width, int height);
/* Format > Picture: how the text runs round the picture at `pos`. */
void w42_pt_set_object_wrap (W42PieceTable *pt, gsize pos, W42Wrap wrap);
void w42_pt_delete       (W42PieceTable *pt, gsize pos, gsize n);

void w42_pt_apply_char_fmt (W42PieceTable    *pt,
                            gsize             pos,
                            gsize             n,
                            W42CharMask       mask,
                            const W42CharFmt *value);

/* Applies a named style to every paragraph touching [pos, pos+n): the
 * paragraph mark takes the style's paragraph formatting and the text its
 * font, size, weight and slant, as one undo step. */
void w42_pt_apply_style (W42PieceTable *pt, gsize pos, gsize n, const char *name);

/* Re-applies a style to every paragraph that carries it, for after its
 * definition has changed. */
void w42_pt_restyle (W42PieceTable *pt, const char *name);
/* The same for the style and every style based on it, after
 * w42_stylesheet_follow has recomputed them.  One undo step. */
void w42_pt_restyle_tree (W42PieceTable *pt, const char *name);
/* A character style: its font, size, weight, slant, underline, colour and
 * case go on to [pos, pos+n) as one undo step. */
void w42_pt_apply_char_style (W42PieceTable *pt, gsize pos, gsize n, const char *name);
/* Every paragraph in style `from` is restyled as `to`, for when `from` is
 * being taken out of the sheet.  One undo step. */
void w42_pt_replace_style (W42PieceTable *pt, const char *from, const char *to);

void w42_pt_apply_para_fmt (W42PieceTable    *pt,
                            gsize             pos,
                            gsize             n,
                            W42ParaMask       mask,
                            const W42ParaFmt *value);

/* ---- Annotations ------------------------------------------------------ */

/* The annotations in the document, in order: each a range and its text. */
typedef struct {
  gsize       start;
  gsize       end;
  const char *text;      /* interned */
} W42Annotation;

GArray *w42_pt_annotations (W42PieceTable *pt);   /* of W42Annotation */

/* ---- Bookmarks -------------------------------------------------------- */

/* The range of text carrying bookmark `name`, or FALSE.  The names in the
 * document, each once, sorted; free with g_strfreev(). */
gboolean w42_pt_find_bookmark  (W42PieceTable *pt, const char *name,
                                gsize *start, gsize *end);
char   **w42_pt_bookmark_names (W42PieceTable *pt);

/* ---- Fragments -------------------------------------------------------- */

/* A new document holding a copy of the range: its paragraphs, their
 * formatting and pictures.  Table and note marks are left out, their
 * text kept.  For the clipboard. */
W42PieceTable *w42_pt_extract (W42PieceTable *pt, gsize start, gsize n);

/* Puts a fragment's paragraphs in at `pos`, formatting and pictures with
 * them, and returns how many positions went in.  Not its own undo
 * group: the caller opens one. */
gsize w42_pt_insert_fragment (W42PieceTable *pt, gsize pos, W42PieceTable *frag);

/* ---- Footnotes -------------------------------------------------------- */

/* Puts a reference mark at `pos` and an empty paragraph for the note in the
 * notes section at the end of the document, as one undo step.  Returns the
 * first caret position in the note.  Deleting the mark deletes the note. */
gsize w42_pt_insert_footnote (W42PieceTable *pt, gsize pos, W42ApIdx ap);

/* The same, but the note goes at the end of the document: an endnote. */
gsize w42_pt_insert_endnote  (W42PieceTable *pt, gsize pos, W42ApIdx ap);

/* Whether note `id` is an endnote; and, for importers, making it one. */
gboolean w42_pt_note_is_endnote (W42PieceTable *pt, int id);
void     w42_pt_set_note_endnote (W42PieceTable *pt, int id, gboolean endnote);

/* The id of the reference mark at `pos`, or -1. */
int   w42_pt_footnote_at (W42PieceTable *pt, gsize pos);

/* The first caret position in note `id`'s paragraphs, and the position of
 * its reference mark; (gsize) -1 when there is no such note. */
gsize w42_pt_note_body      (W42PieceTable *pt, int id);
gsize w42_pt_note_reference (W42PieceTable *pt, int id);

/* Where the notes section starts, or (gsize) -1 when there is none. */
gsize w42_pt_notes_start    (W42PieceTable *pt);

/* ---- Tables ----------------------------------------------------------- */

/* Puts a rows-by-cols table of empty cells at `pos`, which should be the
 * end of a paragraph; a new paragraph follows the table.  One undo step. */
void w42_pt_insert_table (W42PieceTable *pt, gsize pos, int rows, int cols,
                          W42ApIdx ap);

/* The pieces, for importers that meet a table one cell at a time. */
int  w42_pt_insert_table_start (W42PieceTable *pt, gsize pos, int n_cols,
                                const int *widths);
void w42_pt_insert_cell        (W42PieceTable *pt, gsize pos, int table,
                                int row, int col, W42ApIdx ap);
void w42_pt_insert_table_end   (W42PieceTable *pt, gsize pos, W42ApIdx ap);

/* The ENDTABLE mark alone, for when a paragraph already follows. */
void w42_pt_insert_table_end_only (W42PieceTable *pt, gsize pos);

const W42TableProps *w42_pt_table_props (W42PieceTable *pt, int table);

/* New column widths, in twips, n_cols of them.  One undo step. */
void w42_pt_table_set_widths (W42PieceTable *pt, int table,
                              const int *widths, int n);

/* Merges the cells of `row` from `col_from` to `col_to` into one that
 * spans them, keeping every cell's paragraphs.  One undo step. */
void w42_pt_table_merge_cells (W42PieceTable *pt, int table, int row,
                               int col_from, int col_to);

/* Table > Merge Cells down a column: the cell at (row, col) covers
 * `rows` rows, and the cells under it are marked as covered.  FALSE when
 * there is no such cell, or not that many rows under it.  One undo
 * step. */
gboolean w42_pt_merge_cells_down (W42PieceTable *pt, int table, int row, int col, int rows);

/* And the merge undone: the cells under it are their own again. */
gboolean w42_pt_split_cells_down (W42PieceTable *pt, int table, int row, int col);

/* For importers: turns the "starts here" and "covered" marks a file
 * carries into the number of rows each merged cell covers. */
void w42_pt_resolve_vmerges (W42PieceTable *pt, int table);

/* The rows the cell at (row, col) covers: 1 for an ordinary cell,
 * W42_CELL_COVERED for one covered by the cell above. */
int  w42_pt_cell_vspan (W42PieceTable *pt, int table, int row, int col);

/* For importers: the vertical span of the CELL mark at `cell_pos`. */
void w42_pt_set_cell_vspan (W42PieceTable *pt, gsize cell_pos, int vspan);

/* How many columns the cell at (row, col) spans, or 0 when no cell
 * starts there -- a column a merged cell covers has no cell of its own. */
int  w42_pt_cell_span (W42PieceTable *pt, int table, int row, int col);

/* For importers: sets the span of the CELL mark at `cell_pos` in place,
 * outside the undo history. */
void w42_pt_set_cell_span (W42PieceTable *pt, gsize cell_pos, int span);

/* The cell `pos` sits in, or FALSE outside any table. */
gboolean w42_pt_cell_at (W42PieceTable *pt, gsize pos,
                         int *table, int *row, int *col);

/* The first caret position inside a cell, or (gsize) -1 if there is no such
 * cell.  Tab moves the caret with this. */
gsize w42_pt_cell_start (W42PieceTable *pt, int table, int row, int col);
int   w42_pt_table_rows (W42PieceTable *pt, int table);

/* Where a table is: `start` its TABLE mark, `end` one past its ENDTABLE.
 * And where a row is: `start` its first CELL mark, `end` the next row's
 * first CELL mark or the table's ENDTABLE.  FALSE when there is no such
 * table or row. */
gboolean w42_pt_table_bounds (W42PieceTable *pt, int table, gsize *start, gsize *end);
gboolean w42_pt_row_bounds   (W42PieceTable *pt, int table, int row,
                              gsize *start, gsize *end);

/* Table > Split Table: the rows from `row` on become a table of their
 * own, with a paragraph between the two.  One undo step. */
void  w42_pt_table_split (W42PieceTable *pt, int table, int row);

/* The content of a cell: from its first caret position to the mark that
 * ends it.  FALSE when there is no such cell. */
gboolean w42_pt_cell_range (W42PieceTable *pt, int table, int row, int col,
                            gsize *start, gsize *end);

/* Table > Sort: the rows in the order of their first cell's text, the
 * header rows left where they are, the cells' formatting with them.
 * One undo step. */
void w42_pt_table_sort (W42PieceTable *pt, int table, gboolean descending);

/* Table > Convert.  A table becomes paragraphs with tabs between what
 * were its cells; paragraphs become a table, split at their tabs.  The
 * conversions return where the result starts, or (gsize) -1. */
gsize w42_pt_table_to_text (W42PieceTable *pt, int table);
gsize w42_pt_text_to_table (W42PieceTable *pt, gsize start, gsize end);

/* Adds an empty row after `row`; deletes a row, or the whole table when it
 * is the last one.  Each is one undo step. */
void w42_pt_table_insert_row (W42PieceTable *pt, int table, int row);
/* A column after `col`, or the column `col` taken out; every row gets
 * or loses a cell, a merged cell across the place grows or shrinks.
 * One undo step each. */
void w42_pt_table_insert_column (W42PieceTable *pt, int table, int col);
void w42_pt_table_delete_column (W42PieceTable *pt, int table, int col);
/* Table > Table Properties: whether the cells are ruled.  One undo step. */
void w42_pt_table_set_borders (W42PieceTable *pt, int table, gboolean borders);
/* The least height of `row` in twips (0: the text's), and how many rows
 * from the top repeat on every page the table runs on to.  Undo steps. */
void w42_pt_table_set_row_height (W42PieceTable *pt, int table, int row, int twips);
int  w42_pt_table_get_row_height (W42PieceTable *pt, int table, int row);
void w42_pt_table_set_header_rows (W42PieceTable *pt, int table, int n);
/* Splits the merged cell at (row, col) back into cells of one column
 * each; the text stays in the first.  One undo step. */
void w42_pt_table_split_cell (W42PieceTable *pt, int table, int row, int col);
/* A cell's own borders: W42_BORDER_* bits, or -1 for the table's setting.
 * One undo step.  The _at form takes the CELL mark's position and is
 * for importers: not recorded. */
void w42_pt_cell_set_borders    (W42PieceTable *pt, int table, int row, int col, int sides);
int  w42_pt_cell_get_borders    (W42PieceTable *pt, int table, int row, int col);
void w42_pt_cell_set_borders_at (W42PieceTable *pt, gsize cell_pos, int sides);
void w42_pt_table_delete_row (W42PieceTable *pt, int table, int row);

/* The position just past the paragraph containing `pos`: the next mark of
 * any kind, or the end of the document; and the position of the paragraph's
 * own mark.  A paragraph is empty when the two are one apart. */
gsize w42_pt_paragraph_end   (W42PieceTable *pt, gsize pos);
gsize w42_pt_paragraph_start (W42PieceTable *pt, gsize pos);

/* ---- Undo ------------------------------------------------------------- */

/* Everything between begin/end collapses into a single undo step. */
/* Revision marks: text inserted or deleted while changes were being
 * tracked carries revision 1 or 2.  Accepting keeps insertions and drops
 * deletions; rejecting does the opposite.  Either clears the marks, as one
 * undo step.  Returns whether anything changed. */
gboolean w42_pt_has_revisions     (W42PieceTable *pt);
gboolean w42_pt_resolve_revisions (W42PieceTable *pt, gboolean accept);

void     w42_pt_begin_group (W42PieceTable *pt);
void     w42_pt_end_group   (W42PieceTable *pt);

gboolean w42_pt_can_undo    (W42PieceTable *pt);
/* Where the undo history stands: how many records can be undone, and a
 * serial that grows with every new record.  Equal pairs mean the same
 * document state, so a document can tell when undo has taken it back
 * to what was saved. */
void     w42_pt_undo_state  (W42PieceTable *pt, gsize *undo_pos, guint64 *serial);
gboolean w42_pt_can_redo    (W42PieceTable *pt);
/* Both return the position the caret should move to, or (gsize) -1. */
gsize    w42_pt_undo        (W42PieceTable *pt);
gsize    w42_pt_redo        (W42PieceTable *pt);
void     w42_pt_clear_undo  (W42PieceTable *pt);

/* ---- Text extraction -------------------------------------------------- */

/* UTF-8 for [pos, pos+n).  Struxes other than the first become '\n'. */
char *w42_pt_get_text (W42PieceTable *pt, gsize pos, gsize n);

/* Consecutive single-character insertions collapse into one undo step, the
 * way typing a word does in Word.  Anything that should end that run -- a
 * caret move, a click, a menu command -- calls this. */
void w42_pt_break_undo_coalesce (W42PieceTable *pt);

G_END_DECLS
