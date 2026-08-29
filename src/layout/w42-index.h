/* w42-index.h - Insert > Index: the marked words, and the pages they are on
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An index entry is a run of text marked as a field whose code is XE, or
 * XE:term when the entry is filed under something other than the words
 * on the page.  The words stay where they are and read as they did; the
 * index gathers the marked runs, sorts them and says which pages they
 * are on.  The pages come from a layout, which is why this lives beside
 * the layout rather than in the model.
 */

#pragma once

#include "w42-layout.h"
#include "w42-piecetable.h"

G_BEGIN_DECLS

#define W42_INDEX_FIELD    "XE"       /* the field code of a marked run */
#define W42_INDEX_BOOKMARK "_Index"   /* what the index itself carries */

typedef struct {
  char   *term;
  GArray *pages;        /* int, in order, without repeats */
} W42IndexEntry;

/* The marked runs, gathered and sorted.  A GPtrArray of W42IndexEntry*
 * with a free function; empty when nothing is marked. */
GPtrArray *w42_index_gather (W42PieceTable *pt, W42Layout *layout);

/* Puts the index in at `at` -- the first position inside a paragraph --
 * and returns how many entries it made.  An index already in the
 * document is not touched: the caller takes it out first (its bookmark
 * says where it is). */
int w42_index_build (W42PieceTable *pt, W42Layout *layout,
                     const W42PageSetup *page, gsize at, gsize *end);

G_END_DECLS
