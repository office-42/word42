/* w42-thesaurus-dialog.c - the Thesaurus box
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 97's box: "Looked Up" and the word, a list of meanings, a list of
 * synonyms for the meaning chosen with the one picked in a box above it,
 * and Replace, Look Up and Cancel down the right.  Look Up looks up the
 * word in the box, so the thesaurus can be walked from word to word, and
 * Previous goes back a step.
 */

#include "w42-thesaurus-dialog.h"

typedef struct {
  GtkWidget    *window;
  W42View      *view;
  W42Thesaurus *thesaurus;

  GtkWidget    *looked_up;     /* the word, as a label */
  GtkWidget    *meanings;      /* GtkListBox */
  GtkWidget    *synonym;       /* the entry: what Replace puts in */
  GtkWidget    *synonyms;      /* GtkListBox */
  GtkWidget    *status;
  GtkWidget    *replace_btn, *previous_btn;

  GPtrArray    *senses;        /* of W42Sense*, for the word looked up */
  GPtrArray    *history;       /* the words looked up before, for Previous */
} ThesaurusBox;

static void thesaurus_look_up (ThesaurusBox *box, const char *word, gboolean remember);

static void
list_clear (GtkWidget *list)
{
  GtkListBoxRow *row;

  while ((row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (list), 0)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (list), GTK_WIDGET (row));
}

static void
list_add (GtkWidget *list, const char *text)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_margin_start (label, 4);
  gtk_widget_set_margin_end (label, 4);
  gtk_list_box_append (GTK_LIST_BOX (list), label);
}

static void
list_select_first (GtkWidget *list)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (list), 0);

  if (row != NULL)
    gtk_list_box_select_row (GTK_LIST_BOX (list), row);
}

/* The label a row shows. */
static const char *
row_text (GtkListBoxRow *row)
{
  GtkWidget *child = row != NULL ? gtk_list_box_row_get_child (row) : NULL;

  return GTK_IS_LABEL (child) ? gtk_label_get_text (GTK_LABEL (child)) : NULL;
}

static void
on_meaning_selected (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  ThesaurusBox *box = data;
  int index = row != NULL ? gtk_list_box_row_get_index (row) : -1;

  (void) list;
  list_clear (box->synonyms);
  if (box->senses == NULL || index < 0 || (guint) index >= box->senses->len)
    return;
  {
    const W42Sense *sense = g_ptr_array_index (box->senses, index);

    for (guint i = 0; sense->synonyms[i] != NULL; i++)
      list_add (box->synonyms, sense->synonyms[i]);
  }
  list_select_first (box->synonyms);
}

static void
on_synonym_selected (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  ThesaurusBox *box = data;
  const char *text = row_text (row);

  (void) list;
  if (text != NULL)
    {
      /* "(generic term)" and its like are the file's notes on a synonym,
       * not part of the word. */
      char *word = g_strdup (text);
      char *paren = strstr (word, " (");

      if (paren != NULL)
        *paren = '\0';
      gtk_editable_set_text (GTK_EDITABLE (box->synonym), word);
      g_free (word);
    }
}

static void
on_synonym_activated (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  ThesaurusBox *box = data;

  (void) list;
  on_synonym_selected (GTK_LIST_BOX (box->synonyms), row, box);
  thesaurus_look_up (box, gtk_editable_get_text (GTK_EDITABLE (box->synonym)), TRUE);
}

static void
thesaurus_look_up (ThesaurusBox *box, const char *word, gboolean remember)
{
  GPtrArray *senses;
  char *clean;

  if (word == NULL)
    return;
  clean = g_strstrip (g_strdup (word));
  if (*clean == '\0')
    {
      g_free (clean);
      return;
    }

  senses = w42_thesaurus_lookup (box->thesaurus, clean);
  if (senses == NULL)
    {
      char *message = g_strdup_printf ("No entries found for \"%s\".", clean);

      gtk_label_set_text (GTK_LABEL (box->status), message);
      g_free (message);
      g_free (clean);
      return;
    }

  if (remember)
    {
      const char *current = gtk_label_get_text (GTK_LABEL (box->looked_up));

      if (current != NULL && *current != '\0')
        g_ptr_array_add (box->history, g_strdup (current));
    }
  gtk_widget_set_sensitive (box->previous_btn, box->history->len > 0);

  w42_thesaurus_senses_free (box->senses);
  box->senses = senses;
  gtk_label_set_text (GTK_LABEL (box->looked_up), clean);
  gtk_label_set_text (GTK_LABEL (box->status), "");
  list_clear (box->meanings);
  for (guint i = 0; i < senses->len; i++)
    list_add (box->meanings, ((W42Sense *) g_ptr_array_index (senses, i))->meaning);
  list_select_first (box->meanings);
  g_free (clean);
}

static void
on_replace (GtkButton *button, gpointer data)
{
  ThesaurusBox *box = data;
  const char *with = gtk_editable_get_text (GTK_EDITABLE (box->synonym));

  (void) button;
  if (with != NULL && *with != '\0' && w42_view_has_selection (box->view))
    w42_view_insert_text (box->view, with);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

static void
on_look_up (GtkButton *button, gpointer data)
{
  ThesaurusBox *box = data;

  (void) button;
  thesaurus_look_up (box, gtk_editable_get_text (GTK_EDITABLE (box->synonym)), TRUE);
}

static void
on_previous (GtkButton *button, gpointer data)
{
  ThesaurusBox *box = data;

  (void) button;
  if (box->history->len == 0)
    return;
  {
    char *word = g_ptr_array_steal_index (box->history, box->history->len - 1);

    thesaurus_look_up (box, word, FALSE);
    g_free (word);
  }
}

static gboolean
on_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
        GdkModifierType state, gpointer data)
{
  ThesaurusBox *box = data;

  (void) controller; (void) keycode; (void) state;
  if (keyval == GDK_KEY_Escape)
    {
      gtk_window_destroy (GTK_WINDOW (box->window));
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}

static void
box_free (gpointer data, GObject *where)
{
  ThesaurusBox *box = data;

  (void) where;
  w42_thesaurus_senses_free (box->senses);
  g_ptr_array_free (box->history, TRUE);
  g_free (box);
}

static GtkWidget *
side_button (GtkWidget *column, const char *label, GCallback on_click, gpointer data)
{
  GtkWidget *button = gtk_button_new_with_mnemonic (label);

  gtk_widget_set_size_request (button, 96, 26);
  g_signal_connect (button, "clicked", on_click, data);
  gtk_box_append (GTK_BOX (column), button);
  return button;
}

static GtkWidget *
list_in_frame (GtkWidget *list, int height)
{
  GtkWidget *scroller = gtk_scrolled_window_new ();

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), list);
  gtk_widget_set_size_request (scroller, 200, height);
  return scroller;
}

gboolean
w42_thesaurus_dialog_show (GtkWindow *parent, W42View *view, W42Thesaurus *thesaurus)
{
  ThesaurusBox *box;
  GtkWidget *content, *columns, *left, *middle, *right, *label, *cancel;
  GtkEventController *key;
  char *word;
  char *title;

  g_return_val_if_fail (W42_IS_VIEW (view), FALSE);
  g_return_val_if_fail (thesaurus != NULL, FALSE);

  /* The word at the caret, or the selection when there is one, is what
   * is looked up; it stays selected, so Replace has something to replace. */
  if (!w42_view_has_selection (view))
    w42_view_select_word (view);
  word = w42_view_get_selected_text (view);
  if (word != NULL)
    g_strstrip (word);
  if (word == NULL || *word == '\0')
    {
      g_free (word);
      return FALSE;
    }

  box = g_new0 (ThesaurusBox, 1);
  box->view = view;
  box->thesaurus = thesaurus;
  box->history = g_ptr_array_new_with_free_func (g_free);

  box->window = gtk_window_new ();
  title = g_strdup_printf ("Thesaurus: %s", w42_thesaurus_language (thesaurus));
  gtk_window_set_title (GTK_WINDOW (box->window), title);
  g_free (title);
  gtk_window_set_transient_for (GTK_WINDOW (box->window), parent);
  /* The box works on the window's view with the window's thesaurus, and
   * the window frees both when it goes: the box goes with it, and with
   * the view, should Window > Split take that away. */
  gtk_window_set_destroy_with_parent (GTK_WINDOW (box->window), TRUE);
  g_signal_connect_object (view, "destroy", G_CALLBACK (gtk_window_destroy),
                           box->window, G_CONNECT_SWAPPED);
  gtk_window_set_modal (GTK_WINDOW (box->window), TRUE);
  gtk_window_set_resizable (GTK_WINDOW (box->window), FALSE);
  gtk_widget_add_css_class (box->window, "w42");
  g_object_weak_ref (G_OBJECT (box->window), box_free, box);
  g_signal_connect_object (box->window, "destroy", G_CALLBACK (gtk_widget_grab_focus),
                           view, G_CONNECT_SWAPPED);

  key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_key), box);
  gtk_widget_add_controller (box->window, key);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (content, "w42-dialog");
  gtk_widget_set_margin_start (content, 14);
  gtk_widget_set_margin_end (content, 14);
  gtk_widget_set_margin_top (content, 14);
  gtk_widget_set_margin_bottom (content, 14);
  gtk_window_set_child (GTK_WINDOW (box->window), content);

  columns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 14);
  gtk_box_append (GTK_BOX (content), columns);

  left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (columns), left);
  label = gtk_label_new ("Looked Up:");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (left), label);
  box->looked_up = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->looked_up), 0.0);
  gtk_widget_add_css_class (box->looked_up, "w42-spell-word");
  gtk_box_append (GTK_BOX (left), box->looked_up);
  label = gtk_label_new_with_mnemonic ("_Meanings:");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (left), label);
  box->meanings = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->meanings), GTK_SELECTION_SINGLE);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->meanings);
  g_signal_connect (box->meanings, "row-selected", G_CALLBACK (on_meaning_selected), box);
  gtk_box_append (GTK_BOX (left), list_in_frame (box->meanings, 160));

  middle = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (columns), middle);
  label = gtk_label_new_with_mnemonic ("Replace with _Synonym:");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (middle), label);
  box->synonym = gtk_entry_new ();
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->synonym);
  gtk_entry_set_activates_default (GTK_ENTRY (box->synonym), TRUE);
  gtk_box_append (GTK_BOX (middle), box->synonym);
  box->synonyms = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->synonyms), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (box->synonyms), FALSE);
  g_signal_connect (box->synonyms, "row-selected", G_CALLBACK (on_synonym_selected), box);
  g_signal_connect (box->synonyms, "row-activated", G_CALLBACK (on_synonym_activated), box);
  gtk_box_append (GTK_BOX (middle), list_in_frame (box->synonyms, 160));

  /* The column of buttons down the right, as Word 97 laid them out. */
  right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append (GTK_BOX (columns), right);
  box->replace_btn = side_button (right, "_Replace", G_CALLBACK (on_replace), box);
  side_button (right, "_Look Up", G_CALLBACK (on_look_up), box);
  cancel = gtk_button_new_with_mnemonic ("Cancel");
  gtk_widget_set_size_request (cancel, 96, 26);
  g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (gtk_window_destroy), box->window);
  gtk_box_append (GTK_BOX (right), cancel);
  box->previous_btn = side_button (right, "_Previous", G_CALLBACK (on_previous), box);
  gtk_widget_set_margin_top (box->previous_btn, 8);
  gtk_widget_set_sensitive (box->previous_btn, FALSE);

  box->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->status), 0.0);
  gtk_widget_add_css_class (box->status, "w42-dialog-status");
  gtk_box_append (GTK_BOX (content), box->status);

  gtk_window_set_default_widget (GTK_WINDOW (box->window), box->replace_btn);

  thesaurus_look_up (box, word, FALSE);
  if (box->senses == NULL)
    gtk_editable_set_text (GTK_EDITABLE (box->synonym), word);
  g_free (word);

  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->synonym);
  return TRUE;
}
