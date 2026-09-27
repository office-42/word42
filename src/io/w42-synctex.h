/* w42-synctex.h - where TeX set each line of a source
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An engine run with -synctex=1 writes, beside its PDF, a record of every
 * box and glue it set: the source file and line it came from, the page
 * and the place on the page.  That is what lets the LaTeX preview go
 * from a line of the source to its place in the PDF and back.  This
 * reads the records of one source file -- the document's -- and answers
 * both questions.  Places are in PDF points from the page's top left.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _W42SyncTex W42SyncTex;

/* Reads a .synctex or .synctex.gz, keeping what was set from the source
 * file called `source` (its name, without a folder). */
W42SyncTex *w42_synctex_load    (const char *path, const char *source, GError **error);
void        w42_synctex_free    (W42SyncTex *sync);

/* Where `line` was set -- or the nearest line after it that was, within
 * a few -- as its page (from 1), and the top and bottom of the line of
 * type it is in.  FALSE when nothing near it was set. */
gboolean    w42_synctex_forward (W42SyncTex *sync, guint line, int *page,
                                 double *top, double *bottom);

/* The source line set nearest to (x, y) on `page`, or 0. */
guint       w42_synctex_inverse (W42SyncTex *sync, int page, double x, double y);

G_END_DECLS
