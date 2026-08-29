/* w42-pdf.h - writing a document out as PDF, and reading one in
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Export goes through cairo's PDF surface with the same layout engine and
 * the same painting code the screen and the printer use, so the pages come
 * out where the screen said they would.
 *
 * Import goes through poppler, when it was available at build time.  A PDF
 * is a picture of a document rather than the document, so what comes back
 * is the text and the pictures with the paragraphs guessed from the line
 * breaks, which is all any program can do with one.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"

G_BEGIN_DECLS

gboolean w42_pdf_export (W42PieceTable      *pt,
                         const W42PageSetup *page,
                         GFile              *file,
                         GError            **error);

/* TRUE when word42 was built with poppler and can read PDF at all. */
gboolean w42_pdf_import_available (void);

gboolean w42_pdf_import (W42PieceTable *pt,
                         W42PageSetup  *page,
                         GFile         *file,
                         GError       **error);

G_END_DECLS
