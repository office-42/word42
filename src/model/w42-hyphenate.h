/* w42-hyphenate.h - Tools > Hyphenation
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hyphenation puts soft hyphens (U+00AD) into the document's words where
 * the language's rules allow a break; the layout shows a hyphen only where
 * a line actually breaks there.  The rules come from libhyphen's patterns
 * (the hyph_*.dic files LibreOffice uses); without the library or a
 * dictionary there is no hyphenator.
 */

#pragma once

#include "w42-piecetable.h"

G_BEGIN_DECLS

typedef struct _W42Hyphenator W42Hyphenator;

/* NULL when this build has no libhyphen or no dictionary was found. */
W42Hyphenator *w42_hyphenator_new  (void);
void           w42_hyphenator_free (W42Hyphenator *hyph);
const char    *w42_hyphenator_language (W42Hyphenator *hyph);

/* `word` with soft hyphens at its break points, or NULL when it has none. */
char *w42_hyphenator_word (W42Hyphenator *hyph, const char *word);

/* Soft hyphens into every word of the document that can take one, or out
 * of all of them; both one undo step.  Returns how many went in or out. */
int w42_pt_hyphenate   (W42PieceTable *pt, W42Hyphenator *hyph);
int w42_pt_unhyphenate (W42PieceTable *pt);

G_END_DECLS
