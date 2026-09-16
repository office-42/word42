/* w42-thesaurus-dialog.h - the Thesaurus box
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-thesaurus.h"
#include "w42-view.h"

G_BEGIN_DECLS

/* Word 97's Tools > Language > Thesaurus (Shift+F7): the word at the
 * caret looked up, its meanings down the left, the synonyms of the chosen
 * meaning down the right, and Replace, Look Up and Cancel.  Modal.
 * `thesaurus` is borrowed and must outlive the box.  Returns FALSE, and
 * opens nothing, when the caret is not in a word. */
gboolean w42_thesaurus_dialog_show (GtkWindow *parent, W42View *view,
                                    W42Thesaurus *thesaurus);

G_END_DECLS
