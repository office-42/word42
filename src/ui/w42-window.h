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

/* Another window on a document that is already open in one, as Word 97's
 * Window > New Window gave you: both show the same text, and an edit in
 * either appears in both. */
GtkWidget *w42_window_new_for_document (GtkApplication *app, W42Document *doc);
/* Reads `file` into the window's document.  w42_window_load says nothing
 * when the file cannot be read, and the document is left as it was;
 * w42_window_open says so in a message on the window. */
gboolean   w42_window_load (W42Window *self, GFile *file, GError **error);
gboolean   w42_window_open (W42Window *self, GFile *file);
/* For macros: a line in the status bar, and the document written to a
 * file as File > Save As would, with the title and the recent list
 * following -- or, to a format that does not round trip, exported
 * there, with the document left as it was. */
void       w42_window_flash_status (W42Window *self, const char *text);
gboolean   w42_window_save_to (W42Window *self, GFile *file, GError **error);
/* Whether a file name ends in an extension Word42 knows.  One that does
 * not is saved as Rich Text, with .rtf added: a dot in "Mr. Smith" does
 * not make it a text file. */
gboolean   w42_window_name_has_extension (const char *name);
/* Closes the window without asking about unsaved changes, as a macro's
 * ActiveDocument.Close wdDoNotSaveChanges does.  A document still open in
 * another window keeps its changes there. */
void       w42_window_close_discarding (W42Window *self);

/* An empty document in a new window, for the commands that make a
 * document of their own -- an envelope, a sheet of labels.  The window
 * is presented; the document is returned to be filled.  NULL when
 * `from` belongs to no application. */
W42Document *w42_window_new_document (GtkWindow *from);

/* A new document starts in the language Tools > Language > Default
 * last chose: Normal is marked with it, without the document being
 * marked as changed for it. */
void       w42_window_apply_default_language (W42Document *doc);

/* The window already showing `file`, or NULL.  Opening a file twice
 * makes two documents of it, and whichever is saved last silently
 * throws the other's work away; the one open is raised instead. */
W42Window *w42_window_find_file (GtkApplication *app, GFile *file);

/* Tools > Options changed how often the unsaved changes are copied:
 * every window takes the new interval. */
void       w42_window_autosave_changed (GtkApplication *app);

/* Opens a window for every document word42 was editing when it last
 * stopped without saving -- the autosave copies -- and returns how many.
 * Called before the first ordinary window is made. */
int        w42_window_recover_all (GtkApplication *app);

G_END_DECLS
