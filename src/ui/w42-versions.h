/* w42-versions.h - File > Versions, kept in Git
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-view.h"

G_BEGIN_DECLS

/* Word 97's Versions box for the document in `view`, whose window is
 * `parent`: Save Now, the choice to keep a version at every save, and
 * the versions kept, to open, compare with or go back to.  The versions
 * are commits in a Git repository (see w42-git.h). */
void w42_versions_dialog_show (GtkWindow *parent, W42View *view);

G_END_DECLS
