/* w42-versions.c - File > Versions, kept in Git
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 97's box: Save Now, a check box to keep a version automatically,
 * and the versions in a list by date and time, who saved each and their
 * comments, with Open below.  Word42 keeps the versions as commits in
 * Git, so the check box keeps one at every save rather than on close,
 * and the list has Compare and Restore beside Open: Compare marks what
 * has changed since, as Tools > Track Changes > Compare Documents does,
 * and Restore puts the version back in place of the text.  Delete is not
 * here: a commit is part of a history that may have been shared, and a
 * word processor has no business rewriting it.
 */

#include "w42-versions.h"

#include <string.h>
#include <glib/gi18n.h>

#include "w42-dialogs.h"
#include "w42-document.h"
#include "w42-git.h"
#include "w42-io.h"
#include "w42-settings.h"
#include "w42-window.h"

typedef struct {
  GtkWidget   *window;
  GtkWindow   *parent;         /* the document's window */
  W42View     *view;
  GFile       *file;
  GPtrArray   *versions;       /* W42GitVersion*, newest first */

  GtkWidget   *keep;           /* the check box */
  GtkWidget   *where;          /* which repository */
  GtkWidget   *list;
  GtkWidget   *comments;       /* the chosen version's whole comment */
  GtkWidget   *status;
  GtkWidget   *open_btn, *compare_btn, *restore_btn;
  gboolean     syncing;        /* the check box is being set, not clicked */
} VersionsBox;

static void versions_refresh (VersionsBox *box);

static VersionsBox *
box_of (GtkWidget *window)
{
  return window != NULL ? g_object_get_data (G_OBJECT (window), "w42-versions") : NULL;
}

/* The name the author goes by in comments and revisions, which is who a
 * version was saved by when Git has not been told. */
static char *
author_name (void)
{
  char *name = w42_settings_get_string ("user-name", "");

  if (*name == '\0')
    {
      g_free (name);
      name = g_strdup (g_get_real_name ());
    }
  return name;
}

static const W42GitVersion *
versions_selected (VersionsBox *box)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (box->list));
  int index = row != NULL ? gtk_list_box_row_get_index (row) : -1;

  if (box->versions == NULL || index < 0 || (guint) index >= box->versions->len)
    return NULL;
  return g_ptr_array_index (box->versions, index);
}

static char *
version_when (const W42GitVersion *v)
{
  GDateTime *when = g_date_time_new_from_unix_local (v->time);
  char *text;

  /* Translators: how the Versions box shows when a version was saved,
   * in g_date_time_format()'s notation; %x is the date as the language
   * writes it. */
  text = when != NULL ? g_date_time_format (when, _("%x %H:%M")) : NULL;
  if (when != NULL)
    g_date_time_unref (when);
  return text != NULL ? text : g_strdup ("");
}

static void
say (VersionsBox *box, const char *text)
{
  gtk_label_set_text (GTK_LABEL (box->status), text != NULL ? text : "");
}

/* ---- the list --------------------------------------------------------- */

static GtkWidget *
cell (const char *text, int chars, gboolean expand)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_label_set_width_chars (GTK_LABEL (label), chars);
  gtk_label_set_max_width_chars (GTK_LABEL (label), chars);
  gtk_widget_set_hexpand (label, expand);
  return label;
}

static GtkWidget *
row_of (const char *when, const char *who, const char *what)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 10);

  gtk_widget_set_margin_start (row, 4);
  gtk_widget_set_margin_end (row, 4);
  gtk_box_append (GTK_BOX (row), cell (when, 16, FALSE));
  gtk_box_append (GTK_BOX (row), cell (who, 14, FALSE));
  gtk_box_append (GTK_BOX (row), cell (what, 24, TRUE));
  return row;
}

static void
versions_sync_buttons (VersionsBox *box)
{
  const W42GitVersion *v = versions_selected (box);
  gboolean any = v != NULL;

  gtk_widget_set_sensitive (box->open_btn, any);
  gtk_widget_set_sensitive (box->compare_btn, any);
  gtk_widget_set_sensitive (box->restore_btn, any);
  gtk_label_set_text (GTK_LABEL (box->comments), v != NULL ? v->message : "");
}

static void
on_version_selected (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  (void) list; (void) row;
  versions_sync_buttons (data);
}

static void
versions_refresh (VersionsBox *box)
{
  GtkListBoxRow *row;
  GError *error = NULL;
  char *repo;

  while ((row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 0)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (box->list), GTK_WIDGET (row));
  g_clear_pointer (&box->versions, g_ptr_array_unref);

  box->syncing = TRUE;
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->keep), w42_git_is_tracked (box->file));
  box->syncing = FALSE;

  repo = w42_git_repository (box->file);
  if (repo != NULL)
    {
      /* Translators: %s is the folder of the Git repository the
       * versions are kept in. */
      char *text = g_strdup_printf (_("Versions are kept in the Git repository in %s."), repo);

      gtk_label_set_text (GTK_LABEL (box->where), text);
      g_free (text);
    }
  else
    gtk_label_set_text (GTK_LABEL (box->where),
                        _("The document's folder is in no Git repository yet. Word42 makes "
                          "one there when the first version is kept."));
  g_free (repo);

  box->versions = w42_git_history (box->file, &error);
  if (box->versions == NULL)
    {
      say (box, error->message);
      g_clear_error (&error);
    }
  else if (box->versions->len == 0)
    {
      GtkWidget *none = gtk_label_new (_("No versions of this document have been kept yet."));

      gtk_label_set_xalign (GTK_LABEL (none), 0.0);
      gtk_widget_set_margin_start (none, 4);
      gtk_list_box_append (GTK_LIST_BOX (box->list), none);
      gtk_widget_set_sensitive (GTK_WIDGET (gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 0)),
                                FALSE);
      g_clear_pointer (&box->versions, g_ptr_array_unref);
    }
  else
    for (guint i = 0; i < box->versions->len; i++)
      {
        const W42GitVersion *v = g_ptr_array_index (box->versions, i);
        char *when = version_when (v);
        char *first = g_strdup (v->message);
        char *nl = strchr (first, '\n');

        if (nl != NULL)
          *nl = '\0';
        gtk_list_box_append (GTK_LIST_BOX (box->list), row_of (when, v->author, first));
        g_free (first);
        g_free (when);
      }

  row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 0);
  if (box->versions != NULL && row != NULL)
    gtk_list_box_select_row (GTK_LIST_BOX (box->list), row);
  versions_sync_buttons (box);
}

/* ---- a version as a file ---------------------------------------------- */

/* The readers read files, so a version is written out as one first: to
 * the cache folder, under the document's own name so that its extension
 * says what format it is in.  The caller deletes it once it is read. */
static GFile *
version_file (VersionsBox *box, const W42GitVersion *v, GError **error)
{
  GBytes *bytes = w42_git_read_version (box->file, v->id, error);
  char *dir, *base, *name;
  GFile *out = NULL;

  if (bytes == NULL)
    return NULL;
  dir = g_build_filename (g_get_user_cache_dir (), "word42", "versions", NULL);
  base = g_file_get_basename (box->file);
  name = g_strdup_printf ("%s-%s", v->short_id, base);
  if (g_mkdir_with_parents (dir, 0700) == 0)
    {
      out = g_file_new_build_filename (dir, name, NULL);
      if (!g_file_replace_contents (out, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes),
                                    NULL, FALSE, G_FILE_CREATE_PRIVATE, NULL, NULL, error))
        g_clear_object (&out);
    }
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 _("Word42 could not make the folder %s."), dir);
  g_free (name);
  g_free (base);
  g_free (dir);
  g_bytes_unref (bytes);
  return out;
}

static void
version_file_done (GFile *file)
{
  if (file == NULL)
    return;
  g_file_delete (file, NULL, NULL);
  g_object_unref (file);
}

/* ---- the buttons ------------------------------------------------------ */

static void
on_open (GtkButton *button, gpointer data)
{
  VersionsBox *box = data;
  const W42GitVersion *v = versions_selected (box);
  GError *error = NULL;
  GFile *file;

  (void) button;
  if (v == NULL)
    return;
  file = version_file (box, v, &error);
  if (file != NULL)
    {
      char *base = g_file_get_basename (box->file);
      char *dot = strrchr (base, '.');
      char *title;

      if (dot != NULL && dot != base)
        *dot = '\0';
      /* Translators: the name an earlier version goes by when File >
       * Versions opens it: the document's name without its extension,
       * and the version's short id.  It is offered as a file name when
       * the version is saved, so no slashes or colons. */
      title = g_strdup_printf (_("%s (version %s)"), base, v->short_id);
      if (w42_window_open_version (W42_WINDOW (box->parent), file, title, &error))
        gtk_window_destroy (GTK_WINDOW (box->window));
      g_free (title);
      g_free (base);
    }
  if (error != NULL)
    {
      w42_message_show (GTK_WINDOW (box->window), _("Word42 could not open that version."),
                        error->message);
      g_error_free (error);
    }
  version_file_done (file);
}

static void
on_compare (GtkButton *button, gpointer data)
{
  VersionsBox *box = data;
  const W42GitVersion *v = versions_selected (box);
  GError *error = NULL;
  GFile *file;
  W42PieceTable *earlier;

  (void) button;
  if (v == NULL)
    return;
  file = version_file (box, v, &error);
  earlier = w42_pt_new ();
  if (file != NULL && w42_io_load (earlier, NULL, file, &error))
    {
      int n = w42_view_compare_with (box->view, earlier);
      char *when = version_when (v);
      char *text;

      if (n == 0)
        /* Translators: %s is the date and time of a version. */
        text = g_strdup_printf (_("The document is the same as the version of %s."), when);
      else
        /* Translators: the first %d is how many changes, %s the date
         * and time of the version they were made since. */
        text = g_strdup_printf (ngettext ("%d change since the version of %s is marked. "
                                          "Edit > Undo takes the marks away.",
                                          "%d changes since the version of %s are marked. "
                                          "Edit > Undo takes the marks away.",
                                          (unsigned long) n), n, when);
      if (W42_IS_WINDOW (box->parent))
        w42_window_flash_status (W42_WINDOW (box->parent), text);
      g_free (text);
      g_free (when);
      gtk_window_destroy (GTK_WINDOW (box->window));
    }
  else if (error != NULL)
    {
      w42_message_show (GTK_WINDOW (box->window), _("Word42 could not read that version."),
                        error->message);
      g_error_free (error);
    }
  w42_pt_free (earlier);
  version_file_done (file);
}

/* A double-click opens the version, as Word 97's list did. */
static void
on_version_activated (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  (void) list; (void) row;
  on_open (NULL, data);
}

typedef struct {
  GWeakRef  window;
  char     *id;
} Restoring;

static void
on_restore_choice (int choice, gpointer data)
{
  Restoring *r = data;
  GtkWidget *window = g_weak_ref_get (&r->window);
  VersionsBox *box = box_of (window);

  if (choice == 0 && box != NULL && box->versions != NULL)
    {
      const W42GitVersion *v = NULL;
      GError *error = NULL;
      GFile *file;

      for (guint i = 0; i < box->versions->len && v == NULL; i++)
        if (g_str_equal (((W42GitVersion *) g_ptr_array_index (box->versions, i))->id, r->id))
          v = g_ptr_array_index (box->versions, i);
      file = v != NULL ? version_file (box, v, &error) : NULL;
      if (file != NULL && w42_window_restore_version (W42_WINDOW (box->parent), file, &error))
        {
          char *when = version_when (v);
          /* Translators: %s is the date and time of the version put back. */
          char *text = g_strdup_printf (_("The version of %s is back. Save to keep it as the newest."),
                                        when);

          w42_window_flash_status (W42_WINDOW (box->parent), text);
          g_free (text);
          g_free (when);
          gtk_window_destroy (GTK_WINDOW (box->window));
        }
      else if (error != NULL)
        {
          w42_message_show (GTK_WINDOW (box->window), _("Word42 could not restore that version."),
                            error->message);
          g_error_free (error);
        }
      version_file_done (file);
    }
  if (window != NULL)
    g_object_unref (window);
  g_weak_ref_clear (&r->window);
  g_free (r->id);
  g_free (r);
}

static void
on_restore (GtkButton *button, gpointer data)
{
  VersionsBox *box = data;
  const W42GitVersion *v = versions_selected (box);
  const char *const buttons[] = { _("_Restore"), _("Cancel"), NULL };
  Restoring *r;
  char *when, *heading;
  const char *detail;

  (void) button;
  if (v == NULL)
    return;
  r = g_new0 (Restoring, 1);
  g_weak_ref_init (&r->window, box->window);
  r->id = g_strdup (v->id);
  when = version_when (v);
  /* Translators: %s is the date and time of a version. */
  heading = g_strdup_printf (_("Put the version of %s in place of the document?"), when);
  detail = w42_document_get_modified (w42_view_get_document (box->view))
           ? _("The changes made since the document was last saved are lost. Every version "
               "saved stays in the list, and saving keeps this one as the newest.")
           : _("Every version saved stays in the list, and saving keeps this one as the newest.");
  w42_choice_show (GTK_WINDOW (box->window), heading, detail, buttons, 0, 1,
                   on_restore_choice, r);
  g_free (heading);
  g_free (when);
}

static void
on_keep_toggled (GtkCheckButton *check, gpointer data)
{
  VersionsBox *box = data;
  gboolean on = gtk_check_button_get_active (check);
  char *base, *message, *author;
  GError *error = NULL;

  if (box->syncing)
    return;
  base = g_file_get_basename (box->file);
  /* Translators: the comment kept with the first version of a document
   * once versions are kept of it; %s is its file name. */
  message = g_strdup_printf (_("Saved %s"), base);
  author = author_name ();
  if (!w42_git_set_tracked (box->file, on, message, author, &error))
    {
      w42_message_show (GTK_WINDOW (box->window),
                        on ? _("Word42 could not start keeping versions of the document.")
                           : _("Word42 could not stop keeping versions of the document."),
                        error->message);
      g_error_free (error);
    }
  else
    say (box, on ? _("A version is kept each time the document is saved.")
                 : _("Versions are no longer kept when the document is saved. "
                     "Those already kept stay."));
  versions_refresh (box);
  g_free (author);
  g_free (message);
  g_free (base);
}

/* ---- Save Now --------------------------------------------------------- */

typedef struct {
  GWeakRef   versions;       /* the Versions box */
  GtkWidget *window;
  GtkWidget *text;
} SaveNowBox;

static void
save_now_free (gpointer data, GObject *gone)
{
  SaveNowBox *s = data;

  (void) gone;
  g_weak_ref_clear (&s->versions);
  g_free (s);
}

static void
on_save_now_ok (GtkButton *button, gpointer data)
{
  SaveNowBox *s = data;
  GtkWidget *window = g_weak_ref_get (&s->versions);
  VersionsBox *box = box_of (window);
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (s->text));
  GtkTextIter start, end;
  char *comment, *before = NULL;

  (void) button;
  gtk_text_buffer_get_bounds (buffer, &start, &end);
  comment = g_strstrip (gtk_text_buffer_get_text (buffer, &start, &end, FALSE));
  gtk_window_destroy (GTK_WINDOW (s->window));

  if (box != NULL)
    {
      if (box->versions != NULL && box->versions->len > 0)
        before = g_strdup (((W42GitVersion *) g_ptr_array_index (box->versions, 0))->id);
      if (w42_window_save_version (W42_WINDOW (box->parent), comment))
        {
          versions_refresh (box);
          if (box->versions != NULL && box->versions->len > 0 &&
              g_strcmp0 (before, ((W42GitVersion *) g_ptr_array_index (box->versions, 0))->id) != 0)
            {
              /* Translators: %s is the short id Git gives a version. */
              char *text = g_strdup_printf (_("Saved, and kept as version %s."),
                                            ((W42GitVersion *) g_ptr_array_index (box->versions, 0))->short_id);

              say (box, text);
              g_free (text);
            }
          else
            say (box, _("Saved. The document is the same as its last version, so no new one "
                        "was kept."));
        }
    }
  g_free (before);
  g_free (comment);
  if (window != NULL)
    g_object_unref (window);
}

static gboolean
on_save_now_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
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

/* Word 97's Save Version box: when, who, and the comments to keep with
 * it. */
static void
on_save_now (GtkButton *button, gpointer data)
{
  VersionsBox *box = data;
  SaveNowBox *s = g_new0 (SaveNowBox, 1);
  GtkWidget *content, *grid, *label, *scroller, *row, *ok, *cancel;
  GtkEventController *key;
  GDateTime *now = g_date_time_new_now_local ();
  char *when = g_date_time_format (now, _("%x %H:%M"));
  char *author = author_name ();

  (void) button;
  g_weak_ref_init (&s->versions, box->window);
  s->window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (s->window), _("Save Version"));
  gtk_window_set_transient_for (GTK_WINDOW (s->window), GTK_WINDOW (box->window));
  gtk_window_set_destroy_with_parent (GTK_WINDOW (s->window), TRUE);
  gtk_window_set_modal (GTK_WINDOW (s->window), TRUE);
  gtk_window_set_resizable (GTK_WINDOW (s->window), FALSE);
  gtk_widget_add_css_class (s->window, "w42");
  g_object_weak_ref (G_OBJECT (s->window), save_now_free, s);
  key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_save_now_key), s->window);
  gtk_widget_add_controller (s->window, key);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (content, "w42-dialog");
  gtk_widget_set_margin_start (content, 14);
  gtk_widget_set_margin_end (content, 14);
  gtk_widget_set_margin_top (content, 14);
  gtk_widget_set_margin_bottom (content, 14);
  gtk_window_set_child (GTK_WINDOW (s->window), content);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 4);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  label = gtk_label_new (_("Date and time:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  label = gtk_label_new (when);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 1, 0, 1, 1);
  label = gtk_label_new (_("Saved by:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);
  label = gtk_label_new (author);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 1, 1, 1, 1);
  gtk_box_append (GTK_BOX (content), grid);

  label = gtk_label_new_with_mnemonic (_("_Comments on version:"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (content), label);
  s->text = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (s->text), GTK_WRAP_WORD_CHAR);
  /* Tab goes on to OK, as it does from any other field in a box. */
  gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (s->text), FALSE);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), s->text);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), s->text);
  gtk_widget_set_size_request (scroller, 360, 90);
  gtk_box_append (GTK_BOX (content), scroller);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (row, GTK_ALIGN_END);
  ok = gtk_button_new_with_mnemonic (_("_OK"));
  cancel = gtk_button_new_with_mnemonic (_("Cancel"));
  gtk_widget_set_size_request (ok, 92, 26);
  gtk_widget_set_size_request (cancel, 92, 26);
  g_signal_connect (ok, "clicked", G_CALLBACK (on_save_now_ok), s);
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (gtk_window_destroy), s->window);
  gtk_box_append (GTK_BOX (row), ok);
  gtk_box_append (GTK_BOX (row), cancel);
  gtk_box_append (GTK_BOX (content), row);
  gtk_window_set_default_widget (GTK_WINDOW (s->window), ok);

  gtk_window_present (GTK_WINDOW (s->window));
  gtk_widget_grab_focus (s->text);

  g_free (author);
  g_free (when);
  g_date_time_unref (now);
}

/* ---- the box ---------------------------------------------------------- */

static gboolean
on_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
        GdkModifierType state, gpointer data)
{
  VersionsBox *box = data;

  (void) controller; (void) keycode; (void) state;
  if (keyval == GDK_KEY_Escape)
    {
      gtk_window_destroy (GTK_WINDOW (box->window));
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}

static void
box_free (gpointer data)
{
  VersionsBox *box = data;

  g_clear_pointer (&box->versions, g_ptr_array_unref);
  g_clear_object (&box->file);
  g_free (box);
}

static GtkWidget *
button (GtkWidget *row, const char *label, GCallback on_click, gpointer data)
{
  GtkWidget *b = gtk_button_new_with_mnemonic (label);

  gtk_widget_set_size_request (b, 92, 26);
  g_signal_connect (b, "clicked", on_click, data);
  gtk_box_append (GTK_BOX (row), b);
  return b;
}

void
w42_versions_dialog_show (GtkWindow *parent, W42View *view)
{
  W42Document *doc;
  GFile *file;
  VersionsBox *box;
  GtkWidget *content, *top, *label, *header, *scroller, *row, *close, *frame;
  GtkEventController *key;
  char *name, *title;

  g_return_if_fail (W42_IS_WINDOW (parent));
  g_return_if_fail (W42_IS_VIEW (view));

  if (!w42_git_available ())
    {
      w42_message_show (parent, _("Word42 cannot keep versions."),
                        _("This Word42 was built without Git (libgit2), so it cannot "
                          "keep versions of a document."));
      return;
    }
  doc = w42_view_get_document (view);
  file = w42_document_get_file (doc);
  if (file == NULL || !w42_io_format_round_trips (file) || !g_file_is_native (file))
    {
      w42_message_show (parent, _("Save the document first."),
                        _("Versions are kept of a document saved on this computer in a "
                          "format Word42 reads back as it wrote it: Word, RTF, OpenDocument, "
                          "WordPerfect, AbiWord or plain text."));
      return;
    }

  box = g_new0 (VersionsBox, 1);
  box->parent = parent;
  box->view = view;
  box->file = g_object_ref (file);

  box->window = gtk_window_new ();
  name = g_file_get_basename (file);
  /* Translators: the Versions box's title; %s is the document's name. */
  title = g_strdup_printf (_("Versions in %s"), name);
  gtk_window_set_title (GTK_WINDOW (box->window), title);
  g_free (title);
  g_free (name);
  gtk_window_set_transient_for (GTK_WINDOW (box->window), parent);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (box->window), TRUE);
  gtk_window_set_modal (GTK_WINDOW (box->window), TRUE);
  gtk_window_set_resizable (GTK_WINDOW (box->window), FALSE);
  gtk_widget_add_css_class (box->window, "w42");
  g_object_set_data_full (G_OBJECT (box->window), "w42-versions", box, box_free);
  /* The box works on the window's view, and goes with it should Window >
   * Split take the pane away. */
  g_signal_connect_object (view, "destroy", G_CALLBACK (gtk_window_destroy), box->window,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (box->window, "destroy", G_CALLBACK (gtk_widget_grab_focus), view,
                           G_CONNECT_SWAPPED);
  key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_key), box);
  gtk_widget_add_controller (box->window, key);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_add_css_class (content, "w42-dialog");
  gtk_widget_set_margin_start (content, 14);
  gtk_widget_set_margin_end (content, 14);
  gtk_widget_set_margin_top (content, 14);
  gtk_widget_set_margin_bottom (content, 14);
  gtk_window_set_child (GTK_WINDOW (box->window), content);

  /* New versions: Save Now and the check box, as Word 97 had them. */
  frame = gtk_frame_new (_("New versions"));
  top = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start (top, 10);
  gtk_widget_set_margin_end (top, 10);
  gtk_widget_set_margin_top (top, 8);
  gtk_widget_set_margin_bottom (top, 10);
  gtk_frame_set_child (GTK_FRAME (frame), top);
  gtk_box_append (GTK_BOX (content), frame);
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append (GTK_BOX (top), row);
  button (row, _("_Save Now..."), G_CALLBACK (on_save_now), box);
  box->keep = gtk_check_button_new_with_mnemonic (_("_Keep a version each time the document is saved"));
  g_signal_connect (box->keep, "toggled", G_CALLBACK (on_keep_toggled), box);
  gtk_box_append (GTK_BOX (row), box->keep);
  box->where = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->where), 0.0);
  gtk_label_set_wrap (GTK_LABEL (box->where), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (box->where), 64);
  gtk_label_set_selectable (GTK_LABEL (box->where), TRUE);
  gtk_widget_add_css_class (box->where, "w42-dialog-status");
  gtk_box_append (GTK_BOX (top), box->where);

  /* Existing versions. */
  label = gtk_label_new_with_mnemonic (_("E_xisting versions"));
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (content), label);
  header = row_of (_("Date and time"), _("Saved by"), _("Comments"));
  gtk_box_append (GTK_BOX (content), header);
  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_BROWSE);
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (box->list), FALSE);
  gtk_widget_add_css_class (box->list, "w42-versions");
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->list);
  g_signal_connect (box->list, "row-selected", G_CALLBACK (on_version_selected), box);
  g_signal_connect (box->list, "row-activated", G_CALLBACK (on_version_activated), box);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->list);
  gtk_widget_set_size_request (scroller, 520, 190);
  gtk_box_append (GTK_BOX (content), scroller);

  box->comments = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->comments), 0.0);
  gtk_label_set_yalign (GTK_LABEL (box->comments), 0.0);
  gtk_label_set_wrap (GTK_LABEL (box->comments), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (box->comments), 72);
  gtk_label_set_lines (GTK_LABEL (box->comments), 3);
  gtk_label_set_ellipsize (GTK_LABEL (box->comments), PANGO_ELLIPSIZE_END);
  gtk_label_set_selectable (GTK_LABEL (box->comments), TRUE);
  gtk_widget_set_size_request (box->comments, 520, 48);
  gtk_box_append (GTK_BOX (content), box->comments);

  box->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->status), 0.0);
  gtk_label_set_wrap (GTK_LABEL (box->status), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (box->status), 72);
  gtk_widget_add_css_class (box->status, "w42-dialog-status");
  gtk_box_append (GTK_BOX (content), box->status);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  box->open_btn = button (row, _("_Open"), G_CALLBACK (on_open), box);
  box->compare_btn = button (row, _("Co_mpare"), G_CALLBACK (on_compare), box);
  box->restore_btn = button (row, _("_Restore..."), G_CALLBACK (on_restore), box);
  close = gtk_button_new_with_mnemonic (_("Close"));
  gtk_widget_set_size_request (close, 92, 26);
  gtk_widget_set_hexpand (close, TRUE);
  gtk_widget_set_halign (close, GTK_ALIGN_END);
  g_signal_connect_swapped (close, "clicked", G_CALLBACK (gtk_window_destroy), box->window);
  gtk_box_append (GTK_BOX (row), close);
  gtk_box_append (GTK_BOX (content), row);
  gtk_window_set_default_widget (GTK_WINDOW (box->window), close);

  versions_refresh (box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->list);
}
