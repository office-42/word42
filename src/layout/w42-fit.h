/* w42-fit.h - Make It Fit: a document scaled to fill a number of pages
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WordPerfect's Make It Fit, which authors use against a page limit: the
 * document's type, its line spacing and its margins grow or shrink
 * together until the text fills the pages asked for, no more.  Word 97's
 * Shrink to Fit took one page off and nothing else.
 */

#pragma once

#include "w42-piecetable.h"

G_BEGIN_DECLS

/* What Make It Fit may change. */
typedef enum {
  W42_FIT_LEFT_MARGIN   = 1 << 0,
  W42_FIT_RIGHT_MARGIN  = 1 << 1,
  W42_FIT_TOP_MARGIN    = 1 << 2,
  W42_FIT_BOTTOM_MARGIN = 1 << 3,
  W42_FIT_FONT_SIZE     = 1 << 4,
  W42_FIT_LINE_SPACING  = 1 << 5,
  W42_FIT_ALL           = (1 << 6) - 1
} W42FitItems;

/* The pages the document fills as it stands. */
int      w42_fit_count_pages (W42PieceTable *pt, const W42PageSetup *page);

/* Scales what `items` names by one factor -- the largest that still
 * fits the document in `target` pages -- as one undo step in `pt`, and
 * puts the scaled margins in `page`.  FALSE, with nothing changed, when
 * no factor between half and double the present size does it; `pages`
 * says how many the document fills afterwards either way. */
gboolean w42_fit_pages (W42PieceTable *pt, W42PageSetup *page, int target,
                        W42FitItems items, int *pages);

G_END_DECLS
