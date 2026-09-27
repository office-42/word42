/* w42-fit.c - Make It Fit
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One factor scales everything Make It Fit may change: the size of every
 * run of text, line spacing and the space around paragraphs, and the
 * margins.  More of any of them takes more pages, so the pages grow with
 * the factor, and a bisection finds the largest factor whose pages are
 * no more than the target.  Each trial is made in the document itself
 * and undone: the layout is the only judge of how many pages there are.
 */

#include "w42-fit.h"

#include <string.h>

#include "w42-layout.h"

/* The factors tried lie between these; seven halvings of the half that
 * matters come to under half a percent, finer than type sizes go. */
#define FIT_LOW   0.5
#define FIT_HIGH  2.0
#define FIT_STEPS 7

/* No margin narrower than a quarter inch, no type under four points. */
#define MIN_MARGIN 360
#define MIN_SIZE   8

int
w42_fit_count_pages (W42PieceTable *pt, const W42PageSetup *page)
{
  W42Layout *layout = w42_layout_new ();
  int n;

  w42_layout_build_pt (layout, pt, page);
  n = w42_layout_n_pages (layout);
  w42_layout_free (layout);
  return n;
}

static int
scale (int value, double factor)
{
  return (int) (value * factor + (value >= 0 ? 0.5 : -0.5));
}

/* The page's margins scaled, as far as they may go: the text between
 * them is never narrower or shorter than an inch. */
static void
scale_margins (const W42PageSetup *from, W42PageSetup *to, double factor, W42FitItems items)
{
  *to = *from;
  if (items & W42_FIT_LEFT_MARGIN)
    to->margin_left = MAX (scale (from->margin_left, factor), MIN_MARGIN);
  if (items & W42_FIT_RIGHT_MARGIN)
    to->margin_right = MAX (scale (from->margin_right, factor), MIN_MARGIN);
  if (items & W42_FIT_TOP_MARGIN)
    to->margin_top = MAX (scale (from->margin_top, factor), MIN_MARGIN);
  if (items & W42_FIT_BOTTOM_MARGIN)
    to->margin_bottom = MAX (scale (from->margin_bottom, factor), MIN_MARGIN);
  if (to->width - to->margin_left - to->margin_right < 1440)
    {
      to->margin_left = from->margin_left;
      to->margin_right = from->margin_right;
    }
  if (to->height - to->margin_top - to->margin_bottom < 1440)
    {
      to->margin_top = from->margin_top;
      to->margin_bottom = from->margin_bottom;
    }
}

/* The text's sizes and the paragraphs' spacing scaled, as one undo
 * group.  Every run keeps its own size, times the factor. */
static void
scale_text (W42PieceTable *pt, double factor, W42FitItems items)
{
  GPtrArray *blocks = w42_pt_snapshot_blocks (pt);
  W42ApTable *aps = w42_pt_ap_table (pt);

  w42_pt_begin_group (pt);
  for (guint i = 0; i < blocks->len; i++)
    {
      const W42Block *block = g_ptr_array_index (blocks, i);
      const W42Fmt *fmt = w42_ap_table_get (aps, block->ap);

      if (items & W42_FIT_FONT_SIZE)
        {
          for (guint r = 0; r < block->runs->len; r++)
            {
              const W42Run *run = &g_array_index (block->runs, W42Run, r);
              const W42Fmt *rf = w42_ap_table_get (aps, run->ap);
              W42CharFmt want;

              if (run->n_chars == 0)
                continue;
              memset (&want, 0, sizeof want);
              want.size = MAX (scale (rf->ch.size > 0 ? rf->ch.size : 24, factor), MIN_SIZE);
              w42_pt_apply_char_fmt (pt, run->doc_pos, run->n_chars, W42_CHAR_SIZE, &want);
            }
          {
            /* The paragraph mark's own size, which is the height of an
             * empty paragraph's line. */
            W42CharFmt mark = fmt->ch;

            mark.size = MAX (scale (fmt->ch.size > 0 ? fmt->ch.size : 24, factor), MIN_SIZE);
            w42_pt_set_mark_char_fmt (pt, block->start_pos + 1, &mark);
          }
        }

      if (items & W42_FIT_LINE_SPACING)
        {
          W42ParaFmt want = fmt->pa;

          /* Single spacing is 100%: scaled, it becomes a percentage of
           * its own; an exact leading scales as it is. */
          if (fmt->pa.line_spacing > 0 && fmt->pa.line_spacing_pct == 0)
            want.line_spacing = MAX (scale (fmt->pa.line_spacing, factor), 20);
          else
            want.line_spacing_pct = CLAMP (scale (fmt->pa.line_spacing_pct > 0
                                                  ? fmt->pa.line_spacing_pct : 100, factor), 50, 300);
          want.space_before = scale (fmt->pa.space_before, factor);
          want.space_after = scale (fmt->pa.space_after, factor);
          w42_pt_apply_para_fmt (pt, block->start_pos + 1, 0,
                                 W42_PARA_LINE_SPACING | W42_PARA_LINE_SPACING_PCT |
                                 W42_PARA_SPACE_BEFORE | W42_PARA_SPACE_AFTER, &want);
        }
    }
  w42_pt_end_group (pt);
  g_ptr_array_free (blocks, TRUE);
}

/* The pages the document would fill scaled by `factor`: made, counted,
 * and undone. */
static int
trial (W42PieceTable *pt, const W42PageSetup *page, double factor, W42FitItems items)
{
  W42PageSetup scaled;
  int n;

  scale_margins (page, &scaled, factor, items);
  if (items & (W42_FIT_FONT_SIZE | W42_FIT_LINE_SPACING))
    scale_text (pt, factor, items);
  n = w42_fit_count_pages (pt, &scaled);
  if (items & (W42_FIT_FONT_SIZE | W42_FIT_LINE_SPACING))
    w42_pt_undo (pt);
  return n;
}

gboolean
w42_fit_pages (W42PieceTable *pt, W42PageSetup *page, int target,
               W42FitItems items, int *pages)
{
  double low = FIT_LOW, high = FIT_HIGH, best = -1.0;
  int now;

  g_return_val_if_fail (pt != NULL && page != NULL, FALSE);

  now = w42_fit_count_pages (pt, page);
  if (pages != NULL)
    *pages = now;
  if (target < 1 || (items & W42_FIT_ALL) == 0 || now == target)
    return FALSE;

  /* The pages grow with the factor: the largest factor that still fits
   * lies between one that fits and one that does not.  The document as
   * it is, at 1, is one of the two already. */
  if (target < now)
    {
      high = 1.0;
      if (trial (pt, page, low, items) > target)
        return FALSE;
    }
  else
    low = 1.0;
  if (target > now && trial (pt, page, high, items) <= target)
    best = high;
  else
    {
      best = low;
      for (int step = 0; step < FIT_STEPS; step++)
        {
          double mid = (low + high) / 2.0;

          if (trial (pt, page, mid, items) <= target)
            best = low = mid;
          else
            high = mid;
        }
    }

  /* The factor found, made for good. */
  {
    W42PageSetup scaled;

    scale_margins (page, &scaled, best, items);
    if (items & (W42_FIT_FONT_SIZE | W42_FIT_LINE_SPACING))
      scale_text (pt, best, items);
    *page = scaled;
  }
  if (pages != NULL)
    *pages = w42_fit_count_pages (pt, page);
  return TRUE;
}
