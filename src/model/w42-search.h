/* w42-search.h - finding and replacing text in the document
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Searching runs a paragraph at a time rather than over one flattened copy of
 * the document, which is both cheaper and the behaviour you want: a search
 * term does not match across a paragraph mark, exactly as it does not in Word.
 */

#pragma once

#include "w42-piecetable.h"

G_BEGIN_DECLS

typedef struct {
  gboolean match_case;
  gboolean whole_word;
  gboolean backwards;
  gboolean wrap;
} W42SearchOptions;

/* Looks for `needle` starting at `from`.  On a hit, `match_start` and
 * `match_end` bound it as document positions. */
gboolean w42_search_find (W42PieceTable         *pt,
                          gsize                  from,
                          const char            *needle,
                          const W42SearchOptions *options,
                          gsize                 *match_start,
                          gsize                 *match_end);

/* Replaces every occurrence, as one undo step.  Returns how many. */
gsize w42_search_replace_all (W42PieceTable          *pt,
                              const char             *needle,
                              const char             *replacement,
                              const W42SearchOptions *options);

G_END_DECLS
