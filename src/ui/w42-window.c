/* w42-window.c - see w42-window.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-window.h"

#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>
#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#ifdef G_OS_WIN32
#include <windows.h>
#include <gdk/win32/gdkwin32.h>
#endif

#include "w42-dialogs.h"
#include "w42-slideshow.h"
#include "w42-pptx.h"
#include "w42-find-dialog.h"
#include "w42-image.h"
#include "w42-io.h"
#include "w42-html.h"
#include "w42-pdf.h"
#include "w42-print.h"
#include "w42-ruler.h"
#include "w42-rtf.h"
#include "w42-settings.h"
#include "w42-spell-dialog.h"
#include "w42-view.h"

static const char *window_author_name (void);

/* The zoom steps the Standard bar offers; also what Options can make the
 * default. */
static const double ZOOM_STEPS[] = { 0.75, 1.0, 1.5, 2.0 };
static const char  *ZOOM_LABELS[] = { "75%", "100%", "150%", "200%" };

/* The sizes Word 6's Formatting toolbar offered. */
static const int FONT_SIZES[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22,
                                  24, 26, 28, 36, 48, 72 };

struct _W42Window {
  GtkApplicationWindow parent_instance;

  W42Document *doc;
  W42View     *view;

  GtkWidget   *standard_bar;
  GtkWidget   *format_bar;
  GtkWidget   *ruler;

  GtkWidget   *style_drop;
  GtkStringList *style_list;
  GtkWidget   *font_drop;
  GtkWidget   *size_drop;
  GtkWidget   *zoom_drop;
  GMenu       *window_list;   /* the open documents, in the Window menu */
  guint        serial;        /* the order this window was opened in, which
                               * is what the Window menu numbers by: the
                               * application's own list is in the order the
                               * windows were last used, and that changes
                               * under the menu as it is opened */
  char        *window_list_state;   /* what the menu was built from last */
  GtkWidget   *bold_btn;
  GtkWidget   *italic_btn;
  GtkWidget   *underline_btn;
  GtkWidget   *align_btn[4];
  GtkWidget   *bullets_btn;
  GtkWidget   *numbers_btn;

  GtkWidget   *menubar;
  GtkWidget   *status_bar;
  /* What View > Full Screen put away, and whether each was showing. */
  gboolean     full_screen_chrome[4];
  GtkWidget   *status_page;
  GtkWidget   *status_at;
  GtkWidget   *status_ln;
  GtkWidget   *status_col;
  GtkWidget   *status_mod;
  char        *status_flash;    /* a message shown in place of "Modified" */
  guint        status_flash_id;

  GListModel  *families;
  GHashTable  *family_index;  /* interned family name -> position, built once */
  gboolean     updating;      /* guards toolbar -> document feedback */

  /* Closing a modified document asks first.  Because saving may itself have
   * to stop and ask for a filename, the answer arrives long after the
   * close-request was refused, and these two carry the intent across. */
  gboolean     close_after_save;
  gboolean     force_close;

  GMenu       *recent_menu;   /* the File menu's recent files section */

  /* Autosave: every so often a modified document is written to a copy in
   * the user's data directory, which a clean close removes and a crash
   * leaves behind for the next start to recover. */
  guint        autosave_id;
  char        *autosave_path;
  gboolean     autosave_dirty;
  GtkWidget   *find_dialog;   /* modeless; cleared by a weak pointer */
  GtkWidget   *spell_dialog;  /* likewise */
  W42Spell    *spell;         /* NULL when there is no dictionary */
  GtkWidget   *title_label;   /* word42 draws its own title bar */
};

G_DEFINE_FINAL_TYPE (W42Window, w42_window, GTK_TYPE_APPLICATION_WINDOW)

static void window_sync_state (W42Window *self);

/* Commands that can do nothing at all -- no fields to update, no
 * revisions to accept -- say so in the status bar rather than looking
 * broken.  Word 6 wrote its messages there too. */
static gboolean
window_flash_done (gpointer data)
{
  W42Window *self = data;

  self->status_flash_id = 0;
  g_clear_pointer (&self->status_flash, g_free);
  window_sync_state (self);
  return G_SOURCE_REMOVE;
}

static void
window_flash (W42Window *self, const char *format, ...)
{
  va_list args;

  va_start (args, format);
  g_free (self->status_flash);
  self->status_flash = g_strdup_vprintf (format, args);
  va_end (args);

  if (self->status_flash_id != 0)
    g_source_remove (self->status_flash_id);
  self->status_flash_id = g_timeout_add_seconds (6, window_flash_done, self);
  if (self->status_mod != NULL)
    gtk_label_set_text (GTK_LABEL (self->status_mod), self->status_flash);
}

/* Paste and Paste Special are only worth offering when the clipboard has
 * text in it.  The clipboard says when it changes, so this runs then and
 * when the window is built rather than on every keystroke. */
static void
window_sync_paste (W42Window *self)
{
  GdkClipboard *clipboard = gtk_widget_get_clipboard (GTK_WIDGET (self));
  gboolean has_text = FALSE;
  static const char *paste_actions[] = { "paste", "paste-text" };

  if (clipboard != NULL)
    {
      GdkContentFormats *formats = gdk_clipboard_get_formats (clipboard);

      has_text = formats != NULL &&
                 gdk_content_formats_contain_gtype (formats, G_TYPE_STRING);
    }

  for (guint i = 0; i < G_N_ELEMENTS (paste_actions); i++)
    {
      GAction *a = g_action_map_lookup_action (G_ACTION_MAP (self), paste_actions[i]);

      if (a != NULL)
        g_simple_action_set_enabled (G_SIMPLE_ACTION (a), has_text);
    }
}

static void
on_clipboard_changed (GdkClipboard *clipboard, gpointer data)
{
  (void) clipboard;
  window_sync_paste (W42_WINDOW (data));
}

/* ---------------------------------------------------------------------- */
/* Small dialog helpers                                                    */
/* ---------------------------------------------------------------------- */

static void
show_message (W42Window *self, const char *heading, const char *detail)
{
  w42_message_show (GTK_WINDOW (self), heading, detail);
}

static void
show_error (W42Window *self, const char *heading, GError *error)
{
  show_message (self, heading, error != NULL ? error->message : NULL);
}

static void
window_update_title (W42Window *self)
{
  char *name = w42_document_get_title (self->doc);
  char *title;

  /* "Document1 - Word42 0.9.0", and Word's asterisk for unsaved changes. */
  title = g_strdup_printf ("%s%s - Word42 " W42_VERSION, name,
                           w42_document_get_modified (self->doc) ? "*" : "");

  gtk_window_set_title (GTK_WINDOW (self), title);

  if (self->title_label != NULL)
    {
      char *caption = g_strdup_printf ("Word42 " W42_VERSION " - %s%s", name,
                        w42_document_get_modified (self->doc) ? "*" : "");
      gtk_label_set_text (GTK_LABEL (self->title_label), caption);
      g_free (caption);
    }

  g_free (title);
  g_free (name);
}

/* ---------------------------------------------------------------------- */
/* File actions                                                            */
/* ---------------------------------------------------------------------- */

static GtkFileFilter *
named_filter (const char *name, const char * const *patterns)
{
  GtkFileFilter *filter = gtk_file_filter_new ();

  gtk_file_filter_set_name (filter, name);
  for (guint i = 0; patterns[i] != NULL; i++)
    gtk_file_filter_add_pattern (filter, patterns[i]);

  return filter;
}

static GListModel *
file_filters (void)
{
  GListStore *store = g_list_store_new (GTK_TYPE_FILE_FILTER);
  static const char * const all_docs[] = { "*.rtf", "*.docx", "*.doc", "*.odt", "*.abw", "*.zabw", "*.txt", "*.text", "*.pdf", "*.html", "*.htm", "*.pptx", NULL };
  static const char * const odt[] = { "*.odt", NULL };
  static const char * const pptx[] = { "*.pptx", NULL };
  static const char * const docx[] = { "*.docx", NULL };
  static const char * const abw[] = { "*.abw", "*.zabw", NULL };
  static const char * const web[] = { "*.html", "*.htm", NULL };
  static const char * const rtf[] = { "*.rtf", NULL };
  static const char * const doc[] = { "*.doc", NULL };
  static const char * const text[] = { "*.txt", "*.text", NULL };
  static const char * const pdf[] = { "*.pdf", NULL };
  static const char * const any[] = { "*", NULL };

  g_list_store_append (store, named_filter ("All Documents (*.rtf, *.docx, *.doc, *.odt, *.abw, *.txt, *.pdf, *.html)",
                                            all_docs));
  g_list_store_append (store, named_filter ("Rich Text Format (*.rtf)", rtf));
  g_list_store_append (store, named_filter ("Word Document (*.docx)", docx));
  g_list_store_append (store, named_filter ("Word 97-2003 (*.doc)", doc));
  g_list_store_append (store, named_filter ("OpenDocument Text (*.odt)", odt));
  g_list_store_append (store, named_filter ("AbiWord (*.abw, *.zabw)", abw));
  g_list_store_append (store, named_filter ("Web Pages (*.html)", web));
  g_list_store_append (store, named_filter ("Presentations (*.pptx)", pptx));
  g_list_store_append (store, named_filter ("Text Documents (*.txt)", text));
  if (w42_pdf_import_available ())
    g_list_store_append (store, named_filter ("PDF Documents (*.pdf)", pdf));
  g_list_store_append (store, named_filter ("All Files", any));

  return G_LIST_MODEL (store);
}

#define RECENT_MAX 8

/* ---------------------------------------------------------------------- */
/* Autosave and recovery                                                   */
/* ---------------------------------------------------------------------- */

static char *
autosave_dir (void)
{
  return g_build_filename (g_get_user_data_dir (), "word42", "autosave", NULL);
}

/* The copy's name comes from the document's own path, so two windows on
 * one file share a copy and a file reopened finds its old one. */
static char *
autosave_path_for (W42Window *self)
{
  GFile *file = w42_document_get_file (self->doc);
  char *dir = autosave_dir ();
  char *name, *path;

  if (file != NULL)
    {
      char *orig = g_file_get_path (file);
      name = g_strdup_printf ("%u.rtf", g_str_hash (orig != NULL ? orig : ""));
      g_free (orig);
    }
  else
    name = g_strdup_printf ("untitled-%p.rtf", (void *) self);

  path = g_build_filename (dir, name, NULL);
  g_free (name);
  g_free (dir);
  return path;
}

static void
autosave_remove (W42Window *self)
{
  if (self->autosave_path != NULL)
    {
      char *meta = g_strconcat (self->autosave_path, ".txt", NULL);

      g_unlink (self->autosave_path);
      g_unlink (meta);
      g_free (meta);
      g_clear_pointer (&self->autosave_path, g_free);
    }
}

static gboolean
on_autosave (gpointer data)
{
  W42Window *self = data;
  char *path, *dir, *meta;
  GFile *file, *orig;
  GError *error = NULL;

  if (!self->autosave_dirty || !w42_document_get_modified (self->doc))
    return G_SOURCE_CONTINUE;

  /* The document may have been saved under a new name since. */
  path = autosave_path_for (self);
  if (self->autosave_path != NULL && !g_str_equal (path, self->autosave_path))
    autosave_remove (self);
  g_free (self->autosave_path);
  self->autosave_path = path;

  dir = autosave_dir ();
  g_mkdir_with_parents (dir, 0700);
  g_free (dir);

  file = g_file_new_for_path (path);
  if (w42_rtf_save (w42_document_pt (self->doc), w42_document_page_setup (self->doc),
                    file, &error))
    {
      /* Beside it, where the document really lives, for the recovery. */
      meta = g_strconcat (path, ".txt", NULL);
      orig = w42_document_get_file (self->doc);
      if (orig != NULL)
        {
          char *orig_path = g_file_get_path (orig);
          g_file_set_contents (meta, orig_path != NULL ? orig_path : "", -1, NULL);
          g_free (orig_path);
        }
      else
        g_file_set_contents (meta, "", -1, NULL);
      g_free (meta);
      self->autosave_dirty = FALSE;
    }
  else
    g_clear_error (&error);

  g_object_unref (file);
  return G_SOURCE_CONTINUE;
}

int
w42_window_recover_all (GtkApplication *app)
{
  char *dir = autosave_dir ();
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *name;
  int recovered = 0;

  while (d != NULL && (name = g_dir_read_name (d)) != NULL)
    {
      char *path, *meta, *orig = NULL;
      GFile *file;
      W42Window *window;
      GError *error = NULL;

      if (!g_str_has_suffix (name, ".rtf"))
        continue;

      path = g_build_filename (dir, name, NULL);
      meta = g_strconcat (path, ".txt", NULL);
      g_file_get_contents (meta, &orig, NULL, NULL);

      window = W42_WINDOW (w42_window_new (app));
      file = g_file_new_for_path (path);
      if (w42_document_load (window->doc, file, &error))
        {
          /* The copy is loaded; the document is the original again, as
           * modified, so Save puts it back where it belongs. */
          if (orig != NULL && *orig != '\0')
            {
              GFile *o = g_file_new_for_path (orig);
              w42_document_set_file (window->doc, o);
              g_object_unref (o);
            }
          else
            w42_document_set_file (window->doc, NULL);
          w42_document_set_modified (window->doc, TRUE);
          window_update_title (window);
          window_sync_state (window);
          gtk_window_present (GTK_WINDOW (window));

          {
            char *title = w42_document_get_title (window->doc);
            char *heading = g_strdup_printf ("Word42 recovered \342\200\234%s\342\200\235.", title);

            show_message (window, heading,
                          "It was not saved when Word42 last stopped. Save it to "
                          "keep it; close it to let it go.");
            g_free (heading);
            g_free (title);
          }
          recovered++;
        }
      else
        {
          g_clear_error (&error);
          gtk_window_destroy (GTK_WINDOW (window));
        }

      /* Recovered or unreadable, the copy has done what it can. */
      g_unlink (path);
      g_unlink (meta);

      g_object_unref (file);
      g_free (orig);
      g_free (meta);
      g_free (path);
    }

  if (d != NULL)
    g_dir_close (d);
  g_free (dir);
  return recovered;
}

/* Fills a window's recent-files section from the settings. */
static void
window_refresh_recent (W42Window *self)
{
  char **paths;

  if (self->recent_menu == NULL)
    return;

  g_menu_remove_all (self->recent_menu);
  paths = w42_settings_get_strv ("recent");
  for (guint i = 0; paths[i] != NULL && i < RECENT_MAX; i++)
    {
      char *base = g_path_get_basename (paths[i]);
      char *label = g_strdup_printf ("_%u %s", i + 1, base);
      GMenuItem *item = g_menu_item_new (label, NULL);

      g_menu_item_set_action_and_target (item, "win.open-recent", "s", paths[i]);
      g_menu_append_item (self->recent_menu, item);
      g_object_unref (item);
      g_free (label);
      g_free (base);
    }
  g_strfreev (paths);
}

/* Puts `file` at the top of the recent list and shows every window the
 * new list. */
static void
window_note_recent (W42Window *self, GFile *file)
{
  char *path = g_file_get_path (file);
  char **old;
  GPtrArray *list;
  GtkApplication *app;

  if (path == NULL)
    return;

  old = w42_settings_get_strv ("recent");
  list = g_ptr_array_new_with_free_func (g_free);
  g_ptr_array_add (list, g_strdup (path));
  for (guint i = 0; old[i] != NULL && list->len < RECENT_MAX; i++)
    if (!g_str_equal (old[i], path))
      g_ptr_array_add (list, g_strdup (old[i]));
  g_ptr_array_add (list, NULL);
  w42_settings_set_strv ("recent", (const char * const *) list->pdata);
  g_ptr_array_free (list, TRUE);
  g_strfreev (old);
  g_free (path);

  app = gtk_window_get_application (GTK_WINDOW (self));
  for (GList *l = app != NULL ? gtk_application_get_windows (app) : NULL; l != NULL; l = l->next)
    if (W42_IS_WINDOW (l->data))
      window_refresh_recent (W42_WINDOW (l->data));
}

void
w42_window_open (W42Window *self, GFile *file)
{
  GError *error = NULL;

  g_return_if_fail (W42_IS_WINDOW (self));
  g_return_if_fail (G_IS_FILE (file));

  if (!w42_document_load (self->doc, file, &error))
    {
      show_error (self, "Word42 could not open that file.", error);
      g_clear_error (&error);
      return;
    }
  w42_pt_set_author (w42_document_pt (self->doc), window_author_name ());

  window_note_recent (self, file);
  window_update_title (self);
  window_sync_state (self);
}

/* A recent file opens here if this window is untouched, else in its own. */
static void
action_open_recent (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GFile *file = g_file_new_for_path (g_variant_get_string (param, NULL));
  W42Window *target = self;

  (void) action;

  if (w42_document_get_modified (self->doc) || w42_document_get_file (self->doc) != NULL)
    {
      target = W42_WINDOW (w42_window_new (gtk_window_get_application (GTK_WINDOW (self))));
      gtk_window_present (GTK_WINDOW (target));
    }

  w42_window_open (target, file);
  g_object_unref (file);
}

static void
on_open_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      {
        /* File > Open never throws work away: a document with changes,
         * or one already on disk, keeps its window and the file opens in
         * a new one, as Open Recent does. */
        W42Window *target = self;

        if (w42_document_get_modified (self->doc) || w42_document_get_file (self->doc) != NULL)
          {
            target = W42_WINDOW (w42_window_new (gtk_window_get_application (GTK_WINDOW (self))));
            gtk_window_present (GTK_WINDOW (target));
          }
        w42_window_open (target, file);
      }
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    {
      show_error (self, "Word42 could not open that file.", error);
    }

  g_clear_error (&error);
}

static void
action_open (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListModel *filters = file_filters ();

  (void) action; (void) param;

  gtk_file_dialog_set_title (dialog, "Open");
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_open_response, self);

  g_object_unref (filters);
  g_object_unref (dialog);
}

/* Every path that finishes a save ends here, so that a save begun in order to
 * close the window actually closes it -- and, just as importantly, so that a
 * save that failed does not. */
static void
window_saved (W42Window *self, gboolean succeeded)
{
  if (succeeded && w42_document_get_file (self->doc) != NULL)
    window_note_recent (self, w42_document_get_file (self->doc));
  if (succeeded)
    autosave_remove (self);

  window_update_title (self);
  window_sync_state (self);

  if (self->close_after_save && succeeded)
    {
      self->close_after_save = FALSE;
      self->force_close = TRUE;
      gtk_window_destroy (GTK_WINDOW (self));
      return;
    }

  self->close_after_save = FALSE;
}

static void
on_save_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;
  gboolean saved = FALSE;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      /* A name typed without an extension is saved as Rich Text, which
       * keeps everything, rather than silently as plain text. */
      {
        char *base = g_file_get_basename (file);

        if (base != NULL && strchr (base, '.') == NULL)
          {
            GFile *parent = g_file_get_parent (file);
            char *named = g_strconcat (base, ".rtf", NULL);
            GFile *fixed = parent != NULL ? g_file_get_child (parent, named) : g_file_new_for_path (named);

            g_object_unref (file);
            file = fixed;
            g_free (named);
            g_clear_object (&parent);
          }
        g_free (base);
      }
      saved = w42_document_save (self->doc, file, &error);
      if (!saved)
        show_error (self, "Word42 could not save that file.", error);

      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    {
      show_error (self, "Word42 could not save that file.", error);
    }

  g_clear_error (&error);
  window_saved (self, saved);
}

static void
window_save_as (W42Window *self)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListModel *filters = file_filters ();
  char *name = w42_document_get_title (self->doc);

  gtk_file_dialog_set_title (dialog, "Save As");
  gtk_file_dialog_set_filters (dialog, filters);

  /* RTF is what word42 saves by default now: it is the only format it writes
   * that keeps the formatting the document actually has. */
  if (!g_str_has_suffix (name, ".rtf") && !g_str_has_suffix (name, ".txt") &&
      !g_str_has_suffix (name, ".docx") && !g_str_has_suffix (name, ".abw") &&
      !g_str_has_suffix (name, ".zabw") && !g_str_has_suffix (name, ".odt"))
    {
      char *stem = g_strdup (name);
      char *dot = strrchr (stem, '.');
      char *suggested;

      /* "letter.doc" becomes "letter.rtf", not "letter.doc.rtf". */
      if (dot != NULL && (g_str_equal (dot, ".doc") || g_str_equal (dot, ".pdf")))
        *dot = '\0';
      suggested = g_strconcat (stem, ".rtf", NULL);
      gtk_file_dialog_set_initial_name (dialog, suggested);
      g_free (suggested);
      g_free (stem);
    }
  else
    {
      gtk_file_dialog_set_initial_name (dialog, name);
    }

  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL, on_save_response, self);

  g_free (name);
  g_object_unref (filters);
  g_object_unref (dialog);
}

static void
action_save (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GFile *file = w42_document_get_file (self->doc);
  GError *error = NULL;
  gboolean saved;

  (void) action; (void) param;

  if (file == NULL || w42_io_guess_format (file) == W42_FORMAT_DOC ||
      w42_io_guess_format (file) == W42_FORMAT_PDF ||
      w42_io_guess_format (file) == W42_FORMAT_HTML)
    {
      /* Never saved, or read from a format word42 does not write: Save
       * turns into Save As, and window_saved() runs when that dialog comes
       * back rather than now. */
      window_save_as (self);
      return;
    }

  saved = w42_document_save (self->doc, file, &error);
  if (!saved)
    {
      show_error (self, "Word42 could not save that file.", error);
      g_clear_error (&error);
    }

  window_saved (self, saved);
}

static void
action_save_as (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  window_save_as (W42_WINDOW (data));
}

/* ---- Closing a document with unsaved changes -------------------------- */

enum {
  CLOSE_CANCEL = 0,
  CLOSE_DISCARD,
  CLOSE_SAVE
};

/* Dismissing the box -- Escape, or its own close button -- means cancel:
 * losing the document to a stray keypress would be exactly the accident
 * this box exists to prevent. */
static void
on_close_choice (int choice, gpointer data)
{
  W42Window *self = data;

  if (!W42_IS_WINDOW (self))
    return;

  switch (choice)
    {
    case CLOSE_SAVE:
      self->close_after_save = TRUE;
      g_action_group_activate_action (G_ACTION_GROUP (self), "save", NULL);
      break;

    case CLOSE_DISCARD:
      self->force_close = TRUE;
      gtk_window_destroy (GTK_WINDOW (self));
      break;

    case CLOSE_CANCEL:
    default:
      break;
    }
}

static gboolean window_document_shared (W42Window *self);

static gboolean
on_close_request (GtkWindow *window, gpointer data)
{
  W42Window *self = W42_WINDOW (window);
  static const char * const buttons[] = { "Cancel", "Don't Save", "_Save", NULL };
  char *name, *heading;

  (void) data;

  /* Nothing at risk: let it close.  A document still open in another
   * window is not at risk either; that window will ask when it goes. */
  if (self->force_close || !w42_document_get_modified (self->doc) ||
      window_document_shared (self))
    return GDK_EVENT_PROPAGATE;

  name = w42_document_get_title (self->doc);
  heading = g_strdup_printf ("Save changes to \342\200\234%s\342\200\235?", name);

  w42_choice_show (GTK_WINDOW (self), heading,
                   "If you close without saving, the changes you have made "
                   "will be lost.",
                   buttons, CLOSE_SAVE, CLOSE_CANCEL, on_close_choice, self);

  g_free (heading);
  g_free (name);

  return GDK_EVENT_STOP;             /* hold the window open for the answer */
}

/* Whether some other window shows the same document. */
static gboolean
window_document_shared (W42Window *self)
{
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  GList *windows = app != NULL ? gtk_application_get_windows (app) : NULL;

  for (GList *l = windows; l != NULL; l = l->next)
    if (l->data != self && W42_IS_WINDOW (l->data) &&
        W42_WINDOW (l->data)->doc == self->doc)
      return TRUE;

  return FALSE;
}

static void
action_new_window (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkWidget *window;

  (void) action; (void) param;

  window = w42_window_new_for_document (gtk_window_get_application (GTK_WINDOW (self)),
                                        self->doc);
  gtk_window_present (GTK_WINDOW (window));
}

static void
action_options (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_options_dialog_show (GTK_WINDOW (self), self->view);
}

/* What Tools > Options and the View menu remembered, applied to a new
 * window. */
static void
window_apply_settings (W42Window *self)
{
  char *view = w42_settings_get_string ("default-view", "page-layout");
  int zoom = w42_settings_get_int ("zoom", 100);
  gboolean paged = g_str_equal (view, "page-layout");
  GAction *act;

  act = g_action_map_lookup_action (G_ACTION_MAP (self), "view-mode");
  w42_view_set_mode (self->view, paged ? W42_VIEW_PAGE_LAYOUT : W42_VIEW_NORMAL);
  if (act != NULL)
    g_simple_action_set_state (G_SIMPLE_ACTION (act),
                               g_variant_new_string (paged ? "page-layout" : "normal"));
  g_free (view);

  for (guint i = 0; i < G_N_ELEMENTS (ZOOM_STEPS); i++)
    if ((int) lround (ZOOM_STEPS[i] * 100) == zoom)
      {
        self->updating = TRUE;
        w42_view_set_zoom (self->view, ZOOM_STEPS[i]);
        gtk_drop_down_set_selected (GTK_DROP_DOWN (self->zoom_drop), i);
        self->updating = FALSE;
      }

  {
    struct { const char *key; const char *action; GtkWidget *widget; } bars[] = {
      { "show-ruler",        "ruler",        self->ruler },
      { "show-standard-bar", "standard-bar", self->standard_bar },
      { "show-format-bar",   "format-bar",   self->format_bar },
    };

    for (guint i = 0; i < G_N_ELEMENTS (bars); i++)
      {
        gboolean visible = w42_settings_get_bool (bars[i].key, TRUE);

        gtk_widget_set_visible (bars[i].widget, visible);
        act = g_action_map_lookup_action (G_ACTION_MAP (self), bars[i].action);
        if (act != NULL)
          g_simple_action_set_state (G_SIMPLE_ACTION (act), g_variant_new_boolean (visible));
      }
  }
}

static void
action_help_contents (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_help_dialog_show (GTK_WINDOW (self));
}

static void
action_new (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkWidget *window;

  (void) action; (void) param;

  window = w42_window_new (gtk_window_get_application (GTK_WINDOW (self)));
  gtk_window_present (GTK_WINDOW (window));
}

static void
action_close (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  gtk_window_close (GTK_WINDOW (data));
}

/* ---------------------------------------------------------------------- */
/* Edit actions                                                            */
/* ---------------------------------------------------------------------- */

#define VIEW_ACTION(name, call)                                    \
  static void                                                      \
  action_##name (GSimpleAction *action, GVariant *param, gpointer data) \
  {                                                                \
    (void) action; (void) param;                                   \
    call (W42_WINDOW (data)->view);                                \
  }

VIEW_ACTION (undo,       w42_view_undo)
VIEW_ACTION (redo,       w42_view_redo)
VIEW_ACTION (cut,        w42_view_cut)
VIEW_ACTION (copy,       w42_view_copy)
VIEW_ACTION (paste,      w42_view_paste)
VIEW_ACTION (select_all, w42_view_select_all)
VIEW_ACTION (bold,       w42_view_toggle_bold)
VIEW_ACTION (italic,     w42_view_toggle_italic)
VIEW_ACTION (underline,  w42_view_toggle_underline)

#undef VIEW_ACTION

static void
action_align (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  const char *which = g_variant_get_string (param, NULL);
  W42Align align = W42_ALIGN_LEFT;

  (void) action;

  if (g_strcmp0 (which, "center") == 0)
    align = W42_ALIGN_CENTER;
  else if (g_strcmp0 (which, "right") == 0)
    align = W42_ALIGN_RIGHT;
  else if (g_strcmp0 (which, "justify") == 0)
    align = W42_ALIGN_JUSTIFY;

  w42_view_set_align (self->view, align);
}

/* ---- Find and Replace ------------------------------------------------- */

static void
window_show_find (W42Window *self, gboolean replace)
{
  if (self->find_dialog == NULL)
    {
      self->find_dialog = w42_find_dialog_new (GTK_WINDOW (self), self->view);
      g_object_add_weak_pointer (G_OBJECT (self->find_dialog),
                                 (gpointer *) &self->find_dialog);
    }

  w42_find_dialog_set_replace_mode (W42_FIND_DIALOG (self->find_dialog),
                                    replace);
  {
    /* The selection, if it is a short piece of one line, is what to look
     * for; and the box's Find What field takes the keys. */
    char *selected = w42_view_get_selected_text (self->view);

    w42_find_dialog_prime (W42_FIND_DIALOG (self->find_dialog),
                           selected != NULL && strchr (selected, '\n') == NULL && strlen (selected) < 200 ? selected : NULL);
    g_free (selected);
  }
  gtk_window_present (GTK_WINDOW (self->find_dialog));
}

static void
action_find (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  window_show_find (W42_WINDOW (data), FALSE);
}

static void
action_replace (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  window_show_find (W42_WINDOW (data), TRUE);
}

/* F3 repeats the last search.  With no search yet made there is nothing to
 * repeat, so it opens the box instead. */
static void
action_find_next (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;

  if (self->find_dialog != NULL)
    w42_find_dialog_find_again (W42_FIND_DIALOG (self->find_dialog));
  else
    window_show_find (self, FALSE);
}

/* ---------------------------------------------------------------------- */
/* View actions                                                            */
/* ---------------------------------------------------------------------- */

static void
action_view_mode (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  const char *which = g_variant_get_string (param, NULL);
  gboolean paged = g_strcmp0 (which, "page-layout") == 0;

  w42_view_set_mode (self->view,
                     paged ? W42_VIEW_PAGE_LAYOUT : W42_VIEW_NORMAL);
  g_simple_action_set_state (action, g_variant_new_string (which));
}

static void
action_zoom (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  double zoom = g_variant_get_double (param);

  (void) action;
  w42_view_set_zoom (self->view, zoom);
  /* The Zoom box on the toolbar shows the same. */
  for (guint i = 0; i < G_N_ELEMENTS (ZOOM_STEPS); i++)
    if (ABS (ZOOM_STEPS[i] - zoom) < 0.001)
      {
        self->updating = TRUE;
        gtk_drop_down_set_selected (GTK_DROP_DOWN (self->zoom_drop), i);
        self->updating = FALSE;
      }
}

static void
action_show_marks (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GVariant *state = g_action_get_state (G_ACTION (action));
  gboolean on = !g_variant_get_boolean (state);

  (void) param;
  g_variant_unref (state);
  g_simple_action_set_state (action, g_variant_new_boolean (on));
  w42_view_set_show_marks (self->view, on);
  /* Not remembered: a document opens clean, as in Word 6; the marks are a
   * look under the bonnet, one keystroke away. */
}

static void
action_column_break (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  /* In columns a page break starts the next column, which is what a
   * column break is; in one column it is a page break. */
  w42_view_insert_page_break (self->view);
}

static void
action_toggle_ruler (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  gboolean visible;

  (void) param;

  visible = !gtk_widget_get_visible (self->ruler);
  gtk_widget_set_visible (self->ruler, visible);
  g_simple_action_set_state (action, g_variant_new_boolean (visible));
  w42_settings_set_bool ("show-ruler", visible);
}

static void
action_toggle_toolbar (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  const char *name = g_action_get_name (G_ACTION (action));
  GtkWidget *bar;
  gboolean visible;

  (void) param;

  bar = g_str_has_prefix (name, "standard") ? self->standard_bar
                                            : self->format_bar;
  visible = !gtk_widget_get_visible (bar);
  gtk_widget_set_visible (bar, visible);
  g_simple_action_set_state (action, g_variant_new_boolean (visible));
  w42_settings_set_bool (g_str_has_prefix (name, "standard") ? "show-standard-bar"
                                                             : "show-format-bar",
                         visible);
}

/* ---------------------------------------------------------------------- */
/* Tools and Help                                                          */
/* ---------------------------------------------------------------------- */

static void
action_word_count (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  W42PieceTable *pt = w42_document_pt (self->doc);
  gsize first = w42_pt_first_caret_pos (pt);
  char *text = w42_pt_get_text (pt, first, w42_pt_length (pt) - first);
  glong characters = g_utf8_strlen (text, -1);
  gsize words = 0;
  gsize paragraphs = 1;
  gboolean in_word = FALSE;
  char *detail;

  (void) action; (void) param;

  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == '\n')
        paragraphs++;

      if (g_unichar_isspace (c))
        {
          in_word = FALSE;
        }
      else if (!in_word)
        {
          in_word = TRUE;
          words++;
        }
    }

  detail = g_strdup_printf ("Words:\t\t%" G_GSIZE_FORMAT "\n"
                            "Characters:\t%ld\n"
                            "Paragraphs:\t%" G_GSIZE_FORMAT "\n"
                            "Pages:\t\t%d",
                            words, characters, paragraphs,
                            w42_layout_n_pages (w42_view_get_layout (self->view)));

  show_message (self, "Word Count", detail);

  g_free (detail);
  g_free (text);
}

static void
on_font_dialog_done (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  PangoFontDescription *desc;

  desc = gtk_font_dialog_choose_font_finish (GTK_FONT_DIALOG (source),
                                             result, NULL);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
  if (desc == NULL)
    return;

  if (pango_font_description_get_set_fields (desc) & PANGO_FONT_MASK_FAMILY)
    w42_view_set_font_family (self->view, pango_font_description_get_family (desc));

  if (pango_font_description_get_set_fields (desc) & PANGO_FONT_MASK_SIZE)
    {
      double points = pango_font_description_get_size (desc) / (double) PANGO_SCALE;
      w42_view_set_font_size (self->view, (int) (points * 2.0 + 0.5));
    }

  /* The box's Bold and Italic faces count too. */
  {
    W42CharFmt want;
    W42CharMask mask = 0;

    memset (&want, 0, sizeof want);
    if (pango_font_description_get_set_fields (desc) & PANGO_FONT_MASK_WEIGHT)
      {
        want.bold = pango_font_description_get_weight (desc) >= PANGO_WEIGHT_SEMIBOLD;
        mask |= W42_CHAR_BOLD;
      }
    if (pango_font_description_get_set_fields (desc) & PANGO_FONT_MASK_STYLE)
      {
        want.italic = pango_font_description_get_style (desc) != PANGO_STYLE_NORMAL;
        mask |= W42_CHAR_ITALIC;
      }
    if (mask != 0)
      w42_view_apply_char_fmt (self->view, mask, &want);
  }

  pango_font_description_free (desc);
}

/* Ctrl+] and Ctrl+[: the size a point up or down, as Word 6 stepped it. */
/* The name from Options > User Info, or the account's. */
static const char *
window_author_name (void)
{
  static char *cached;
  char *name = w42_settings_get_string ("user-name", "");

  g_free (cached);
  if (*name == '\0')
    {
      const char *real = g_get_real_name ();

      g_free (name);
      name = g_strdup (real != NULL && !g_str_equal (real, "Unknown") ? real : g_get_user_name ());
    }
  cached = name;
  return cached;
}

static void
action_font_step (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  W42CharFmt fmt;
  int step = g_str_equal (g_action_get_name (G_ACTION (action)), "font-grow") ? 2 : -2;

  (void) param;
  w42_view_get_char_fmt (self->view, &fmt);
  w42_view_set_font_size (self->view, CLAMP ((fmt.size > 0 ? fmt.size : 20) + step, 2, 3276));
}

static void
action_font_dialog (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFontDialog *dialog = gtk_font_dialog_new ();
  W42CharFmt fmt;
  PangoFontDescription *desc = pango_font_description_new ();

  (void) action; (void) param;

  w42_view_get_char_fmt (self->view, &fmt);
  pango_font_description_set_family (desc, fmt.family ? fmt.family : "Serif");
  pango_font_description_set_size (desc,
    (int) (w42_halfpt_to_pt (fmt.size) * PANGO_SCALE));
  if (fmt.bold)
    pango_font_description_set_weight (desc, PANGO_WEIGHT_BOLD);
  if (fmt.italic)
    pango_font_description_set_style (desc, PANGO_STYLE_ITALIC);

  gtk_font_dialog_set_title (dialog, "Font");
  gtk_font_dialog_choose_font (dialog, GTK_WINDOW (self), desc, NULL,
                               on_font_dialog_done, self);

  pango_font_description_free (desc);
  g_object_unref (dialog);
}

/* Word 6's About box was a banner, a version line and an OK button, and so is
 * this one.  It is built by hand rather than with GtkAboutDialog because the
 * whole point of the banner is that it should not look like every other GTK
 * dialog on the desktop. */
/* ---- Pictures --------------------------------------------------------- */


/* Every format gdk-pixbuf has a loader for, by file extension.  This is
 * what gtk_file_filter_add_pixbuf_formats() did before GTK deprecated it. */
static void
add_picture_patterns (GtkFileFilter *filter)
{
  GSList *formats = gdk_pixbuf_get_formats ();

  for (GSList *l = formats; l != NULL; l = l->next)
    {
      char **extensions = gdk_pixbuf_format_get_extensions (l->data);

      for (guint i = 0; extensions != NULL && extensions[i] != NULL; i++)
        {
          char *pattern = g_strdup_printf ("*.%s", extensions[i]);
          gtk_file_filter_add_pattern (filter, pattern);
          g_free (pattern);
        }

      g_strfreev (extensions);
    }

  g_slist_free (formats);
}

static void
on_picture_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      int width = 0, height = 0;
      const char *format = NULL;
      GBytes *bytes = w42_image_load_file (file, &width, &height, &format,
                                           &error);

      if (bytes != NULL)
        {
          w42_view_insert_picture (self->view, bytes, format, width, height);
          g_bytes_unref (bytes);
        }
      else
        {
          show_error (self, "Word42 could not insert that picture.", error);
        }

      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    {
      show_error (self, "Word42 could not open that picture.", error);
    }

  g_clear_error (&error);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_insert_picture (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  GtkFileFilter *pictures = gtk_file_filter_new ();

  (void) action; (void) param;

  /* Every format gdk-pixbuf has a loader for, which is every format that
   * word42 can show. */
  gtk_file_filter_set_name (pictures, "Pictures");
  add_picture_patterns (pictures);
  g_list_store_append (filters, pictures);

  gtk_file_dialog_set_title (dialog, "Insert Picture");
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL,
                        on_picture_response, self);

  g_object_unref (filters);
  g_object_unref (dialog);
}

/* ---- Export as PDF ---------------------------------------------------- */

static void
on_export_pdf_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      /* Export does not make the PDF the document's file: the document is
       * still the RTF or text it came from, and Save keeps going there. */
      if (!w42_pdf_export (w42_document_pt (self->doc),
                           w42_document_page_setup (self->doc), file, &error))
        show_error (self, "Word42 could not export the PDF.", error);

      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    {
      show_error (self, "Word42 could not export the PDF.", error);
    }

  g_clear_error (&error);
}

/* ---- Export as web page ----------------------------------------------- */

static void
on_export_html_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      if (!w42_html_export (w42_document_pt (self->doc),
                            w42_document_page_setup (self->doc), file, &error))
        show_error (self, "Word42 could not export the web page.", error);
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, "Word42 could not export the web page.", error);

  g_clear_error (&error);
}

static void
action_export_html (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  static const char * const html[] = { "*.html", "*.htm", NULL };
  char *name = w42_document_get_title (self->doc);
  char *dot = strrchr (name, '.');
  char *suggested;

  (void) action; (void) param;

  if (dot != NULL)
    *dot = '\0';
  suggested = g_strconcat (name, ".html", NULL);

  g_list_store_append (filters, named_filter ("Web Pages (*.html)", html));
  gtk_file_dialog_set_title (dialog, "Export as Web Page");
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, suggested);
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL,
                        on_export_html_response, self);

  g_free (suggested);
  g_free (name);
  g_object_unref (filters);
  g_object_unref (dialog);
}

/* File > Export as Presentation: the document's outline as slides. */
static void
on_export_pptx_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      if (!w42_pptx_save (w42_document_pt (self->doc),
                          w42_document_page_setup (self->doc), file, &error))
        show_error (self, "Word42 could not export the presentation.", error);
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, "Word42 could not export the presentation.", error);

  g_clear_error (&error);
}

static void
action_export_pptx (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  static const char * const pptx[] = { "*.pptx", NULL };
  char *name = w42_document_get_title (self->doc);
  char *dot = strrchr (name, '.');
  char *suggested;
  GPtrArray *slides;

  (void) action; (void) param;

  slides = w42_slides_from_document (w42_document_pt (self->doc));
  if (slides->len == 0)
    {
      w42_slides_free (slides);
      g_free (name);
      g_object_unref (filters);
      g_object_unref (dialog);
      show_message (self, "There is nothing to make slides of.",
                    "A slide is a heading and the lines under it: give the "
                    "document's headings the Heading 1 style, or a Title.");
      return;
    }
  {
    char *detail = g_strdup_printf (slides->len == 1 ? "1 slide." : "%u slides.", slides->len);

    window_flash (self, "%s", detail);
    g_free (detail);
  }
  w42_slides_free (slides);

  if (dot != NULL)
    *dot = '\0';
  suggested = g_strconcat (name, ".pptx", NULL);

  g_list_store_append (filters, named_filter ("Presentations (*.pptx)", pptx));
  gtk_file_dialog_set_title (dialog, "Export as Presentation");
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, suggested);
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL,
                        on_export_pptx_response, self);

  g_free (suggested);
  g_free (name);
  g_object_unref (filters);
  g_object_unref (dialog);
}

static void
action_export_pdf (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  w42_view_update_fields (self->view);
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  static const char * const pdf[] = { "*.pdf", NULL };
  char *name = w42_document_get_title (self->doc);
  char *dot = strrchr (name, '.');
  char *suggested;

  (void) action; (void) param;

  if (dot != NULL)
    *dot = '\0';
  suggested = g_strconcat (name, ".pdf", NULL);

  g_list_store_append (filters, named_filter ("PDF Documents (*.pdf)", pdf));
  gtk_file_dialog_set_title (dialog, "Export as PDF");
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, suggested);
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL,
                        on_export_pdf_response, self);

  g_free (suggested);
  g_free (name);
  g_object_unref (filters);
  g_object_unref (dialog);
}

static void
action_print (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  w42_view_update_fields (self->view);

  (void) action; (void) param;
  w42_print_document (GTK_WINDOW (self), self->doc, FALSE);
}

static void
action_print_preview (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  w42_view_update_fields (self->view);

  (void) action; (void) param;
  w42_print_document (GTK_WINDOW (self), self->doc, TRUE);
}

static void
action_apply_style (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action;
  w42_view_apply_style (self->view, g_variant_get_string (param, NULL));
}

static void
action_style_dialog (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_style_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_heading_numbering (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  W42StyleSheet *styles = w42_pt_stylesheet (w42_document_pt (self->doc));
  gboolean on = !w42_stylesheet_get_number_headings (styles);

  (void) param;

  w42_stylesheet_set_number_headings (styles, on);
  g_simple_action_set_state (action, g_variant_new_boolean (on));
  w42_document_mark_unsaved (self->doc);
  w42_document_touch (self->doc);
}

static void
action_list (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  const char *name = g_action_get_name (G_ACTION (action));
  W42ListKind kind = g_str_equal (name, "list-bullets") ? W42_LIST_BULLET
                                                        : W42_LIST_NUMBER;
  GVariant *state = g_action_get_state (G_ACTION (action));
  gboolean on = !g_variant_get_boolean (state);

  (void) param;
  g_variant_unref (state);

  w42_view_set_list (self->view, on ? kind : W42_LIST_NONE);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_table_merge (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_view_table_merge_cells (self->view);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_table_select (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  const char *what = g_variant_get_string (param, NULL);

  (void) action;
  if (g_str_equal (what, "row"))
    w42_view_table_select_row (self->view);
  else
    w42_view_table_select_table (self->view);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_table_split_table (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  if (!w42_view_table_split_table (self->view))
    window_flash (self, "The caret is in the first row: there is nothing above "
                        "it to split off.");
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_table_sort (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  const char *how = g_variant_get_string (param, NULL);

  (void) action;
  w42_view_table_sort (self->view, g_str_equal (how, "descending"));
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_table_convert (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  gboolean ok;

  (void) action; (void) param;
  if (w42_view_in_table (self->view))
    ok = w42_view_table_to_text (self->view);
  else
    ok = w42_view_text_to_table (self->view);
  if (!ok)
    show_message (self, "There is nothing to convert here.",
                  "In a table this makes paragraphs with tabs between the cells; "
                  "on selected paragraphs it makes a table, split at their tabs.");
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_gridlines (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GVariant *state = g_action_get_state (G_ACTION (action));
  gboolean on = !g_variant_get_boolean (state);

  (void) param;
  g_variant_unref (state);
  g_simple_action_set_state (action, g_variant_new_boolean (on));
  w42_view_set_gridlines (self->view, on);
  w42_settings_set_bool ("gridlines", on);
}

static void
action_table_split (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_view_table_split_cell (self->view);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_insert_toc (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;

  /* Down in a note there is nothing to list and nowhere sensible to put
   * the table; the caret has to be in the body. */
  if (w42_view_caret_in_note (self->view))
    {
      window_flash (self, "The caret is in a note: put it in the body of the "
                          "document first.");
      return;
    }

  if (w42_view_insert_toc (self->view) == 0)
    show_message (self, "There are no headings to list.",
                  "Give the document's headings the Heading 1, 2 or 3 style "
                  "and try again.");
}

static void
action_columns (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_columns_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_effects (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_effects_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_hyperlink (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_hyperlink_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_annotation (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_annotations_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_update_toc (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  if (!w42_view_update_toc (self->view))
    show_message (self, "There is no table of contents to update.",
                  "Insert > Table of Contents puts one in.");
}

static void
action_change_case (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  const char *kind = g_variant_get_string (param, NULL);
  W42CaseKind which = W42_CASE_SENTENCE;

  (void) action;
  if (g_str_equal (kind, "lower"))
    which = W42_CASE_LOWER;
  else if (g_str_equal (kind, "upper"))
    which = W42_CASE_UPPER;
  else if (g_str_equal (kind, "title"))
    which = W42_CASE_TITLE;
  else if (g_str_equal (kind, "toggle"))
    which = W42_CASE_TOGGLE;

  /* With nothing selected the word at the caret changes, as in Word. */
  if (!w42_view_has_selection (self->view))
    w42_view_select_word (self->view);
  if (!w42_view_has_selection (self->view))
    return;
  w42_view_change_case (self->view, which);
}

static void
action_hyphenate (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  int n = w42_view_hyphenate (self->view, FALSE);

  (void) action; (void) param;
  if (n < 0)
    show_message (self, "No hyphenation dictionary was found.",
                  "Hyphenation needs libhyphen and a hyph_*.dic pattern file "
                  "for your language, the ones LibreOffice uses.");
  else if (n == 0)
    show_message (self, "There is nothing to hyphenate.", NULL);
}

static void
action_unhyphenate (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  int n;

  (void) action; (void) param;
  n = w42_view_hyphenate (self->view, TRUE);
  if (n > 0)
    window_flash (self, n == 1 ? "1 hyphen removed." : "%d hyphens removed.", n);
  else
    window_flash (self, "There are no soft hyphens to remove.");
}

static void
action_bullets_numbering (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_list_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_table_insert_column (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  w42_view_table_insert_column (W42_WINDOW (data)->view);
}

static void
action_table_delete_column (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  w42_view_table_delete_column (W42_WINDOW (data)->view);
}

static void
action_drop_cap (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_drop_cap_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_frame (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_frame_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_format_picture (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_picture_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_table_properties (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_table_properties_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_table_autoformat (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_table_autoformat_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_field (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_field_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_update_fields (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  int n;

  (void) action; (void) param;
  n = w42_view_update_fields (self->view);
  if (n > 0)
    window_flash (self, n == 1 ? "1 field updated." : "%d fields updated.", n);
  else
    window_flash (self, "There are no fields in this document.");
}

static void
action_drawing (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_drawing_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_section_break (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_view_insert_section_break (self->view);
}

static void
action_cross_reference (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_cross_reference_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_mail_merge (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_mail_merge_dialog_show (GTK_WINDOW (self), self->view);
}

/* Insert > Caption: a "Figure N: " paragraph in the Caption style below
 * the current one, N being one more than the figures captioned so far. */
static void
action_caption (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  W42PieceTable *pt = w42_document_pt (self->doc);
  char *text = w42_pt_get_text (pt, 0, w42_pt_length (pt));
  int n = 0;
  char *label;

  (void) action; (void) param;

  for (const char *p = text; (p = strstr (p, "Figure ")) != NULL; p += 7)
    if ((p == text || p[-1] == '\n') && g_ascii_isdigit (p[7]))
      n = MAX (n, atoi (p + 7));
  g_free (text);

  label = g_strdup_printf ("Figure %d: ", n + 1);
  w42_view_insert_caption (self->view, label);
  g_free (label);
}

/* Tools > AutoCorrect: what is put right as you type. */
static void
action_autocorrect (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_autocorrect_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_bookmark (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_bookmark_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_insert_footnote (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_view_insert_footnote (self->view);
}

static void
action_insert_endnote (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_view_insert_endnote (self->view);
}

static void
action_go_to_note (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  if (!w42_view_go_to_note (self->view))
    window_flash (self, "Put the caret at a note's mark, or in the note itself.");
}

static void
action_borders (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_borders_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_tabs (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_tabs_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_go_to (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_go_to_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_insert_date (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_date_time_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_insert_symbol (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_symbol_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_spelling (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;

  if (self->spell == NULL)
    {
      show_message (self, "No spelling dictionary was found.",
                    "Install a Hunspell dictionary for your language and "
                    "start Word42 again.");
      return;
    }

  if (self->spell_dialog == NULL)
    {
      self->spell_dialog = w42_spell_dialog_new (GTK_WINDOW (self), self->view,
                                                 self->spell);
      g_object_add_weak_pointer (G_OBJECT (self->spell_dialog),
                                 (gpointer *) &self->spell_dialog);
    }

  gtk_window_present (GTK_WINDOW (self->spell_dialog));
  w42_spell_dialog_start (W42_SPELL_DIALOG (self->spell_dialog));
}

static void
action_auto_spell (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GVariant *state = g_action_get_state (G_ACTION (action));
  gboolean on = !g_variant_get_boolean (state);

  (void) param;
  g_variant_unref (state);

  g_simple_action_set_state (action, g_variant_new_boolean (on));
  w42_view_set_spell (self->view, on ? self->spell : NULL);
  w42_settings_set_bool ("auto-spell", on);
}

static void
action_track_changes (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GVariant *state = g_action_get_state (G_ACTION (action));
  gboolean on = !g_variant_get_boolean (state);

  (void) param;
  g_variant_unref (state);

  g_simple_action_set_state (action, g_variant_new_boolean (on));
  w42_view_set_track_changes (self->view, on);
}

static void
action_accept_revisions (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  if (w42_view_resolve_revisions (self->view, TRUE))
    window_flash (self, "Revisions accepted.");
  else
    window_flash (self, "There are no revision marks in this document.");
}

static void
action_reject_revisions (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  if (w42_view_resolve_revisions (self->view, FALSE))
    window_flash (self, "Revisions rejected.");
  else
    window_flash (self, "There are no revision marks in this document.");
}

static void
action_header_footer (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_header_footer_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_page_numbers (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_page_numbers_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_insert_break (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_view_insert_page_break (self->view);
}

static void
action_table_insert (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_insert_table_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_table_insert_row (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  w42_view_table_insert_row (W42_WINDOW (data)->view);
}

static void
action_table_delete_row (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  w42_view_table_delete_row (W42_WINDOW (data)->view);
}

static void
action_page_setup (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_page_setup_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_paragraph (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_paragraph_dialog_show (GTK_WINDOW (self), self->view);
}

static gboolean
about_escape (GtkEventControllerKey *controller, guint keyval, guint keycode,
              GdkModifierType state, gpointer data)
{
  (void) controller; (void) keycode; (void) state;
  if (keyval == GDK_KEY_Escape)
    {
      gtk_window_destroy (GTK_WINDOW (data));
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}

/* The operating system's name and build, the way it describes itself. */
static char *
about_os_string (void)
{
#ifdef G_OS_WIN32
  /* GetVersionEx lies to programs without a manifest; RtlGetVersion does
   * not, and needs no manifest. */
  typedef LONG (WINAPI *RtlGetVersionFn) (PRTL_OSVERSIONINFOW);
  HMODULE ntdll = GetModuleHandleW (L"ntdll.dll");
  RtlGetVersionFn get_version = ntdll != NULL
    ? (RtlGetVersionFn) (void (*) (void)) GetProcAddress (ntdll, "RtlGetVersion") : NULL;
  RTL_OSVERSIONINFOW v;

  memset (&v, 0, sizeof v);
  v.dwOSVersionInfoSize = sizeof v;
  if (get_version != NULL && get_version (&v) == 0)
    return g_strdup_printf ("Windows %lu.%lu build %lu%s",
                            (unsigned long) v.dwMajorVersion, (unsigned long) v.dwMinorVersion,
                            (unsigned long) v.dwBuildNumber,
                            v.dwBuildNumber >= 22000 ? " (Windows 11)" : v.dwMajorVersion == 10 ? " (Windows 10)" : "");
  return g_strdup ("Windows");
#else
  char *name = g_get_os_info (G_OS_INFO_KEY_PRETTY_NAME);
  char *version = g_get_os_info (G_OS_INFO_KEY_VERSION);
  char *out;

  if (name == NULL)
    name = g_strdup ("Unix");
  out = version != NULL && strstr (name, version) == NULL
        ? g_strdup_printf ("%s %s", name, version) : g_strdup (name);
  g_free (name);
  g_free (version);
  return out;
#endif
}

static char *
about_system_info (void)
{
  char *os = about_os_string ();
  char *out = g_strdup_printf (
    "Word42 %s\n"
    "GTK %u.%u.%u (built against %d.%d.%d)  \u00b7  GLib %u.%u.%u\n"
    "Pango %s  \u00b7  Cairo %s  \u00b7  GdkPixbuf %s\n"
    "%s\n"
    "Spelling: %s  \u00b7  Hyphenation: %s  \u00b7  PDF import: %s",
    W42_VERSION,
    gtk_get_major_version (), gtk_get_minor_version (), gtk_get_micro_version (),
    GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION,
    glib_major_version, glib_minor_version, glib_micro_version,
    pango_version_string (), cairo_version_string (), gdk_pixbuf_version,
    os,
#ifdef HAVE_ENCHANT
    "Enchant",
#else
    "none",
#endif
#ifdef HAVE_HYPHEN
    "libhyphen",
#else
    "none",
#endif
#ifdef HAVE_POPPLER
    "Poppler"
#else
    "none"
#endif
    );

  g_free (os);
  return out;
}

static void
action_about (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkWidget *window, *box, *banner, *version, *blurb, *licence, *button;

  (void) action; (void) param;

  window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (window), "About Word42");
  {
    /* Escape closes it, as it does every other box. */
    GtkEventController *key = gtk_event_controller_key_new ();

    g_signal_connect (key, "key-pressed", G_CALLBACK (about_escape), window);
    gtk_widget_add_controller (window, key);
  }
  gtk_window_set_transient_for (GTK_WINDOW (window), GTK_WINDOW (self));
  gtk_window_set_modal (GTK_WINDOW (window), TRUE);
  gtk_window_set_resizable (GTK_WINDOW (window), FALSE);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (box, "w42-about");
  gtk_window_set_child (GTK_WINDOW (window), box);

  banner = gtk_picture_new_for_resource ("/org/word42/word42/about.png");
  gtk_picture_set_content_fit (GTK_PICTURE (banner), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request (banner, 400, 112);
  gtk_box_append (GTK_BOX (box), banner);

  version = gtk_label_new ("Version " W42_VERSION);
  gtk_widget_add_css_class (version, "w42-about-version");
  gtk_label_set_xalign (GTK_LABEL (version), 0.0);
  gtk_widget_set_margin_start (version, 20);
  gtk_widget_set_margin_top (version, 16);
  gtk_box_append (GTK_BOX (box), version);

  blurb = gtk_label_new ("Written in C on GTK 4, Pango and Cairo.");
  gtk_label_set_xalign (GTK_LABEL (blurb), 0.0);
  gtk_widget_set_margin_start (blurb, 20);
  gtk_widget_set_margin_top (blurb, 8);
  gtk_box_append (GTK_BOX (box), blurb);

  /* What this copy is running on, for bug reports. */
  {
    char *info = about_system_info ();
    GtkWidget *system = gtk_label_new (info);

    gtk_label_set_xalign (GTK_LABEL (system), 0.0);
    gtk_label_set_selectable (GTK_LABEL (system), TRUE);
    gtk_widget_set_focusable (system, FALSE);   /* selectable by mouse, not lit up by default */
    gtk_widget_add_css_class (system, "w42-about-licence");
    gtk_widget_set_margin_start (system, 20);
    gtk_widget_set_margin_top (system, 10);
    gtk_box_append (GTK_BOX (box), system);
    g_free (info);
  }

  licence = gtk_label_new (
    "Copyright (C) 2026 Andreas Røsdal.\n\n"
    "This program is free software: you can redistribute it and/or modify it "
    "under the terms of the GNU General Public License as published by the "
    "Free Software Foundation, either version 3 of the License, or (at your "
    "option) any later version.  It comes with ABSOLUTELY NO WARRANTY.\n\n"
    "Word42 is an independent program, not affiliated with or endorsed by "
    "the makers of any other word processor.  The names of file formats "
    "appear only to say which format is meant.");
  gtk_label_set_wrap (GTK_LABEL (licence), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (licence), 52);
  gtk_label_set_xalign (GTK_LABEL (licence), 0.0);
  gtk_widget_add_css_class (licence, "w42-about-licence");
  gtk_widget_set_margin_start (licence, 20);
  gtk_widget_set_margin_end (licence, 20);
  gtk_widget_set_margin_top (licence, 14);
  gtk_box_append (GTK_BOX (box), licence);

  button = gtk_button_new_with_mnemonic ("_OK");
  gtk_widget_set_halign (button, GTK_ALIGN_END);
  gtk_widget_set_margin_end (button, 20);
  gtk_widget_set_margin_top (button, 18);
  gtk_widget_set_margin_bottom (button, 16);
  gtk_widget_set_size_request (button, 88, 26);
  g_signal_connect_swapped (button, "clicked",
                            G_CALLBACK (gtk_window_destroy), window);
  gtk_box_append (GTK_BOX (box), button);

  gtk_window_present (GTK_WINDOW (window));
}

/* ---------------------------------------------------------------------- */
/* Title bar                                                               */
/* ---------------------------------------------------------------------- */

/* Word 6's title bar was navy with its name centred on it in white, and the
 * desktop's own title bar cannot be made to look like that.  So word42 draws
 * its own: a GtkWindowHandle, which keeps dragging and the double-click to
 * maximise working, wrapped round a centre box. */

static void
on_titlebar_minimise (GtkButton *button, gpointer data)
{
  (void) button;
  gtk_window_minimize (GTK_WINDOW (data));
}

static void
on_titlebar_maximise (GtkButton *button, gpointer data)
{
  GtkWindow *window = data;

  (void) button;

  if (gtk_window_is_maximized (window))
    gtk_window_unmaximize (window);
  else
    gtk_window_maximize (window);
}

static void
on_titlebar_close (GtkButton *button, gpointer data)
{
  (void) button;
  gtk_window_close (GTK_WINDOW (data));
}

/* The glyphs are drawn rather than set in a font: a hyphen is not a minimise
 * bar and a multiplication sign is not a close cross, and at sixteen pixels
 * the difference shows. */
static void
draw_caption_glyph (GtkDrawingArea *area, cairo_t *cr,
                    int width, int height, gpointer data)
{
  const char *which = data;
  double cx = width / 2.0;
  double cy = height / 2.0;

  (void) area;

  cairo_set_source_rgb (cr, 0, 0, 0);
  cairo_set_line_width (cr, 1.0);

  if (g_strcmp0 (which, "minimise") == 0)
    {
      cairo_rectangle (cr, cx - 3, cy + 2, 7, 2);
      cairo_fill (cr);
    }
  else if (g_strcmp0 (which, "maximise") == 0)
    {
      cairo_rectangle (cr, cx - 4.5, cy - 4.5, 9, 9);
      cairo_stroke (cr);
      cairo_rectangle (cr, cx - 4.5, cy - 4.5, 9, 2);
      cairo_fill (cr);
    }
  else
    {
      cairo_move_to (cr, cx - 3.5, cy - 3.5);
      cairo_line_to (cr, cx + 3.5, cy + 3.5);
      cairo_move_to (cr, cx + 3.5, cy - 3.5);
      cairo_line_to (cr, cx - 3.5, cy + 3.5);
      cairo_set_line_width (cr, 1.4);
      cairo_stroke (cr);
    }
}

static GtkWidget *
caption_button (const char *glyph, const char *tooltip,
                GCallback callback, gpointer data)
{
  GtkWidget *button = gtk_button_new ();
  GtkWidget *area = gtk_drawing_area_new ();

  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (area), 14);
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (area), 12);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), draw_caption_glyph,
                                  (gpointer) glyph, NULL);

  gtk_button_set_child (GTK_BUTTON (button), area);
  gtk_widget_set_tooltip_text (button, tooltip);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_widget_set_focusable (button, FALSE);
  g_signal_connect (button, "clicked", callback, data);

  return button;
}

static GtkWidget *
build_titlebar (W42Window *self)
{
  GtkWidget *handle = gtk_window_handle_new ();
  GtkWidget *centre = gtk_center_box_new ();
  GtkWidget *icon = gtk_image_new_from_icon_name ("org.word42.word42");
  GtkWidget *right = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);

  gtk_widget_add_css_class (centre, "w42-titlebar");

  gtk_image_set_pixel_size (GTK_IMAGE (icon), 16);
  gtk_widget_set_margin_start (icon, 4);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_center_box_set_start_widget (GTK_CENTER_BOX (centre), icon);

  self->title_label = gtk_label_new ("Word42");
  gtk_widget_add_css_class (self->title_label, "w42-titlebar-label");
  gtk_label_set_ellipsize (GTK_LABEL (self->title_label), PANGO_ELLIPSIZE_END);
  gtk_center_box_set_center_widget (GTK_CENTER_BOX (centre), self->title_label);

  gtk_box_append (GTK_BOX (right),
                  caption_button ("minimise", "Minimize",
                                  G_CALLBACK (on_titlebar_minimise), self));
  gtk_box_append (GTK_BOX (right),
                  caption_button ("maximise", "Maximize",
                                  G_CALLBACK (on_titlebar_maximise), self));
  gtk_box_append (GTK_BOX (right),
                  caption_button ("close", "Close",
                                  G_CALLBACK (on_titlebar_close), self));
  gtk_center_box_set_end_widget (GTK_CENTER_BOX (centre), right);

  gtk_window_handle_set_child (GTK_WINDOW_HANDLE (handle), centre);
  return handle;
}

/* ---------------------------------------------------------------------- */
/* Toolbars                                                                */
/* ---------------------------------------------------------------------- */

static GtkWidget *
tool_button (const char *icon, const char *tooltip, const char *action)
{
  GtkWidget *button = gtk_button_new_from_icon_name (icon);

  gtk_widget_set_tooltip_text (button, tooltip);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (button), action);
  gtk_widget_set_focusable (button, FALSE);

  return button;
}

static GtkWidget *
toggle_button (const char *icon, const char *tooltip)
{
  GtkWidget *button = gtk_toggle_button_new ();

  gtk_button_set_child (GTK_BUTTON (button),
                        gtk_image_new_from_icon_name (icon));
  gtk_widget_set_tooltip_text (button, tooltip);
  gtk_widget_set_focusable (button, FALSE);

  return button;
}

static GtkWidget *
tool_separator (void)
{
  GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);

  gtk_widget_set_margin_start (sep, 4);
  gtk_widget_set_margin_end (sep, 4);
  gtk_widget_set_margin_top (sep, 3);
  gtk_widget_set_margin_bottom (sep, 3);

  return sep;
}

static void
on_zoom_selected (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
  W42Window *self = data;
  guint index;

  (void) pspec;

  if (self->updating)
    return;

  index = gtk_drop_down_get_selected (drop);
  if (index == GTK_INVALID_LIST_POSITION || index >= G_N_ELEMENTS (ZOOM_STEPS))
    return;

  w42_view_set_zoom (self->view, ZOOM_STEPS[index]);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static GtkWidget *
build_standard_bar (void)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (bar, "w42-toolbar");

  gtk_box_append (GTK_BOX (bar), tool_button ("w42-new",           "New",           "win.new"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-open",          "Open",          "win.open"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-save",          "Save",          "win.save"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-print",         "Print",         "win.print"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-print-preview", "Print Preview", "win.print-preview"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-spelling",      "Spelling",      "win.spelling"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-cut",           "Cut",           "win.cut"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-copy",          "Copy",          "win.copy"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-paste",         "Paste",         "win.paste"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-undo",          "Undo",          "win.undo"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-redo",          "Redo",          "win.redo"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-find",          "Find",          "win.find"));

  return bar;
}

/* Word 6 kept the zoom control at the right-hand end of the Standard bar. */
static GtkWidget *
build_zoom_drop (W42Window *self)
{
  GtkStringList *steps = gtk_string_list_new (NULL);

  for (guint i = 0; i < G_N_ELEMENTS (ZOOM_LABELS); i++)
    gtk_string_list_append (steps, ZOOM_LABELS[i]);

  self->zoom_drop = gtk_drop_down_new (G_LIST_MODEL (steps), NULL);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (self->zoom_drop), 1);
  gtk_widget_set_size_request (self->zoom_drop, 72, -1);
  gtk_widget_set_tooltip_text (self->zoom_drop, "Zoom Control");
  g_signal_connect (self->zoom_drop, "notify::selected",
                    G_CALLBACK (on_zoom_selected), self);

  return self->zoom_drop;
}

static void
on_style_selected (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
  W42Window *self = data;
  GtkStringObject *item;

  (void) pspec;

  if (self->updating)
    return;

  item = gtk_drop_down_get_selected_item (drop);
  if (item == NULL)
    return;

  w42_view_apply_style (self->view, gtk_string_object_get_string (item));
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

/* Keeps the Style box's list in step with the stylesheet, which a loaded
 * file can have added to. */
static void
window_sync_style_list (W42Window *self)
{
  W42StyleSheet *styles = w42_pt_stylesheet (w42_document_pt (self->doc));
  guint n = w42_stylesheet_size (styles);
  guint have = g_list_model_get_n_items (G_LIST_MODEL (self->style_list));
  gboolean same = (n == have);

  for (guint i = 0; same && i < n; i++)
    {
      GtkStringObject *item =
        g_list_model_get_item (G_LIST_MODEL (self->style_list), i);
      same = g_strcmp0 (gtk_string_object_get_string (item),
                        w42_stylesheet_get (styles, i)->name) == 0;
      g_object_unref (item);
    }

  if (same)
    return;

  gtk_string_list_splice (self->style_list, 0, have, NULL);
  for (guint i = 0; i < n; i++)
    gtk_string_list_append (self->style_list, w42_stylesheet_get (styles, i)->name);
}

static void
on_font_selected (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
  W42Window *self = data;
  GtkStringObject *item;

  (void) pspec;

  if (self->updating)
    return;

  item = gtk_drop_down_get_selected_item (drop);
  if (item == NULL)
    return;

  w42_view_set_font_family (self->view, gtk_string_object_get_string (item));
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
on_size_selected (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
  W42Window *self = data;
  guint index;

  (void) pspec;

  if (self->updating)
    return;

  index = gtk_drop_down_get_selected (drop);
  if (index == GTK_INVALID_LIST_POSITION || index >= G_N_ELEMENTS (FONT_SIZES))
    return;

  w42_view_set_font_size (self->view, FONT_SIZES[index] * 2);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
on_style_toggled (GtkToggleButton *button, gpointer data)
{
  W42Window *self = data;

  if (self->updating)
    return;

  if (GTK_WIDGET (button) == self->bold_btn)
    w42_view_toggle_bold (self->view);
  else if (GTK_WIDGET (button) == self->italic_btn)
    w42_view_toggle_italic (self->view);
  else
    w42_view_toggle_underline (self->view);

  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
on_align_toggled (GtkToggleButton *button, gpointer data)
{
  W42Window *self = data;

  if (self->updating || !gtk_toggle_button_get_active (button))
    return;

  for (int i = 0; i < 4; i++)
    {
      if (GTK_WIDGET (button) == self->align_btn[i])
        {
          w42_view_set_align (self->view, (W42Align) i);
          break;
        }
    }

  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
on_list_toggled (GtkToggleButton *button, gpointer data)
{
  W42Window *self = data;
  W42ListKind kind = GTK_WIDGET (button) == self->bullets_btn
                       ? W42_LIST_BULLET : W42_LIST_NUMBER;

  if (self->updating)
    return;

  w42_view_set_list (self->view,
                     gtk_toggle_button_get_active (button) ? kind
                                                           : W42_LIST_NONE);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static GListModel *
list_font_families (void)
{
  PangoFontMap *map = pango_cairo_font_map_get_default ();
  PangoFontFamily **families = NULL;
  int n = 0;
  GtkStringList *list = gtk_string_list_new (NULL);
  GPtrArray *names = g_ptr_array_new ();

  pango_font_map_list_families (map, &families, &n);

  for (int i = 0; i < n; i++)
    g_ptr_array_add (names, (gpointer) pango_font_family_get_name (families[i]));

  g_ptr_array_sort_values (names, (GCompareFunc) g_ascii_strcasecmp);

  for (guint i = 0; i < names->len; i++)
    gtk_string_list_append (list, g_ptr_array_index (names, i));

  g_ptr_array_free (names, TRUE);
  g_free (families);

  return G_LIST_MODEL (list);
}

static GtkWidget *
build_format_bar (W42Window *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkStringList *sizes = gtk_string_list_new (NULL);
  static const char *align_icons[4] = {
    "w42-align-left", "w42-align-center",
    "w42-align-right", "w42-align-justify"
  };
  static const char *align_names[4] = {
    "Align Left", "Center", "Align Right", "Justify"
  };

  gtk_widget_add_css_class (bar, "w42-toolbar");

  {
    /* The Style box, at the left end as Word 6 had it. */
    self->style_list = gtk_string_list_new (NULL);
    self->style_drop = gtk_drop_down_new (g_object_ref (G_LIST_MODEL (self->style_list)), NULL);
    gtk_widget_set_size_request (self->style_drop, 120, -1);
    gtk_widget_set_tooltip_text (self->style_drop, "Style");
    g_signal_connect (self->style_drop, "notify::selected",
                      G_CALLBACK (on_style_selected), self);
    gtk_box_append (GTK_BOX (bar), self->style_drop);
  }

  self->families = list_font_families ();

  self->family_index = g_hash_table_new (g_str_hash, g_str_equal);
  {
    guint n = g_list_model_get_n_items (self->families);

    for (guint i = 0; i < n; i++)
      {
        GtkStringObject *item = g_list_model_get_item (self->families, i);
        /* Interned, so the key matches the interned name a W42CharFmt holds
         * and stays valid for as long as the process does. */
        g_hash_table_insert (self->family_index,
                             (gpointer) g_intern_string (
                               gtk_string_object_get_string (item)),
                             GUINT_TO_POINTER (i + 1));
        g_object_unref (item);
      }
  }

  self->font_drop = gtk_drop_down_new (g_object_ref (self->families), NULL);
  gtk_drop_down_set_enable_search (GTK_DROP_DOWN (self->font_drop), TRUE);
  gtk_widget_set_size_request (self->font_drop, 180, -1);
  gtk_widget_set_tooltip_text (self->font_drop, "Font");
  g_signal_connect (self->font_drop, "notify::selected-item",
                    G_CALLBACK (on_font_selected), self);
  gtk_box_append (GTK_BOX (bar), self->font_drop);

  for (guint i = 0; i < G_N_ELEMENTS (FONT_SIZES); i++)
    {
      char label[8];
      g_snprintf (label, sizeof label, "%d", FONT_SIZES[i]);
      gtk_string_list_append (sizes, label);
    }

  self->size_drop = gtk_drop_down_new (G_LIST_MODEL (sizes), NULL);
  gtk_widget_set_size_request (self->size_drop, 70, -1);
  gtk_widget_set_tooltip_text (self->size_drop, "Font Size");
  g_signal_connect (self->size_drop, "notify::selected",
                    G_CALLBACK (on_size_selected), self);
  gtk_box_append (GTK_BOX (bar), self->size_drop);

  gtk_box_append (GTK_BOX (bar), tool_separator ());

  self->bold_btn      = toggle_button ("w42-bold", "Bold");
  self->italic_btn    = toggle_button ("w42-italic", "Italic");
  self->underline_btn = toggle_button ("w42-underline", "Underline");

  g_signal_connect (self->bold_btn, "toggled", G_CALLBACK (on_style_toggled), self);
  g_signal_connect (self->italic_btn, "toggled", G_CALLBACK (on_style_toggled), self);
  g_signal_connect (self->underline_btn, "toggled", G_CALLBACK (on_style_toggled), self);

  gtk_box_append (GTK_BOX (bar), self->bold_btn);
  gtk_box_append (GTK_BOX (bar), self->italic_btn);
  gtk_box_append (GTK_BOX (bar), self->underline_btn);

  gtk_box_append (GTK_BOX (bar), tool_separator ());

  for (int i = 0; i < 4; i++)
    {
      self->align_btn[i] = toggle_button (align_icons[i], align_names[i]);

      if (i > 0)
        gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (self->align_btn[i]),
                                     GTK_TOGGLE_BUTTON (self->align_btn[0]));

      g_signal_connect (self->align_btn[i], "toggled",
                        G_CALLBACK (on_align_toggled), self);
      gtk_box_append (GTK_BOX (bar), self->align_btn[i]);
    }

  gtk_box_append (GTK_BOX (bar), tool_separator ());

  self->numbers_btn = toggle_button ("w42-numbering", "Numbering");
  self->bullets_btn = toggle_button ("w42-bullets", "Bullets");
  g_signal_connect (self->numbers_btn, "toggled", G_CALLBACK (on_list_toggled), self);
  g_signal_connect (self->bullets_btn, "toggled", G_CALLBACK (on_list_toggled), self);
  gtk_box_append (GTK_BOX (bar), self->numbers_btn);
  gtk_box_append (GTK_BOX (bar), self->bullets_btn);

  return bar;
}

static GtkWidget *
build_status_bar (W42Window *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);

  gtk_widget_add_css_class (bar, "w42-statusbar");
  gtk_widget_set_margin_start (bar, 6);
  gtk_widget_set_margin_end (bar, 6);
  gtk_widget_set_margin_top (bar, 2);
  gtk_widget_set_margin_bottom (bar, 2);

  self->status_page = gtk_label_new ("Page 1");
  self->status_at   = gtk_label_new ("At 1.0\"");
  self->status_ln   = gtk_label_new ("Ln 1");
  self->status_col  = gtk_label_new ("Col 1");
  self->status_mod  = gtk_label_new ("");

  gtk_widget_set_hexpand (self->status_mod, TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->status_mod), 0.0);

  /* Each reading sits in its own sunken well, as Word 6's readings did. */
  {
    GtkWidget *cells[] = { self->status_page, self->status_at,
                           self->status_ln, self->status_col,
                           self->status_mod };

    for (guint i = 0; i < G_N_ELEMENTS (cells); i++)
      {
        gtk_widget_add_css_class (cells[i], "w42-status-cell");
        gtk_label_set_xalign (GTK_LABEL (cells[i]), 0.0);
        gtk_widget_set_size_request (cells[i],
                                     cells[i] == self->status_mod ? -1 : 66,
                                     -1);
        gtk_box_append (GTK_BOX (bar), cells[i]);
      }
  }

  return bar;
}

/* ---------------------------------------------------------------------- */
/* Keeping the chrome in step with the document                            */
/* The Window menu lists the open documents, numbered as Word 6 did. */
static int
window_by_serial (gconstpointer a, gconstpointer b)
{
  const W42Window *wa = a, *wb = b;

  return wa->serial < wb->serial ? -1 : wa->serial > wb->serial ? 1 : 0;
}

static void
window_refresh_window_list (W42Window *self)
{
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  GList *windows = app != NULL ? gtk_application_get_windows (app) : NULL;
  GMenu *list = self->window_list;
  GList *ours = NULL;
  GString *state;
  int index = 0;

  if (list == NULL)
    return;

  for (GList *l = windows; l != NULL; l = l->next)
    if (W42_IS_WINDOW (l->data))
      ours = g_list_prepend (ours, l->data);
  ours = g_list_sort (ours, window_by_serial);

  /* This runs whenever the document changes -- on every keystroke -- so
   * rebuild the menu only when it would come out different. */
  state = g_string_new (NULL);
  for (GList *l = ours; l != NULL; l = l->next)
    {
      W42Window *w = l->data;
      char *name = w42_document_get_title (w->doc);

      g_string_append_printf (state, "%u:%s%s\n", w->serial, name,
                              w42_document_get_modified (w->doc) ? "*" : "");
      g_free (name);
    }
  if (g_strcmp0 (state->str, self->window_list_state) == 0)
    {
      g_string_free (state, TRUE);
      g_list_free (ours);
      return;
    }
  g_free (self->window_list_state);
  self->window_list_state = g_string_free (state, FALSE);

  g_menu_remove_all (list);
  for (GList *l = ours; l != NULL; l = l->next)
    {
      W42Window *w = l->data;
      char *name = w42_document_get_title (w->doc);
      char *label = g_strdup_printf ("_%d %s%s", index + 1, name,
                                     w42_document_get_modified (w->doc) ? "*" : "");
      GMenuItem *item = g_menu_item_new (label, NULL);

      /* The target is the window's own number, not its place in the
       * list: opening the menu raises this window and reorders the
       * application's list under it. */
      g_menu_item_set_action_and_target_value (item, "win.window-go",
                                               g_variant_new_int32 ((int) w->serial));
      g_menu_append_item (list, item);
      g_object_unref (item);
      g_free (label);
      g_free (name);
      index++;
    }
  g_list_free (ours);
}

/* ---- Window, Edit and File commands ----------------------------------- */

/* Edit > Paste Special: for now the one thing worth choosing -- the
 * clipboard's text with none of its formatting. */
static void
action_paste_text (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_view_paste_text (self->view);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_clear (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_view_clear (self->view);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

/* File > Save All: every window whose document has changes. */
static void
action_summary (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_summary_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_save_all (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  GList *windows = app != NULL ? gtk_application_get_windows (app) : NULL;
  int saved = 0, asked = 0;

  (void) action; (void) param;
  for (GList *l = windows; l != NULL; l = l->next)
    {
      W42Window *w = W42_IS_WINDOW (l->data) ? W42_WINDOW (l->data) : NULL;
      GFile *file;

      if (w == NULL || !w42_document_get_modified (w->doc))
        continue;
      file = w42_document_get_file (w->doc);
      if (file == NULL ||
          w42_io_guess_format (file) == W42_FORMAT_DOC ||
          w42_io_guess_format (file) == W42_FORMAT_PDF ||
          w42_io_guess_format (file) == W42_FORMAT_HTML)
        {
          /* Never saved, or read from a format Save turns into Save As
           * for: either way it needs a name of its own. */
          asked++;
          continue;
        }
      {
        GError *error = NULL;

        if (w42_document_save (w->doc, file, &error))
          {
            window_saved (w, TRUE);
            saved++;
          }
        else
          {
            show_error (w, "Word42 could not save that file.", error);
            g_clear_error (&error);
          }
      }
    }
  if (asked > 0)
    show_message (self, saved > 0 ? "The rest are saved." : "Nothing was saved.",
                  "A document that has never been saved, or that came from a "
                  "format Word42 does not write back, needs a name: use "
                  "File > Save As for it.");
}

/* Insert > File: another document's text, at the caret.  Tables and notes
 * of the file that comes in are left behind, as the clipboard leaves
 * them. */
static void
on_insert_file_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      W42PieceTable *other = w42_pt_new ();
      W42PageSetup page = { 0 };

      if (w42_io_load (other, &page, file, &error))
        {
          W42PieceTable *frag = w42_pt_extract (other, 0, w42_pt_length (other));

          if (frag != NULL)
            {
              w42_view_insert_fragment (self->view, frag);
              w42_pt_free (frag);
            }
        }
      else
        show_error (self, "Word42 could not read that file.", error);
      w42_pt_free (other);
      g_object_unref (file);
    }
  g_clear_error (&error);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_insert_file (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListModel *filters = file_filters ();

  (void) action; (void) param;
  gtk_file_dialog_set_title (dialog, "Insert File");
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_insert_file_response, self);
  g_object_unref (filters);
  g_object_unref (dialog);
}

/* View > Full Screen gives the page the whole screen: the menu bar, the
 * two toolbars, the ruler and the status bar go away, and come back as
 * they were.  Escape brings them back, as it did in the classics. */
static void
window_set_full_screen (W42Window *self, gboolean on)
{
  GtkWidget *chrome[4];
  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), "full-screen");

  chrome[0] = self->menubar;
  chrome[1] = self->standard_bar;
  chrome[2] = self->format_bar;
  chrome[3] = self->ruler;

  for (guint i = 0; i < G_N_ELEMENTS (chrome); i++)
    {
      if (chrome[i] == NULL)
        continue;
      if (on)
        {
          self->full_screen_chrome[i] = gtk_widget_get_visible (chrome[i]);
          gtk_widget_set_visible (chrome[i], FALSE);
        }
      else
        gtk_widget_set_visible (chrome[i], self->full_screen_chrome[i]);
    }
  if (self->status_bar != NULL)
    gtk_widget_set_visible (self->status_bar, !on);

  if (action != NULL)
    g_simple_action_set_state (G_SIMPLE_ACTION (action), g_variant_new_boolean (on));

  if (on)
    gtk_window_fullscreen (GTK_WINDOW (self));
  else
    gtk_window_unfullscreen (GTK_WINDOW (self));
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

/* Escape leaves Full Screen, and does nothing else: any other key is the
 * document's. */
static gboolean
window_escape (GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, gpointer data)
{
  W42Window *self = data;
  GAction *action;
  GVariant *value;
  gboolean on;

  (void) controller; (void) keycode; (void) state;
  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;

  action = g_action_map_lookup_action (G_ACTION_MAP (self), "full-screen");
  if (action == NULL)
    return GDK_EVENT_PROPAGATE;
  value = g_action_get_state (action);
  on = g_variant_get_boolean (value);
  g_variant_unref (value);
  if (!on)
    return GDK_EVENT_PROPAGATE;

  window_set_full_screen (self, FALSE);
  return GDK_EVENT_STOP;
}

static void
action_full_screen (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GVariant *state = g_action_get_state (G_ACTION (action));
  gboolean on = !g_variant_get_boolean (state);

  (void) param;
  g_variant_unref (state);
  window_set_full_screen (self, on);
}

/* View > Slide Show: the document's outline, one heading at a time, on
 * the whole screen.  A talk is a document read aloud. */
static void
action_slide_show (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_slideshow_show (GTK_WINDOW (self), self->view);
}

/* Window > Arrange All: the windows side by side, as far as the system
 * lets a program place them. */
static void
action_arrange_all (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  GList *windows = app != NULL ? gtk_application_get_windows (app) : NULL;
  GList *ours = NULL;
  int n = 0;

  (void) action; (void) param;
  for (GList *l = windows; l != NULL; l = l->next)
    if (W42_IS_WINDOW (l->data))
      {
        ours = g_list_append (ours, l->data);
        n++;
      }
  if (n == 0)
    return;

#ifdef G_OS_WIN32
  {
    /* Side by side across the work area. */
    RECT work;
    int index = 0;

    if (!SystemParametersInfoW (SPI_GETWORKAREA, 0, &work, 0))
      {
        work.left = work.top = 0;
        work.right = 1280;
        work.bottom = 800;
      }
    for (GList *l = ours; l != NULL; l = l->next, index++)
      {
        GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (l->data));
        HWND hwnd = surface != NULL && GDK_IS_WIN32_SURFACE (surface)
                    ? gdk_win32_surface_get_handle (surface) : NULL;
        int width = (work.right - work.left) / n;

        if (hwnd == NULL)
          continue;
        gtk_window_unmaximize (GTK_WINDOW (l->data));
        ShowWindow (hwnd, SW_RESTORE);
        SetWindowPos (hwnd, HWND_TOP, work.left + index * width, work.top,
                      width, work.bottom - work.top, SWP_NOACTIVATE);
      }
  }
#else
  /* Elsewhere the window manager places windows, not the program: the
   * best it can do is show them all, unmaximised. */
  for (GList *l = ours; l != NULL; l = l->next)
    {
      gtk_window_unmaximize (GTK_WINDOW (l->data));
      gtk_window_present (GTK_WINDOW (l->data));
    }
#endif
  gtk_window_present (GTK_WINDOW (self));
  g_list_free (ours);
}

/* Window > 1, 2, 3...: the open documents. */
static void
action_window_go (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  GList *windows = app != NULL ? gtk_application_get_windows (app) : NULL;
  int want = param != NULL ? g_variant_get_int32 (param) : 0;
  int index = 0;

  (void) action;
  (void) index;
  for (GList *l = windows; l != NULL; l = l->next)
    if (W42_IS_WINDOW (l->data) && (int) W42_WINDOW (l->data)->serial == want)
      {
        gtk_window_present (GTK_WINDOW (l->data));
        return;
      }
}

/* ---------------------------------------------------------------------- */

static void
window_sync_state (W42Window *self)
{
  /* The title carries the modified mark, and another window on the same
   * document may have been the one to make the change. */
  window_update_title (self);
  if (w42_document_get_modified (self->doc))
    self->autosave_dirty = TRUE;

  W42PieceTable *pt = w42_document_pt (self->doc);
  W42Layout *layout = w42_view_get_layout (self->view);
  W42CharFmt fmt;
  W42Align align;
  int page = 1, line = 1, column = 1;
  char buffer[64];
  gboolean has_sel;

  self->updating = TRUE;

  window_sync_style_list (self);
  {
    const char *style = w42_view_get_style (self->view);
    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->style_list));

    for (guint i = 0; i < n; i++)
      {
        GtkStringObject *item =
          g_list_model_get_item (G_LIST_MODEL (self->style_list), i);
        gboolean hit = g_ascii_strcasecmp (gtk_string_object_get_string (item),
                                           style) == 0;
        g_object_unref (item);

        if (hit)
          {
            if (gtk_drop_down_get_selected (GTK_DROP_DOWN (self->style_drop)) != i)
              gtk_drop_down_set_selected (GTK_DROP_DOWN (self->style_drop), i);
            break;
          }
      }
  }

  {
    GAction *act = g_action_map_lookup_action (G_ACTION_MAP (self),
                                               "heading-numbering");
    W42StyleSheet *styles = w42_pt_stylesheet (w42_document_pt (self->doc));

    if (act != NULL)
      g_simple_action_set_state (G_SIMPLE_ACTION (act),
        g_variant_new_boolean (w42_stylesheet_get_number_headings (styles)));
  }

  w42_view_get_char_fmt (self->view, &fmt);
  align = w42_view_get_align (self->view);

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->bold_btn), fmt.bold);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->italic_btn), fmt.italic);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->underline_btn),
                                fmt.underline);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->align_btn[align]), TRUE);

  {
    W42ListKind list = w42_view_get_list (self->view);
    GAction *bullets = g_action_map_lookup_action (G_ACTION_MAP (self), "list-bullets");
    GAction *numbers = g_action_map_lookup_action (G_ACTION_MAP (self), "list-numbers");

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->bullets_btn),
                                  w42_list_is_bullet (list));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->numbers_btn),
                                  w42_list_is_numbered (list));
    if (bullets != NULL)
      g_simple_action_set_state (G_SIMPLE_ACTION (bullets),
                                 g_variant_new_boolean (w42_list_is_bullet (list)));
    if (numbers != NULL)
      g_simple_action_set_state (G_SIMPLE_ACTION (numbers),
                                 g_variant_new_boolean (w42_list_is_numbered (list)));
  }

  for (guint i = 0; i < G_N_ELEMENTS (FONT_SIZES); i++)
    {
      if (FONT_SIZES[i] * 2 == fmt.size)
        {
          if (gtk_drop_down_get_selected (GTK_DROP_DOWN (self->size_drop)) != i)
            gtk_drop_down_set_selected (GTK_DROP_DOWN (self->size_drop), i);
          break;
        }
    }

  /* Looked up rather than searched.  This runs on every keystroke, and
   * walking several hundred font names each time was slow enough that fast
   * typing outran the input queue. */
  if (fmt.family != NULL && self->family_index != NULL)
    {
      gpointer found = g_hash_table_lookup (self->family_index, fmt.family);

      if (found != NULL)
        gtk_drop_down_set_selected (GTK_DROP_DOWN (self->font_drop),
                                    GPOINTER_TO_UINT (found) - 1);
    }

  w42_layout_describe_pos (layout, w42_view_get_caret (self->view),
                           &page, &line, &column);

  g_snprintf (buffer, sizeof buffer, "Page %d", page);
  gtk_label_set_text (GTK_LABEL (self->status_page), buffer);
  g_snprintf (buffer, sizeof buffer, "Ln %d", line);
  gtk_label_set_text (GTK_LABEL (self->status_ln), buffer);
  g_snprintf (buffer, sizeof buffer, "Col %d", column);
  gtk_label_set_text (GTK_LABEL (self->status_col), buffer);

  {
    int cpage = 0;
    double x = 0, y = 0, h = 0;

    if (w42_layout_pos_to_caret (layout, w42_view_get_caret (self->view),
                                 &cpage, &x, &y, &h))
      {
        /* The distance from the top of the page, in the user's unit. */
        g_snprintf (buffer, sizeof buffer, "At %.1f%s",
                    w42_settings_from_twips ((int) (y * W42_TWIPS_PER_PX)),
                    w42_settings_unit_name ());
        gtk_label_set_text (GTK_LABEL (self->status_at), buffer);
      }
  }

  gtk_label_set_text (GTK_LABEL (self->status_mod),
                      self->status_flash != NULL ? self->status_flash :
                      w42_document_get_modified (self->doc) ? "Modified" : "");

  has_sel = w42_view_has_selection (self->view);

  window_refresh_window_list (self);

  {
    gboolean in_table = w42_view_in_table (self->view);
    static const char *table_actions[] = { "table-insert-rows", "table-delete-rows",
                                           "table-insert-cols", "table-delete-cols", "table-split",
                                           "table-merge", "table-properties", "table-autoformat",
                                           "table-select", "table-split-table", "table-sort" };

    for (guint i = 0; i < G_N_ELEMENTS (table_actions); i++)
      {
        GAction *a = g_action_map_lookup_action (G_ACTION_MAP (self), table_actions[i]);
        if (a != NULL)
          g_simple_action_set_enabled (G_SIMPLE_ACTION (a), in_table);
      }
    {
      GAction *a = g_action_map_lookup_action (G_ACTION_MAP (self), "table-insert");
      if (a != NULL)
        g_simple_action_set_enabled (G_SIMPLE_ACTION (a), !in_table);
    }
  }

  {
    static const char *sel_actions[] = { "cut", "copy", "clear" };

    for (guint i = 0; i < G_N_ELEMENTS (sel_actions); i++)
      {
        GAction *a = g_action_map_lookup_action (G_ACTION_MAP (self),
                                                 sel_actions[i]);
        if (a != NULL)
          g_simple_action_set_enabled (G_SIMPLE_ACTION (a), has_sel);
      }
  }

  {
    GAction *a = g_action_map_lookup_action (G_ACTION_MAP (self), "undo");
    if (a != NULL)
      g_simple_action_set_enabled (G_SIMPLE_ACTION (a), w42_pt_can_undo (pt));

    a = g_action_map_lookup_action (G_ACTION_MAP (self), "redo");
    if (a != NULL)
      g_simple_action_set_enabled (G_SIMPLE_ACTION (a), w42_pt_can_redo (pt));
  }

  window_update_title (self);

  self->updating = FALSE;
}

static void
on_view_state_changed (W42View *view, gpointer data)
{
  (void) view;
  window_sync_state (W42_WINDOW (data));
}

/* The caret goes into the document as soon as the canvas is on screen.  It
 * has to be the canvas that is mapped, not the window: focusing it sends the
 * input method looking for the widget's surface, and on Windows GtkIMContext
 * complains if it is asked before the widget has one. */
static void
on_view_mapped (GtkWidget *widget, gpointer data)
{
  (void) data;
  gtk_widget_grab_focus (widget);
}

/* ---------------------------------------------------------------------- */
/* Actions table                                                           */
/* ---------------------------------------------------------------------- */

static const GActionEntry WINDOW_ACTIONS[] = {
  { "new",        action_new,        NULL, NULL,    NULL, { 0 } },
  { "open",       action_open,       NULL, NULL,    NULL, { 0 } },
  { "save",       action_save,       NULL, NULL,    NULL, { 0 } },
  { "save-as",    action_save_as,    NULL, NULL,    NULL, { 0 } },
  { "close",      action_close,      NULL, NULL,    NULL, { 0 } },
  { "undo",       action_undo,       NULL, NULL,    NULL, { 0 } },
  { "redo",       action_redo,       NULL, NULL,    NULL, { 0 } },
  { "cut",        action_cut,        NULL, NULL,    NULL, { 0 } },
  { "copy",       action_copy,       NULL, NULL,    NULL, { 0 } },
  { "paste",      action_paste,      NULL, NULL,    NULL, { 0 } },
  { "select-all", action_select_all, NULL, NULL,    NULL, { 0 } },
  { "bold",       action_bold,       NULL, NULL,    NULL, { 0 } },
  { "italic",     action_italic,     NULL, NULL,    NULL, { 0 } },
  { "underline",  action_underline,  NULL, NULL,    NULL, { 0 } },
  { "align",      action_align,      "s",  NULL,    NULL, { 0 } },
  { "zoom",       action_zoom,       "d",  NULL,    NULL, { 0 } },
  { "view-mode",  action_view_mode,  "s",  "'normal'", NULL, { 0 } },
  { "font",       action_font_dialog, NULL, NULL,   NULL, { 0 } },
  { "font-grow",   action_font_step, NULL, NULL, NULL, { 0 } },
  { "font-shrink", action_font_step, NULL, NULL, NULL, { 0 } },
  { "find",       action_find,       NULL, NULL,    NULL, { 0 } },
  { "replace",    action_replace,    NULL, NULL,    NULL, { 0 } },
  { "find-next",  action_find_next,  NULL, NULL,    NULL, { 0 } },
  { "print",         action_print,         NULL, NULL, NULL, { 0 } },
  { "export-pdf",    action_export_pdf,    NULL, NULL, NULL, { 0 } },
  { "insert-picture", action_insert_picture, NULL, NULL, NULL, { 0 } },
  { "print-preview", action_print_preview, NULL, NULL, NULL, { 0 } },
  { "page-setup", action_page_setup, NULL, NULL,    NULL, { 0 } },
  { "table-insert",      action_table_insert,     NULL, NULL, NULL, { 0 } },
  { "table-insert-rows", action_table_insert_row, NULL, NULL, NULL, { 0 } },
  { "table-delete-rows", action_table_delete_row, NULL, NULL, NULL, { 0 } },
  { "table-insert-cols", action_table_insert_column, NULL, NULL, NULL, { 0 } },
  { "table-delete-cols", action_table_delete_column, NULL, NULL, NULL, { 0 } },
  { "table-properties", action_table_properties, NULL, NULL, NULL, { 0 } },
  { "table-autoformat", action_table_autoformat, NULL, NULL, NULL, { 0 } },
  { "format-picture", action_format_picture, NULL, NULL, NULL, { 0 } },
  { "drop-cap", action_drop_cap, NULL, NULL, NULL, { 0 } },
  { "frame", action_frame, NULL, NULL, NULL, { 0 } },
  { "header-footer", action_header_footer, NULL, NULL, NULL, { 0 } },
  { "insert-page-numbers", action_page_numbers, NULL, NULL, NULL, { 0 } },
  { "insert-break", action_insert_break, NULL, NULL, NULL, { 0 } },
  { "apply-style", action_apply_style, "s", NULL,   NULL, { 0 } },
  { "style",      action_style_dialog, NULL, NULL,  NULL, { 0 } },
  { "heading-numbering", action_heading_numbering, NULL, "false", NULL, { 0 } },
  { "list-bullets", action_list, NULL, "false", NULL, { 0 } },
  { "spelling",   action_spelling,   NULL, NULL,   NULL, { 0 } },
  { "go-to",         action_go_to,         NULL, NULL, NULL, { 0 } },
  { "new-window",    action_new_window,    NULL, NULL, NULL, { 0 } },
  { "options",       action_options,       NULL, NULL, NULL, { 0 } },
  { "help-contents", action_help_contents, NULL, NULL, NULL, { 0 } },
  { "tabs",          action_tabs,          NULL, NULL, NULL, { 0 } },
  { "borders",       action_borders,       NULL, NULL, NULL, { 0 } },
  { "insert-footnote", action_insert_footnote, NULL, NULL, NULL, { 0 } },
  { "hyperlink",     action_hyperlink,     NULL, NULL, NULL, { 0 } },
  { "effects",       action_effects,       NULL, NULL, NULL, { 0 } },
  { "columns",       action_columns,       NULL, NULL, NULL, { 0 } },
  { "insert-toc",    action_insert_toc,    NULL, NULL, NULL, { 0 } },
  { "open-recent",   action_open_recent,   "s",  NULL, NULL, { 0 } },
  { "export-html",   action_export_html,   NULL, NULL, NULL, { 0 } },
  { "bookmark",      action_bookmark,      NULL, NULL, NULL, { 0 } },
  { "annotation",    action_annotation,    NULL, NULL, NULL, { 0 } },
  { "mail-merge",    action_mail_merge,    NULL, NULL, NULL, { 0 } },
  { "update-toc",    action_update_toc,    NULL, NULL, NULL, { 0 } },
  { "cross-reference", action_cross_reference, NULL, NULL, NULL, { 0 } },
  { "section-break", action_section_break, NULL, NULL, NULL, { 0 } },
  { "drawing",       action_drawing,       NULL, NULL, NULL, { 0 } },
  { "field",         action_field,         NULL, NULL, NULL, { 0 } },
  { "update-fields", action_update_fields, NULL, NULL, NULL, { 0 } },
  { "bullets-numbering", action_bullets_numbering, NULL, NULL, NULL, { 0 } },
  { "hyphenate",     action_hyphenate,     NULL, NULL, NULL, { 0 } },
  { "unhyphenate",   action_unhyphenate,   NULL, NULL, NULL, { 0 } },
  { "change-case",   action_change_case,   "s",  NULL, NULL, { 0 } },
  { "caption",       action_caption,       NULL, NULL, NULL, { 0 } },
  { "go-to-note",    action_go_to_note,    NULL, NULL, NULL, { 0 } },
  { "insert-endnote", action_insert_endnote, NULL, NULL, NULL, { 0 } },
  { "table-merge",   action_table_merge,   NULL, NULL, NULL, { 0 } },
  { "table-split",   action_table_split,   NULL, NULL, NULL, { 0 } },
  { "table-select",  action_table_select,  "s",  NULL, NULL, { 0 } },
  { "paste-text",    action_paste_text,    NULL, NULL, NULL, { 0 } },
  { "clear",         action_clear,         NULL, NULL, NULL, { 0 } },
  { "save-all",      action_save_all,      NULL, NULL, NULL, { 0 } },
  { "summary",       action_summary,       NULL, NULL, NULL, { 0 } },
  { "insert-file",   action_insert_file,   NULL, NULL, NULL, { 0 } },
  { "full-screen",   action_full_screen,   NULL, "false", NULL, { 0 } },
  { "slide-show",    action_slide_show,    NULL, NULL, NULL, { 0 } },
  { "autocorrect",   action_autocorrect,   NULL, NULL, NULL, { 0 } },
  { "export-pptx",   action_export_pptx,   NULL, NULL, NULL, { 0 } },
  { "arrange-all",   action_arrange_all,   NULL, NULL, NULL, { 0 } },
  { "window-go",     action_window_go,     "i",  NULL, NULL, { 0 } },
  { "table-split-table", action_table_split_table, NULL, NULL, NULL, { 0 } },
  { "table-sort",    action_table_sort,    "s",  NULL, NULL, { 0 } },
  { "table-convert", action_table_convert, NULL, NULL, NULL, { 0 } },
  { "gridlines",     action_gridlines,     NULL, "false", NULL, { 0 } },
  { "insert-date",   action_insert_date,   NULL, NULL, NULL, { 0 } },
  { "insert-symbol", action_insert_symbol, NULL, NULL, NULL, { 0 } },
  { "auto-spell", action_auto_spell, NULL, "true", NULL, { 0 } },
  { "track-changes", action_track_changes, NULL, "false", NULL, { 0 } },
  { "accept-revisions", action_accept_revisions, NULL, NULL, NULL, { 0 } },
  { "reject-revisions", action_reject_revisions, NULL, NULL, NULL, { 0 } },
  { "list-numbers", action_list, NULL, "false", NULL, { 0 } },
  { "paragraph",  action_paragraph,  NULL, NULL,    NULL, { 0 } },
  { "word-count", action_word_count, NULL, NULL,    NULL, { 0 } },
  { "about",      action_about,      NULL, NULL,    NULL, { 0 } },
  { "ruler",       action_toggle_ruler,   NULL, "true", NULL, { 0 } },
  { "show-marks",  action_show_marks,     NULL, "false", NULL, { 0 } },
  { "column-break", action_column_break,  NULL, NULL, NULL, { 0 } },
  { "standard-bar", action_toggle_toolbar, NULL, "true", NULL, { 0 } },
  { "format-bar",   action_toggle_toolbar, NULL, "true", NULL, { 0 } },
};

/* Features the menus name but that are not built yet.  They appear greyed
 * out, which is more honest than hiding them: it says what word42 intends to
 * be without pretending it is there already. */
static const char *PLANNED_ACTIONS[] = {
  NULL   /* nothing waiting on the shelf just now */
};

static void
action_not_implemented (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param; (void) data;
}

/* ---------------------------------------------------------------------- */
/* Construction                                                            */
/* ---------------------------------------------------------------------- */

static void
w42_window_dispose (GObject *object)
{
  W42Window *self = W42_WINDOW (object);

  if (self->status_flash_id != 0)
    {
      g_source_remove (self->status_flash_id);
      self->status_flash_id = 0;
    }
  g_clear_pointer (&self->status_flash, g_free);
  g_clear_pointer (&self->window_list_state, g_free);

  if (self->find_dialog != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (self->find_dialog),
                                    (gpointer *) &self->find_dialog);
      gtk_window_destroy (GTK_WINDOW (self->find_dialog));
      self->find_dialog = NULL;
    }

  if (self->spell_dialog != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (self->spell_dialog),
                                    (gpointer *) &self->spell_dialog);
      gtk_window_destroy (GTK_WINDOW (self->spell_dialog));
      self->spell_dialog = NULL;
    }

  if (self->spell != NULL)
    {
      if (self->view != NULL)
        w42_view_set_spell (self->view, NULL);
      g_clear_pointer (&self->spell, w42_spell_free);
    }

  if (self->autosave_id != 0)
    {
      g_source_remove (self->autosave_id);
      self->autosave_id = 0;
    }
  /* A window that closes in the ordinary way needs no recovering. */
  autosave_remove (self);

  g_clear_object (&self->recent_menu);

  g_clear_object (&self->window_list);
  g_clear_pointer (&self->family_index, g_hash_table_destroy);
  g_clear_object (&self->families);
  g_clear_object (&self->doc);

  G_OBJECT_CLASS (w42_window_parent_class)->dispose (object);
}

static void
w42_window_class_init (W42WindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = w42_window_dispose;
}

static void
w42_window_init (W42Window *self)
{
  GtkWidget *box, *scrolled, *menubar;
  GtkBuilder *builder;
  GMenuModel *model;

  static guint next_serial;

  self->serial = ++next_serial;
  self->doc = w42_document_new ();
  w42_pt_set_author (w42_document_pt (self->doc), window_author_name ());

  g_action_map_add_action_entries (G_ACTION_MAP (self), WINDOW_ACTIONS,
                                   G_N_ELEMENTS (WINDOW_ACTIONS), self);

  for (guint i = 0; i < G_N_ELEMENTS (PLANNED_ACTIONS) && PLANNED_ACTIONS[i] != NULL; i++)
    {
      GSimpleAction *action = g_simple_action_new (PLANNED_ACTIONS[i], NULL);

      g_signal_connect (action, "activate",
                        G_CALLBACK (action_not_implemented), self);
      g_simple_action_set_enabled (action, FALSE);
      g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (action));
      g_object_unref (action);
    }

  gtk_window_set_default_size (GTK_WINDOW (self), 900, 780);
  gtk_widget_add_css_class (GTK_WIDGET (self), "w42");
  gtk_window_set_titlebar (GTK_WINDOW (self), build_titlebar (self));

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child (GTK_WINDOW (self), box);

  builder = gtk_builder_new_from_resource ("/org/word42/word42/menus.ui");
  model = G_MENU_MODEL (gtk_builder_get_object (builder, "menubar"));
  self->recent_menu = G_MENU (g_object_ref (gtk_builder_get_object (builder, "recent-section")));
  {
    GObject *list = gtk_builder_get_object (builder, "window-list");

    self->window_list = G_IS_MENU (list) ? G_MENU (g_object_ref (list)) : NULL;
  }
  window_refresh_recent (self);
  menubar = gtk_popover_menu_bar_new_from_model (model);
  gtk_widget_add_css_class (menubar, "w42-menubar");
  gtk_box_append (GTK_BOX (box), menubar);
  self->menubar = menubar;
  g_object_unref (builder);

  self->standard_bar = build_standard_bar ();
  gtk_box_append (GTK_BOX (self->standard_bar), build_zoom_drop (self));
  gtk_box_append (GTK_BOX (box), self->standard_bar);

  self->view = W42_VIEW (w42_view_new ());

  /* Check spelling as you type, when there is a dictionary to check it
   * against; otherwise the toggle is greyed out rather than lying. */
  w42_view_set_show_marks (self->view, FALSE);

  /* Tools > Options: correct as you type, as it was left last time. */
  w42_view_set_autocorrect (self->view, w42_settings_get_bool ("auto-correct", TRUE));

  {
    /* Table > Table Gridlines, as it was left last time. */
    gboolean on = w42_settings_get_bool ("gridlines", FALSE);
    GAction *gridlines = g_action_map_lookup_action (G_ACTION_MAP (self), "gridlines");

    w42_view_set_gridlines (self->view, on);
    if (gridlines != NULL)
      g_simple_action_set_state (G_SIMPLE_ACTION (gridlines), g_variant_new_boolean (on));
  }

  self->spell = w42_spell_new ();
  if (self->spell != NULL)
    {
      gboolean on = w42_settings_get_bool ("auto-spell", TRUE);
      GAction *auto_spell = g_action_map_lookup_action (G_ACTION_MAP (self),
                                                        "auto-spell");

      if (on)
        w42_view_set_spell (self->view, self->spell);
      if (auto_spell != NULL)
        g_simple_action_set_state (G_SIMPLE_ACTION (auto_spell),
                                   g_variant_new_boolean (on));
    }
  else
    {
      GAction *auto_spell = g_action_map_lookup_action (G_ACTION_MAP (self),
                                                        "auto-spell");
      if (auto_spell != NULL)
        {
          g_simple_action_set_state (G_SIMPLE_ACTION (auto_spell),
                                     g_variant_new_boolean (FALSE));
          g_simple_action_set_enabled (G_SIMPLE_ACTION (auto_spell), FALSE);
        }
    }


  {
    /* Every two minutes, unless the environment says otherwise (which the
     * tests do). */
    const char *env = g_getenv ("W42_AUTOSAVE_SECONDS");
    guint seconds = env != NULL ? (guint) MAX (atoi (env), 1) : 120;

    self->autosave_id = g_timeout_add_seconds (seconds, on_autosave, self);
  }

  self->format_bar = build_format_bar (self);
  gtk_box_append (GTK_BOX (box), self->format_bar);

  self->ruler = w42_ruler_new (self->view);
  window_apply_settings (self);   /* now that the bars it sets exist */
  gtk_box_append (GTK_BOX (box), self->ruler);

  scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scrolled, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled),
                                 GTK_WIDGET (self->view));
  gtk_box_append (GTK_BOX (box), scrolled);

  {
    GtkEventController *keys = gtk_event_controller_key_new ();

    gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
    g_signal_connect (keys, "key-pressed", G_CALLBACK (window_escape), self);
    gtk_widget_add_controller (GTK_WIDGET (self), keys);
  }

  {
    GdkClipboard *clipboard = gtk_widget_get_clipboard (GTK_WIDGET (self));

    if (clipboard != NULL)
      g_signal_connect_object (clipboard, "changed",
                               G_CALLBACK (on_clipboard_changed), self, 0);
    window_sync_paste (self);
  }

  self->status_bar = build_status_bar (self);
  gtk_box_append (GTK_BOX (box), self->status_bar);

  g_signal_connect (self->view, "state-changed",
                    G_CALLBACK (on_view_state_changed), self);

  w42_view_set_document (self->view, self->doc);
  window_sync_state (self);

  g_signal_connect (self->view, "map", G_CALLBACK (on_view_mapped), self);
  g_signal_connect (self, "close-request", G_CALLBACK (on_close_request), NULL);
}

GtkWidget *
w42_window_new (GtkApplication *app)
{
  return g_object_new (W42_TYPE_WINDOW, "application", app, NULL);
}

GtkWidget *
w42_window_new_for_document (GtkApplication *app, W42Document *doc)
{
  W42Window *self = g_object_new (W42_TYPE_WINDOW, "application", app, NULL);

  g_return_val_if_fail (W42_IS_DOCUMENT (doc), GTK_WIDGET (self));

  g_set_object (&self->doc, doc);
  w42_view_set_document (self->view, doc);
  window_update_title (self);
  window_sync_state (self);

  return GTK_WIDGET (self);
}
