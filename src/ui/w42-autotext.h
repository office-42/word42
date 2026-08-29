/* w42-autotext.h - Edit > AutoText: pieces of text kept by name
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 6 kept a list of named pieces of text -- an address, a closing,
 * a company name -- and put one in wherever the caret was, either from
 * the Edit menu or by typing the name and pressing F3.  These are the
 * entries, kept in the settings file between runs.  What is kept is the
 * text, not its formatting: an entry takes the formatting of the place
 * it lands in.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* The names, in the order they were made.  g_strfreev(). */
char **w42_autotext_names (void);

/* The text of an entry, or NULL when there is no such entry.  g_free(). */
char  *w42_autotext_get (const char *name);

/* Adds an entry, or replaces the one of that name.  An empty name or
 * empty text does nothing. */
void   w42_autotext_set (const char *name, const char *text);

/* Takes one out.  Nothing happens when there is no such entry. */
void   w42_autotext_remove (const char *name);

/* A name for a piece of text, the way Word 6 offered one: its first few
 * words, cut short.  g_free(). */
char  *w42_autotext_suggest_name (const char *text);

G_END_DECLS
