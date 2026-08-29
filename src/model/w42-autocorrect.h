/* w42-autocorrect.h - Tools > AutoCorrect: the corrections made as you type
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 6 corrected four things while you typed, and they are the four
 * everybody still expects: straight quotes become the curly ones a
 * typesetter would use, TWo INitial CApitals become one, the first letter
 * of a sentence is capitalised, and a short list of misspellings is put
 * right.  A correction is worked out from the text before the caret and
 * the character just typed, so that the model, the view and the tests can
 * all ask the same question without a document between them.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* What to do with the text that ends at the caret: replace the last
 * `back` characters with `text`.  `back` of 0 and a NULL text mean
 * nothing needs correcting. */
typedef struct {
  gsize       back;      /* characters before the caret to replace */
  const char *text;      /* interned, or NULL */
} W42Correction;

/* `before` is the text of the paragraph up to the caret, `typed` the
 * character that has just been added to it (already part of `before`).
 * `sentence_start` says whether the paragraph's first word is at hand,
 * which the capital rules need. */
W42Correction w42_autocorrect (const char *before, gunichar typed);

/* The list of misspellings, for the dialog that shows them.  NULL-ended
 * pairs: wrong, right, wrong, right... */
const char *const *w42_autocorrect_replacements (void);

G_END_DECLS
