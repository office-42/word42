/* w42-latex-preview.h - File > LaTeX Preview: the document as LaTeX sets it
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A pane at the right of the page with the document typeset by LaTeX,
 * set again whenever the typing pauses: the pages drawn from the PDF, or
 * the LaTeX source they were made from, and what LaTeX said when it
 * could not.  SyncTeX ties the two together: the preview keeps the place
 * the caret is at in sight, and a double-click on a page puts the caret
 * at what was clicked.  Built only with poppler, which draws the pages.
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-view.h"

G_BEGIN_DECLS

/* The pane, following the document in `view`.  It typesets while it is
 * shown and rests while it is hidden. */
GtkWidget *w42_latex_preview_new      (W42View *view);

/* Points the pane at another view: with the window split, it follows
 * the pane being edited. */
void       w42_latex_preview_set_view (GtkWidget *preview, W42View *view);

G_END_DECLS
