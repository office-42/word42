/* w42-latex-run.h - typesetting a document with LaTeX
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LaTeX mode sends the document through a TeX engine for its PDF: the
 * source w42-latex.c writes, compiled by Tectonic, LuaLaTeX, XeLaTeX or
 * pdfLaTeX, whichever is installed.
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-document.h"

G_BEGIN_DECLS

/* The engine Word42 would typeset with, or NULL when none is installed. */
char *w42_latex_find_engine (void);

/* Typesets `doc` in the background: into `target`, or, when that is
 * NULL, into a PDF of its own that is then opened to look at.  What
 * happens is said on `parent`. */
void w42_latex_typeset (GtkWindow *parent, W42Document *doc, GFile *target);

G_END_DECLS
