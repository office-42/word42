/* w42-spell.h - a spelling dictionary, and the words of a text
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A thin wrapper over Enchant, which in turn wraps whichever of Hunspell,
 * Aspell or the platform's own checker is installed.  Built without Enchant
 * there is no dictionary, w42_spell_new() returns NULL, and everything that
 * checks spelling quietly does not.
 *
 * Splitting text into words is here too, so that the layout, which
 * underlines, and the Spelling box, which walks, agree on what a word is.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _W42Spell W42Spell;

/* A dictionary for the user's language, or the nearest one there is, or
 * NULL when there is none at all. */
W42Spell   *w42_spell_new      (void);
void        w42_spell_free     (W42Spell *spell);

/* The language of the default dictionary, as a BCP-47 tag ("nb-NO"). */
const char *w42_spell_language (W42Spell *spell);

/* Makes the dictionary for `lang` -- a BCP-47 tag, or NULL for the one
 * the desktop's language chooses -- the default: the one
 * w42_spell_check(), w42_spell_suggest() and w42_spell_check_lang() with
 * no tag use, and w42_spell_add() adds to.  A bare language finds its
 * usual country, and "no" finds Bokmål.  Bumps the serial when the
 * default changes; FALSE, and nothing changed, when there is no
 * dictionary for it. */
gboolean    w42_spell_set_language (W42Spell *spell, const char *lang);

/* TRUE when the word is spelt right, or has been ignored or added.  Soft
 * hyphens in it, here and in everything below, are looked past. */
gboolean    w42_spell_check    (W42Spell *spell, const char *word, gssize len);

/* The same, with the language the run of text is marked with: a BCP-47
 * tag, or NULL for the document's own dictionary.  A run marked
 * W42_LANG_NONE ("zxx") is not language and is never wrong; a language
 * there is no dictionary for is not judged either. */
gboolean    w42_spell_check_lang (W42Spell *spell, const char *lang,
                                  const char *word, gssize len);

/* TRUE when there is a dictionary for the tag. */
gboolean    w42_spell_has_language (W42Spell *spell, const char *lang);

/* NULL-terminated, for g_strfreev(); NULL when there is nothing to offer. */
char      **w42_spell_suggest  (W42Spell *spell, const char *word, gssize len);
char      **w42_spell_suggest_lang (W42Spell *spell, const char *lang,
                                    const char *word, gssize len);

/* Ignore All: right for the rest of this session.  Add: right for good,
 * in the user's own dictionary. */
void        w42_spell_ignore   (W42Spell *spell, const char *word);
void        w42_spell_add      (W42Spell *spell, const char *word);

/* Bumped whenever a word is ignored or added or the default dictionary
 * changes, so that anything holding on to what the checker said before
 * -- the layout's shaped paragraphs, with their red underlines -- knows
 * to work it out again. */
guint       w42_spell_serial   (W42Spell *spell);

/* The next word of `text` at or after byte `*end`: on TRUE, [*start, *end)
 * are its byte bounds.  A word is a run of letters, with apostrophes inside
 * it; anything with a digit in it is not a word and is not checked. */
gboolean    w42_spell_next_word (const char *text, gsize len,
                                 gsize *start, gsize *end);

G_END_DECLS
