/* w42-autoformat.h - Format > AutoFormat: a typed document tidied up
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 6's AutoFormat looked over a document that had been typed as
 * though on a typewriter and made it a word processor's: short lines
 * that stand alone became headings, lines that began with a dash became
 * a bulleted list, runs of empty paragraphs became one, and the straight
 * quotes became the shapes a printer would set.  This does the same, in
 * one undo step, and says how many paragraphs it changed.
 */

#pragma once

#include "w42-piecetable.h"

G_BEGIN_DECLS

typedef struct {
  gboolean headings;   /* short paragraphs that stand alone */
  gboolean lists;      /* "- " and "1. " at the start of a line */
  gboolean quotes;     /* straight quotes and two hyphens */
  gboolean blanks;     /* runs of empty paragraphs */
} W42AutoFormat;

/* Everything on, which is what the dialog offers first. */
void w42_autoformat_defaults (W42AutoFormat *what);

int  w42_pt_autoformat (W42PieceTable *pt, const W42AutoFormat *what);

G_END_DECLS
