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

/* What a typesetting came to.  The strings belong to the runner and last
 * as long as the call they are given to. */
typedef struct {
  gboolean    ok;
  const char *pdf;        /* the PDF made, when ok */
  const char *synctex;    /* where SyncTeX says each source line went, or NULL */
  const char *errors;     /* what LaTeX said went wrong, when not ok */
  int         line;       /* the line of document.tex the error is on, or 0 */
  const char *engine;     /* the engine's name, as "pdflatex" */
} W42LatexResult;

typedef void (*W42LatexDone) (const W42LatexResult *result, gpointer data);

/* Runs `engine` on document.tex, already written in `dir`, in the
 * background -- again when the cross-references ask for it, so that page
 * numbers and the table of contents come out right -- and tells `done`
 * how it went.  With `synctex` the engine also records where each source
 * line was set.  Once `cancellable` is cancelled the engine is stopped
 * and `done` is not called. */
void w42_latex_compile (const char *engine, const char *dir, gboolean synctex,
                        GCancellable *cancellable, W42LatexDone done, gpointer data);

/* Deletes a folder the typesetting was done in, and everything in it. */
void w42_latex_remove_dir (const char *dir);

G_END_DECLS
