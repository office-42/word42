/* w42-spell-dialog.h - the Spelling box
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-spell.h"
#include "w42-view.h"

G_BEGIN_DECLS

#define W42_TYPE_SPELL_DIALOG (w42_spell_dialog_get_type ())
G_DECLARE_FINAL_TYPE (W42SpellDialog, w42_spell_dialog, W42, SPELL_DIALOG, GtkWindow)

/* Word 6's Tools > Spelling: walks the document from the caret, stops on
 * each word the dictionary does not know, and offers Ignore, Ignore All,
 * Change, Change All and Add, with suggestions to pick from.  Modeless.
 * `spell` is borrowed and must outlive the box. */
GtkWidget *w42_spell_dialog_new (GtkWindow *parent, W42View *view, W42Spell *spell);

/* Begins, or begins again, from the caret. */
void w42_spell_dialog_start (W42SpellDialog *self);

G_END_DECLS
