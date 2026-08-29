/* w42-template.h - the documents File > New can start from
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 6's File > New listed templates and made a document out of the
 * one chosen.  These are the ones that travel with the program, built in
 * the model rather than read from a file so that they cannot go missing;
 * a document saved into the templates folder joins the same list.
 */

#pragma once

#include "w42-piecetable.h"

G_BEGIN_DECLS

typedef struct {
  const char *name;     /* "Letter" */
  const char *hint;     /* one line, for the dialog */
} W42Template;

/* The built-in ones, in the order the dialog lists them. */
const W42Template *w42_templates (int *n);

/* Fills `pt` with the template, and says in `page` what paper it wants.
 * Whatever was in `pt` is replaced, and the undo history is cleared: a
 * new document has nothing to undo. */
void w42_template_make (W42PieceTable *pt, W42PageSetup *page, int which);

/* Where a document saved as a template goes, made if it is not there.
 * g_free(). */
char *w42_template_folder (void);

/* The templates in that folder, as file names without their path, in
 * alphabetical order.  g_strfreev(). */
char **w42_template_files (void);

G_END_DECLS
