/* w42-autocorrect.h - Tools > AutoCorrect: the corrections made as you type
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 97 corrected four things while you typed, and they are the four
 * everybody still expects: straight quotes become the curly ones a
 * typesetter would use, TWo INitial CApitals become one, the first letter
 * of a sentence is capitalised, and a short list of misspellings is put
 * right.  The quotes are the ones the text's language sets -- « » in
 * Norwegian or French, „ “ in German -- and a hyphen that opens a line
 * of dialogue, or stands alone between two words, becomes the dash a
 * typesetter would have put there.  A correction is worked out from the
 * text before the caret and the character just typed, so that the model,
 * the view and the tests can all ask the same question without a
 * document between them.
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

/* How a language sets its text: the quotation marks it uses, outer and
 * inner, the dash a line of dialogue opens with in a novel (0 where the
 * language does not do that), and whether a number with a full stop is
 * an ordinal -- "3. september" -- rather than a sentence's end. */
typedef struct {
  gunichar open, close;
  gunichar open2, close2;
  gunichar dialogue;
  gboolean ordinal_dot;
} W42Typography;

/* For a BCP-47 tag, or NULL for English's; never NULL. */
const W42Typography *w42_typography_for (const char *lang);

/* `before` is the text of the paragraph up to the caret, `typed` the
 * character that has just been added to it (already part of `before`),
 * and `lang` the language the text is in (a BCP-47 tag, or NULL for
 * English), which decides the quotation marks, the dashes and what
 * counts as the end of a sentence. */
W42Correction w42_autocorrect (const char *before, gunichar typed,
                               const char *lang);

/* The list of misspellings, for the dialog that shows them.  NULL-ended
 * pairs: wrong, right, wrong, right... */
const char *const *w42_autocorrect_replacements (void);

G_END_DECLS
