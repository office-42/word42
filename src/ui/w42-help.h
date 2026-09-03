/* w42-help.h - the help window
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 6's Help menu had Contents, a search and an index.  This is the
 * same, built from the user guide that ships with the program: the
 * guide's sections are the topics, its sub-headings are the index,
 * and the search looks through the lot.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Help > Contents, and Help > Search for Help on...  `find` puts the
 * window straight into a search: NULL for the contents, "" for an empty
 * search box with the caret in it. */
void w42_help_window_show (GtkWindow *parent, const char *find);

/* Help > Index: the same window, showing the index rather than the
 * contents. */
void w42_help_index_show (GtkWindow *parent);

/* The guide as the help window sees it: the titles of its sections, and
 * one section's text with and without the markup.  NULL for a title the
 * guide does not have.  g_strfreev()/g_free(). */
char **w42_help_topic_titles (void);
char  *w42_help_topic_markup (const char *title);
char  *w42_help_topic_text   (const char *title);

G_END_DECLS
