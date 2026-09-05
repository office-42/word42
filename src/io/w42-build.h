/* w42-build.h - building a document one paragraph at a time, for importers
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A reader that walks a file front to back (HTML, .docx, .abw) wants to
 * say "text in this formatting", "end of paragraph", "a cell", "a note"
 * and have the piece table's positions kept straight for it.  This is that.
 */

#pragma once

#include "w42-piecetable.h"

G_BEGIN_DECLS

typedef struct {
  W42PieceTable *pt;
  gsize          pos;           /* where the next text goes */
  W42CharFmt     ch;            /* the formatting text goes in with */
  W42ParaFmt     pa;            /* the paragraph's, applied when it ends */
  gboolean       in_para;       /* the current paragraph has content */

  int            table;         /* the table being built, or -1 */
  int            row, col, n_cols;
  gboolean       in_cell;
  gboolean       cell_break_pending;
  gboolean       table_before_block;
  gsize          cell_pos;      /* the CELL mark of the open cell */

  gsize          note_return;   /* where the body goes on after a note, or -1 */
} W42Builder;

void w42_builder_init  (W42Builder *b, W42PieceTable *pt);
/* Drops the empty paragraph the last end_paragraph left, closes any open
 * table. */
void w42_builder_finish (W42Builder *b);

void w42_builder_reset_char (W42Builder *b);   /* ch back to the default */
void w42_builder_reset_para (W42Builder *b);   /* pa back to the default */

void w42_builder_text   (W42Builder *b, const char *utf8);
void w42_builder_object (W42Builder *b, GBytes *data, const char *format,
                         int pixel_w, int pixel_h, int width, int height);
void w42_builder_end_paragraph (W42Builder *b);
/* Sets how the text wraps round the picture w42_builder_object just put in. */
void w42_builder_object_wrap (W42Builder *b, W42Wrap wrap);
/* And where it sits: twips from the column's left and its paragraph's top. */
void w42_builder_object_position (W42Builder *b, int x, int y);
/* A shape rather than a picture: `width` by `height` twips, with its
 * outline, its fill and the text in it (or NULL). */
void w42_builder_shape (W42Builder *b, W42ShapeKind kind, int width, int height,
                        double line_pt, guint32 line_rgb, gboolean filled, guint32 fill_rgb,
                        const char *text);

/* A note: the mark goes in at the current position, and text until
 * end_note goes into the note's body. */
void w42_builder_begin_note (W42Builder *b, gboolean endnote);
void w42_builder_end_note   (W42Builder *b);

/* Tables: cells one after another, rows ended explicitly; widths in
 * twips, or NULL for equal shares of the column. */
void w42_builder_begin_table (W42Builder *b, int n_cols, const int *widths);
void w42_builder_begin_cell  (W42Builder *b, int span);
void w42_builder_end_cell    (W42Builder *b);
void w42_builder_end_row     (W42Builder *b);
void w42_builder_end_table   (W42Builder *b);
gboolean w42_builder_in_table (W42Builder *b);

G_END_DECLS
