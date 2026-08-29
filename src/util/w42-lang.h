/* w42-lang.h - languages: the tags, the names, and Word's numbers
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A run of text may be marked with the language it is written in, so
 * that the spelling checker uses the right dictionary and the file
 * formats can say what they say.  Word42 keeps the language as a
 * BCP-47 tag ("en-GB", "nb-NO"); Word's formats keep a number, and
 * these turn one into the other.  The tag "zxx" is the standard way of
 * saying that a run is not language at all -- Word's "do not check
 * spelling".
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define W42_LANG_NONE "zxx"       /* not language: never spell-checked */

typedef struct {
  const char *tag;      /* "en-GB" */
  const char *name;     /* "English (United Kingdom)" */
  int         lcid;     /* what Word's formats write */
} W42Language;

/* The languages offered, in the order the dialog lists them. */
const W42Language *w42_languages (int *n);

/* An interned tag for Word's number, or NULL when it is not one we
 * know; 1024 (Word's "no language") gives W42_LANG_NONE. */
const char *w42_lang_from_lcid (int lcid);

/* And back: 0 when the tag is not one we know.  A tag with no country
 * ("en") matches the first entry of that language. */
int w42_lang_to_lcid (const char *tag);

/* The tag spelt the way this table spells it -- "en" becomes "en-US",
 * "NB-no" becomes "nb-NO" -- interned, or NULL for one we do not know. */
const char *w42_lang_normalise (const char *tag);

/* The name to show for a tag, or the tag itself when it is not one of
 * ours. */
const char *w42_lang_name (const char *tag);

/* The tag for the language the desktop is set to, interned. */
const char *w42_lang_default (void);

G_END_DECLS
