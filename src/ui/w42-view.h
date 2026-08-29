/* w42-view.h - the editing canvas: pages, caret, selection, input
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-document.h"
#include "w42-layout.h"
#include "w42-tableformat.h"

G_BEGIN_DECLS

/* Word 6 opened in Normal view -- a plain white galley -- and kept Page
 * Layout, with its grey desktop and paper edges, for when you wanted to see
 * where the pages fell. */
typedef enum {
  W42_VIEW_NORMAL = 0,
  W42_VIEW_PAGE_LAYOUT
} W42ViewMode;

#define W42_TYPE_VIEW (w42_view_get_type ())
G_DECLARE_FINAL_TYPE (W42View, w42_view, W42, VIEW, GtkWidget)

GtkWidget *w42_view_new (void);

void         w42_view_set_document (W42View *self, W42Document *doc);
W42Document *w42_view_get_document (W42View *self);
W42Layout   *w42_view_get_layout   (W42View *self);

/* Where the text column sits in widget coordinates, so that the ruler can
 * line up with it in either view. */
void        w42_view_get_text_column (W42View *self,
                                      double  *left,
                                      double  *width,
                                      double  *page_left,
                                      double  *page_width);

void        w42_view_set_mode (W42View *self, W42ViewMode mode);
W42ViewMode w42_view_get_mode (W42View *self);

void   w42_view_set_zoom (W42View *self, double zoom);
double w42_view_get_zoom (W42View *self);

gsize    w42_view_get_caret         (W42View *self);
/* The selection's two ends in order; both the caret when there is none. */
void     w42_view_get_selection_bounds (W42View *self, gsize *start, gsize *end);
gboolean w42_view_has_selection     (W42View *self);

/* ---- Editing ---------------------------------------------------------- */

void w42_view_insert_text (W42View *self, const char *utf8);

/* Ctrl+Enter: a new paragraph that starts a new page. */
void w42_view_insert_page_break (W42View *self);
/* Tools > Options: the corrections made as you type. */
void     w42_view_set_autocorrect   (W42View *self, gboolean on);
gboolean w42_view_get_autocorrect   (W42View *self);

/* Whether the caret is in a footnote's or endnote's text. */
gboolean w42_view_caret_in_note     (W42View *self);

/* Edit > AutoText: the entry named by what was typed before the caret
 * takes its place.  FALSE when nothing there names one. */
gboolean w42_view_expand_autotext    (W42View *self);

/* Insert > Caption: a paragraph under the caret's, in the Caption
 * style, with `label` in it, as one undo step. */
void     w42_view_insert_caption    (W42View *self, const char *label);

/* Enter: the paragraph is split at the caret. */
void w42_view_insert_paragraph (W42View *self);
/* Selects the word the caret is in, as a double click does. */
void w42_view_select_word (W42View *self);

/* Tables.  Insert puts one after the caret's paragraph; the caret lands in
 * the first cell.  Rows are added after or deleted at the caret's row. */
void     w42_view_insert_table     (W42View *self, int rows, int cols);
gboolean w42_view_in_table         (W42View *self);
void     w42_view_table_insert_row (W42View *self);
void     w42_view_table_delete_row (W42View *self);
void     w42_view_table_insert_column (W42View *self);
void     w42_view_table_delete_column (W42View *self);
/* Table Properties: rules on or off, and shading for the caret's cell. */
void     w42_view_table_set_borders (W42View *self, gboolean borders);
gboolean w42_view_table_get_borders (W42View *self);
void     w42_view_cell_set_shading (W42View *self, int percent);
/* The caret's cell's ruled sides as W42_BORDER_* bits (its own, or the
 * table's), and setting its own; -1 takes them away again. */
int      w42_view_cell_get_borders (W42View *self);
void     w42_view_cell_set_borders (W42View *self, int sides);

/* Merges the cells the selection touches, if they lie in one row. */
void     w42_view_table_merge_cells (W42View *self);
/* Splits the caret's merged cell back into single cells. */
void     w42_view_table_split_cell (W42View *self);
/* Table > Table AutoFormat: a ready-made look on the caret's table. */
void     w42_view_table_autoformat (W42View *self, const W42TableFormat *fmt,
                                    gboolean heading, gboolean first_column);

/* Table > Select: the caret's row, or the whole table. */
void     w42_view_table_select_row   (W42View *self);
void     w42_view_table_select_table (W42View *self);
/* Table > Split Table: the caret's row starts a table of its own. */
/* TRUE when the caret's row became the first row of a table of its
 * own: FALSE outside a table, and on the first row, which has
 * nothing above it to split off. */
gboolean w42_view_table_split_table  (W42View *self);
/* Table > Sort: the rows by the text of their first cell, the header
 * rows left where they are. */
void     w42_view_table_sort (W42View *self, gboolean descending);
/* Table > Convert: the caret's table becomes paragraphs with tabs
 * between the cells, or the selected paragraphs become a table, split
 * at their tabs.  FALSE when there is nothing to convert. */
gboolean w42_view_table_to_text (W42View *self);
gboolean w42_view_text_to_table (W42View *self);
/* The caret's row's least height in twips, and the header rows repeated
 * on every page, from Table Properties. */
int      w42_view_table_get_row_height (W42View *self);
void     w42_view_table_set_row_height (W42View *self, int twips);
int      w42_view_table_get_header_rows (W42View *self);
void     w42_view_table_set_header_rows (W42View *self, int n);

/* Puts a picture at the caret, replacing the selection if there is one.
 * The view takes its own reference to `data`. */
void w42_view_insert_picture (W42View    *self,
                              GBytes     *data,
                              const char *format,
                              int         pixel_w,
                              int         pixel_h);
/* Format > Picture: the selected picture's size in twips and its wrap,
 * FALSE when no picture is selected; and setting them. */
gboolean w42_view_get_picture (W42View *self, int *width, int *height, W42Wrap *wrap);
void     w42_view_set_picture (W42View *self, int width, int height, W42Wrap wrap);

void w42_view_undo        (W42View *self);
void w42_view_redo        (W42View *self);
void w42_view_cut         (W42View *self);
void w42_view_copy        (W42View *self);
void w42_view_paste       (W42View *self);
/* Edit > Paste Special: the clipboard's text without its formatting. */
void w42_view_paste_text  (W42View *self);
/* Edit > Clear: the selection goes, and does not reach the clipboard. */
void w42_view_clear       (W42View *self);
/* Insert > File: another document's paragraphs at the caret. */
void w42_view_insert_fragment (W42View *self, W42PieceTable *frag);
void w42_view_select_all  (W42View *self);

/* Selects [start, end) and brings it on screen.  Find uses this to show a
 * match; anything that needs to point at a stretch of the document can. */
void w42_view_select_range (W42View *self, gsize start, gsize end);

/* The selected text, or NULL when there is no selection. */
char *w42_view_get_selected_text (W42View *self);

/* ---- Formatting ------------------------------------------------------- */

/* The formatting that would apply to text typed right now: the selection's
 * if there is one, otherwise the caret's plus anything toggled since the
 * caret last moved. */
void w42_view_get_char_fmt (W42View *self, W42CharFmt *out);
W42Align w42_view_get_align (W42View *self);

/* The paragraph formatting at the caret, and a way to set any part of it. */
void w42_view_get_para_fmt   (W42View *self, W42ParaFmt *out);
void w42_view_apply_para_fmt (W42View *self, W42ParaMask mask,
                              const W42ParaFmt *value);

/* Any character formatting on the selection -- or, with none, on what is
 * typed next -- by mask. */
void w42_view_apply_char_fmt   (W42View *self, W42CharMask mask, const W42CharFmt *value);
void w42_view_toggle_bold      (W42View *self);
void w42_view_toggle_italic    (W42View *self);
void w42_view_toggle_underline (W42View *self);
void w42_view_set_font_family  (W42View *self, const char *family);
void w42_view_set_font_size    (W42View *self, int half_points);
void w42_view_set_align        (W42View *self, W42Align align);

/* The list the caret's paragraph is in, and making the selected paragraphs
 * items of one (or of none).  Joining a list gives a paragraph a hanging
 * indent of a quarter inch for the marker to sit in; leaving takes it
 * away again. */
W42ListKind w42_view_get_list (W42View *self);
void        w42_view_set_list (W42View *self, W42ListKind kind);
/* Restarts the numbering at the caret's paragraph at `start`; 0 continues. */
void        w42_view_set_list_start (W42View *self, int start);
/* Moves the caret's list item a level in or out; FALSE if not in a list. */
gboolean    w42_view_list_level_by (W42View *self, int delta);

/* ---- Hyperlinks and bookmarks ------------------------------------------ */

/* Makes the selection a link to `url` (NULL removes the link); with no
 * selection, inserts `text` as the link.  The link at the caret, or NULL. */
void        w42_view_set_link      (W42View *self, const char *url, const char *text);
const char *w42_view_get_link      (W42View *self);
gboolean    w42_view_follow_link   (W42View *self, const char *url);

/* An annotation on the selection (NULL removes it); the one at the caret. */
void        w42_view_set_comment   (W42View *self, const char *text);
const char *w42_view_get_comment   (W42View *self);

/* Names the selection `name` (NULL clears); selects a named place. */
void        w42_view_set_bookmark  (W42View *self, const char *name);
gboolean    w42_view_go_to_bookmark (W42View *self, const char *name);

/* ---- Table of contents ------------------------------------------------ */

/* Puts a table of contents at the caret: one paragraph per heading, indented
 * by level, with the page number at a right tab stop at the margin.  Plain
 * paragraphs, as Word 6's field result was: edit or delete them freely.
 * Returns how many entries were made. */
int  w42_view_insert_toc (W42View *self);

/* ---- Footnotes -------------------------------------------------------- */

/* A reference mark at the caret and an empty note to type into; the caret
 * goes to the note.  With the caret on a mark, or in a note, the other
 * function jumps between the two. */
void w42_view_insert_footnote (W42View *self);
void w42_view_insert_endnote  (W42View *self);
/* TRUE when the caret moved: FALSE when it is at neither a note
 * nor a note's mark. */
gboolean w42_view_go_to_note  (W42View *self);

/* ---- Go To ------------------------------------------------------------ */

/* Pages and lines are counted from 1, as the status bar counts them; a
 * number past the end goes to the end.  Lines are the laid-out lines of
 * the current view. */
void w42_view_go_to_page (W42View *self, int page);
void w42_view_go_to_line (W42View *self, int line);
int  w42_view_line_count (W42View *self);

/* ---- Spelling --------------------------------------------------------- */

/* The dictionary the view underlines against, or NULL for none.  Borrowed:
 * whoever sets it keeps it alive, and sets NULL before freeing it. */
void     w42_view_set_spell     (W42View *self, W42Spell *spell);

/* Tools > Revisions.  While marking, typing is underlined and deleting
 * strikes through instead of removing; accept or reject settles them. */
typedef enum {
  W42_CASE_SENTENCE,
  W42_CASE_LOWER,
  W42_CASE_UPPER,
  W42_CASE_TITLE,
  W42_CASE_TOGGLE
} W42CaseKind;

void     w42_view_change_case (W42View *self, W42CaseKind kind);

/* Insert > Index Entry: the selection is marked as an entry, filed under
 * `term` when that differs from the words marked.  FALSE with nothing
 * selected.  Insert > Index gathers the marked runs into an index at the
 * caret -- or replaces the index already there -- and returns how many
 * entries it made. */
gboolean w42_view_mark_index_entry (W42View *self, const char *term);
int      w42_view_insert_index     (W42View *self);

/* Insert > Cross-reference: the page number the bookmark is on, or the
 * bookmarked text itself, put in at the caret.  FALSE if no such
 * bookmark. */
gboolean w42_view_insert_cross_reference (W42View *self, const char *bookmark,
                                          gboolean page_number);

/* Rebuilds the table of contents Insert > Table of Contents put in;
 * FALSE when there is none. */
gboolean w42_view_update_toc  (W42View *self);

/* Insert > Section Break: the caret's paragraph is split and the new one
 * starts a section on a new page, with the columns the old one had. */
void     w42_view_insert_section_break (W42View *self);

/* Tools > Hyphenation: soft hyphens into or out of every word.  Returns
 * the count, or -1 when there is no hyphenator in this build. */
int      w42_view_hyphenate (W42View *self, gboolean remove);

/* Insert > Field: a field of the given code at the caret, with its result
 * worked out; and Update Fields (F9), which renews every field's result.
 * Returns how many changed. */
void     w42_view_insert_field  (W42View *self, const char *code);
int      w42_view_update_fields (W42View *self);

/* Format > Columns.  DOCUMENT sets the page's columns, which sections
 * without their own use; SECTION the caret's section; FORWARD starts a
 * new section at the caret with these columns, as Word's "This point
 * forward" did. */
typedef enum { W42_COLUMNS_DOCUMENT, W42_COLUMNS_SECTION, W42_COLUMNS_FORWARD } W42ColumnsScope;
void     w42_view_set_columns (W42View *self, int columns, int gap, W42ColumnsScope scope);
/* The columns in force at the caret, whichever level they come from. */
void     w42_view_get_columns (W42View *self, int *columns, int *gap);

void     w42_view_set_show_marks (W42View *self, gboolean show);
void     w42_view_set_gridlines  (W42View *self, gboolean show);

/* Format > Drop Cap and Format > Frame, on the paragraphs the selection
 * touches: the lines a dropped first letter spans (0 for none); the side
 * the paragraph is framed at (W42_FRAME_NONE takes it out) and the
 * frame's width in twips (0 for a third of the column). */
void     w42_view_set_drop_cap (W42View *self, int lines);
void     w42_view_set_frame    (W42View *self, int side, int width);

void     w42_view_set_track_changes (W42View *self, gboolean on);
gboolean w42_view_get_track_changes (W42View *self);
/* TRUE when there were revision marks to resolve. */
gboolean w42_view_resolve_revisions (W42View *self, gboolean accept);
void     w42_view_spell_refresh (W42View *self);

/* The first word at or after `from` that `spell` does not know, as a
 * document range. */
gboolean w42_view_find_misspelling (W42View *self, W42Spell *spell, gsize from,
                                    gsize *start, gsize *end);

/* The style of the paragraph at the caret, and applying one to every
 * paragraph the selection touches. */
const char *w42_view_get_style   (W42View *self);
void        w42_view_apply_style (W42View *self, const char *name);

G_END_DECLS
