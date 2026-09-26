/* w42-window.c - see w42-window.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-window.h"

#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
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
#include "w42-epub.h"
#include "w42-pdf.h"
#include "w42-print.h"
#include "w42-scan.h"
#include "w42-ruler.h"
#include "w42-docmap.h"
#include "w42-rtf.h"
#include "w42-settings.h"
#include "w42-spell-dialog.h"
#include "w42-thesaurus.h"
#include "w42-thesaurus-dialog.h"
#include "w42-autotext.h"
#include "w42-template.h"
#include "w42-help.h"
#include "w42-macro.h"
#include "w42-view.h"

static const char *window_author_name (void);
static void window_saved (W42Window *self, gboolean succeeded);

/* The zoom steps the Standard bar offers; also what Options can make the
 * default. */
static const double ZOOM_STEPS[] = { 0.5, 0.75, 1.0, 1.5, 2.0 };
static const char  *ZOOM_LABELS[] = { "50%", "75%", "100%", "150%", "200%" };
/* After the steps, the box offers the two fits Word 97's did; and after
 * those, when the zoom is none of the above, one entry saying what it
 * is, spliced in and out as the zoom changes. */
static const char  *ZOOM_FIT_LABELS[] = { N_("Page Width"), N_("Whole Page") };
#define ZOOM_N_FIXED (G_N_ELEMENTS (ZOOM_LABELS) + G_N_ELEMENTS (ZOOM_FIT_LABELS))

/* The sizes Word 97's Formatting toolbar offered. */
static const int FONT_SIZES[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22,
                                  24, 26, 28, 36, 48, 72 };

struct _W42Window {
  GtkApplicationWindow parent_instance;

  W42Document *doc;
  W42View     *view;          /* the pane being edited: commands, toolbars
                               * and the ruler follow it */
  W42View     *view1;         /* the top pane's view, the one made at start */
  W42View     *view2;         /* the bottom pane's, while the window is split */
  GtkWidget   *paned;         /* holds the pane(s) below the ruler */

  GtkWidget   *standard_bar;
  GtkWidget   *format_bar;
  GtkWidget   *ruler;
  GtkWidget   *doc_map;       /* View > Document Map, at the left of the page */

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
  gboolean     full_screen_chrome[5];
  GtkWidget   *status_page;
  GtkWidget   *status_at;
  GtkWidget   *status_ln;
  GtkWidget   *status_col;
  GtkWidget   *status_pages;    /* "3/152": the page and how many, as Word
                                 * 97's status bar showed where one stood */
  GtkWidget   *status_words;    /* the running count, toward the goal */
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
  W42Thesaurus *thesaurus;    /* made when first asked for; NULL until then */
  const char  *spell_lang;    /* the document language the checker was
                               * last set to, interned; NULL for none */

  /* The words and pages the status bar shows.  Counting reads the whole
   * document, so it waits until the typing stops; the undo state says
   * whether anything has changed since the last count. */
  guint        count_id;
  gboolean     count_valid;
  gsize        count_undo_pos;
  guint64      count_serial;
  gsize        words;
  gssize       words_at_open;  /* the count when the document was opened,
                                * for what this session has written; -1
                                * before the first count */
  int          goal;           /* the words the author means to reach; 0 none */
  W42Layout   *count_layout;   /* the printed pages, when the view is a galley */
  int          n_pages;
  GtkWidget   *title_label;   /* word42 draws its own title bar */
};

G_DEFINE_FINAL_TYPE (W42Window, w42_window, GTK_TYPE_APPLICATION_WINDOW)

static void window_sync_state (W42Window *self);
static void on_view_state_changed (W42View *view, gpointer data);
static void window_load_goal (W42Window *self);
static void window_store_goal (W42Window *self);

/* Commands that can do nothing at all -- no fields to update, no
 * revisions to accept -- say so in the status bar rather than looking
 * broken.  Word 97 wrote its messages there too. */
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
  gboolean has_text = FALSE, has_picture = FALSE;
  GAction *a;

  if (clipboard != NULL)
    {
      GdkContentFormats *formats = gdk_clipboard_get_formats (clipboard);

      has_text = formats != NULL &&
                 gdk_content_formats_contain_gtype (formats, G_TYPE_STRING);
      has_picture = formats != NULL &&
                    gdk_content_formats_contain_gtype (formats, GDK_TYPE_TEXTURE);
    }

  /* Paste takes a picture too; Paste Special is text only. */
  a = g_action_map_lookup_action (G_ACTION_MAP (self), "paste");
  if (a != NULL)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (a), has_text || has_picture);
  a = g_action_map_lookup_action (G_ACTION_MAP (self), "paste-text");
  if (a != NULL)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (a), has_text);
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
  /* Translators: the window's title, "Document1 - Word42 0.9.0".  The
   * first %s is the document's name, the second an asterisk when it has
   * unsaved changes (else nothing), the third Word42's version. */
  title = g_strdup_printf (_("%s%s - Word42 %s"), name,
                           w42_document_get_modified (self->doc) ? "*" : "",
                           W42_VERSION);

  gtk_window_set_title (GTK_WINDOW (self), title);

  if (self->title_label != NULL)
    {
      /* Translators: the title in Word42's own title bar, "Word42 0.9.0 -
       * Document1".  The first %s is Word42's version, the second the
       * document's name, the third an asterisk when it has unsaved
       * changes (else nothing). */
      char *caption = g_strdup_printf (_("Word42 %s - %s%s"), W42_VERSION, name,
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

/* The store takes a reference of its own. */
static void
append_filter (GListStore *store, GtkFileFilter *filter)
{
  g_list_store_append (store, filter);
  g_object_unref (filter);
}

/* What Open lists, or, with `saving`, what Save As offers: only what
 * Word42 writes, so no Word 97, whose .doc it reads and cannot write. */
static GListModel *
file_filters (gboolean saving)
{
  GListStore *store = g_list_store_new (GTK_TYPE_FILE_FILTER);
  static const char * const all_docs[] = { "*.rtf", "*.docx", "*.doc", "*.odt", "*.abw", "*.zabw", "*.txt", "*.text", "*.pdf", "*.html", "*.htm", "*.pptx", NULL };
  static const char * const all_written[] = { "*.rtf", "*.docx", "*.odt", "*.abw", "*.zabw", "*.txt", "*.text", "*.pdf", "*.html", "*.htm", "*.pptx", NULL };
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

  if (saving)
    append_filter (store, named_filter (_("All Documents (*.rtf, *.docx, *.odt, *.abw, *.txt, *.pdf, *.html)"),
                                        all_written));
  else
    append_filter (store, named_filter (_("All Documents (*.rtf, *.docx, *.doc, *.odt, *.abw, *.txt, *.pdf, *.html)"),
                                        all_docs));
  append_filter (store, named_filter (_("Rich Text Format (*.rtf)"), rtf));
  append_filter (store, named_filter (_("Word Document (*.docx)"), docx));
  if (!saving)
    append_filter (store, named_filter (_("Word 97 (*.doc)"), doc));
  append_filter (store, named_filter (_("OpenDocument Text (*.odt)"), odt));
  append_filter (store, named_filter (_("AbiWord (*.abw, *.zabw)"), abw));
  append_filter (store, named_filter (_("Web Pages (*.html)"), web));
  append_filter (store, named_filter (_("Presentations (*.pptx)"), pptx));
  append_filter (store, named_filter (_("Text Documents (*.txt)"), text));
  if (saving || w42_pdf_import_available ())
    append_filter (store, named_filter (_("PDF Documents (*.pdf)"), pdf));
  append_filter (store, named_filter (_("All Files"), any));

  return G_LIST_MODEL (store);
}

gboolean
w42_window_name_has_extension (const char *name)
{
  static const char * const known[] = { ".rtf", ".docx", ".doc", ".odt", ".abw",
                                        ".zabw", ".txt", ".text", ".html", ".htm",
                                        ".pdf", ".pptx", ".ppsx", ".epub", NULL };
  char *lower;
  gboolean yes = FALSE;

  g_return_val_if_fail (name != NULL, FALSE);

  lower = g_ascii_strdown (name, -1);
  for (guint i = 0; known[i] != NULL && !yes; i++)
    yes = g_str_has_suffix (lower, known[i]) && strlen (lower) > strlen (known[i]);
  g_free (lower);
  return yes;
}

/* A label for a menu, where an underscore marks the mnemonic: a file
 * called my_report.rtf is not "myreport.rtf" with the R underlined. */
static char *
mnemonic_escape (const char *text)
{
  GString *out = g_string_new (NULL);

  for (const char *p = text; *p != '\0'; p++)
    {
      if (*p == '_')
        g_string_append_c (out, '_');
      g_string_append_c (out, *p);
    }
  return g_string_free (out, FALSE);
}

/* A file box answers whenever it is closed, and the window it was opened
 * for may have been closed in the meantime -- from another window, or
 * behind a file chooser the desktop runs outside the program.  Each box
 * holds a reference to its window, so the memory is still there; but a
 * closed window has left its application at once, and been disposed,
 * letting its document go, as soon as nothing held it.  Either way there
 * is nothing left to do the work in. */
static gboolean
window_gone (W42Window *self)
{
  return self->doc == NULL || gtk_window_get_application (GTK_WINDOW (self)) == NULL;
}

/* A choice box goes with its window, and answers Cancel as it goes, which
 * is while the window is being disposed.  So it is given a weak reference
 * rather than a strong one: a strong one would keep the window from being
 * disposed, and so the box from going, and it would sit on the screen
 * over a window that had been closed. */
static gpointer
window_weak_ref (W42Window *self)
{
  GWeakRef *ref = g_new0 (GWeakRef, 1);

  g_weak_ref_init (ref, self);
  return ref;
}

/* The window again, as a strong reference, or NULL if it has gone; the
 * weak reference is used up. */
static W42Window *
window_from_weak_ref (gpointer data)
{
  GWeakRef *ref = data;
  W42Window *self = g_weak_ref_get (ref);

  g_weak_ref_clear (ref);
  g_free (ref);
  if (self != NULL && window_gone (self))
    g_clear_object (&self);
  return self;
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
autosave_unlink (const char *path)
{
  char *meta = g_strconcat (path, ".txt", NULL);

  g_unlink (path);
  g_unlink (meta);
  g_free (meta);
}

/* The copy goes: the one this window wrote, and the one the document's
 * name would give, which another window on the same file may have
 * written. */
static void
autosave_remove (W42Window *self)
{
  char *path = autosave_path_for (self);

  if (self->autosave_path != NULL)
    autosave_unlink (self->autosave_path);
  autosave_unlink (path);
  g_free (path);
  g_clear_pointer (&self->autosave_path, g_free);
}

static gboolean
on_autosave (gpointer data)
{
  W42Window *self = data;
  char *path, *dir, *meta;
  GFile *file, *orig;
  GError *error = NULL;

  /* Undone back to the saved text, or reverted: a copy of what was
   * thrown away would come back as "recovered" after a crash. */
  if (!w42_document_get_modified (self->doc))
    {
      if (self->autosave_path != NULL)
        autosave_remove (self);
      return G_SOURCE_CONTINUE;
    }
  if (!self->autosave_dirty)
    return G_SOURCE_CONTINUE;

  path = autosave_path_for (self);

  dir = autosave_dir ();
  g_mkdir_with_parents (dir, 0700);
  g_free (dir);

  file = g_file_new_for_path (path);
  if (w42_rtf_save (w42_document_pt (self->doc), w42_document_page_setup (self->doc),
                    file, &error))
    {
      /* The document may have been saved under a new name since, or be
       * a recovered one still under the name it was recovered from: the
       * old copy goes once the new one is safely written, never before. */
      if (self->autosave_path != NULL && !g_str_equal (path, self->autosave_path))
        autosave_unlink (self->autosave_path);
      g_free (self->autosave_path);
      self->autosave_path = g_strdup (path);

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
  g_free (path);
  return G_SOURCE_CONTINUE;
}

/* Every so many minutes -- two, unless Tools > Options says otherwise,
 * or the environment does, which the tests use. */
static void
window_start_autosave (W42Window *self)
{
  const char *env = g_getenv ("W42_AUTOSAVE_SECONDS");
  guint seconds = env != NULL
    ? (guint) MAX (atoi (env), 1)
    : (guint) CLAMP (w42_settings_get_int ("autosave-minutes", 2), 1, 120) * 60;

  if (self->autosave_id != 0)
    g_source_remove (self->autosave_id);
  self->autosave_id = g_timeout_add_seconds (seconds, on_autosave, self);
}

void
w42_window_autosave_changed (GtkApplication *app)
{
  for (GList *l = app != NULL ? gtk_application_get_windows (app) : NULL;
       l != NULL; l = l->next)
    if (W42_IS_WINDOW (l->data))
      window_start_autosave (W42_WINDOW (l->data));
}

/* Tools > Options > Always create backup copy: the file about to be
 * written over is kept beside it first, as "Backup of" and its name, so
 * that a save regretted -- a chapter deleted, a wrong format chosen --
 * can be had back.  Word 97 kept a .wbk; the name here keeps the file's
 * own extension, so the copy opens as what it is. */
static void
window_backup_before_save (GFile *file)
{
  GFile *parent, *backup;
  char *name, *backup_name;

  if (!w42_settings_get_bool ("backup-copy", FALSE) ||
      !g_file_query_exists (file, NULL))
    return;

  parent = g_file_get_parent (file);
  name = g_file_get_basename (file);
  if (parent == NULL || name == NULL)
    {
      g_clear_object (&parent);
      g_free (name);
      return;
    }
  /* Translators: the name of the copy Tools > Options > Always create
   * backup copy keeps beside a file; %s is the file's own name.  It is a
   * file name: no slashes, backslashes or colons, and %s last, so that the
   * copy keeps the file's extension and opens as the file does. */
  backup_name = g_strdup_printf (_("Backup of %s"), name);
  backup = g_file_get_child (parent, backup_name);
  /* Best effort: a copy that cannot be made must not stop the save. */
  g_file_copy (file, backup, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);

  g_object_unref (backup);
  g_free (backup_name);
  g_free (name);
  g_object_unref (parent);
}

static gboolean
window_save_document (W42Document *doc, GFile *file, GError **error)
{
  window_backup_before_save (file);
  return w42_document_save (doc, file, error);
}

int
w42_window_recover_all (GtkApplication *app)
{
  static gboolean done;
  char *dir;
  GDir *d;
  const char *name;
  int recovered = 0, unreadable = 0;
  W42Window *first = NULL;

  /* Only what an earlier run left behind.  The application is single
   * instance, so starting Word42 again, or opening a file from the file
   * manager, lands here in the running one -- whose autosave folder holds
   * the live copies of the documents open in it now. */
  if (done)
    return 0;
  done = TRUE;

  dir = autosave_dir ();
  d = g_dir_open (dir, 0, NULL);
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
            /* Translators: %s is the recovered document's name. */
            char *heading = g_strdup_printf (_("Word42 recovered \342\200\234%s\342\200\235."), title);

            show_message (window, heading,
                          _("It was not saved when Word42 last stopped. Save it to "
                            "keep it; close it to let it go."));
            g_free (heading);
            g_free (title);
          }
          if (first == NULL)
            first = window;
          recovered++;

          /* The copy stays until the recovered document is saved, closed
           * or copied again: until then it is the only one there is, and
           * a second crash must not lose it. */
          window->autosave_path = g_strdup (path);
          window->autosave_dirty = TRUE;
        }
      else
        {
          /* Not deleted: it may be the only copy of someone's work, and
           * a later Word42 may read what this one cannot.  Renamed, so
           * that the next start does not stumble over it again. */
          char *bad = g_strconcat (path, ".bad", NULL);
          char *bad_meta = g_strconcat (bad, ".txt", NULL);

          g_clear_error (&error);
          gtk_window_destroy (GTK_WINDOW (window));
          g_rename (path, bad);
          g_rename (meta, bad_meta);
          g_free (bad_meta);
          g_free (bad);
          unreadable++;
        }

      g_object_unref (file);
      g_free (orig);
      g_free (meta);
      g_free (path);
    }

  if (d != NULL)
    g_dir_close (d);

  if (unreadable > 0)
    {
      /* Translators: %d is how many autosaved copies could not be read,
       * %s the folder they are kept in. */
      char *detail = g_strdup_printf (ngettext ("%d document left when Word42 last stopped "
                                                "could not be read. It is kept in %s, with "
                                                "names ending .bad.",
                                                "%d documents left when Word42 last stopped "
                                                "could not be read. They are kept in %s, with "
                                                "names ending .bad.",
                                                (unsigned long) unreadable),
                                      unreadable, dir);

      w42_message_show (first != NULL ? GTK_WINDOW (first) : NULL,
                        _("Word42 could not recover everything."), detail);
      g_free (detail);
    }

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
      char *shown = mnemonic_escape (base);
      char *label = g_strdup_printf ("_%u %s", i + 1, shown);
      GMenuItem *item = g_menu_item_new (label, NULL);

      g_menu_item_set_action_and_target (item, "win.open-recent", "s", paths[i]);
      g_menu_append_item (self->recent_menu, item);
      g_object_unref (item);
      g_free (label);
      g_free (shown);
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

W42Document *
w42_window_new_document (GtkWindow *from)
{
  GtkApplication *app = from != NULL ? gtk_window_get_application (from) : NULL;
  GtkWidget *window;

  if (app == NULL)
    return NULL;

  window = w42_window_new (app);
  gtk_window_present (GTK_WINDOW (window));
  return W42_WINDOW (window)->doc;
}

/* Reads `file` into the window's document, and says nothing if it cannot:
 * the caller knows which window should carry the message. */
gboolean
w42_window_load (W42Window *self, GFile *file, GError **error)
{
  W42View *views[2];
  gsize first;

  g_return_val_if_fail (W42_IS_WINDOW (self), FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (!w42_document_load (self->doc, file, error))
    return FALSE;
  w42_pt_set_author (w42_document_pt (self->doc), window_author_name ());

  /* A caret and a selection belong to the text they were made in; kept
   * across a load, or a Revert, they would select something arbitrary
   * in the new one. */
  first = w42_pt_first_caret_pos (w42_document_pt (self->doc));
  views[0] = self->view1;
  views[1] = self->view2;
  for (guint i = 0; i < G_N_ELEMENTS (views); i++)
    if (views[i] != NULL)
      w42_view_select_range (views[i], first, first);

  /* Another document: its own count, session and goal. */
  self->count_valid = FALSE;
  self->words_at_open = -1;
  self->goal = 0;
  window_load_goal (self);

  window_note_recent (self, file);
  window_update_title (self);
  window_sync_state (self);
  return TRUE;
}

gboolean
w42_window_open (W42Window *self, GFile *file)
{
  GError *error = NULL;

  g_return_val_if_fail (W42_IS_WINDOW (self), FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (!w42_window_load (self, file, &error))
    {
      show_error (self, _("Word42 could not open that file."), error);
      g_clear_error (&error);
      return FALSE;
    }
  return TRUE;
}

void
w42_window_apply_default_language (W42Document *doc)
{
  char *lang;
  W42StyleSheet *sheet;
  const W42Style *normal;

  g_return_if_fail (W42_IS_DOCUMENT (doc));

  lang = w42_settings_get_string ("default-language", "");
  sheet = w42_pt_stylesheet (w42_document_pt (doc));
  normal = w42_stylesheet_find (sheet, "Normal");
  if (lang != NULL && *lang != '\0' && normal != NULL)
    {
      W42Style with = *normal;

      with.ch.lang = g_intern_string (lang);
      w42_stylesheet_set (sheet, &with);
      w42_stylesheet_follow (sheet, "Normal");
    }
  g_free (lang);
}

W42Window *
w42_window_find_file (GtkApplication *app, GFile *file)
{
  g_return_val_if_fail (G_IS_FILE (file), NULL);

  for (GList *l = app != NULL ? gtk_application_get_windows (app) : NULL;
       l != NULL; l = l->next)
    {
      GFile *has;

      if (!W42_IS_WINDOW (l->data))
        continue;
      has = w42_document_get_file (W42_WINDOW (l->data)->doc);
      if (has != NULL && g_file_equal (has, file))
        return W42_WINDOW (l->data);
    }
  return NULL;
}

/* File > Open and Open Recent never throw work away: a document with
 * changes, or one already on disk, keeps its window, and the file opens
 * in a new one.  That window is shown only once the file is in it, and
 * goes again if the file cannot be read -- the message goes on the
 * window the command came from, since a message on the new one would go
 * with it. */
static void
window_open_file (W42Window *self, GFile *file)
{
  W42Window *target = self;
  GError *error = NULL;
  W42Window *open = w42_window_find_file (gtk_window_get_application (GTK_WINDOW (self)), file);

  if (open != NULL)
    {
      gtk_window_present (GTK_WINDOW (open));
      return;
    }
  if (w42_document_get_modified (self->doc) || w42_document_get_file (self->doc) != NULL)
    target = W42_WINDOW (w42_window_new (gtk_window_get_application (GTK_WINDOW (self))));

  if (!w42_window_load (target, file, &error))
    {
      if (target != self)
        gtk_window_destroy (GTK_WINDOW (target));
      show_error (self, _("Word42 could not open that file."), error);
      g_clear_error (&error);
      return;
    }
  if (target != self)
    gtk_window_present (GTK_WINDOW (target));
}

void
w42_window_flash_status (W42Window *self, const char *text)
{
  g_return_if_fail (W42_IS_WINDOW (self));
  window_flash (self, "%s", text != NULL ? text : "");
}

/* A PDF, a web page or a presentation made from the document.  The file
 * is written, and the document stays what it was and where it was: made
 * the document's own file, it would be marked saved, and closing would
 * lose everything that format cannot hold without a word. */
static gboolean
window_export (W42Window *self, GFile *file, GError **error)
{
  char *base;

  if (!w42_io_save (w42_document_pt (self->doc),
                    w42_document_page_setup (self->doc), file, error))
    return FALSE;

  base = g_file_get_basename (file);
  /* Translators: %s is the name of the file exported to. */
  window_flash (self, _("Exported as %s. The document itself is not saved there: "
                        "Word42 cannot read that format back as it was."), base);
  g_free (base);
  return TRUE;
}

gboolean
w42_window_save_to (W42Window *self, GFile *file, GError **error)
{
  gboolean ok;

  g_return_val_if_fail (W42_IS_WINDOW (self), FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (!w42_io_format_round_trips (file))
    return window_export (self, file, error);

  ok = window_save_document (self->doc, file, error);
  window_saved (self, ok);
  return ok;
}

/* A recent file opens here if this window is untouched, else in its own. */
static void
action_open_recent (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GFile *file = g_file_new_for_path (g_variant_get_string (param, NULL));

  (void) action;

  window_open_file (self, file);
  g_object_unref (file);
}

static void
on_open_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (!window_gone (self))
    {
      if (file != NULL)
        window_open_file (self, file);
      else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                                  GTK_DIALOG_ERROR_DISMISSED))
        show_error (self, _("Word42 could not open that file."), error);
    }

  g_clear_object (&file);
  g_clear_error (&error);
  g_object_unref (self);
}

/* File > Revert: the document as it was when it was last saved.  The
 * answer comes back here -- as Cancel, when the window is closing and
 * takes the box with it, by which time the document has gone. */
static void
on_revert_choice (int choice, gpointer data)
{
  W42Window *self = window_from_weak_ref (data);

  if (self == NULL)
    return;
  if (choice == 0 && w42_document_get_file (self->doc) != NULL)
    {
      GFile *file = g_object_ref (w42_document_get_file (self->doc));

      /* The changes thrown away must not come back as recovered. */
      if (w42_window_open (self, file))
        autosave_remove (self);
      g_object_unref (file);
    }
  g_object_unref (self);
}

static void
action_revert (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GFile *file = w42_document_get_file (self->doc);
  const char *const buttons[] = { _("_Revert"), _("Cancel"), NULL };
  char *name, *heading;

  (void) action; (void) param;

  if (file == NULL)
    {
      window_flash (self, "%s", _("This document has not been saved yet, so there is "
                                  "nothing to go back to."));
      return;
    }
  if (!w42_document_get_modified (self->doc))
    {
      window_flash (self, "%s", _("The document has no unsaved changes."));
      return;
    }

  name = w42_document_get_title (self->doc);
  /* Translators: %s is the document's name. */
  heading = g_strdup_printf (_("Revert \342\200\234%s\342\200\235 to the saved version?"), name);
  w42_choice_show (GTK_WINDOW (self), heading,
                   _("The changes made since the document was last saved "
                     "will be lost."),
                   buttons, 1, 1, on_revert_choice, window_weak_ref (self));
  g_free (heading);
  g_free (name);
}

static void
action_open (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListModel *filters = file_filters (FALSE);

  (void) action; (void) param;

  gtk_file_dialog_set_title (dialog, _("Open"));
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_open_response,
                        g_object_ref (self));

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
  /* A goal set on an untitled document, or before Save As, goes with
   * the file it now has. */
  if (succeeded && self->goal > 0)
    window_store_goal (self);

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

/* What a save box's answer comes to, once the name has its extension:
 * `finish` writes the file, or, given NULL, learns that nothing is to
 * be written after all. */
typedef void (*SaveFinish) (W42Window *self, GFile *file);

typedef struct {
  gpointer    self;             /* window_weak_ref() */
  GFile      *file;
  SaveFinish  finish;
} Replacing;

static void
on_replace_choice (int choice, gpointer data)
{
  Replacing *r = data;
  W42Window *self = window_from_weak_ref (r->self);

  if (self != NULL)
    {
      r->finish (self, choice == 0 ? r->file : NULL);
      g_object_unref (self);
    }
  g_object_unref (r->file);
  g_free (r);
}

/* A name typed with no extension Word42 knows is saved as Rich Text,
 * which keeps everything, rather than as the plain text an unknown
 * extension would make of it -- and a dot does not make an extension:
 * "Mr. Smith" is a letter, not a file of type " Smith".  The box asked
 * about replacing the name as it was typed, though, not the name with
 * .rtf on the end, so a file already there under that name is asked
 * about here before anything is written over it. */
static void
window_save_chosen (W42Window *self, GFile *chosen, SaveFinish finish)
{
  char *base = g_file_get_basename (chosen);
  GFile *parent, *file;
  char *named;

  if (base == NULL || w42_window_name_has_extension (base))
    {
      g_free (base);
      finish (self, chosen);
      return;
    }

  parent = g_file_get_parent (chosen);
  named = g_strconcat (base, ".rtf", NULL);
  file = parent != NULL ? g_file_get_child (parent, named) : g_file_new_for_path (named);
  g_clear_object (&parent);
  g_free (base);

  if (g_file_query_exists (file, NULL))
    {
      const char *const buttons[] = { _("_Replace"), _("Cancel"), NULL };
      Replacing *r = g_new0 (Replacing, 1);
      /* Translators: %s is a file name. */
      char *heading = g_strdup_printf (_("\342\200\234%s\342\200\235 already exists. "
                                         "Replace it?"), named);

      r->self = window_weak_ref (self);
      r->file = file;
      r->finish = finish;
      w42_choice_show (GTK_WINDOW (self), heading,
                       _("The file that is there now will be lost."),
                       buttons, 1, 1, on_replace_choice, r);
      g_free (heading);
      g_free (named);
      return;
    }

  finish (self, file);
  g_object_unref (file);
  g_free (named);
}

static void
save_as_finish (W42Window *self, GFile *file)
{
  GError *error = NULL;
  gboolean saved = FALSE;

  if (file != NULL && !w42_io_format_round_trips (file))
    {
      if (!window_export (self, file, &error))
        show_error (self, _("Word42 could not save that file."), error);
    }
  else if (file != NULL)
    {
      saved = window_save_document (self->doc, file, &error);
      if (!saved)
        show_error (self, _("Word42 could not save that file."), error);
    }

  g_clear_error (&error);
  window_saved (self, saved);
}

static void
on_save_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (!window_gone (self) && file != NULL)
    window_save_chosen (self, file, save_as_finish);
  else if (!window_gone (self))
    {
      if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                             GTK_DIALOG_ERROR_DISMISSED))
        show_error (self, _("Word42 could not save that file."), error);
      window_saved (self, FALSE);
    }

  g_clear_object (&file);
  g_clear_error (&error);
  g_object_unref (self);
}

static void
window_save_as (W42Window *self)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListModel *filters = file_filters (TRUE);
  GFile *file = w42_document_get_file (self->doc);
  char *name = w42_document_get_title (self->doc);

  gtk_file_dialog_set_title (dialog, _("Save As"));
  gtk_file_dialog_set_filters (dialog, filters);

  /* A document read from a format Word42 writes back is offered under
   * its own name.  Anything else -- never saved, or read from a file that
   * does not round trip -- is offered as Rich Text, the one format Word42
   * writes that keeps all the document has: "letter.doc" becomes
   * "letter.rtf", not "letter.doc.rtf". */
  if (file != NULL && w42_io_format_round_trips (file))
    gtk_file_dialog_set_initial_name (dialog, name);
  else
    {
      char *dot = strrchr (name, '.');
      char *suggested;

      if (file != NULL && dot != NULL && dot != name)
        *dot = '\0';
      suggested = g_strconcat (name, ".rtf", NULL);
      gtk_file_dialog_set_initial_name (dialog, suggested);
      g_free (suggested);
    }

  /* In the folder the document came from, not wherever the program was
   * started. */
  if (file != NULL)
    {
      GFile *folder = g_file_get_parent (file);

      if (folder != NULL)
        gtk_file_dialog_set_initial_folder (dialog, folder);
      g_clear_object (&folder);
    }

  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL, on_save_response,
                        g_object_ref (self));

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

  if (file == NULL || !w42_io_format_round_trips (file))
    {
      /* Never saved, or read from a format that Word42 does not write
       * back as it read it: Save turns into Save As, and window_saved()
       * runs when that dialog comes back rather than now. */
      window_save_as (self);
      return;
    }

  saved = window_save_document (self->doc, file, &error);
  if (!saved)
    {
      show_error (self, _("Word42 could not save that file."), error);
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

/* File > New from Template. */
static void
action_new_from_template (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_template_dialog_show (GTK_WINDOW (self), self->view);
}

/* File > Save as Template: a copy in the templates folder, which
 * New from Template then lists.  The document keeps the file it had.
 * The copy is written past the document rather than through it: a save
 * through the document takes the template for its file and the undo
 * history's present state for the saved one, and putting the file back
 * afterwards does not put that back, so an Undo would later mark edits
 * that were never saved as saved. */
static void
template_finish (W42Window *self, GFile *file)
{
  GError *error = NULL;

  if (file == NULL)
    return;

  if (!w42_io_save (w42_document_pt (self->doc),
                    w42_document_page_setup (self->doc), file, &error))
    show_error (self, _("Word42 could not save that template."), error);
  else
    window_flash (self, "%s", _("Saved as a template.  File > New from Template "
                                "lists it."));
  g_clear_error (&error);
}

static void
on_template_saved (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (!window_gone (self) && file != NULL)
    window_save_chosen (self, file, template_finish);
  else if (!window_gone (self) && error != NULL &&
           !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, _("Word42 could not save that template."), error);

  g_clear_object (&file);
  g_clear_error (&error);
  g_object_unref (self);
}

static void
action_save_as_template (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  char *dir = w42_template_folder ();
  GFile *folder = g_file_new_for_path (dir);
  char *name = w42_document_get_title (self->doc);
  char *stem = g_strdup (name);
  char *dot = strrchr (stem, '.');
  char *suggested;

  (void) action; (void) param;

  if (dot != NULL)
    *dot = '\0';
  suggested = g_strconcat (stem, ".rtf", NULL);

  gtk_file_dialog_set_title (dialog, _("Save as Template"));
  gtk_file_dialog_set_initial_folder (dialog, folder);
  gtk_file_dialog_set_initial_name (dialog, suggested);
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL, on_template_saved,
                        g_object_ref (self));

  g_free (suggested);
  g_free (stem);
  g_free (name);
  g_object_unref (folder);
  g_free (dir);
  g_object_unref (dialog);
}

/* ---- Closing a document with unsaved changes -------------------------- */

enum {
  CLOSE_CANCEL = 0,
  CLOSE_DISCARD,
  CLOSE_SAVE
};

/* Dismissing the box -- Escape, or its own close button -- means cancel:
 * losing the document to a stray keypress would be exactly the accident
 * this box exists to prevent.  The window may have been closed some
 * other way while the box was up, and then there is nothing to do. */
static void
on_close_choice (int choice, gpointer data)
{
  W42Window *self = window_from_weak_ref (data);

  if (self == NULL)
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
  g_object_unref (self);
}

static gboolean window_document_shared (W42Window *self);

static gboolean
on_close_request (GtkWindow *window, gpointer data)
{
  W42Window *self = W42_WINDOW (window);
  const char * const buttons[] = { _("Cancel"), _("Don't Save"), _("_Save"), NULL };
  char *name, *heading;

  (void) data;

  /* Nothing at risk: let it close.  A document still open in another
   * window is not at risk either; that window will ask when it goes. */
  if (self->force_close || !w42_document_get_modified (self->doc) ||
      window_document_shared (self))
    return GDK_EVENT_PROPAGATE;

  name = w42_document_get_title (self->doc);
  /* Translators: %s is the document's name. */
  heading = g_strdup_printf (_("Save changes to \342\200\234%s\342\200\235?"), name);

  w42_choice_show (GTK_WINDOW (self), heading,
                   _("If you close without saving, the changes you have made "
                     "will be lost."),
                   buttons, CLOSE_SAVE, CLOSE_CANCEL, on_close_choice,
                   window_weak_ref (self));

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

void
w42_window_close_discarding (W42Window *self)
{
  g_return_if_fail (W42_IS_WINDOW (self));

  if (window_gone (self))
    return;
  /* The window goes without asking; the document is not marked clean,
   * since another window on it would then close without asking too. */
  self->force_close = TRUE;
  gtk_window_close (GTK_WINDOW (self));
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

/* Window > Split: a second pane on the same document, under the first,
 * with a bar to drag between them; each pane scrolls and keeps a caret of
 * its own, and the commands, the toolbars and the ruler follow the pane
 * being edited.  Split again puts the window back to one pane. */

/* The ruler and the boxes that stay open -- Find, Spelling -- work on the
 * pane being edited.  The second pane goes when the window is unsplit,
 * so a box left pointing at it would be pointing at nothing. */
static void
window_pane_changed (W42Window *self)
{
  w42_ruler_set_view (self->ruler, self->view);
  if (self->doc_map != NULL)
    w42_docmap_set_view (self->doc_map, self->view);
  if (self->find_dialog != NULL)
    w42_find_dialog_set_view (W42_FIND_DIALOG (self->find_dialog), self->view);
  if (self->spell_dialog != NULL)
    w42_spell_dialog_set_view (W42_SPELL_DIALOG (self->spell_dialog), self->view);
}

static void
on_pane_focus_enter (GtkEventControllerFocus *controller, gpointer data)
{
  W42Window *self = data;
  GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));

  if (!W42_IS_VIEW (widget) || W42_VIEW (widget) == self->view)
    return;
  self->view = W42_VIEW (widget);
  window_pane_changed (self);
  window_sync_state (self);
}

static void
window_track_pane_focus (W42Window *self, W42View *view)
{
  GtkEventController *focus = gtk_event_controller_focus_new ();

  g_signal_connect (focus, "enter", G_CALLBACK (on_pane_focus_enter), self);
  gtk_widget_add_controller (GTK_WIDGET (view), focus);
}

/* A window action's boolean state, for the settings the new pane copies. */
static gboolean
window_action_state (W42Window *self, const char *name)
{
  GAction *a = g_action_map_lookup_action (G_ACTION_MAP (self), name);
  GVariant *state;
  gboolean on;

  if (a == NULL)
    return FALSE;
  state = g_action_get_state (a);
  if (state == NULL)
    return FALSE;
  on = g_variant_get_boolean (state);
  g_variant_unref (state);
  return on;
}

static void
window_set_split (W42Window *self, gboolean split)
{
  if (split == (self->view2 != NULL))
    return;

  if (split)
    {
      GtkWidget *scrolled = gtk_scrolled_window_new ();
      W42View *view = W42_VIEW (w42_view_new ());

      /* The new pane shows the same document with the same settings;
       * only its caret and its scroll position are its own. */
      w42_view_set_document (view, self->doc);
      w42_view_set_mode (view, w42_view_get_mode (self->view1));
      w42_view_set_zoom (view, w42_view_get_zoom (self->view1));
      w42_view_set_autocorrect (view, w42_view_get_autocorrect (self->view1));
      w42_view_set_track_changes (view, w42_view_get_track_changes (self->view1));
      w42_view_set_show_marks (view, window_action_state (self, "show-marks"));
      w42_view_set_gridlines (view, window_action_state (self, "gridlines"));
      w42_view_set_typewriter (view, window_action_state (self, "typewriter"));
      if (self->spell != NULL && window_action_state (self, "auto-spell"))
        w42_view_set_spell (view, self->spell);

      gtk_widget_set_vexpand (scrolled, TRUE);
      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                      GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
      gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled),
                                     GTK_WIDGET (view));
      gtk_paned_set_end_child (GTK_PANED (self->paned), scrolled);
      gtk_paned_set_resize_end_child (GTK_PANED (self->paned), TRUE);
      gtk_paned_set_shrink_end_child (GTK_PANED (self->paned), FALSE);
      gtk_paned_set_position (GTK_PANED (self->paned),
                              gtk_widget_get_height (self->paned) / 2);

      g_signal_connect (view, "state-changed",
                        G_CALLBACK (on_view_state_changed), self);
      window_track_pane_focus (self, view);
      self->view2 = view;
    }
  else
    {
      if (self->view == self->view2)
        {
          self->view = self->view1;
          window_pane_changed (self);
        }
      self->view2 = NULL;
      gtk_paned_set_end_child (GTK_PANED (self->paned), NULL);
      gtk_widget_grab_focus (GTK_WIDGET (self->view1));
      window_sync_state (self);
    }
}

static void
action_split_window (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GVariant *state = g_action_get_state (G_ACTION (action));
  gboolean on = !g_variant_get_boolean (state);

  (void) param;
  g_variant_unref (state);
  g_simple_action_set_state (action, g_variant_new_boolean (on));
  window_set_split (self, on);
}

static void
action_options (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_options_dialog_show (GTK_WINDOW (self), self->view);
}

/* The View menu's names for the three views, which the settings file and
 * the view-mode action use too. */
static W42ViewMode
view_mode_from_name (const char *name)
{
  if (g_strcmp0 (name, "page-layout") == 0)
    return W42_VIEW_PAGE_LAYOUT;
  if (g_strcmp0 (name, "online") == 0)
    return W42_VIEW_ONLINE;
  return W42_VIEW_NORMAL;
}

/* What Tools > Options and the View menu remembered, applied to a new
 * window. */
static void
window_apply_settings (W42Window *self)
{
  char *view = w42_settings_get_string ("default-view", "page-layout");
  int zoom = w42_settings_get_int ("zoom", 100);
  W42ViewMode mode = view_mode_from_name (view);
  GAction *act;

  act = g_action_map_lookup_action (G_ACTION_MAP (self), "view-mode");
  w42_view_set_mode (self->view, mode);
  if (act != NULL)
    g_simple_action_set_state (G_SIMPLE_ACTION (act),
                               g_variant_new_string (mode == W42_VIEW_PAGE_LAYOUT ? "page-layout"
                                                     : mode == W42_VIEW_ONLINE ? "online" : "normal"));
  g_free (view);

  if (zoom >= 25 && zoom <= 500)
    w42_view_set_zoom (self->view, zoom / 100.0);

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

  /* The Document Map starts out away, as Word 97's did, unless it was
   * showing when the program was last used. */
  {
    gboolean visible = w42_settings_get_bool ("show-document-map", FALSE);

    gtk_widget_set_visible (self->doc_map, visible);
    act = g_action_map_lookup_action (G_ACTION_MAP (self), "document-map");
    if (act != NULL)
      g_simple_action_set_state (G_SIMPLE_ACTION (act), g_variant_new_boolean (visible));
  }
}

static void
action_help_contents (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_help_window_show (GTK_WINDOW (self), NULL);
}

static void
action_help_search (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_help_window_show (GTK_WINDOW (self), "");
}

static void
action_help_index (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_help_index_show (GTK_WINDOW (self));
}

/* The project's own pages, opened in whatever the desktop uses for the
 * web.  Nothing about the document goes with it. */
static void
action_help_web (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  {
    GtkUriLauncher *launcher = gtk_uri_launcher_new ("https://word42.org");

    gtk_uri_launcher_launch (launcher, GTK_WINDOW (self), NULL, NULL, NULL);
    g_object_unref (launcher);
  }
}

static void
action_report_bug (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  {
    GtkUriLauncher *launcher =
      gtk_uri_launcher_new ("https://github.com/office-42/word42/issues");

    gtk_uri_launcher_launch (launcher, GTK_WINDOW (self), NULL, NULL, NULL);
    g_object_unref (launcher);
  }
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
VIEW_ACTION (repeat,     w42_view_repeat)
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

  w42_view_set_mode (self->view, view_mode_from_name (which));
  g_simple_action_set_state (action, g_variant_new_string (which));
}

static void
action_zoom (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  double zoom = g_variant_get_double (param);

  (void) action;
  w42_view_set_zoom (self->view, zoom);
}

static void
action_zoom_fit (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  const char *what = g_variant_get_string (param, NULL);

  (void) action;
  w42_view_set_zoom (self->view,
                     w42_view_fit_zoom (self->view, g_str_equal (what, "page")));
}

static void
action_zoom_dialog (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_zoom_dialog_show (GTK_WINDOW (self), self->view);
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
  /* Not remembered: a document opens clean, as in Word 97; the marks are a
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

/* View > Typewriter Scrolling: the line being written stays at the
 * middle of the window, in every pane, and the choice is remembered. */
static void
action_typewriter (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GVariant *state = g_action_get_state (G_ACTION (action));
  gboolean on = !g_variant_get_boolean (state);

  (void) param;
  g_variant_unref (state);
  g_simple_action_set_state (action, g_variant_new_boolean (on));
  w42_view_set_typewriter (self->view1, on);
  if (self->view2 != NULL)
    w42_view_set_typewriter (self->view2, on);
  w42_settings_set_bool ("typewriter", on);
}

/* View > Document Map */
static void
action_document_map (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  gboolean visible;

  (void) param;

  visible = !gtk_widget_get_visible (self->doc_map);
  gtk_widget_set_visible (self->doc_map, visible);
  g_simple_action_set_state (action, g_variant_new_boolean (visible));
  w42_settings_set_bool ("show-document-map", visible);
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

  (void) action; (void) param;
  w42_word_count_dialog_show (GTK_WINDOW (self), self->view);
}

static void
on_font_dialog_done (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  PangoFontDescription *desc;

  desc = gtk_font_dialog_choose_font_finish (GTK_FONT_DIALOG (source),
                                             result, NULL);
  if (window_gone (self))
    {
      g_clear_pointer (&desc, pango_font_description_free);
      g_object_unref (self);
      return;
    }
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
  if (desc == NULL)
    {
      g_object_unref (self);
      return;
    }

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
  g_object_unref (self);
}

/* Ctrl+] and Ctrl+[: the size a point up or down, as Word 97 stepped it. */
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

  gtk_font_dialog_set_title (dialog, _("Font"));
  gtk_font_dialog_choose_font (dialog, GTK_WINDOW (self), desc, NULL,
                               on_font_dialog_done, g_object_ref (self));

  pango_font_description_free (desc);
  g_object_unref (dialog);
}

/* Word 97's About box was a banner, a version line and an OK button, and so is
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

  if (window_gone (self))
    {
      g_clear_object (&file);
      g_clear_error (&error);
      g_object_unref (self);
      return;
    }

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
          show_error (self, _("Word42 could not insert that picture."), error);
        }

      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    {
      show_error (self, _("Word42 could not open that picture."), error);
    }

  g_clear_error (&error);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
  g_object_unref (self);
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
  gtk_file_filter_set_name (pictures, _("Pictures"));
  add_picture_patterns (pictures);
  append_filter (filters, pictures);

  gtk_file_dialog_set_title (dialog, _("Insert Picture"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL,
                        on_picture_response, g_object_ref (self));

  g_object_unref (filters);
  g_object_unref (dialog);
}

/* Insert > Picture > From Scanner or Camera: the scanner's own dialog,
 * and what it scanned into the text as a picture. */
static void
action_insert_scan (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  const char *format = NULL;
  GBytes *bytes;

  (void) action; (void) param;
  if (!w42_scan_available ())
    {
      w42_message_show (GTK_WINDOW (self), _("No scanner can be reached from here."),
                        _("Word42 scans through Windows Image Acquisition on Windows and "
                          "through SANE's scanimage on Linux; neither was found."));
      return;
    }
  bytes = w42_scan_acquire (GTK_WINDOW (self), &format, &error);
  if (bytes != NULL)
    {
      int width = 0, height = 0;
      const char *probed = NULL;

      if (w42_image_probe (bytes, &width, &height, &probed))
        w42_view_insert_picture (self->view, bytes, probed != NULL ? probed : format, width, height);
      else
        w42_message_show (GTK_WINDOW (self), _("The scanner sent a picture Word42 cannot read."), NULL);
      g_bytes_unref (bytes);
    }
  else if (error != NULL)
    show_error (self, _("Word42 could not scan."), error);
  g_clear_error (&error);
}

/* ---- Export as PDF ---------------------------------------------------- */

static void
on_export_pdf_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (window_gone (self))
    g_clear_object (&file);
  else if (file != NULL)
    {
      /* Export does not make the PDF the document's file: the document is
       * still the RTF or text it came from, and Save keeps going there. */
      if (!w42_pdf_export (w42_document_pt (self->doc),
                           w42_document_page_setup (self->doc), file, &error))
        show_error (self, _("Word42 could not export the PDF."), error);

      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    {
      show_error (self, _("Word42 could not export the PDF."), error);
    }

  g_clear_error (&error);
  g_object_unref (self);
}

/* ---- Export as web page ----------------------------------------------- */

static void
on_export_html_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (window_gone (self))
    g_clear_object (&file);
  else if (file != NULL)
    {
      if (!w42_html_export (w42_document_pt (self->doc),
                            w42_document_page_setup (self->doc), file, &error))
        show_error (self, _("Word42 could not export the web page."), error);
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, _("Word42 could not export the web page."), error);

  g_clear_error (&error);
  g_object_unref (self);
}

/* File > Web Page Preview: Word 97 wrote the document out as a web page
 * and opened it in the browser, so that what a reader on the web would
 * see could be seen.  The page goes to the cache folder, one file written
 * over each time, so nothing is left lying about. */
static void
action_web_preview (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  char *dir = g_build_filename (g_get_user_cache_dir (), "word42", NULL);
  char *path = g_build_filename (dir, "preview.html", NULL);
  GFile *file;

  (void) action; (void) param;

  g_mkdir_with_parents (dir, 0700);
  file = g_file_new_for_path (path);
  if (w42_html_export (w42_document_pt (self->doc),
                       w42_document_page_setup (self->doc), file, &error))
    {
      char *uri = g_file_get_uri (file);
      GtkUriLauncher *launcher = gtk_uri_launcher_new (uri);

      gtk_uri_launcher_launch (launcher, GTK_WINDOW (self), NULL, NULL, NULL);
      g_object_unref (launcher);
      g_free (uri);
    }
  else
    show_error (self, _("Word42 could not write the web page to preview."), error);
  g_clear_error (&error);
  g_object_unref (file);
  g_free (path);
  g_free (dir);
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

  append_filter (filters, named_filter (_("Web Pages (*.html)"), html));
  gtk_file_dialog_set_title (dialog, _("Export as Web Page"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, suggested);
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL,
                        on_export_html_response, g_object_ref (self));

  g_free (suggested);
  g_free (name);
  g_object_unref (filters);
  g_object_unref (dialog);
}

/* File > Export as E-book: the document as an EPUB 3 book, a chapter to
 * a file, for a reader's device or a shop.  Like the other exports it
 * leaves the document what and where it was. */
static void
on_export_epub_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (window_gone (self))
    g_clear_object (&file);
  else if (file != NULL)
    {
      w42_view_update_fields (self->view);
      if (!w42_epub_export (w42_document_pt (self->doc),
                            w42_document_page_setup (self->doc), file, &error))
        show_error (self, _("Word42 could not export the e-book."), error);
      else
        window_flash (self, "%s", _("The e-book is written."));
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, _("Word42 could not export the e-book."), error);

  g_clear_error (&error);
  g_object_unref (self);
}

static void
action_export_epub (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  static const char * const epub[] = { "*.epub", NULL };
  char *name = w42_document_get_title (self->doc);
  char *dot = strrchr (name, '.');
  char *suggested;

  (void) action; (void) param;

  if (dot != NULL)
    *dot = '\0';
  suggested = g_strconcat (name, ".epub", NULL);

  append_filter (filters, named_filter (_("E-books (*.epub)"), epub));
  gtk_file_dialog_set_title (dialog, _("Export as E-book"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, suggested);
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL,
                        on_export_epub_response, g_object_ref (self));

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

  if (window_gone (self))
    g_clear_object (&file);
  else if (file != NULL)
    {
      if (!w42_pptx_save (w42_document_pt (self->doc),
                          w42_document_page_setup (self->doc), file, &error))
        show_error (self, _("Word42 could not export the presentation."), error);
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, _("Word42 could not export the presentation."), error);

  g_clear_error (&error);
  g_object_unref (self);
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
      show_message (self, _("There is nothing to make slides of."),
                    _("A slide is a heading and the lines under it: give the "
                      "document's headings the Heading 1 style, or a Title."));
      return;
    }
  {
    char *detail = g_strdup_printf (ngettext ("%u slide.", "%u slides.", slides->len),
                                    slides->len);

    window_flash (self, "%s", detail);
    g_free (detail);
  }
  w42_slides_free (slides);

  if (dot != NULL)
    *dot = '\0';
  suggested = g_strconcat (name, ".pptx", NULL);

  append_filter (filters, named_filter (_("Presentations (*.pptx)"), pptx));
  gtk_file_dialog_set_title (dialog, _("Export as Presentation"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, suggested);
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL,
                        on_export_pptx_response, g_object_ref (self));

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

  append_filter (filters, named_filter (_("PDF Documents (*.pdf)"), pdf));
  gtk_file_dialog_set_title (dialog, _("Export as PDF"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, suggested);
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL,
                        on_export_pdf_response, g_object_ref (self));

  g_free (suggested);
  g_free (name);
  g_object_unref (filters);
  g_object_unref (dialog);
}

static void
action_print (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  W42PrintExtras extras;
  int page = 0, line = 0, column = 0;

  w42_view_update_fields (self->view);

  (void) action; (void) param;
  /* The selection, for "Selection"; the caret's page, for "Current page". */
  extras.selection = NULL;
  if (w42_view_has_selection (self->view))
    {
      gsize start = 0, end = 0;

      w42_view_get_selection_bounds (self->view, &start, &end);
      if (end > start)
        extras.selection = w42_pt_extract (w42_document_pt (self->doc), start, end - start);
    }
  {
    /* Normal view's layout is one galley, on which every position is on
     * page 1; the printed page comes from a paginated layout. */
    W42Layout *layout = w42_view_get_layout (self->view);
    W42Layout *paged = NULL;

    if (w42_layout_get_galley (layout))
      {
        paged = w42_layout_new ();
        w42_layout_set_galley (paged, FALSE);
        w42_layout_build (paged, self->doc);
        layout = paged;
      }
    w42_layout_describe_pos (layout, w42_view_get_caret (self->view),
                             &page, &line, &column);
    if (paged != NULL)
      w42_layout_free (paged);
  }
  extras.current_page = page;
  w42_print_document (GTK_WINDOW (self), self->doc, FALSE, &extras);
}

static void
action_print_preview (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  w42_view_update_fields (self->view);

  (void) action; (void) param;
  w42_print_document (GTK_WINDOW (self), self->doc, TRUE, NULL);
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
  else if (g_str_equal (what, "column"))
    w42_view_table_select_column (self->view);
  else if (g_str_equal (what, "cell"))
    w42_view_table_select_cell (self->view);
  else
    w42_view_table_select_table (self->view);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_table_insert_row_above (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  w42_view_table_insert_row_above (W42_WINDOW (data)->view);
}

static void
action_table_insert_column_left (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  w42_view_table_insert_column_left (W42_WINDOW (data)->view);
}

static void
action_table_delete_table (GSimpleAction *action, GVariant *param, gpointer data)
{
  (void) action; (void) param;
  w42_view_table_delete_table (W42_WINDOW (data)->view);
}

static void
action_table_autofit (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  const char *what = g_variant_get_string (param, NULL);

  (void) action;
  if (g_str_equal (what, "rows"))
    w42_view_table_distribute_rows (self->view);
  else if (g_str_equal (what, "columns"))
    w42_view_table_distribute_columns (self->view);
  else if (g_str_equal (what, "contents"))
    w42_view_table_autofit_contents (self->view);
  else
    w42_view_table_autofit_window (self->view);
}

static void
action_table_heading_rows (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  /* The first row repeats on every page the table runs on to, or stops. */
  w42_view_table_set_header_rows (self->view,
                                  w42_view_table_get_header_rows (self->view) > 0 ? 0 : 1);
}

static void
action_table_formula (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_formula_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_table_split_table (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  if (!w42_view_table_split_table (self->view))
    window_flash (self, "%s", _("The caret is in the first row: there is nothing above "
                                "it to split off."));
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
    show_message (self, _("There is nothing to convert here."),
                  _("In a table this makes paragraphs with tabs between the cells; "
                    "on selected paragraphs it makes a table, split at their tabs."));
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

/* Insert > Index Entry, and Insert > Index. */
static void
action_index_entry (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_index_entry_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_insert_index (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;

  if (w42_view_caret_in_note (self->view))
    {
      window_flash (self, "%s", _("The caret is in a note: put it in the body of the "
                                  "document first."));
      return;
    }

  if (w42_view_insert_index (self->view) == 0)
    show_message (self, _("There is nothing marked for the index."),
                  _("Select a word and use Insert \u25b8 Index Entry to "
                    "mark it, then ask for the index again."));
}

static void
action_table_of_figures (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;

  if (w42_view_caret_in_note (self->view))
    {
      window_flash (self, "%s", _("The caret is in a note: put it in the body of the "
                                  "document first."));
      return;
    }

  if (w42_view_insert_table_of_figures (self->view) == 0)
    show_message (self, _("There are no captions to list."),
                  _("Insert \u25b8 Caption puts a caption under a picture, "
                    "and the table of figures lists them."));
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
      window_flash (self, "%s", _("The caret is in a note: put it in the body of the "
                                  "document first."));
      return;
    }

  if (w42_view_insert_toc (self->view) == 0)
    show_message (self, _("There are no headings to list."),
                  _("Give the document's headings the Heading 1, 2 or 3 style "
                    "and try again."));
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
    show_message (self, _("There is no table of contents to update."),
                  _("Insert > Table of Contents puts one in."));
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
    show_message (self, _("No hyphenation dictionary was found."),
                  _("Hyphenation needs libhyphen and a hyph_*.dic pattern file "
                    "for your language, the ones LibreOffice uses."));
  else if (n == 0)
    show_message (self, _("There is nothing to hyphenate."), NULL);
}

static void
action_unhyphenate (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  int n;

  (void) action; (void) param;
  n = w42_view_hyphenate (self->view, TRUE);
  if (n > 0)
    window_flash (self, ngettext ("%d hyphen removed.", "%d hyphens removed.",
                                  (unsigned long) n), n);
  else
    window_flash (self, "%s", _("There are no soft hyphens to remove."));
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
  /* A selected drawing gets the Drawing dialog, which knows its shape,
   * outline, fill and text as well as its size. */
  {
    const W42Object *object = w42_view_get_object (self->view);

    if (object != NULL && object->shape != W42_SHAPE_PICTURE)
      {
        w42_drawing_dialog_show (GTK_WINDOW (self), self->view);
        return;
      }
  }
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
action_envelopes (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_envelope_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_autoformat (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_autoformat_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_background (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_background_dialog_show (GTK_WINDOW (self), self->view);
}

static void
action_macros (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_macros_dialog_show (GTK_WINDOW (self), self->view);
}

/* Alt+F11: the editor on the macro last edited, or a first one. */
static void
action_macro_editor (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  char *name = w42_settings_get_string ("last-macro", "Macro1");

  (void) action; (void) param;
  if (!w42_macro_name_ok (name))
    {
      g_free (name);
      name = g_strdup ("Macro1");
    }
  w42_settings_set_string ("last-macro", name);
  w42_macro_editor_show (GTK_WINDOW (self), self->view, name);
  g_free (name);
}

static void
action_autotext (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_autotext_dialog_show (GTK_WINDOW (self), self->view);
}

/* The name typed before the caret becomes the entry it names. */
static void
action_autotext_expand (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  if (!w42_view_expand_autotext (self->view))
    window_flash (self, "%s", _("Type the name of an AutoText entry, then press "
                                "Ctrl+F3.  Edit > AutoText makes one from the selection."));
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
action_language (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_language_dialog_show (GTK_WINDOW (self), self->view, self->spell);
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
    window_flash (self, ngettext ("%d field updated.", "%d fields updated.",
                                  (unsigned long) n), n);
  else
    window_flash (self, "%s", _("There are no fields in this document."));
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

/* The figure's number, if the paragraph at `p` begins as a caption made
 * from `format` ("Figure %d: ") does: what comes before the number, then
 * a digit.  Else 0. */
static int
caption_number (const char *p, const char *format)
{
  const char *hole = strstr (format, "%d");
  gsize len;

  if (hole == NULL || hole == format)
    return 0;
  len = (gsize) (hole - format);
  if (strncmp (p, format, len) != 0 || !g_ascii_isdigit (p[len]))
    return 0;
  return atoi (p + len);
}

/* Insert > Caption: a "Figure N: " paragraph in the Caption style below
 * the current one, N being one more than the figures captioned so far --
 * counting those made in English as well as those made in the language
 * the label is in now. */
static void
action_caption (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  W42PieceTable *pt = w42_document_pt (self->doc);
  char *text = w42_pt_get_text (pt, 0, w42_pt_length (pt));
  /* Translators: the label Insert > Caption puts in the document; %d is
   * the figure's number.  Keep a word before the number, and keep %d as
   * it is: the captions already there are counted by what comes before
   * it. */
  const char *format = _("Figure %d: ");
  int n = 0;
  char *label;

  (void) action; (void) param;

  for (const char *p = text; p != NULL; )
    {
      const char *nl = strchr (p, '\n');

      n = MAX (n, caption_number (p, "Figure %d: "));
      n = MAX (n, caption_number (p, format));
      p = nl != NULL ? nl + 1 : NULL;
    }
  g_free (text);

  label = g_strdup_printf (_("Figure %d: "), n + 1);
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
  /* A note inside a note has nowhere to be numbered from. */
  if (w42_view_caret_in_note (self->view))
    {
      window_flash (self, "%s", _("The caret is in a note: put it in the body of the "
                                  "document first."));
      return;
    }
  w42_view_insert_footnote (self->view);
}

static void
action_insert_endnote (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  /* As Insert Footnote. */
  if (w42_view_caret_in_note (self->view))
    {
      window_flash (self, "%s", _("The caret is in a note: put it in the body of the "
                                  "document first."));
      return;
    }
  w42_view_insert_endnote (self->view);
}

static void
action_go_to_note (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  if (!w42_view_go_to_note (self->view))
    window_flash (self, "%s", _("Put the caret at a note's mark, or in the note itself."));
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

/* Tools > Language > Thesaurus (Shift+F7).  The thesaurus is read the
 * first time it is asked for: its index is a few megabytes, which is
 * not worth every window's start. */
static void
action_thesaurus (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;

  /* The thesaurus of the language being written in: a Norwegian word
   * looked up in an English thesaurus is looked up in vain. */
  {
    const char *lang = w42_view_get_language (self->view);
    const char *have = w42_thesaurus_language (self->thesaurus);

    if (self->thesaurus != NULL && have != NULL && lang != NULL &&
        g_ascii_strncasecmp (have, lang, 2) != 0)
      {
        W42Thesaurus *other = w42_thesaurus_new_for (lang);

        if (other != NULL && g_ascii_strncasecmp (w42_thesaurus_language (other), lang, 2) == 0)
          {
            w42_thesaurus_free (self->thesaurus);
            self->thesaurus = other;
          }
        else
          w42_thesaurus_free (other);
      }
    if (self->thesaurus == NULL)
      self->thesaurus = w42_thesaurus_new_for (lang);
  }
  if (self->thesaurus == NULL)
    {
      show_message (self, _("No thesaurus was found."),
                    _("The thesaurus needs a MyThes file for the language of the "
                      "text -- th_en_US_v2.dat and its .idx for English, "
                      "th_nb_NO_v2 for Norwegian, the ones LibreOffice uses -- "
                      "in the mythes folder."));
      return;
    }
  if (!w42_thesaurus_dialog_show (GTK_WINDOW (self), self->view, self->thesaurus))
    window_flash (self, "%s", _("Put the caret in a word to look it up in the thesaurus."));
}

static void
action_spelling (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;

  if (self->spell == NULL)
    {
      show_message (self, _("No spelling dictionary was found."),
                    _("Install a Hunspell dictionary for your language and "
                      "start Word42 again."));
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
    window_flash (self, "%s", _("Revisions accepted."));
  else
    window_flash (self, "%s", _("There are no revision marks in this document."));
}

static void
action_reject_revisions (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  if (w42_view_resolve_revisions (self->view, FALSE))
    window_flash (self, "%s", _("Revisions rejected."));
  else
    window_flash (self, "%s", _("There are no revision marks in this document."));
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
  /* Refused in the text of a note, as the other commands that put
   * structure into the document are. */
  if (w42_view_caret_in_note (self->view))
    {
      window_flash (self, "%s", _("The caret is in a note: put it in the body of the "
                                  "document first."));
      return;
    }
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
    /* Translators: the Windows version in the About box, as "Windows
     * 10.0 build 19045 (Windows 10)": the major and minor version, the
     * build number, then the release's name in brackets or nothing. */
    return g_strdup_printf (_("Windows %lu.%lu build %lu%s"),
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
    /* Translators: what the About box says this copy runs on, for bug
     * reports.  The version numbers and library names fill the
     * placeholders in turn: Word42's version; GTK's running version and
     * the one built against; GLib's; Pango's, Cairo's and GdkPixbuf's;
     * the operating system; then the spelling, hyphenation and PDF
     * import libraries, or "none". */
    _("Word42 %s\n"
      "GTK %u.%u.%u (built against %d.%d.%d)  \u00b7  GLib %u.%u.%u\n"
      "Pango %s  \u00b7  Cairo %s  \u00b7  GdkPixbuf %s\n"
      "%s\n"
      "Spelling: %s  \u00b7  Hyphenation: %s  \u00b7  PDF import: %s"),
    W42_VERSION,
    gtk_get_major_version (), gtk_get_minor_version (), gtk_get_micro_version (),
    GTK_MAJOR_VERSION, GTK_MINOR_VERSION, GTK_MICRO_VERSION,
    glib_major_version, glib_minor_version, glib_micro_version,
    pango_version_string (), cairo_version_string (), gdk_pixbuf_version,
    os,
#ifdef HAVE_ENCHANT
    "Enchant",
#else
    /* Translators: no library for this, in the About box. */
    C_("library", "none"),
#endif
#ifdef HAVE_HYPHEN
    "libhyphen",
#else
    /* Translators: no library for this, in the About box. */
    C_("library", "none"),
#endif
#ifdef HAVE_POPPLER
    "Poppler"
#else
    /* Translators: no library for this, in the About box. */
    C_("library", "none")
#endif
    );

  g_free (os);
  return out;
}

/* The address in the About box is a link, and it opens where Help â¸
 * Word42 on the Web opens: whatever the desktop uses for the web.  GTK
 * would launch it itself, but going through the launcher keeps the box
 * the parent window, so the browser comes up over it. */
static gboolean
on_about_link (GtkLabel *label, const char *uri, gpointer data)
{
  GtkUriLauncher *launcher = gtk_uri_launcher_new (uri);

  (void) label;
  gtk_uri_launcher_launch (launcher, GTK_WINDOW (data), NULL, NULL, NULL);
  g_object_unref (launcher);
  return TRUE;
}

static void
action_about (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkWidget *window, *box, *banner, *version, *blurb, *link, *licence, *button;

  (void) action; (void) param;

  window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (window), _("About Word42"));
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

  {
    /* Translators: %s is Word42's version number. */
    char *text = g_strdup_printf (_("Version %s"), W42_VERSION);

    version = gtk_label_new (text);
    g_free (text);
  }
  gtk_widget_add_css_class (version, "w42-about-version");
  gtk_label_set_xalign (GTK_LABEL (version), 0.0);
  gtk_widget_set_margin_start (version, 20);
  gtk_widget_set_margin_top (version, 16);
  gtk_box_append (GTK_BOX (box), version);

  blurb = gtk_label_new (_("Written in C on GTK 4, Pango and Cairo."));
  gtk_label_set_xalign (GTK_LABEL (blurb), 0.0);
  gtk_widget_set_margin_start (blurb, 20);
  gtk_widget_set_margin_top (blurb, 8);
  gtk_box_append (GTK_BOX (box), blurb);

  link = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (link),
                        "<a href=\"https://word42.org\">word42.org</a>");
  gtk_label_set_xalign (GTK_LABEL (link), 0.0);
  gtk_widget_set_halign (link, GTK_ALIGN_START);   /* the focus rectangle hugs the address */
  gtk_widget_add_css_class (link, "w42-about-link");
  gtk_widget_set_margin_start (link, 20);
  gtk_widget_set_margin_top (link, 4);
  g_signal_connect (link, "activate-link", G_CALLBACK (on_about_link), window);
  gtk_box_append (GTK_BOX (box), link);

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

  licence = gtk_label_new (_(
    "Copyright (C) 2026 Andreas Røsdal.\n\n"
    "This program is free software: you can redistribute it and/or modify it "
    "under the terms of the GNU General Public License as published by the "
    "Free Software Foundation, either version 3 of the License, or (at your "
    "option) any later version.  It comes with ABSOLUTELY NO WARRANTY.\n\n"
    "Word42 is an independent program, not affiliated with or endorsed by "
    "the makers of any other word processor.  The names of file formats "
    "appear only to say which format is meant.\n\n"
    "Macros run on MY-BASIC by Tony Wang, used under the MIT licence."));
  gtk_label_set_wrap (GTK_LABEL (licence), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (licence), 52);
  gtk_label_set_xalign (GTK_LABEL (licence), 0.0);
  gtk_widget_add_css_class (licence, "w42-about-licence");
  gtk_widget_set_margin_start (licence, 20);
  gtk_widget_set_margin_end (licence, 20);
  gtk_widget_set_margin_top (licence, 14);
  gtk_box_append (GTK_BOX (box), licence);

  button = gtk_button_new_with_mnemonic (_("_OK"));
  gtk_widget_set_halign (button, GTK_ALIGN_END);
  gtk_widget_set_margin_end (button, 20);
  gtk_widget_set_margin_top (button, 18);
  gtk_widget_set_margin_bottom (button, 16);
  gtk_widget_set_size_request (button, 88, 26);
  g_signal_connect_swapped (button, "clicked",
                            G_CALLBACK (gtk_window_destroy), window);
  gtk_box_append (GTK_BOX (box), button);

  gtk_window_present (GTK_WINDOW (window));
  gtk_widget_grab_focus (button);
}

/* ---------------------------------------------------------------------- */
/* Title bar                                                               */
/* ---------------------------------------------------------------------- */

/* Word 97's title bar was navy with its name on it in white, and the
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
                  caption_button ("minimise", _("Minimize"),
                                  G_CALLBACK (on_titlebar_minimise), self));
  gtk_box_append (GTK_BOX (right),
                  caption_button ("maximise", _("Maximize"),
                                  G_CALLBACK (on_titlebar_maximise), self));
  gtk_box_append (GTK_BOX (right),
                  caption_button ("close", _("Close"),
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
  if (index == GTK_INVALID_LIST_POSITION || index >= ZOOM_N_FIXED)
    return;

  if (index < G_N_ELEMENTS (ZOOM_STEPS))
    w42_view_set_zoom (self->view, ZOOM_STEPS[index]);
  else
    w42_view_set_zoom (self->view,
                       w42_view_fit_zoom (self->view,
                                          index - G_N_ELEMENTS (ZOOM_STEPS) == 1));
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

/* The Zoom box shows the zoom the pane being edited has: one of its
 * steps, or an entry of its own for any other figure. */
static void
window_sync_zoom_box (W42Window *self)
{
  double zoom;
  GtkStringList *list;
  guint n;
  guint want = GTK_INVALID_LIST_POSITION;

  if (self->zoom_drop == NULL || self->view == NULL)
    return;
  zoom = w42_view_get_zoom (self->view);
  list = GTK_STRING_LIST (gtk_drop_down_get_model (GTK_DROP_DOWN (self->zoom_drop)));
  n = g_list_model_get_n_items (G_LIST_MODEL (list));

  for (guint i = 0; i < G_N_ELEMENTS (ZOOM_STEPS); i++)
    if (ABS (ZOOM_STEPS[i] - zoom) < 0.005)
      want = i;

  if (want == GTK_INVALID_LIST_POSITION)
    {
      char label[16];
      const char *labels[] = { label, NULL };

      g_snprintf (label, sizeof label, "%d%%", (int) lround (zoom * 100));
      gtk_string_list_splice (list, ZOOM_N_FIXED, n - ZOOM_N_FIXED, labels);
      want = ZOOM_N_FIXED;
    }
  else if (n > ZOOM_N_FIXED)
    gtk_string_list_splice (list, ZOOM_N_FIXED, n - ZOOM_N_FIXED, NULL);

  if (gtk_drop_down_get_selected (GTK_DROP_DOWN (self->zoom_drop)) != want)
    gtk_drop_down_set_selected (GTK_DROP_DOWN (self->zoom_drop), want);
}

static GtkWidget *
build_standard_bar (void)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class (bar, "w42-toolbar");

  gtk_box_append (GTK_BOX (bar), tool_button ("w42-new",           _("New"),           "win.new"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-open",          _("Open"),          "win.open"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-save",          _("Save"),          "win.save"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-print",         _("Print"),         "win.print"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-print-preview", _("Print Preview"), "win.print-preview"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-spelling",      _("Spelling"),      "win.spelling"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-cut",           _("Cut"),           "win.cut"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-copy",          _("Copy"),          "win.copy"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-paste",         _("Paste"),         "win.paste"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-undo",          _("Undo"),          "win.undo"));
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-redo",          _("Redo"),          "win.redo"));
  gtk_box_append (GTK_BOX (bar), tool_separator ());
  gtk_box_append (GTK_BOX (bar), tool_button ("w42-find",          _("Find"),          "win.find"));

  return bar;
}

/* Word 97 kept the zoom control at the right-hand end of the Standard bar. */
static GtkWidget *
build_zoom_drop (W42Window *self)
{
  GtkStringList *steps = gtk_string_list_new (NULL);

  for (guint i = 0; i < G_N_ELEMENTS (ZOOM_LABELS); i++)
    gtk_string_list_append (steps, ZOOM_LABELS[i]);
  for (guint i = 0; i < G_N_ELEMENTS (ZOOM_FIT_LABELS); i++)
    gtk_string_list_append (steps, _(ZOOM_FIT_LABELS[i]));

  self->zoom_drop = gtk_drop_down_new (G_LIST_MODEL (steps), NULL);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (self->zoom_drop), 2);
  gtk_widget_set_size_request (self->zoom_drop, 72, -1);
  gtk_widget_set_tooltip_text (self->zoom_drop, _("Zoom Control"));
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
  const char *align_names[4] = {
    _("Align Left"),
    /* Translators: the tooltip of the button that centres the paragraph. */
    C_("alignment", "Center"),
    _("Align Right"), _("Justify")
  };

  gtk_widget_add_css_class (bar, "w42-toolbar");

  {
    /* The Style box, at the left end as Word 97 had it. */
    self->style_list = gtk_string_list_new (NULL);
    self->style_drop = gtk_drop_down_new (g_object_ref (G_LIST_MODEL (self->style_list)), NULL);
    gtk_widget_set_size_request (self->style_drop, 120, -1);
    gtk_widget_set_tooltip_text (self->style_drop, _("Style"));
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
  gtk_widget_set_tooltip_text (self->font_drop, _("Font"));
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
  gtk_widget_set_tooltip_text (self->size_drop, _("Font Size"));
  g_signal_connect (self->size_drop, "notify::selected",
                    G_CALLBACK (on_size_selected), self);
  gtk_box_append (GTK_BOX (bar), self->size_drop);

  gtk_box_append (GTK_BOX (bar), tool_separator ());

  self->bold_btn      = toggle_button ("w42-bold", _("Bold"));
  self->italic_btn    = toggle_button ("w42-italic", _("Italic"));
  self->underline_btn = toggle_button ("w42-underline", _("Underline"));

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

  self->numbers_btn = toggle_button ("w42-numbering", _("Numbering"));
  self->bullets_btn = toggle_button ("w42-bullets", _("Bullets"));
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

  {
    char buffer[64];

    g_snprintf (buffer, sizeof buffer, _("Page %d"), 1);
    self->status_page = gtk_label_new (buffer);
    g_snprintf (buffer, sizeof buffer, _("At %.1f%s"), 1.0, "\"");
    self->status_at   = gtk_label_new (buffer);
    g_snprintf (buffer, sizeof buffer, _("Ln %d"), 1);
    self->status_ln   = gtk_label_new (buffer);
    g_snprintf (buffer, sizeof buffer, _("Col %d"), 1);
    self->status_col  = gtk_label_new (buffer);
  }
  self->status_pages = gtk_label_new ("1/1");
  self->status_words = gtk_label_new ("");
  self->status_mod  = gtk_label_new ("");

  gtk_widget_set_hexpand (self->status_mod, TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->status_mod), 0.0);

  /* Each reading sits in its own sunken well, as Word 97's readings did. */
  {
    GtkWidget *cells[] = { self->status_page, self->status_pages,
                           self->status_at, self->status_ln,
                           self->status_col, self->status_words,
                           self->status_mod };

    for (guint i = 0; i < G_N_ELEMENTS (cells); i++)
      {
        gtk_widget_add_css_class (cells[i], "w42-status-cell");
        gtk_label_set_xalign (GTK_LABEL (cells[i]), 0.0);
        gtk_widget_set_size_request (cells[i],
                                     cells[i] == self->status_mod ? -1
                                     : cells[i] == self->status_words ? 150 : 66,
                                     -1);
        gtk_box_append (GTK_BOX (bar), cells[i]);
      }
  }

  return bar;
}

/* ---------------------------------------------------------------------- */
/* The count of words and pages                                            */
/* ---------------------------------------------------------------------- */

/* 45000 as 45,000: a count an author reads at a glance. */
static char *
count_text (gsize n)
{
  char *digits = g_strdup_printf ("%" G_GSIZE_FORMAT, n);
  GString *out = g_string_new (NULL);
  gsize len = strlen (digits);

  for (gsize i = 0; i < len; i++)
    {
      if (i > 0 && (len - i) % 3 == 0)
        g_string_append_c (out, ',');
      g_string_append_c (out, digits[i]);
    }
  g_free (digits);
  return g_string_free (out, FALSE);
}

/* A goal belongs with its manuscript, and is remembered by the file's
 * path; an untitled document keeps its goal in the window until it has
 * one. */
static char *
goal_key (GFile *file)
{
  char *path = file != NULL ? g_file_get_path (file) : NULL;
  char *key = path != NULL ? g_strdup_printf ("goal-%08x", g_str_hash (path)) : NULL;

  g_free (path);
  return key;
}

static void
window_load_goal (W42Window *self)
{
  char *key = goal_key (w42_document_get_file (self->doc));

  if (key != NULL)
    self->goal = MAX (w42_settings_get_int (key, 0), 0);
  g_free (key);
}

static void
window_store_goal (W42Window *self)
{
  char *key = goal_key (w42_document_get_file (self->doc));

  if (key != NULL)
    w42_settings_set_int (key, self->goal);
  g_free (key);
}

static void
window_show_counts (W42Window *self)
{
  char *words, *text, *tip;

  if (!self->count_valid || self->status_words == NULL)
    return;

  words = count_text (self->words);
  if (self->goal > 0)
    {
      char *goal = count_text ((gsize) self->goal);

      /* Translators: the status bar's word count toward the goal set in
       * Tools > Word Count Goal: "12,000 of 80,000 words".  The first %s
       * is the words written, the second the goal. */
      text = g_strdup_printf (ngettext ("%s of %s word", "%s of %s words",
                                        (unsigned long) self->goal),
                              words, goal);
      g_free (goal);
    }
  else
    /* Translators: the status bar's word count; %s is the number, with
     * thousands separators. */
    text = g_strdup_printf (ngettext ("%s word", "%s words", (unsigned long) self->words),
                            words);
  gtk_label_set_text (GTK_LABEL (self->status_words), text);

  {
    GString *t = g_string_new (NULL);

    if (self->goal > 0)
      {
        /* Translators: in the word count's tooltip; %d%% is how far
         * toward the goal the document is, as "40%". */
        g_string_append_printf (t, _("%d%% of the goal."),
                                (int) MIN (self->words * 100 / (gsize) self->goal, 999));
        g_string_append_c (t, ' ');
      }
    if (self->words_at_open >= 0)
      {
        gssize session = (gssize) self->words - self->words_at_open;

        /* Translators: in the word count's tooltip; %ld is the words
         * written since the document was opened, which can be less than
         * nothing when more was deleted. */
        g_string_append_printf (t, ngettext ("Written since the document was opened: %ld word.",
                                             "Written since the document was opened: %ld words.",
                                             (unsigned long) ABS (session)),
                                (long) session);
      }
    g_string_append_c (t, '\n');
    g_string_append (t, _("Tools \342\226\270 Word Count Goal sets the goal."));
    tip = g_string_free (t, FALSE);
  }
  gtk_widget_set_tooltip_text (self->status_words, tip);
  g_free (tip);
  g_free (text);
  g_free (words);
}

static gboolean
on_count (gpointer data)
{
  W42Window *self = data;
  W42PieceTable *pt = w42_document_pt (self->doc);
  W42Stats stats;

  self->count_id = 0;
  w42_pt_undo_state (pt, &self->count_undo_pos, &self->count_serial);
  w42_pt_statistics (pt, FALSE, &stats);
  self->words = stats.words;
  if (self->words_at_open < 0)
    self->words_at_open = (gssize) stats.words;
  self->count_valid = TRUE;

  /* Normal and Online Layout show one long galley; the page a writer is
   * on, and how many there are, are the printed ones, so a paginated
   * layout is kept beside it.  It keeps its own shaped paragraphs, so
   * after the first count it costs what a keystroke costs. */
  if (w42_layout_get_galley (w42_view_get_layout (self->view)))
    {
      if (self->count_layout == NULL)
        self->count_layout = w42_layout_new ();
      w42_layout_build (self->count_layout, self->doc);
      self->n_pages = w42_layout_n_pages (self->count_layout);
    }
  else
    {
      g_clear_pointer (&self->count_layout, w42_layout_free);
      self->n_pages = w42_layout_n_pages (w42_view_get_layout (self->view));
    }

  window_show_counts (self);
  window_sync_state (self);
  return G_SOURCE_REMOVE;
}

/* Counts again once the typing has paused, if anything has changed. */
static void
window_schedule_count (W42Window *self)
{
  gsize pos;
  guint64 serial;

  w42_pt_undo_state (w42_document_pt (self->doc), &pos, &serial);
  if (self->count_valid && pos == self->count_undo_pos && serial == self->count_serial &&
      (self->count_layout != NULL) == w42_layout_get_galley (w42_view_get_layout (self->view)))
    return;
  if (self->count_id != 0)
    g_source_remove (self->count_id);
  self->count_id = g_timeout_add (self->count_valid ? 700 : 50, on_count, self);
}

static void
on_goal_set (int goal, gpointer data)
{
  W42Window *self = data;

  self->goal = MAX (goal, 0);
  window_store_goal (self);
  window_show_counts (self);
}

static void
action_word_goal (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;

  (void) action; (void) param;
  w42_goal_dialog_show (GTK_WINDOW (self), self->view, self->goal,
                        self->count_valid ? self->words : 0,
                        self->count_valid && self->words_at_open >= 0
                          ? (gssize) self->words - self->words_at_open : -1,
                        on_goal_set, self);
}

/* The spelling follows the document's language: Normal's, set with
 * Tools > Language > Default, else the desktop's. */
static void
window_sync_language (W42Window *self)
{
  const char *lang = w42_stylesheet_language (w42_pt_stylesheet (w42_document_pt (self->doc)));

  if (self->spell == NULL || lang == self->spell_lang)
    return;
  self->spell_lang = lang;
  w42_spell_set_language (self->spell, lang);
  if (self->view1 != NULL)
    w42_view_spell_refresh (self->view1);
  if (self->view2 != NULL)
    w42_view_spell_refresh (self->view2);
}

/* ---------------------------------------------------------------------- */
/* Keeping the chrome in step with the document                            */
/* The Window menu lists the open documents, numbered as Word 97 did. */
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
      char *shown = mnemonic_escape (name);
      char *label = g_strdup_printf ("_%d %s%s", index + 1, shown,
                                     w42_document_get_modified (w->doc) ? "*" : "");
      GMenuItem *item = g_menu_item_new (label, NULL);

      g_free (shown);

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
      if (file == NULL || !w42_io_format_round_trips (file))
        {
          /* Never saved, or read from a format Save turns into Save As
           * for: either way it needs a name of its own. */
          asked++;
          continue;
        }
      {
        GError *error = NULL;

        if (window_save_document (w->doc, file, &error))
          {
            window_saved (w, TRUE);
            saved++;
          }
        else
          {
            show_error (w, _("Word42 could not save that file."), error);
            g_clear_error (&error);
          }
      }
    }
  if (asked > 0)
    show_message (self, saved > 0 ? _("The rest are saved.") : _("Nothing was saved."),
                  _("A document that has never been saved, or that came from a "
                    "format Word42 does not write back, needs a name: use "
                    "File > Save As for it."));
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

  if (window_gone (self))
    {
      g_clear_object (&file);
      g_clear_error (&error);
      g_object_unref (self);
      return;
    }

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
        show_error (self, _("Word42 could not read that file."), error);
      w42_pt_free (other);
      g_object_unref (file);
    }
  g_clear_error (&error);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
  g_object_unref (self);
}

/* Tools > Track Changes > Compare Documents: the file chosen is the
 * original, and what this document has that it had not is marked
 * inserted, what it had that this has not is put back marked deleted. */
static void
on_compare_response (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Window *self = data;
  GError *error = NULL;
  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), result, &error);

  if (window_gone (self))
    {
      g_clear_object (&file);
      g_clear_error (&error);
      g_object_unref (self);
      return;
    }

  if (file != NULL)
    {
      W42PieceTable *other = w42_pt_new ();

      if (w42_io_load (other, NULL, file, &error))
        {
          int n = w42_view_compare_with (self->view, other);

          if (n == 0)
            window_flash (self, "%s", _("The two documents are the same."));
          else
            window_flash (self, ngettext ("%d change marked.",
                                          "%d changes marked.", (unsigned long) n), n);
        }
      else
        show_error (self, _("Word42 could not read that file."), error);
      w42_pt_free (other);
      g_object_unref (file);
    }
  else if (error != NULL && !g_error_matches (error, GTK_DIALOG_ERROR,
                                              GTK_DIALOG_ERROR_DISMISSED))
    show_error (self, _("Word42 could not open that file."), error);
  g_clear_error (&error);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
  g_object_unref (self);
}

static void
action_compare_documents (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListModel *filters = file_filters (FALSE);

  (void) action; (void) param;
  gtk_file_dialog_set_title (dialog, _("Compare Documents: the original"));
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_compare_response,
                        g_object_ref (self));
  g_object_unref (filters);
  g_object_unref (dialog);
}

static void
action_insert_file (GSimpleAction *action, GVariant *param, gpointer data)
{
  W42Window *self = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListModel *filters = file_filters (FALSE);

  (void) action; (void) param;
  gtk_file_dialog_set_title (dialog, _("Insert File"));
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, on_insert_file_response,
                        g_object_ref (self));
  g_object_unref (filters);
  g_object_unref (dialog);
}

/* View > Full Screen gives the page the whole screen: the menu bar, the
 * two toolbars, the ruler and the status bar go away, and come back as
 * they were.  Escape brings them back, as it did in the classics. */
static void
window_set_full_screen (W42Window *self, gboolean on)
{
  GtkWidget *chrome[5];
  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), "full-screen");

  chrome[0] = self->menubar;
  chrome[1] = self->standard_bar;
  chrome[2] = self->format_bar;
  chrome[3] = self->ruler;
  chrome[4] = self->doc_map;

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
  window_sync_zoom_box (self);

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

      if (found == NULL)
        {
          /* A face the machine does not have -- a Word file's Calibri on
           * a machine without it -- is still what the document says,
           * and the box says so, as Word's does, rather than showing
           * whichever installed face happens to come first.  The name
           * joins the list, at the end, so the drop-down can show it. */
          guint n = g_list_model_get_n_items (self->families);

          gtk_string_list_append (GTK_STRING_LIST (self->families), fmt.family);
          g_hash_table_insert (self->family_index, (gpointer) fmt.family,
                               GUINT_TO_POINTER (n + 1));
          found = GUINT_TO_POINTER (n + 1);
        }
      gtk_drop_down_set_selected (GTK_DROP_DOWN (self->font_drop),
                                  GPOINTER_TO_UINT (found) - 1);
    }

  w42_layout_describe_pos (layout, w42_view_get_caret (self->view),
                           &page, &line, &column);

  /* In a galley the page is the printed one, from the layout kept for
   * counting. */
  if (self->count_layout != NULL && w42_layout_get_galley (layout))
    {
      int pline = 1, pcolumn = 1;

      w42_layout_describe_pos (self->count_layout, w42_view_get_caret (self->view),
                               &page, &pline, &pcolumn);
    }
  /* Translators: the status bar's page number. */
  g_snprintf (buffer, sizeof buffer, _("Page %d"), page);
  gtk_label_set_text (GTK_LABEL (self->status_page), buffer);
  g_snprintf (buffer, sizeof buffer, "%d/%d", page,
              MAX (self->n_pages > 0 ? self->n_pages : w42_layout_n_pages (layout), page));
  gtk_label_set_text (GTK_LABEL (self->status_pages), buffer);
  window_schedule_count (self);
  window_sync_language (self);
  /* Translators: the status bar's line number, "Ln" short for Line. */
  g_snprintf (buffer, sizeof buffer, _("Ln %d"), line);
  gtk_label_set_text (GTK_LABEL (self->status_ln), buffer);
  /* Translators: the status bar's column number, "Col" short for
   * Column: how many characters in from the start of the line. */
  g_snprintf (buffer, sizeof buffer, _("Col %d"), column);
  gtk_label_set_text (GTK_LABEL (self->status_col), buffer);

  {
    int cpage = 0;
    double x = 0, y = 0, h = 0;

    if (w42_layout_pos_to_caret (layout, w42_view_get_caret (self->view),
                                 &cpage, &x, &y, &h))
      {
        /* The distance from the top of the page, in the user's unit. */
        /* Translators: the status bar's reading of how far down the page
         * the caret is, "At" short for "at so far from the top": %.1f is
         * the distance, %s the unit, already translated (" or cm). */
        g_snprintf (buffer, sizeof buffer, _("At %.1f%s"),
                    w42_settings_from_twips ((int) (y * W42_TWIPS_PER_PX)),
                    w42_settings_unit_name ());
        gtk_label_set_text (GTK_LABEL (self->status_at), buffer);
      }
  }

  gtk_label_set_text (GTK_LABEL (self->status_mod),
                      self->status_flash != NULL ? self->status_flash :
                      w42_document_get_modified (self->doc) ? _("Modified") : "");

  has_sel = w42_view_has_selection (self->view);

  window_refresh_window_list (self);

  {
    gboolean in_table = w42_view_in_table (self->view);
    static const char *table_actions[] = { "table-insert-rows", "table-delete-rows",
                                           "table-insert-cols", "table-delete-cols", "table-split",
                                           "table-merge", "table-properties", "table-autoformat",
                                           "table-select", "table-split-table", "table-sort",
                                           "table-insert-rows-above", "table-insert-cols-left",
                                           "table-delete-table", "table-autofit",
                                           "table-heading-rows", "table-formula" };

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
    /* Revert has something to go back to only when the document came
     * from a file and has been changed since. */
    GAction *a = g_action_map_lookup_action (G_ACTION_MAP (self), "revert");

    if (a != NULL)
      g_simple_action_set_enabled (G_SIMPLE_ACTION (a),
                                   w42_document_get_file (self->doc) != NULL &&
                                   w42_document_get_modified (self->doc));
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

    a = g_action_map_lookup_action (G_ACTION_MAP (self), "repeat");
    if (a != NULL)
      g_simple_action_set_enabled (G_SIMPLE_ACTION (a),
                                   w42_view_can_repeat (self->view));
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
  { "revert",     action_revert,     NULL, NULL,    NULL, { 0 } },
  { "save",       action_save,       NULL, NULL,    NULL, { 0 } },
  { "save-as",    action_save_as,    NULL, NULL,    NULL, { 0 } },
  { "new-from-template", action_new_from_template, NULL, NULL, NULL, { 0 } },
  { "save-as-template",  action_save_as_template,  NULL, NULL, NULL, { 0 } },
  { "close",      action_close,      NULL, NULL,    NULL, { 0 } },
  { "undo",       action_undo,       NULL, NULL,    NULL, { 0 } },
  { "redo",       action_redo,       NULL, NULL,    NULL, { 0 } },
  { "repeat",     action_repeat,     NULL, NULL,    NULL, { 0 } },
  { "cut",        action_cut,        NULL, NULL,    NULL, { 0 } },
  { "copy",       action_copy,       NULL, NULL,    NULL, { 0 } },
  { "paste",      action_paste,      NULL, NULL,    NULL, { 0 } },
  { "select-all", action_select_all, NULL, NULL,    NULL, { 0 } },
  { "bold",       action_bold,       NULL, NULL,    NULL, { 0 } },
  { "italic",     action_italic,     NULL, NULL,    NULL, { 0 } },
  { "underline",  action_underline,  NULL, NULL,    NULL, { 0 } },
  { "align",      action_align,      "s",  NULL,    NULL, { 0 } },
  { "zoom",       action_zoom,       "d",  NULL,    NULL, { 0 } },
  { "zoom-fit",   action_zoom_fit,   "s",  NULL,    NULL, { 0 } },
  { "zoom-dialog", action_zoom_dialog, NULL, NULL,  NULL, { 0 } },
  { "macros",     action_macros,     NULL, NULL,    NULL, { 0 } },
  { "macro-editor", action_macro_editor, NULL, NULL, NULL, { 0 } },
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
  { "insert-scan", action_insert_scan, NULL, NULL, NULL, { 0 } },
  { "print-preview", action_print_preview, NULL, NULL, NULL, { 0 } },
  { "page-setup", action_page_setup, NULL, NULL,    NULL, { 0 } },
  { "table-insert",      action_table_insert,     NULL, NULL, NULL, { 0 } },
  { "table-insert-rows", action_table_insert_row, NULL, NULL, NULL, { 0 } },
  { "table-insert-rows-above", action_table_insert_row_above, NULL, NULL, NULL, { 0 } },
  { "table-insert-cols-left", action_table_insert_column_left, NULL, NULL, NULL, { 0 } },
  { "table-delete-table", action_table_delete_table, NULL, NULL, NULL, { 0 } },
  { "table-autofit", action_table_autofit, "s", NULL, NULL, { 0 } },
  { "table-heading-rows", action_table_heading_rows, NULL, NULL, NULL, { 0 } },
  { "table-formula", action_table_formula, NULL, NULL, NULL, { 0 } },
  { "table-delete-rows", action_table_delete_row, NULL, NULL, NULL, { 0 } },
  { "table-insert-cols", action_table_insert_column, NULL, NULL, NULL, { 0 } },
  { "table-delete-cols", action_table_delete_column, NULL, NULL, NULL, { 0 } },
  { "table-properties", action_table_properties, NULL, NULL, NULL, { 0 } },
  { "table-autoformat", action_table_autoformat, NULL, NULL, NULL, { 0 } },
  { "language",     action_language,     NULL, NULL, NULL, { 0 } },
  { "autotext",     action_autotext,     NULL, NULL, NULL, { 0 } },
  { "background",   action_background,   NULL, NULL, NULL, { 0 } },
  { "autoformat",   action_autoformat,   NULL, NULL, NULL, { 0 } },
  { "envelopes",    action_envelopes,    NULL, NULL, NULL, { 0 } },
  { "autotext-expand", action_autotext_expand, NULL, NULL, NULL, { 0 } },
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
  { "thesaurus",  action_thesaurus,  NULL, NULL,   NULL, { 0 } },
  { "go-to",         action_go_to,         NULL, NULL, NULL, { 0 } },
  { "new-window",    action_new_window,    NULL, NULL, NULL, { 0 } },
  { "split-window",  action_split_window,  NULL, "false", NULL, { 0 } },
  { "options",       action_options,       NULL, NULL, NULL, { 0 } },
  { "help-contents", action_help_contents, NULL, NULL, NULL, { 0 } },
  { "help-search",  action_help_search,  NULL, NULL, NULL, { 0 } },
  { "help-index",   action_help_index,   NULL, NULL, NULL, { 0 } },
  { "help-web",     action_help_web,     NULL, NULL, NULL, { 0 } },
  { "report-bug",   action_report_bug,   NULL, NULL, NULL, { 0 } },
  { "tabs",          action_tabs,          NULL, NULL, NULL, { 0 } },
  { "borders",       action_borders,       NULL, NULL, NULL, { 0 } },
  { "insert-footnote", action_insert_footnote, NULL, NULL, NULL, { 0 } },
  { "hyperlink",     action_hyperlink,     NULL, NULL, NULL, { 0 } },
  { "effects",       action_effects,       NULL, NULL, NULL, { 0 } },
  { "columns",       action_columns,       NULL, NULL, NULL, { 0 } },
  { "insert-toc",    action_insert_toc,    NULL, NULL, NULL, { 0 } },
  { "index-entry",   action_index_entry,   NULL, NULL, NULL, { 0 } },
  { "insert-index",  action_insert_index,  NULL, NULL, NULL, { 0 } },
  { "open-recent",   action_open_recent,   "s",  NULL, NULL, { 0 } },
  { "export-html",   action_export_html,   NULL, NULL, NULL, { 0 } },
  { "export-epub",   action_export_epub,   NULL, NULL, NULL, { 0 } },
  { "web-preview",   action_web_preview,   NULL, NULL, NULL, { 0 } },
  { "table-of-figures", action_table_of_figures, NULL, NULL, NULL, { 0 } },
  { "compare-documents", action_compare_documents, NULL, NULL, NULL, { 0 } },
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
  { "word-goal",  action_word_goal,  NULL, NULL,    NULL, { 0 } },
  { "about",      action_about,      NULL, NULL,    NULL, { 0 } },
  { "ruler",       action_toggle_ruler,   NULL, "true", NULL, { 0 } },
  { "typewriter",  action_typewriter,     NULL, "false", NULL, { 0 } },
  { "document-map", action_document_map,  NULL, "false", NULL, { 0 } },
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
  g_clear_pointer (&self->thesaurus, w42_thesaurus_free);

  if (self->autosave_id != 0)
    {
      g_source_remove (self->autosave_id);
      self->autosave_id = 0;
    }
  if (self->count_id != 0)
    {
      g_source_remove (self->count_id);
      self->count_id = 0;
    }
  g_clear_pointer (&self->count_layout, w42_layout_free);
  /* A window that closes in the ordinary way needs no recovering -- but
   * a document still open in another window does, and the copy on disk
   * may be the one they share.  That window writes its own on its next
   * turn. */
  if (self->doc != NULL && window_document_shared (self))
    {
      GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));

      for (GList *l = app != NULL ? gtk_application_get_windows (app) : NULL;
           l != NULL; l = l->next)
        if (l->data != self && W42_IS_WINDOW (l->data) &&
            W42_WINDOW (l->data)->doc == self->doc)
          W42_WINDOW (l->data)->autosave_dirty = TRUE;
      g_clear_pointer (&self->autosave_path, g_free);
    }
  else if (self->doc != NULL)
    autosave_remove (self);

  g_clear_object (&self->recent_menu);
  g_clear_object (&self->style_list);   /* the drop-down has its own */

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
  GtkWidget *box, *right, *scrolled, *menubar;
  GtkBuilder *builder;
  GMenuModel *model;

  static guint next_serial;

  self->serial = ++next_serial;
  self->doc = w42_document_new ();
  w42_pt_set_author (w42_document_pt (self->doc), window_author_name ());
  self->words_at_open = -1;

  w42_window_apply_default_language (self->doc);

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

  /* Every window opens maximised; the default size is what Restore Down
   * gives back. */
  gtk_window_set_default_size (GTK_WINDOW (self), 900, 780);
  gtk_window_maximize (GTK_WINDOW (self));
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
  self->view1 = self->view;
  window_track_pane_focus (self, self->view);

  /* Check spelling as you type, when there is a dictionary to check it
   * against; otherwise the toggle is greyed out rather than lying. */
  w42_view_set_show_marks (self->view, FALSE);

  /* Tools > Options: correct as you type, as it was left last time. */
  w42_view_set_autocorrect (self->view, w42_settings_get_bool ("auto-correct", TRUE));

  {
    /* View > Typewriter Scrolling, as it was left last time. */
    gboolean on = w42_settings_get_bool ("typewriter", FALSE);
    GAction *typewriter = g_action_map_lookup_action (G_ACTION_MAP (self), "typewriter");

    w42_view_set_typewriter (self->view, on);
    if (typewriter != NULL)
      g_simple_action_set_state (G_SIMPLE_ACTION (typewriter), g_variant_new_boolean (on));
  }

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


  window_start_autosave (self);

  self->format_bar = build_format_bar (self);
  gtk_box_append (GTK_BOX (box), self->format_bar);

  self->ruler = w42_ruler_new (self->view);
  self->doc_map = w42_docmap_new (self->view);
  window_apply_settings (self);   /* now that the bars it sets exist */

  /* The ruler sits over the page, not over the Document Map, so the two
   * share a box at the right of the map: the ruler's zero is then the
   * page's, whatever the map's width. */
  right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand (right, TRUE);
  gtk_box_append (GTK_BOX (right), self->ruler);

  scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scrolled, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled),
                                 GTK_WIDGET (self->view));

  /* The page sits in a paned so that Window > Split can put a second
   * pane on the same document under it; unsplit, the paned is just the
   * one pane. */
  self->paned = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_vexpand (self->paned, TRUE);
  gtk_paned_set_start_child (GTK_PANED (self->paned), scrolled);
  gtk_paned_set_resize_start_child (GTK_PANED (self->paned), TRUE);
  gtk_paned_set_shrink_start_child (GTK_PANED (self->paned), FALSE);
  gtk_box_append (GTK_BOX (right), self->paned);

  /* The Document Map at the left, with a bar to drag between it and the
   * page; hidden, the paned is just the page. */
  {
    GtkWidget *hpaned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);

    gtk_widget_set_vexpand (hpaned, TRUE);
    gtk_paned_set_start_child (GTK_PANED (hpaned), self->doc_map);
    gtk_paned_set_resize_start_child (GTK_PANED (hpaned), FALSE);
    gtk_paned_set_shrink_start_child (GTK_PANED (hpaned), FALSE);
    gtk_paned_set_end_child (GTK_PANED (hpaned), right);
    gtk_paned_set_resize_end_child (GTK_PANED (hpaned), TRUE);
    gtk_paned_set_shrink_end_child (GTK_PANED (hpaned), FALSE);
    gtk_paned_set_position (GTK_PANED (hpaned), 200);
    gtk_box_append (GTK_BOX (box), hpaned);
  }

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
