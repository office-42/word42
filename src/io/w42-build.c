/* w42-build.c - see w42-build.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-build.h"

#include <string.h>

static W42ApIdx
builder_ap (W42Builder *b)
{
  W42Fmt fmt;

  w42_fmt_init_default (&fmt);
  fmt.ch = b->ch;
  fmt.pa = b->pa;
  return w42_ap_table_intern (w42_pt_ap_table (b->pt), &fmt);
}

void
w42_builder_init (W42Builder *b, W42PieceTable *pt)
{
  memset (b, 0, sizeof *b);
  w42_pt_load_text (pt, "");
  b->pt = pt;
  b->pos = w42_pt_first_caret_pos (pt);
  b->table = -1;
  b->note_return = (gsize) -1;
  w42_builder_reset_char (b);
  w42_builder_reset_para (b);
}

void
w42_builder_reset_char (W42Builder *b)
{
  W42Fmt def;

  w42_fmt_init_default (&def);
  b->ch = def.ch;
}

void
w42_builder_reset_para (W42Builder *b)
{
  W42Fmt def;

  w42_fmt_init_default (&def);
  b->pa = def.pa;
}

/* In a cell, a paragraph that ended waits for more text before its
 * successor is made, so a cell never ends with an empty one. */
static void
cell_break (W42Builder *b)
{
  if (b->table >= 0 && b->in_cell && b->cell_break_pending)
    {
      w42_pt_insert_block (b->pt, b->pos, builder_ap (b));
      b->pos += 1;
      b->cell_break_pending = FALSE;
    }
}

void
w42_builder_text (W42Builder *b, const char *utf8)
{
  if (utf8 == NULL || *utf8 == '\0')
    return;
  cell_break (b);
  w42_pt_insert_text (b->pt, b->pos, utf8, builder_ap (b));
  b->pos += g_utf8_strlen (utf8, -1);
  b->in_para = TRUE;
}

void
w42_builder_object (W42Builder *b, GBytes *data, const char *format,
                    int pixel_w, int pixel_h, int width, int height)
{
  W42ObjectIdx idx;

  if (data == NULL || pixel_w <= 0 || pixel_h <= 0)
    return;
  cell_break (b);
  idx = w42_object_table_add (w42_pt_object_table (b->pt), data, format,
                              pixel_w, pixel_h,
                              width > 0 ? width : pixel_w * 15,
                              height > 0 ? height : pixel_h * 15);
  w42_pt_insert_object (b->pt, b->pos, idx, builder_ap (b));
  b->pos += 1;
  b->in_para = TRUE;
}

/* A paragraph's properties are known only once it ends, and they belong to
 * the BLOCK strux in front of its text.  w42_pt_apply_para_fmt widens
 * backwards to find that strux, so it has to start from the last thing
 * written rather than from `pos` itself: a document that opens with a table
 * keeps its final empty paragraph's BLOCK sitting at `pos`, and widening
 * from there would format that one instead -- which is why the first
 * paragraph of every cell used to come back with nothing on it. */
static void
builder_apply_para (W42Builder *b)
{
  w42_pt_apply_para_fmt (b->pt, b->pos > 0 ? b->pos - 1 : 0, 0,
                         W42_PARA_ALL, &b->pa);
}

void
w42_builder_end_paragraph (W42Builder *b)
{
  builder_apply_para (b);

  if (b->table >= 0 && b->in_cell)
    {
      /* A second empty paragraph in a row: the first one is made now. */
      if (b->cell_break_pending)
        {
          w42_pt_insert_block (b->pt, b->pos, builder_ap (b));
          b->pos += 1;
        }
      b->cell_break_pending = TRUE;
    }
  else
    {
      w42_pt_insert_block (b->pt, b->pos, builder_ap (b));
      b->pos += 1;
    }
  b->in_para = FALSE;
  /* The next paragraph starts plain; a reader sets what it wants. */
  w42_builder_reset_para (b);
}

/* ---- notes ------------------------------------------------------------ */

void
w42_builder_begin_note (W42Builder *b, gboolean endnote)
{
  gsize body;

  if (b->note_return != (gsize) -1)
    return;                       /* no notes within notes */
  cell_break (b);
  body = endnote ? w42_pt_insert_endnote (b->pt, b->pos, builder_ap (b))
                 : w42_pt_insert_footnote (b->pt, b->pos, builder_ap (b));
  b->note_return = b->pos + 1;
  b->pos = body;
  b->in_para = TRUE;
}

void
w42_builder_end_note (W42Builder *b)
{
  if (b->note_return == (gsize) -1)
    return;
  if (b->in_para)
    builder_apply_para (b);
  b->pos = b->note_return;
  b->note_return = (gsize) -1;
  b->in_para = TRUE;
}

/* ---- tables ----------------------------------------------------------- */

gboolean
w42_builder_in_table (W42Builder *b)
{
  return b->table >= 0;
}

void
w42_builder_begin_table (W42Builder *b, int n_cols, const int *widths)
{
  if (b->table >= 0)
    return;
  n_cols = CLAMP (n_cols, 1, 1023);

  /* Only a paragraph that is actually open needs ending; one that has just
   * ended left its mark behind, and ending it again would leave an empty
   * paragraph in front of the table. */
  if (b->in_para)
    w42_builder_end_paragraph (b);

  /* The document now ends in a paragraph mark -- the one just made, the one
   * the paragraph before left, or the one an empty document started life
   * with.  The table goes in ahead of it, and it becomes the paragraph that
   * follows the table; otherwise every table would arrive with a blank line
   * above it. */
  b->table_before_block = FALSE;
  if (b->pos >= 2 && b->pos == w42_pt_length (b->pt))
    {
      char *tail = w42_pt_get_text (b->pt, b->pos - 1, 1);

      b->table_before_block = (tail != NULL && *tail == '\n');
      g_free (tail);
    }
  if (b->table_before_block)
    b->pos -= 1;

  b->n_cols = MAX (n_cols, 1);
  b->table = w42_pt_insert_table_start (b->pt, b->pos, b->n_cols, widths);
  b->pos += 1;
  b->row = 0;
  b->col = 0;
  b->in_cell = FALSE;
  b->cell_break_pending = FALSE;
}

void
w42_builder_begin_cell (W42Builder *b, int span)
{
  if (b->table < 0 || b->in_cell)
    {
      b->cell_pos = (gsize) -1;       /* or a stray cell element would hang
                                       * its properties on an earlier cell */
      return;
    }
  if (b->col >= b->n_cols || b->row > 4095)
    {
      b->cell_pos = (gsize) -1;       /* nothing to hang cell properties on */
      return;                         /* more cells than columns, or rows than the mark holds: dropped */
    }

  b->cell_pos = b->pos;
  w42_pt_insert_cell (b->pt, b->pos, b->table, b->row, b->col, builder_ap (b));
  b->pos += 2;
  if (span > 1)
    w42_pt_set_cell_span (b->pt, b->cell_pos, MIN (span, b->n_cols - b->col));
  b->in_cell = TRUE;
  b->cell_break_pending = FALSE;
  b->in_para = FALSE;
  b->col += MAX (span, 1);
}

void
w42_builder_end_cell (W42Builder *b)
{
  if (b->table < 0 || !b->in_cell)
    return;
  if (b->in_para)
    builder_apply_para (b);
  b->in_cell = FALSE;
  b->in_para = FALSE;
  b->cell_break_pending = FALSE;
}

void
w42_builder_end_row (W42Builder *b)
{
  if (b->table < 0)
    return;
  w42_builder_end_cell (b);
  while (b->col < b->n_cols && b->row <= 4095)
    {
      int before = b->col;

      w42_builder_begin_cell (b, 1);
      w42_builder_end_cell (b);
      if (b->col == before)
        break;                        /* nothing was made: do not spin */
    }
  b->row++;
  b->col = 0;
}

void
w42_builder_end_table (W42Builder *b)
{
  if (b->table < 0)
    return;
  if (b->in_cell || b->col > 0)
    w42_builder_end_row (b);
  if (b->row == 0)
    {
      /* No rows at all: one empty one, so the table is well formed. */
      w42_builder_end_row (b);
    }

  if (b->table_before_block)
    {
      w42_pt_insert_table_end_only (b->pt, b->pos);
      b->pos += 2;
    }
  else
    {
      w42_pt_insert_table_end (b->pt, b->pos, builder_ap (b));
      b->pos += 2;
    }
  b->table = -1;
  b->in_para = FALSE;
}

void
w42_builder_finish (W42Builder *b)
{
  if (b->note_return != (gsize) -1)
    w42_builder_end_note (b);
  w42_builder_end_table (b);

  /* The last paragraph's end left an empty paragraph behind, as a
   * trailing newline would in a text file.  Drop it -- remembering that the
   * body ends where the notes begin, not where the document does, so that a
   * document with footnotes drops it too. */
  {
    gsize body_end = w42_pt_notes_start (b->pt);

    if (body_end == (gsize) -1)
      body_end = w42_pt_length (b->pt);

    if (!b->in_para && b->pos >= 1 && b->pos == body_end)
      {
        gsize first = w42_pt_first_caret_pos (b->pt);

        if (b->pos - 1 > first)
          w42_pt_delete (b->pt, b->pos - 1, 1);
      }
    else if (b->in_para)
      builder_apply_para (b);
  }
}

void
w42_builder_object_wrap (W42Builder *b, W42Wrap wrap)
{
  W42ObjectTable *table = w42_pt_object_table (b->pt);
  guint n = w42_object_table_size (table);

  if (n > 0 && wrap != W42_WRAP_INLINE)
    w42_object_table_set_wrap (table, n - 1, wrap);
}
