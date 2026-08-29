/* w42-find-dialog.c - see w42-find-dialog.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-find-dialog.h"

#include "w42-search.h"

struct _W42FindDialog {
  GtkWindow  parent_instance;

  W42View   *view;          /* not owned; the window outlives this dialog */

  GtkWidget *find_entry;
  GtkWidget *replace_entry;
  GtkWidget *replace_row;
  GtkWidget *replace_button;
  GtkWidget *replace_all_button;
  GtkWidget *match_case;
  GtkWidget *whole_word;
  GtkWidget *backwards;
  GtkWidget *status;
};

G_DEFINE_FINAL_TYPE (W42FindDialog, w42_find_dialog, GTK_TYPE_WINDOW)

static void
dialog_options (W42FindDialog *self, W42SearchOptions *options)
{
  options->match_case =
    gtk_check_button_get_active (GTK_CHECK_BUTTON (self->match_case));
  options->whole_word =
    gtk_check_button_get_active (GTK_CHECK_BUTTON (self->whole_word));
  options->backwards =
    gtk_check_button_get_active (GTK_CHECK_BUTTON (self->backwards));
  options->wrap = TRUE;
}

static const char *
dialog_needle (W42FindDialog *self)
{
  return gtk_editable_get_text (GTK_EDITABLE (self->find_entry));
}

static void
set_status (W42FindDialog *self, const char *text)
{
  gtk_label_set_text (GTK_LABEL (self->status), text != NULL ? text : "");
}

/* Searching starts from the far end of the selection, so that Find Next moves
 * on rather than finding the match it is already sitting on -- and so that a
 * backwards search does not walk forwards into it. */
static gsize
search_origin (W42FindDialog *self, const W42SearchOptions *options)
{
  W42PieceTable *pt = w42_document_pt (w42_view_get_document (self->view));
  gsize caret = w42_view_get_caret (self->view);

  if (!w42_view_has_selection (self->view))
    return caret;

  /* Searching up starts before the selected match, so Find Next does
   * not find it again. */
  if (options->backwards)
    {
      gsize start = 0, end = 0;

      w42_view_get_selection_bounds (self->view, &start, &end);
      return MIN (start, w42_pt_length (pt));
    }
  return caret;
}

static gboolean
do_find (W42FindDialog *self, gboolean quiet)
{
  W42Document *doc = w42_view_get_document (self->view);
  W42SearchOptions options;
  gsize start = 0, end = 0;
  const char *needle = dialog_needle (self);

  if (doc == NULL || needle == NULL || *needle == '\0')
    {
      if (!quiet)
        set_status (self, "Type what to look for.");
      return FALSE;
    }

  dialog_options (self, &options);

  if (!w42_search_find (w42_document_pt (doc), search_origin (self, &options),
                        needle, &options, &start, &end))
    {
      if (!quiet)
        set_status (self, "Word42 has finished searching the document. "
                          "The search item was not found.");
      return FALSE;
    }

  w42_view_select_range (self->view, start, end);
  set_status (self, "");
  return TRUE;
}

static void
on_find_next (GtkButton *button, gpointer data)
{
  (void) button;
  do_find (W42_FIND_DIALOG (data), FALSE);
}

/* Word replaces the selection only when it is already the thing being looked
 * for; otherwise Replace behaves as Find Next.  That way the first press
 * always highlights, and the second always replaces what you can see. */
static void
on_replace (GtkButton *button, gpointer data)
{
  W42FindDialog *self = data;
  W42SearchOptions options;
  char *selected;
  const char *needle = dialog_needle (self);
  gboolean matches = FALSE;

  (void) button;

  if (needle == NULL || *needle == '\0')
    {
      set_status (self, "Type what to look for.");
      return;
    }

  dialog_options (self, &options);
  selected = w42_view_get_selected_text (self->view);

  if (selected != NULL)
    {
      if (options.match_case)
        matches = g_strcmp0 (selected, needle) == 0;
      else
        {
          char *a = g_utf8_casefold (selected, -1), *b = g_utf8_casefold (needle, -1);

          matches = g_strcmp0 (a, b) == 0;
          g_free (a);
          g_free (b);
        }
    }

  g_free (selected);

  if (matches)
    {
      const char *with =
        gtk_editable_get_text (GTK_EDITABLE (self->replace_entry));

      /* Replacing with nothing is a deletion, and inserting nothing is
       * not one: the match would have stayed where it was. */
      if (with == NULL || *with == '\0')
        w42_view_clear (self->view);
      else
        w42_view_insert_text (self->view, with);
      set_status (self, "");
    }

  do_find (self, matches);
}

static void
on_replace_all (GtkButton *button, gpointer data)
{
  W42FindDialog *self = data;
  W42Document *doc = w42_view_get_document (self->view);
  W42SearchOptions options;
  const char *needle = dialog_needle (self);
  const char *with;
  char *message;
  gsize n;

  (void) button;

  if (doc == NULL || needle == NULL || *needle == '\0')
    {
      set_status (self, "Type what to look for.");
      return;
    }

  dialog_options (self, &options);
  with = gtk_editable_get_text (GTK_EDITABLE (self->replace_entry));

  n = w42_search_replace_all (w42_document_pt (doc), needle, with, &options);

  if (n > 0)
    {
      w42_document_set_modified (doc, TRUE);
      w42_document_touch (doc);
    }

  message = g_strdup_printf (
    n == 1 ? "1 replacement made." : "%" G_GSIZE_FORMAT " replacements made.",
    n);
  set_status (self, n > 0 ? message : "The search item was not found.");
  g_free (message);
}

static void
on_close (GtkButton *button, gpointer data)
{
  (void) button;
  gtk_window_close (GTK_WINDOW (data));
}

static gboolean
on_find_key (GtkEventControllerKey *controller, guint keyval,
             guint keycode, GdkModifierType state, gpointer data)
{
  (void) controller; (void) keycode; (void) state;

  if (keyval == GDK_KEY_Escape)
    {
      gtk_window_close (GTK_WINDOW (data));
      return GDK_EVENT_STOP;
    }

  return GDK_EVENT_PROPAGATE;
}

void
w42_find_dialog_find_again (W42FindDialog *self)
{
  g_return_if_fail (W42_IS_FIND_DIALOG (self));
  do_find (self, FALSE);
}

void
w42_find_dialog_set_replace_mode (W42FindDialog *self, gboolean replace)
{
  g_return_if_fail (W42_IS_FIND_DIALOG (self));

  gtk_widget_set_visible (self->replace_row, replace);
  gtk_widget_set_visible (self->replace_button, replace);
  gtk_widget_set_visible (self->replace_all_button, replace);

  gtk_window_set_title (GTK_WINDOW (self), replace ? "Replace" : "Find");
}

static void
w42_find_dialog_class_init (W42FindDialogClass *klass)
{
  (void) klass;
}

static GtkWidget *
labelled_row (GtkWidget *grid, int row, const char *label, GtkWidget *entry)
{
  GtkWidget *text = gtk_label_new_with_mnemonic (label);

  gtk_label_set_xalign (GTK_LABEL (text), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (text), entry);

  gtk_widget_set_hexpand (entry, TRUE);
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);

  gtk_grid_attach (GTK_GRID (grid), text, 0, row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), entry, 1, row, 1, 1);

  return text;
}

static void
w42_find_dialog_init (W42FindDialog *self)
{
  GtkWidget *box, *grid, *options, *buttons;

  gtk_window_set_title (GTK_WINDOW (self), "Find");
  gtk_window_set_resizable (GTK_WINDOW (self), FALSE);
  gtk_window_set_default_size (GTK_WINDOW (self), 460, -1);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (box, "w42-dialog");
  gtk_widget_set_margin_start (box, 14);
  gtk_widget_set_margin_end (box, 14);
  gtk_widget_set_margin_top (box, 14);
  gtk_widget_set_margin_bottom (box, 14);
  gtk_window_set_child (GTK_WINDOW (self), box);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  gtk_box_append (GTK_BOX (box), grid);

  self->find_entry = gtk_entry_new ();
  labelled_row (grid, 0, "Fi_nd What:", self->find_entry);

  self->replace_entry = gtk_entry_new ();
  self->replace_row = labelled_row (grid, 1, "Re_place With:",
                                    self->replace_entry);

  options = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  self->match_case = gtk_check_button_new_with_mnemonic ("Match _Case");
  self->whole_word = gtk_check_button_new_with_mnemonic ("Find _Whole Words Only");
  self->backwards  = gtk_check_button_new_with_mnemonic ("Search _Up");
  gtk_box_append (GTK_BOX (options), self->match_case);
  gtk_box_append (GTK_BOX (options), self->whole_word);
  gtk_box_append (GTK_BOX (options), self->backwards);
  gtk_box_append (GTK_BOX (box), options);

  self->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->status), 0.0);
  gtk_label_set_wrap (GTK_LABEL (self->status), TRUE);
  gtk_widget_add_css_class (self->status, "w42-dialog-status");
  gtk_box_append (GTK_BOX (box), self->status);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (box), buttons);

  {
    GtkWidget *find_next = gtk_button_new_with_mnemonic ("_Find Next");
    GtkWidget *close = gtk_button_new_with_mnemonic ("Close");

    self->replace_button = gtk_button_new_with_mnemonic ("_Replace");
    self->replace_all_button = gtk_button_new_with_mnemonic ("Replace _All");

    gtk_widget_set_size_request (find_next, 100, 26);
    gtk_widget_set_size_request (self->replace_button, 100, 26);
    gtk_widget_set_size_request (self->replace_all_button, 100, 26);
    gtk_widget_set_size_request (close, 100, 26);

    g_signal_connect (find_next, "clicked", G_CALLBACK (on_find_next), self);
    g_signal_connect (self->replace_button, "clicked",
                      G_CALLBACK (on_replace), self);
    g_signal_connect (self->replace_all_button, "clicked",
                      G_CALLBACK (on_replace_all), self);
    g_signal_connect (close, "clicked", G_CALLBACK (on_close), self);

    gtk_box_append (GTK_BOX (buttons), find_next);
    gtk_box_append (GTK_BOX (buttons), self->replace_button);
    gtk_box_append (GTK_BOX (buttons), self->replace_all_button);
    gtk_box_append (GTK_BOX (buttons), close);

    gtk_window_set_default_widget (GTK_WINDOW (self), find_next);
  }

  /* Enter in either entry runs the default button, which is Find Next. */
  g_signal_connect_swapped (self->find_entry, "activate",
                            G_CALLBACK (w42_find_dialog_find_again), self);

  /* Escape closes the box, as it closes every box. */
  {
    GtkEventController *key = gtk_event_controller_key_new ();

    g_signal_connect (key, "key-pressed", G_CALLBACK (on_find_key), self);
    gtk_widget_add_controller (GTK_WIDGET (self), key);
  }
}

GtkWidget *
w42_find_dialog_new (GtkWindow *parent, W42View *view)
{
  W42FindDialog *self = g_object_new (W42_TYPE_FIND_DIALOG, NULL);

  self->view = view;

  gtk_window_set_transient_for (GTK_WINDOW (self), parent);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (self), TRUE);

  /* Modeless, as Word 6's was: you can go on editing with it open. */
  gtk_window_set_modal (GTK_WINDOW (self), FALSE);

  /* Closing it hands the keyboard back to the document. */
  g_signal_connect_object (self, "destroy",
                           G_CALLBACK (gtk_widget_grab_focus), view, G_CONNECT_SWAPPED);

  w42_find_dialog_set_replace_mode (self, FALSE);

  return GTK_WIDGET (self);
}

void
w42_find_dialog_prime (W42FindDialog *self, const char *text)
{
  g_return_if_fail (W42_IS_FIND_DIALOG (self));
  if (text != NULL && *text != '\0')
    gtk_editable_set_text (GTK_EDITABLE (self->find_entry), text);
  gtk_widget_grab_focus (self->find_entry);
  gtk_editable_select_region (GTK_EDITABLE (self->find_entry), 0, -1);
}
