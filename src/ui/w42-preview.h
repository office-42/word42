/* w42-preview.h - Print Preview
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GTK's own print preview hands a PDF to an external viewer, which is fine
 * on a desktop that has one and nothing at all on Windows or macOS.  Word 6
 * had a preview window of its own -- pages on a grey ground, a zoom, a Print
 * button -- and so does word42.  It is the same layout engine as the screen
 * and the printer, so what it shows is what will print.
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-document.h"

G_BEGIN_DECLS

#define W42_TYPE_PREVIEW (w42_preview_get_type ())
G_DECLARE_FINAL_TYPE (W42Preview, w42_preview, W42, PREVIEW, GtkWindow)

GtkWidget *w42_preview_new (GtkWindow *parent, W42Document *doc);

G_END_DECLS
