/* w42-thesaurus.h - Tools > Language > Thesaurus
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The thesaurus reads MyThes files -- th_en_US_v2.idx and .dat, the pair
 * LibreOffice's thesaurus uses -- and answers a word with its meanings
 * and the synonyms of each.  No library is needed: the files are two
 * lists of lines.  Without a file for any of the user's languages there
 * is no thesaurus, and Tools > Language > Thesaurus says so.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _W42Thesaurus W42Thesaurus;

/* One meaning of a word: what kind of word it is there, and the words
 * that mean the same. */
typedef struct {
  char  *meaning;      /* "(noun) neighbourhood": the part of speech and
                        * the first synonym, as the file heads the sense */
  char **synonyms;     /* NULL-terminated */
} W42Sense;

/* NULL when no thesaurus file was found. */
W42Thesaurus *w42_thesaurus_new  (void);
/* The one for a language, a BCP-47 tag such as "nb-NO", before the
 * desktop's; NULL takes the desktop's alone. */
W42Thesaurus *w42_thesaurus_new_for (const char *lang);
void          w42_thesaurus_free (W42Thesaurus *self);
const char   *w42_thesaurus_language (W42Thesaurus *self);

/* The senses of `word` -- of W42Sense*, to free with
 * w42_thesaurus_senses_free() -- or NULL when the thesaurus has not got
 * it.  The look-up ignores case. */
GPtrArray *w42_thesaurus_lookup (W42Thesaurus *self, const char *word);
void       w42_thesaurus_senses_free (GPtrArray *senses);

G_END_DECLS
