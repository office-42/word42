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

/* A soft hyphen (U+00AD), which Tools > Hyphenation puts inside words, is
 * looked through: "vanskelige" finds a hyphenated "van-ske-lige", and the
 * match covers the soft hyphens in it. */

/* Looks for `needle` starting at `from`.  On a hit, `match_start` and
 * `match_end` bound it as document positions. */
gboolean w42_search_find (W42PieceTable         *pt,
                          gsize                  from,
                          const char            *needle,
                          const W42SearchOptions *options,
                          gsize                 *match_start,
                          gsize                 *match_end);

/* TRUE when `text` -- the selection, say -- is `needle` and nothing more,
 * compared the way the search compares: what Replace asks before it
 * replaces what Find Next selected.  The options' whole_word, backwards
 * and wrap do not come into it. */
gboolean w42_search_is_match (const char             *text,
                              const char             *needle,
                              const W42SearchOptions *options);

/* Replaces every occurrence, as one undo step.  Returns how many. */
gsize w42_search_replace_all (W42PieceTable          *pt,
                              const char             *needle,
                              const char             *replacement,
                              const W42SearchOptions *options);

G_END_DECLS
