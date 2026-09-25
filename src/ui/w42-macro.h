/* w42-macro.h - Tools > Macro: running Word42 Basic over a document
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A macro is a file of Word42 Basic -- the VBA dialect w42-vba.c
 * translates -- kept in the macros folder of the user's data directory,
 * one Sub or more to a file.  Running one puts the object model over the
 * window's document: Selection, ActiveDocument, Application, Documents,
 * and MsgBox, InputBox and Debug.Print, all as native functions the
 * MY-BASIC engine calls.  Nothing runs unless the user asks: there is no
 * AutoOpen, and a document carries no macros.
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-view.h"

G_BEGIN_DECLS

/* Runs Sub `entry` of `source` on `view`, whose window is `parent`.
 * Documents.Add and Documents.Open move it on to the new document's
 * window, which Word makes the active document.  What the macro prints
 * goes to `output` when that is given.  The edits the macro makes are
 * one undo step in each document.  FALSE with a message -- a line of
 * the macro, and what went wrong on it -- when it could not be
 * translated or stopped on an error. */
gboolean w42_macro_run (GtkWindow *parent, W42View *view, const char *source,
                        const char *entry, GString *output, char **error);

/* The macros folder, made if it is missing; the names of the macros in
 * it, sorted, free with g_strfreev(); and one macro's text.  A name is
 * the file's without its .bas. */
char    *w42_macro_dir    (void);
char   **w42_macro_names  (void);
char    *w42_macro_load   (const char *name, GError **error);
gboolean w42_macro_save   (const char *name, const char *source, GError **error);
gboolean w42_macro_delete (const char *name, GError **error);

/* Whether `name` can be a macro's: letters, digits and underscores. */
gboolean w42_macro_name_ok (const char *name);

G_END_DECLS
