/* w42-ruler.h - the horizontal ruler above the page
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-view.h"

G_BEGIN_DECLS

/* Returns a drawing area that tracks `view` for its page setup and zoom. */
GtkWidget *w42_ruler_new (W42View *view);

G_END_DECLS
