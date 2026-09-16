/* w42-docmap.h - View > Document Map: the headings in a pane at the left
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-view.h"

G_BEGIN_DECLS

/* Returns a scrolling list of the document's headings, indented by level,
 * that follows `view`: clicking a heading puts the caret on it, and the
 * heading the caret is under is shown selected.  Word 97's Document Map. */
GtkWidget *w42_docmap_new (W42View *view);

/* Points the map at another view: with the window split into two panes,
 * the map follows the pane being edited. */
void w42_docmap_set_view (GtkWidget *map, W42View *view);

G_END_DECLS
