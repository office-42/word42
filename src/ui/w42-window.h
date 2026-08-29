/* w42-window.h - the document window: menus, toolbars, ruler, status bar
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-document.h"

G_BEGIN_DECLS

#define W42_TYPE_WINDOW (w42_window_get_type ())
G_DECLARE_FINAL_TYPE (W42Window, w42_window, W42, WINDOW, GtkApplicationWindow)

GtkWidget *w42_window_new  (GtkApplication *app);

/* Another window on a document that is already open in one, as Word 6's
 * Window > New Window gave you: both show the same text, and an edit in
 * either appears in both. */
GtkWidget *w42_window_new_for_document (GtkApplication *app, W42Document *doc);
void       w42_window_open (W42Window *self, GFile *file);

/* Opens a window for every document word42 was editing when it last
 * stopped without saving -- the autosave copies -- and returns how many.
 * Called before the first ordinary window is made. */
int        w42_window_recover_all (GtkApplication *app);

G_END_DECLS
