/* w42-find-dialog.h - the Find and Replace box
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-view.h"

G_BEGIN_DECLS

#define W42_TYPE_FIND_DIALOG (w42_find_dialog_get_type ())
G_DECLARE_FINAL_TYPE (W42FindDialog, w42_find_dialog, W42, FIND_DIALOG, GtkWindow)

GtkWidget *w42_find_dialog_new (GtkWindow *parent, W42View *view);

/* Word 6 had a Find box and a Replace box that were the same box with one
 * row hidden.  This is that box; `replace` decides whether the row shows. */
void w42_find_dialog_set_replace_mode (W42FindDialog *self, gboolean replace);

/* Repeats the last search without showing the dialog, for F3. */
void w42_find_dialog_find_again (W42FindDialog *self);
/* Puts `text` (NULL keeps what is there) in Find What and focuses it,
 * selected, so typing replaces it. */
void w42_find_dialog_prime (W42FindDialog *self, const char *text);

G_END_DECLS
