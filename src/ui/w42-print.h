/* w42-print.h - File > Print and Print Preview
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-document.h"
#include "w42-piecetable.h"

G_BEGIN_DECLS

/* What Word XP's Print dialog could be told beyond the document: the
 * selection, to print that alone, and the page the caret is on, for
 * "Current page".  `selection` is a fragment the caller made and the
 * print takes; NULL when there is none. */
typedef struct {
  W42PieceTable *selection;
  int            current_page;   /* 1-based; 0 for unknown */
} W42PrintExtras;

void w42_print_document (GtkWindow *parent, W42Document *doc, gboolean preview,
                         const W42PrintExtras *extras);

/* The settings the print dialog was last used with, kept between jobs
 * and sessions the way Word kept the printer and its options. */
GtkPrintSettings *w42_print_settings (void);

G_END_DECLS
