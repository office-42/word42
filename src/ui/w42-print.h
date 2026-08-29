/* w42-print.h - printing and print preview
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-document.h"

G_BEGIN_DECLS

void w42_print_document (GtkWindow *parent, W42Document *doc, gboolean preview);

G_END_DECLS
