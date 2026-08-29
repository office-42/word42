/* w42-spell-dialog.c - see w42-spell-dialog.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-spell-dialog.h"

#include "w42-search.h"

struct _W42SpellDialog {
  GtkWindow  parent_instance;

  W42View   *view;          /* not owned; the window outlives this dialog */
  W42Spell  *spell;         /* not owned */

  GtkWidget *word_label;
  GtkWidget *change_entry;
  GtkWidget *suggestions;   /* GtkListBox */
  GtkWidget *status;
  GtkWidget *ignore_btn, *ignore_all_btn;
  GtkWidget *change_btn, *change_all_btn;
  GtkWidget *add_btn;

  char      *word;          /* the word the box is stopped on, or NULL */

  /* Where the check began, so that after wrapping round it knows when it
   * has come full circle. */
  gsize      origin;
  gboolean   wrapped;
};

G_DEFINE_FINAL_TYPE (W42SpellDialog, w42_spell_dialog, GTK_TYPE_WINDOW)

static void
set_status (W42SpellDialog *self, const char *text)
{
  gtk_label_set_text (GTK_LABEL (self->status), text != NULL ? text : "");
}

static void
set_buttons_sensitive (W42SpellDialog *self, gboolean on)
{
  gtk_widget_set_sensitive (self->ignore_btn, on);
  gtk_widget_set_sensitive (self->ignore_all_btn, on);
  gtk_widget_set_sensitive (self->change_btn, on);
  gtk_widget_set_sensitive (self->change_all_btn, on);
  gtk_widget_set_sensitive (self->add_btn, on);
  gtk_widget_set_sensitive (self->change_entry, on);
  gtk_widget_set_sensitive (self->suggestions, on);
}

static void
clear_suggestions (W42SpellDialog *self)
{
  GtkWidget *row;

  while ((row = gtk_widget_get_first_child (self->suggestions)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->suggestions), row);
}

/* The box is stopped on a word: show it, and what the dictionary would
 * rather it were. */
static void
present_word (W42SpellDialog *self, gsize start, gsize end)
{
  char **suggestions;

  w42_view_select_range (self->view, start, end);

  g_free (self->word);
  self->word = w42_view_get_selected_text (self->view);

  gtk_label_set_text (GTK_LABEL (self->word_label), self->word);
  clear_suggestions (self);

  suggestions = w42_spell_suggest (self->spell, self->word, -1);
  for (guint i = 0; suggestions != NULL && suggestions[i] != NULL; i++)
    {
      GtkWidget *label = gtk_label_new (suggestions[i]);

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_margin_start (label, 4);
      gtk_list_box_append (GTK_LIST_BOX (self->suggestions), label);
    }

  gtk_editable_set_text (GTK_EDITABLE (self->change_entry),
                         suggestions != NULL && suggestions[0] != NULL
                           ? suggestions[0] : self->word);
  if (suggestions != NULL && suggestions[0] != NULL)
    gtk_list_box_select_row (GTK_LIST_BOX (self->suggestions),
                             gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->suggestions), 0));
  else
    set_status (self, "(no suggestions)");

  g_strfreev (suggestions);
  set_buttons_sensitive (self, TRUE);
  gtk_widget_grab_focus (self->change_entry);
}

static void
finish (W42SpellDialog *self)
{
  g_clear_pointer (&self->word, g_free);
  gtk_label_set_text (GTK_LABEL (self->word_label), "");
  clear_suggestions (self);
  gtk_editable_set_text (GTK_EDITABLE (self->change_entry), "");
  set_buttons_sensitive (self, FALSE);
  set_status (self, "The spelling check is complete.");
}

/* On to the next word the dictionary does not know, from the caret; round
 * to the top once, and stop where the check began. */
static void
spell_next (W42SpellDialog *self)
{
  W42Document *doc = w42_view_get_document (self->view);
  W42PieceTable *pt;
  gsize from, start, end;

  if (doc == NULL)
    return;

  pt = w42_document_pt (doc);
  set_status (self, NULL);

  from = w42_view_get_caret (self->view);

  if (w42_view_find_misspelling (self->view, self->spell, from, &start, &end) &&
      (!self->wrapped || start < self->origin))
    {
      present_word (self, start, end);
      return;
    }

  if (!self->wrapped)
    {
      self->wrapped = TRUE;
      from = w42_pt_first_caret_pos (pt);

      if (w42_view_find_misspelling (self->view, self->spell, from, &start, &end) &&
          start < self->origin)
        {
          present_word (self, start, end);
          return;
        }
    }

  finish (self);
}

/* ---------------------------------------------------------------------- */
/* Buttons                                                                 */
/* ---------------------------------------------------------------------- */

static void
on_ignore (GtkButton *button, gpointer data)
{
  (void) button;
  spell_next (W42_SPELL_DIALOG (data));
}

static void
on_ignore_all (GtkButton *button, gpointer data)
{
  W42SpellDialog *self = data;

  (void) button;

  if (self->word != NULL)
    {
      w42_spell_ignore (self->spell, self->word);
      w42_view_spell_refresh (self->view);
    }
  spell_next (self);
}

static void
on_change (GtkButton *button, gpointer data)
{
  W42SpellDialog *self = data;
  const char *with = gtk_editable_get_text (GTK_EDITABLE (self->change_entry));

  (void) button;

  if (self->word != NULL && with != NULL && *with != '\0' &&
      w42_view_has_selection (self->view))
    w42_view_insert_text (self->view, with);

  spell_next (self);
}

static void
on_change_all (GtkButton *button, gpointer data)
{
  W42SpellDialog *self = data;
  W42Document *doc = w42_view_get_document (self->view);
  const char *with = gtk_editable_get_text (GTK_EDITABLE (self->change_entry));
  W42SearchOptions options = { TRUE, TRUE, FALSE, TRUE };

  (void) button;

  if (doc != NULL && self->word != NULL && with != NULL && *with != '\0')
    {
      gsize n = w42_search_replace_all (w42_document_pt (doc), self->word,
                                        with, &options);
      if (n > 0)
        {
          w42_document_set_modified (doc, TRUE);
          w42_document_touch (doc);
        }
    }

  spell_next (self);
}

static void
on_add (GtkButton *button, gpointer data)
{
  W42SpellDialog *self = data;

  (void) button;

  if (self->word != NULL)
    {
      w42_spell_add (self->spell, self->word);
      w42_view_spell_refresh (self->view);
    }
  spell_next (self);
}

static void
on_suggestion_selected (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  W42SpellDialog *self = data;
  GtkWidget *label;

  (void) box;

  if (row == NULL)
    return;

  label = gtk_list_box_row_get_child (row);
  if (GTK_IS_LABEL (label))
    gtk_editable_set_text (GTK_EDITABLE (self->change_entry),
                           gtk_label_get_text (GTK_LABEL (label)));
}

static void
on_suggestion_activated (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
  on_suggestion_selected (box, row, data);
  on_change (NULL, data);
}

static gboolean
on_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
        GdkModifierType state, gpointer data)
{
  (void) controller; (void) keycode; (void) state;

  if (keyval == GDK_KEY_Escape)
    {
      gtk_window_close (GTK_WINDOW (data));
      return GDK_EVENT_STOP;
    }

  return GDK_EVENT_PROPAGATE;
}

/* ---------------------------------------------------------------------- */
/* Construction                                                            */
/* ---------------------------------------------------------------------- */

static void
w42_spell_dialog_finalize (GObject *object)
{
  W42SpellDialog *self = W42_SPELL_DIALOG (object);

  g_free (self->word);

  G_OBJECT_CLASS (w42_spell_dialog_parent_class)->finalize (object);
}

static void
w42_spell_dialog_class_init (W42SpellDialogClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = w42_spell_dialog_finalize;
}

static GtkWidget *
side_button (GtkWidget *column, const char *label, GCallback on_click,
             gpointer data)
{
  GtkWidget *button = gtk_button_new_with_mnemonic (label);

  gtk_widget_set_size_request (button, 96, 26);
  g_signal_connect (button, "clicked", on_click, data);
  gtk_box_append (GTK_BOX (column), button);

  return button;
}

static void
w42_spell_dialog_init (W42SpellDialog *self)
{
  GtkWidget *box, *grid, *columns, *left, *right, *scroller, *label, *close;
  GtkEventController *key;

  gtk_window_set_title (GTK_WINDOW (self), "Spelling");
  gtk_window_set_resizable (GTK_WINDOW (self), FALSE);

  key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), key);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (box, "w42-dialog");
  gtk_widget_set_margin_start (box, 14);
  gtk_widget_set_margin_end (box, 14);
  gtk_widget_set_margin_top (box, 14);
  gtk_widget_set_margin_bottom (box, 14);
  gtk_window_set_child (GTK_WINDOW (self), box);

  columns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_box_append (GTK_BOX (box), columns);

  left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_hexpand (left, TRUE);
  gtk_box_append (GTK_BOX (columns), left);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  gtk_box_append (GTK_BOX (left), grid);

  label = gtk_label_new ("Not in Dictionary:");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  self->word_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->word_label), 0.0);
  gtk_widget_add_css_class (self->word_label, "w42-spell-word");
  gtk_widget_set_size_request (self->word_label, 220, -1);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), self->word_label, 1, 0, 1, 1);

  label = gtk_label_new_with_mnemonic ("Change _To:");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  self->change_entry = gtk_entry_new ();
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), self->change_entry);
  gtk_widget_set_hexpand (self->change_entry, TRUE);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), self->change_entry, 1, 1, 1, 1);

  label = gtk_label_new_with_mnemonic ("Suggestio_ns:");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (left), label);

  self->suggestions = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->suggestions),
                                   GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (self->suggestions),
                                             FALSE);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), self->suggestions);
  g_signal_connect (self->suggestions, "row-selected",
                    G_CALLBACK (on_suggestion_selected), self);
  g_signal_connect (self->suggestions, "row-activated",
                    G_CALLBACK (on_suggestion_activated), self);

  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), self->suggestions);
  gtk_widget_set_size_request (scroller, -1, 132);
  gtk_box_append (GTK_BOX (left), scroller);

  self->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->status), 0.0);
  gtk_widget_add_css_class (self->status, "w42-dialog-status");
  gtk_box_append (GTK_BOX (left), self->status);

  /* The column of buttons down the right, as Word 6 laid them out. */
  right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (columns), right);

  self->ignore_btn     = side_button (right, "_Ignore",     G_CALLBACK (on_ignore),     self);
  self->ignore_all_btn = side_button (right, "I_gnore All", G_CALLBACK (on_ignore_all), self);
  self->change_btn     = side_button (right, "_Change",     G_CALLBACK (on_change),     self);
  self->change_all_btn = side_button (right, "Change A_ll", G_CALLBACK (on_change_all), self);
  self->add_btn        = side_button (right, "_Add",        G_CALLBACK (on_add),        self);

  close = gtk_button_new_with_mnemonic ("Close");
  gtk_widget_set_size_request (close, 96, 26);
  gtk_widget_set_margin_top (close, 8);
  g_signal_connect_swapped (close, "clicked", G_CALLBACK (gtk_window_close), self);
  gtk_box_append (GTK_BOX (right), close);

  /* Enter in the Change To box is Change. */
  gtk_entry_set_activates_default (GTK_ENTRY (self->change_entry), TRUE);
  gtk_window_set_default_widget (GTK_WINDOW (self), self->change_btn);

  set_buttons_sensitive (self, FALSE);
}

GtkWidget *
w42_spell_dialog_new (GtkWindow *parent, W42View *view, W42Spell *spell)
{
  W42SpellDialog *self = g_object_new (W42_TYPE_SPELL_DIALOG, NULL);

  self->view = view;
  self->spell = spell;

  gtk_window_set_transient_for (GTK_WINDOW (self), parent);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (self), TRUE);
  gtk_window_set_modal (GTK_WINDOW (self), FALSE);

  g_signal_connect_swapped (self, "destroy",
                            G_CALLBACK (gtk_widget_grab_focus), view);

  return GTK_WIDGET (self);
}

void
w42_spell_dialog_start (W42SpellDialog *self)
{
  g_return_if_fail (W42_IS_SPELL_DIALOG (self));

  self->origin = w42_view_get_caret (self->view);
  self->wrapped = FALSE;
  spell_next (self);
}
