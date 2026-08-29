/* w42-envelope.c - see w42-envelope.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-envelope.h"

#include <string.h>

/* A millimetre is 56.7 twips; an inch is 1440. */
#define MM(x) ((int) ((x) * 56.7 + 0.5))
#define IN(x) ((int) ((x) * 1440.0 + 0.5))

/* The envelope sizes a letter is likely to go in, named by what they
 * measure.  The width is the long side: an envelope is printed the way
 * it is addressed, lying on its side. */
static const W42EnvelopeSize ENVELOPES[] = {
  { "Envelope #10 (9 1/2 x 4 1/8 in)", IN (9.5),  IN (4.125) },
  { "Envelope DL (220 x 110 mm)",      MM (220),  MM (110) },
  { "Envelope C5 (229 x 162 mm)",      MM (229),  MM (162) },
  { "Envelope C6 (162 x 114 mm)",      MM (162),  MM (114) },
  { "Envelope Monarch (7 1/2 x 3 7/8 in)", IN (7.5), IN (3.875) },
  { "Envelope 6 3/4 (6 1/2 x 3 5/8 in)",   IN (6.5), IN (3.625) },
};

/* Sheets of labels, named by how many there are and what they measure,
 * with the page they are printed on. */
static const W42LabelSheet LABELS[] = {
  { "24 per sheet, A4 (63.5 x 33.9 mm)",
    MM (210), MM (297), 3, 8, MM (63.5), MM (33.9), MM (7), MM (13) },
  { "21 per sheet, A4 (70 x 42.3 mm)",
    MM (210), MM (297), 3, 7, MM (70), MM (42.3), MM (0), MM (0) },
  { "14 per sheet, A4 (99.1 x 38.1 mm)",
    MM (210), MM (297), 2, 7, MM (99.1), MM (38.1), MM (5), MM (16) },
  { "10 per sheet, A4 (99.1 x 57 mm)",
    MM (210), MM (297), 2, 5, MM (99.1), MM (57), MM (5), MM (13) },
  { "30 per sheet, Letter (2 5/8 x 1 in)",
    IN (8.5), IN (11), 3, 10, IN (2.625), IN (1), IN (0.19), IN (0.5) },
  { "20 per sheet, Letter (4 x 2 in)",
    IN (8.5), IN (11), 2, 10, IN (4), IN (2), IN (0.16), IN (0.5) },
};

const W42EnvelopeSize *
w42_envelope_sizes (int *n)
{
  if (n != NULL)
    *n = (int) G_N_ELEMENTS (ENVELOPES);
  return ENVELOPES;
}

const W42LabelSheet *
w42_label_sheets (int *n)
{
  if (n != NULL)
    *n = (int) G_N_ELEMENTS (LABELS);
  return LABELS;
}

/* The text of an address as paragraphs, one line each, put in at `pos`
 * and returned as the position after the last of them. */
static gsize
insert_lines (W42PieceTable *pt, gsize pos, const char *text, W42ApIdx ap)
{
  char **lines;

  if (text == NULL || *text == '\0')
    return pos;

  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++)
    {
      char *line = g_strchomp (lines[i]);

      if (i > 0)
        {
          w42_pt_insert_block (pt, pos, ap);
          pos++;
        }
      if (*line != '\0')
        {
          w42_pt_insert_text (pt, pos, line, ap);
          pos += g_utf8_strlen (line, -1);
        }
    }
  g_strfreev (lines);
  return pos;
}

void
w42_envelope_make (W42PieceTable *pt, W42PageSetup *page, int size,
                   const char *delivery, const char *sender)
{
  const W42EnvelopeSize *env;
  W42ParaFmt pa;
  W42CharFmt ch;
  gsize pos, sender_start, delivery_start;

  g_return_if_fail (pt != NULL);

  env = &ENVELOPES[CLAMP (size, 0, (int) G_N_ELEMENTS (ENVELOPES) - 1)];

  if (page != NULL)
    {
      page->width = env->width;
      page->height = env->height;
      page->margin_left = IN (0.4);
      page->margin_right = IN (0.4);
      page->margin_top = IN (0.4);
      page->margin_bottom = IN (0.4);
      page->columns = 0;
      page->column_gap = 0;
    }

  w42_pt_load_text (pt, "");
  pos = w42_pt_first_caret_pos (pt);

  /* The sender in the top left corner, small. */
  sender_start = pos;
  pos = insert_lines (pt, pos, sender, w42_pt_ap_at (pt, pos));
  if (pos > sender_start)
    {
      memset (&ch, 0, sizeof ch);
      ch.size = 16;                        /* 8pt, as a return address is */
      w42_pt_apply_char_fmt (pt, sender_start, pos - sender_start,
                             W42_CHAR_SIZE, &ch);
      w42_pt_insert_block (pt, pos, w42_pt_ap_at (pt, pos));
      pos++;
    }

  /* The delivery address in the middle: down about two fifths of the
   * envelope and in from the left about a third, where a sorting machine
   * looks for it. */
  delivery_start = pos;
  pos = insert_lines (pt, pos, delivery, w42_pt_ap_at (pt, pos));

  memset (&pa, 0, sizeof pa);
  pa.indent_left = env->width / 3;
  pa.space_before = env->height * 2 / 5;
  w42_pt_apply_para_fmt (pt, delivery_start, pos > delivery_start ? pos - delivery_start : 0,
                         W42_PARA_INDENT_LEFT | W42_PARA_SPACE_BEFORE, &pa);

  /* Only the first line of the address is pushed down; the rest follow
   * it line by line. */
  if (pos > delivery_start)
    {
      gsize second = w42_pt_paragraph_end (pt, delivery_start) + 1;

      if (second < pos)
        {
          memset (&pa, 0, sizeof pa);
          pa.indent_left = env->width / 3;
          pa.space_before = 0;
          w42_pt_apply_para_fmt (pt, second, pos - second,
                                 W42_PARA_INDENT_LEFT | W42_PARA_SPACE_BEFORE, &pa);
        }
    }

  w42_pt_clear_undo (pt);
}

void
w42_labels_make (W42PieceTable *pt, W42PageSetup *page, int sheet,
                 const char *text, gboolean same)
{
  const W42LabelSheet *ls;
  int *widths;
  gsize at;

  g_return_if_fail (pt != NULL);

  ls = &LABELS[CLAMP (sheet, 0, (int) G_N_ELEMENTS (LABELS) - 1)];

  if (page != NULL)
    {
      page->width = ls->page_width;
      page->height = ls->page_height;
      page->margin_left = ls->margin_left;
      page->margin_right = ls->margin_left;
      page->margin_top = ls->margin_top;
      page->margin_bottom = ls->margin_top;
      page->columns = 0;
      page->column_gap = 0;
    }

  w42_pt_load_text (pt, "");
  at = w42_pt_paragraph_end (pt, w42_pt_first_caret_pos (pt));
  w42_pt_insert_table (pt, at, ls->down, ls->across, w42_pt_ap_at (pt, at));

  widths = g_new (int, ls->across);
  for (int c = 0; c < ls->across; c++)
    widths[c] = ls->label_width;
  w42_pt_table_set_widths (pt, 0, widths, ls->across);
  g_free (widths);

  /* A label has no rules round it: the sheet is cut, not drawn. */
  w42_pt_table_set_borders (pt, 0, FALSE);
  for (int r = 0; r < ls->down; r++)
    w42_pt_table_set_row_height (pt, 0, r, ls->label_height);

  if (text != NULL && *text != '\0')
    {
      for (int r = 0; r < ls->down; r++)
        for (int c = 0; c < ls->across; c++)
          {
            gsize start = w42_pt_cell_start (pt, 0, r, c);

            if (start == (gsize) -1)
              continue;
            insert_lines (pt, start, text, w42_pt_ap_at (pt, start));
            if (!same)
              {
                r = ls->down;      /* the first label only */
                break;
              }
          }
    }

  w42_pt_clear_undo (pt);
}
