/* w42-pdf-view.h - the pages of a PDF, one under another
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the LaTeX preview shows its PDF in: the pages down a grey ground,
 * as the print preview shows Word42's own, drawn by poppler.  Only the
 * pages in sight are drawn, each once at the size it is shown, so a
 * thesis of three hundred pages scrolls as easily as a letter.  Put in a
 * GtkScrolledWindow, which it scrolls in.
 */

#pragma once

#include <gtk/gtk.h>
#include <poppler.h>

G_BEGIN_DECLS

#define W42_TYPE_PDF_VIEW (w42_pdf_view_get_type ())
G_DECLARE_FINAL_TYPE (W42PdfView, w42_pdf_view, W42, PDF_VIEW, GtkWidget)

GtkWidget *w42_pdf_view_new          (void);

/* Shows `doc`, keeping the place scrolled to when it is a new version of
 * the one shown before.  NULL shows nothing. */
void       w42_pdf_view_set_document (W42PdfView *self, PopplerDocument *doc);
int        w42_pdf_view_get_n_pages  (W42PdfView *self);

/* The size the pages are shown at, as a share of their own: 1.0 is
 * actual size.  0 fits the widest page to the width. */
void       w42_pdf_view_set_zoom     (W42PdfView *self, double zoom);
double     w42_pdf_view_get_zoom     (W42PdfView *self);
/* The size they come out at now, whether chosen or fitted. */
double     w42_pdf_view_get_scale    (W42PdfView *self);

/* Scrolls so that the band from `top` to `bottom` of `page` (from 1; in
 * PDF points from the page's top) is in sight, if it is not already,
 * and marks it for a moment when `mark` is TRUE. */
void       w42_pdf_view_show         (W42PdfView *self, int page, double top,
                                      double bottom, gboolean mark);

/* ::point-activated (page, x, y) is emitted on a double-click on a page,
 * with the place clicked in PDF points from the page's top left, and
 * ::scale-changed when the pages come out at another size -- zoomed, or
 * fitted to a new width. */

G_END_DECLS
