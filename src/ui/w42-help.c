/* w42-help.c - see w42-help.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-help.h"

#include "w42-settings.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* The guide, read into topics                                             */
/* ---------------------------------------------------------------------- */

typedef struct {
  char      *title;      /* "Tables" */
  GString   *markup;     /* the section, in Pango markup */
  GString   *plain;      /* the same without the markup, for searching */
  GPtrArray *entries;    /* char*: the sub-headings, for the index */
} Topic;

static void
topic_free (gpointer data)
{
  Topic *topic = data;

  g_free (topic->title);
  g_string_free (topic->markup, TRUE);
  g_string_free (topic->plain, TRUE);
  g_ptr_array_free (topic->entries, TRUE);
  g_free (topic);
}

/* One line of the guide's markdown, as Pango markup.  Only what the
 * guide actually uses is understood: bold, code, links and escapes. */
static void
append_inline (GString *out, GString *plain, const char *line)
{
  const char *p = line;

  while (*p != '\0')
    {
      if (p[0] == '*' && p[1] == '*')
        {
          const char *end = strstr (p + 2, "**");

          if (end != NULL)
            {
              char *inner = g_strndup (p + 2, (gsize) (end - (p + 2)));
              char *escaped = g_markup_escape_text (inner, -1);

              g_string_append_printf (out, "<b>%s</b>", escaped);
              g_string_append (plain, inner);
              g_free (escaped);
              g_free (inner);
              p = end + 2;
              continue;
            }
        }
      if (p[0] == '`')
        {
          const char *end = strchr (p + 1, '`');

          if (end != NULL)
            {
              char *inner = g_strndup (p + 1, (gsize) (end - (p + 1)));
              char *escaped = g_markup_escape_text (inner, -1);

              g_string_append_printf (out, "<tt>%s</tt>", escaped);
              g_string_append (plain, inner);
              g_free (escaped);
              g_free (inner);
              p = end + 1;
              continue;
            }
        }
      if (p[0] == '[')
        {
          const char *close = strchr (p, ']');

          if (close != NULL && close[1] == '(')
            {
              const char *end = strchr (close, ')');

              if (end != NULL)
                {
                  char *inner = g_strndup (p + 1, (gsize) (close - (p + 1)));
                  char *escaped = g_markup_escape_text (inner, -1);

                  g_string_append (out, escaped);
                  g_string_append (plain, inner);
                  g_free (escaped);
                  g_free (inner);
                  p = end + 1;
                  continue;
                }
            }
        }
      {
        char one[2] = { *p, '\0' };
        char *escaped = g_markup_escape_text (one, 1);

        g_string_append (out, escaped);
        g_string_append (plain, one);
        g_free (escaped);
        p++;
      }
    }
}

/* A table row of the guide, as a line of its own: the bars become gaps,
 * and the row is set in the fixed-width face so that columns line up. */
static void
append_table_row (GString *out, GString *plain, const char *line)
{
  char **cells = g_strsplit (line, "|", -1);
  GString *row = g_string_new (NULL);

  for (guint i = 0; cells[i] != NULL; i++)
    {
      char *cell = g_strstrip (g_strdup (cells[i]));

      if (*cell != '\0')
        g_string_append_printf (row, "%s%s", row->len > 0 ? "  \342\200\224  " : "", cell);
      g_free (cell);
    }
  if (row->len > 0)
    {
      g_string_append (out, "    ");
      append_inline (out, plain, row->str);
      g_string_append_c (out, '\n');
      g_string_append_c (plain, '\n');
    }
  g_string_free (row, TRUE);
  g_strfreev (cells);
}

static Topic *
topic_new (const char *title)
{
  Topic *topic = g_new0 (Topic, 1);

  topic->title = g_strdup (title);
  topic->markup = g_string_new (NULL);
  topic->plain = g_string_new (NULL);
  topic->entries = g_ptr_array_new_with_free_func (g_free);
  return topic;
}

/* "## 9. Tables" -> "Tables" */
static char *
clean_title (const char *heading)
{
  const char *p = heading;

  while (*p == '#' || *p == ' ')
    p++;
  while (g_ascii_isdigit (*p))
    p++;
  if (*p == '.')
    p++;
  while (*p == ' ')
    p++;
  return g_strstrip (g_strdup (p));
}

/* The guide as a list of topics.  Read once and kept: it does not change
 * while the program runs. */
static GPtrArray *
help_topics (void)
{
  static GPtrArray *topics;
  GBytes *bytes;
  const char *text;
  gsize size = 0;
  char **lines;
  Topic *topic = NULL;
  gboolean paragraph_open = FALSE;

  if (topics != NULL)
    return topics;

  topics = g_ptr_array_new_with_free_func (topic_free);
  bytes = g_resources_lookup_data ("/org/word42/word42/guide.md",
                                   G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  if (bytes == NULL)
    return topics;

  text = g_bytes_get_data (bytes, &size);
  lines = g_strsplit (text, "\n", -1);

  for (guint i = 0; lines[i] != NULL; i++)
    {
      char *line = lines[i];
      gsize len;

      /* Strip a carriage return the file may carry. */
      len = strlen (line);
      if (len > 0 && line[len - 1] == '\r')
        line[len - 1] = '\0';

      if (g_str_has_prefix (line, "## "))
        {
          char *title = clean_title (line);

          topic = topic_new (title);
          g_ptr_array_add (topics, topic);
          paragraph_open = FALSE;
          g_free (title);
          continue;
        }
      if (topic == NULL)
        continue;                       /* the title and the contents list */

      if (*line == '\0')
        {
          if (paragraph_open)
            {
              g_string_append (topic->markup, "\n\n");
              g_string_append_c (topic->plain, '\n');
              paragraph_open = FALSE;
            }
          continue;
        }
      if (g_str_has_prefix (line, "---"))
        continue;                       /* a rule between sections */

      if (g_str_has_prefix (line, "### ") || g_str_has_prefix (line, "#### "))
        {
          char *title = clean_title (line);

          if (paragraph_open)
            g_string_append (topic->markup, "\n\n");
          g_string_append (topic->markup, "<b>");
          append_inline (topic->markup, topic->plain, title);
          g_string_append (topic->markup, "</b>\n\n");
          g_string_append_c (topic->plain, '\n');
          g_ptr_array_add (topic->entries, g_strdup (title));
          paragraph_open = FALSE;
          g_free (title);
          continue;
        }
      if (line[0] == '|')
        {
          if (strstr (line, "---") != NULL)
            continue;                   /* the rule under a table's head */
          if (paragraph_open)
            {
              g_string_append_c (topic->markup, '\n');
              paragraph_open = FALSE;
            }
          append_table_row (topic->markup, topic->plain, line);
          continue;
        }
      if (g_str_has_prefix (line, "- ") || g_str_has_prefix (line, "* "))
        {
          if (paragraph_open)
            g_string_append_c (topic->markup, '\n');
          g_string_append (topic->markup, "    \342\200\242 ");
          append_inline (topic->markup, topic->plain, line + 2);
          g_string_append_c (topic->markup, '\n');
          g_string_append_c (topic->plain, '\n');
          paragraph_open = FALSE;
          continue;
        }

      /* An ordinary line: the guide is wrapped by hand, and the window
       * wraps for itself, so the lines of a paragraph are joined. */
      if (paragraph_open)
        {
          g_string_append_c (topic->markup, ' ');
          g_string_append_c (topic->plain, ' ');
        }
      append_inline (topic->markup, topic->plain, g_strchug (line));
      paragraph_open = TRUE;
    }

  g_strfreev (lines);
  g_bytes_unref (bytes);
  return topics;
}

char **
w42_help_topic_titles (void)
{
  GPtrArray *topics = help_topics ();
  char **out = g_new0 (char *, topics->len + 1);

  for (guint i = 0; i < topics->len; i++)
    out[i] = g_strdup (((const Topic *) g_ptr_array_index (topics, i))->title);
  return out;
}

static const Topic *
topic_by_title (const char *title)
{
  GPtrArray *topics = help_topics ();

  for (guint i = 0; i < topics->len; i++)
    {
      const Topic *topic = g_ptr_array_index (topics, i);

      if (title != NULL && g_ascii_strcasecmp (topic->title, title) == 0)
        return topic;
    }
  return NULL;
}

char *
w42_help_topic_markup (const char *title)
{
  const Topic *topic = topic_by_title (title);

  return topic != NULL ? g_strdup (topic->markup->str) : NULL;
}

char *
w42_help_topic_text (const char *title)
{
  const Topic *topic = topic_by_title (title);

  return topic != NULL ? g_strdup (topic->plain->str) : NULL;
}

/* ---------------------------------------------------------------------- */
/* The window                                                              */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  GtkWidget *search;
  GtkWidget *list;
  GtkWidget *body;
  GtkWidget *scroller;
  GtkWidget *what;          /* the label over the list */
  GPtrArray *shown;         /* int: which topic each row is */
  GPtrArray *anchors;       /* char*: the sub-heading a row points at, or NULL */
  gboolean   index_mode;
} HelpBox;

static void
help_free (gpointer data, GObject *where)
{
  HelpBox *box = data;

  (void) where;
  g_ptr_array_free (box->shown, TRUE);
  g_ptr_array_free (box->anchors, TRUE);
  g_free (box);
}

static void
help_clear_list (HelpBox *box)
{
  GtkWidget *row;

  while ((row = gtk_widget_get_first_child (box->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (box->list), row);
  g_ptr_array_set_size (box->shown, 0);
  g_ptr_array_set_size (box->anchors, 0);
}

static void
help_add_row (HelpBox *box, const char *text, int topic, const char *anchor)
{
  GtkWidget *label = gtk_label_new (text);

  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_set_margin_start (label, 6);
  gtk_widget_set_margin_end (label, 6);
  gtk_widget_set_margin_top (label, 1);
  gtk_widget_set_margin_bottom (label, 1);
  gtk_list_box_append (GTK_LIST_BOX (box->list), label);
  g_ptr_array_add (box->shown, GINT_TO_POINTER (topic));
  g_ptr_array_add (box->anchors, anchor != NULL ? g_strdup (anchor) : NULL);
}

/* The contents: one row per section of the guide. */
static void
help_show_contents (HelpBox *box)
{
  GPtrArray *topics = help_topics ();

  box->index_mode = FALSE;
  gtk_label_set_text (GTK_LABEL (box->what), "Contents");
  help_clear_list (box);
  for (guint i = 0; i < topics->len; i++)
    {
      const Topic *topic = g_ptr_array_index (topics, i);

      help_add_row (box, topic->title, (int) i, NULL);
    }
}

static int
compare_rows (gconstpointer a, gconstpointer b)
{
  const char * const *x = a;
  const char * const *y = b;

  return g_ascii_strcasecmp (*x, *y);
}

/* The index: every sub-heading in the guide, in alphabetical order, with
 * the section it is in. */
static void
help_show_index (HelpBox *box)
{
  GPtrArray *topics = help_topics ();
  GPtrArray *rows = g_ptr_array_new_with_free_func (g_free);
  GHashTable *where = g_hash_table_new (g_str_hash, g_str_equal);

  box->index_mode = TRUE;
  gtk_label_set_text (GTK_LABEL (box->what), "Index");
  help_clear_list (box);

  for (guint i = 0; i < topics->len; i++)
    {
      const Topic *topic = g_ptr_array_index (topics, i);

      for (guint e = 0; e < topic->entries->len; e++)
        {
          char *entry = g_ptr_array_index (topic->entries, e);

          if (g_hash_table_contains (where, entry))
            continue;
          g_hash_table_insert (where, entry, GINT_TO_POINTER ((int) i));
          g_ptr_array_add (rows, g_strdup (entry));
        }
      /* The section's own name belongs in the index too. */
      if (!g_hash_table_contains (where, topic->title))
        {
          g_hash_table_insert (where, topic->title, GINT_TO_POINTER ((int) i));
          g_ptr_array_add (rows, g_strdup (topic->title));
        }
    }

  g_ptr_array_sort (rows, compare_rows);
  for (guint i = 0; i < rows->len; i++)
    {
      const char *entry = g_ptr_array_index (rows, i);

      help_add_row (box, entry, GPOINTER_TO_INT (g_hash_table_lookup (where, entry)), entry);
    }

  g_hash_table_destroy (where);
  g_ptr_array_free (rows, TRUE);
}

/* A search: the sections whose text holds the words asked for, the best
 * -- the one that has it in its title -- first. */
static void
help_show_search (HelpBox *box, const char *term)
{
  GPtrArray *topics = help_topics ();
  char *needle = g_utf8_casefold (term, -1);
  int found = 0;

  gtk_label_set_text (GTK_LABEL (box->what), "Found");
  help_clear_list (box);

  for (int pass = 0; pass < 2; pass++)
    for (guint i = 0; i < topics->len; i++)
      {
        const Topic *topic = g_ptr_array_index (topics, i);
        char *title = g_utf8_casefold (topic->title, -1);
        char *plain = g_utf8_casefold (topic->plain->str, -1);
        gboolean in_title = strstr (title, needle) != NULL;
        gboolean in_body = strstr (plain, needle) != NULL;

        if ((pass == 0 && in_title) || (pass == 1 && in_body && !in_title))
          {
            help_add_row (box, topic->title, (int) i, term);
            found++;
          }
        g_free (title);
        g_free (plain);
      }

  if (found == 0)
    help_add_row (box, "(nothing found)", -1, NULL);
  g_free (needle);
}

static void
help_row_chosen (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  HelpBox *box = data;
  GPtrArray *topics = help_topics ();
  int index, which;
  const Topic *topic;

  (void) list;
  if (row == NULL)
    return;
  index = gtk_list_box_row_get_index (row);
  if (index < 0 || (guint) index >= box->shown->len)
    return;
  which = GPOINTER_TO_INT (g_ptr_array_index (box->shown, (guint) index));
  if (which < 0 || (guint) which >= topics->len)
    {
      gtk_label_set_text (GTK_LABEL (box->body),
                          "Nothing in the guide answers to that.  Try a "
                          "shorter word, or look through the contents.");
      return;
    }

  topic = g_ptr_array_index (topics, (guint) which);
  gtk_label_set_markup (GTK_LABEL (box->body), topic->markup->str);
  gtk_window_set_title (GTK_WINDOW (box->window),
                        topic->title != NULL ? topic->title : "Word42 Help");
  {
    GtkAdjustment *up = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (box->scroller));

    if (up != NULL)
      gtk_adjustment_set_value (up, 0.0);
  }
}

static void
help_search_changed (GtkEditable *entry, gpointer data)
{
  HelpBox *box = data;
  const char *term = gtk_editable_get_text (entry);

  if (term == NULL || *term == '\0')
    {
      if (box->index_mode)
        help_show_index (box);
      else
        help_show_contents (box);
    }
  else
    {
      help_show_search (box, term);
    }
  gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                           gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 0));
}

static void
help_contents_clicked (GtkButton *button, gpointer data)
{
  HelpBox *box = data;

  (void) button;
  gtk_editable_set_text (GTK_EDITABLE (box->search), "");
  help_show_contents (box);
  gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                           gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 0));
}

static void
help_index_clicked (GtkButton *button, gpointer data)
{
  HelpBox *box = data;

  (void) button;
  gtk_editable_set_text (GTK_EDITABLE (box->search), "");
  help_show_index (box);
  gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                           gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 0));
}

static gboolean
help_key (GtkEventControllerKey *key, guint keyval, guint code,
          GdkModifierType state, gpointer data)
{
  (void) key; (void) code; (void) state;
  if (keyval == GDK_KEY_Escape)
    {
      gtk_window_destroy (GTK_WINDOW (data));
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}

/* One help window at a time, as Word 6 had one: asking again brings the
 * one that is open to the front. */
static GtkWidget *open_help;

static void
help_gone (gpointer data, GObject *where)
{
  (void) data; (void) where;
  open_help = NULL;
}

static HelpBox *
help_build (GtkWindow *parent)
{
  HelpBox *box = g_new0 (HelpBox, 1);
  GtkWidget *content, *top, *paned, *left, *right, *buttons;
  GtkWidget *contents_button, *index_button;
  GtkEventController *key = gtk_event_controller_key_new ();

  box->shown = g_ptr_array_new ();
  box->anchors = g_ptr_array_new_with_free_func (g_free);

  box->window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (box->window), "Word42 Help");
  gtk_window_set_transient_for (GTK_WINDOW (box->window), parent);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (box->window), TRUE);
  gtk_window_set_default_size (GTK_WINDOW (box->window), 760, 580);
  gtk_widget_add_css_class (box->window, "w42");
  g_signal_connect (key, "key-pressed", G_CALLBACK (help_key), box->window);
  gtk_widget_add_controller (box->window, key);
  g_object_weak_ref (G_OBJECT (box->window), help_free, box);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start (content, 10);
  gtk_widget_set_margin_end (content, 10);
  gtk_widget_set_margin_top (content, 10);
  gtk_widget_set_margin_bottom (content, 10);
  gtk_window_set_child (GTK_WINDOW (box->window), content);

  top = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  box->search = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (box->search), "Search for help on...");
  gtk_widget_set_hexpand (box->search, TRUE);
  contents_button = gtk_button_new_with_mnemonic ("_Contents");
  index_button = gtk_button_new_with_mnemonic ("_Index");
  gtk_widget_set_size_request (contents_button, 92, 26);
  gtk_widget_set_size_request (index_button, 92, 26);
  gtk_box_append (GTK_BOX (top), box->search);
  gtk_box_append (GTK_BOX (top), contents_button);
  gtk_box_append (GTK_BOX (top), index_button);
  gtk_box_append (GTK_BOX (content), top);

  paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_vexpand (paned, TRUE);
  gtk_box_append (GTK_BOX (content), paned);

  box->what = gtk_label_new ("Contents");
  gtk_label_set_xalign (GTK_LABEL (box->what), 0.0);
  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_BROWSE);
  left = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (left),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (left), box->list);
  gtk_widget_set_size_request (left, 210, -1);
  {
    GtkWidget *side = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

    gtk_box_append (GTK_BOX (side), box->what);
    gtk_widget_set_vexpand (left, TRUE);
    gtk_box_append (GTK_BOX (side), left);
    gtk_paned_set_start_child (GTK_PANED (paned), side);
  }

  box->body = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (box->body), TRUE);
  gtk_label_set_xalign (GTK_LABEL (box->body), 0.0);
  gtk_label_set_yalign (GTK_LABEL (box->body), 0.0);
  gtk_label_set_selectable (GTK_LABEL (box->body), TRUE);
  gtk_widget_set_margin_start (box->body, 12);
  gtk_widget_set_margin_end (box->body, 12);
  gtk_widget_set_margin_top (box->body, 8);
  gtk_widget_set_margin_bottom (box->body, 8);
  gtk_widget_add_css_class (box->body, "w42-help");
  right = gtk_scrolled_window_new ();
  box->scroller = right;
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (right),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (right), box->body);
  gtk_paned_set_end_child (GTK_PANED (paned), right);
  gtk_paned_set_position (GTK_PANED (paned), 220);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  {
    GtkWidget *close = gtk_button_new_with_mnemonic ("_Close");

    gtk_widget_set_size_request (close, 92, 26);
    g_signal_connect_swapped (close, "clicked", G_CALLBACK (gtk_window_destroy),
                              box->window);
    gtk_box_append (GTK_BOX (buttons), close);
  }
  gtk_box_append (GTK_BOX (content), buttons);

  g_signal_connect (box->list, "row-selected", G_CALLBACK (help_row_chosen), box);
  g_signal_connect (box->search, "changed", G_CALLBACK (help_search_changed), box);
  g_signal_connect (contents_button, "clicked", G_CALLBACK (help_contents_clicked), box);
  g_signal_connect (index_button, "clicked", G_CALLBACK (help_index_clicked), box);

  open_help = box->window;
  g_object_weak_ref (G_OBJECT (box->window), help_gone, NULL);
  return box;
}

static HelpBox *
help_of (GtkWidget *window)
{
  return g_object_get_data (G_OBJECT (window), "w42-help-box");
}

void
w42_help_window_show (GtkWindow *parent, const char *find)
{
  HelpBox *box;

  if (open_help != NULL)
    {
      box = help_of (open_help);
      gtk_window_present (GTK_WINDOW (open_help));
    }
  else
    {
      box = help_build (parent);
      g_object_set_data (G_OBJECT (box->window), "w42-help-box", box);
      help_show_contents (box);
      gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                               gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 0));
      gtk_window_present (GTK_WINDOW (box->window));
    }

  if (box != NULL && find != NULL)
    {
      gtk_editable_set_text (GTK_EDITABLE (box->search), find);
      gtk_widget_grab_focus (box->search);
    }
}

void
w42_help_index_show (GtkWindow *parent)
{
  HelpBox *box;

  w42_help_window_show (parent, NULL);
  box = open_help != NULL ? help_of (open_help) : NULL;
  if (box == NULL)
    return;
  gtk_editable_set_text (GTK_EDITABLE (box->search), "");
  help_show_index (box);
  gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                           gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 0));
}

/* ---------------------------------------------------------------------- */
/* Tip of the Day                                                          */
/* ---------------------------------------------------------------------- */

static const char * const TIPS[] = {
  "Ctrl+F3 puts an AutoText entry in wherever the caret is: type the "
  "entry's name and press it.  Edit \342\226\270 AutoText makes an entry "
  "out of whatever is selected.",

  "The ruler is live.  Drag the markers to set the indents, click the "
  "ruler to put a tab stop where you clicked, drag a stop off the ruler "
  "to be rid of it, and click the box at its left end to change what "
  "kind of stop the next one will be.",

  "Tab in the last cell of a table adds a row.  Shift+Tab goes back a "
  "cell, and Table \342\226\270 Table AutoFormat puts a whole look on a "
  "table at once.",

  "Select down a column and Table \342\226\270 Merge Cells makes one cell "
  "as tall as the rows it covers.  Split Cells gives the rows back.",

  "Ctrl+Shift+8 shows the formatting marks: a dot for a space, an arrow "
  "for a tab, a pilcrow at the end of every paragraph.",

  "Word42 saves a copy of a changed document every two minutes.  If the "
  "program stops without a clean close, the next start offers the copy.",

  "Tools \342\226\270 Language marks the selected text as written in a "
  "language of its own, and the spelling checker then uses that "
  "language's dictionary.",

  "F7 opens the spelling box; the words the dictionary does not know are "
  "underlined in red as you type.  Tools \342\226\270 Automatic Spell "
  "Checking turns the underlining off.",

  "Insert \342\226\270 Table of Contents makes a contents page out of the "
  "headings, with the page numbers at a right tab stop and dots leading "
  "to them.",

  "Ctrl+Enter starts a new page.  Insert \342\226\270 Break puts in a "
  "column break or a section break instead.",

  "The style box at the left of the formatting toolbar applies a style; "
  "Format \342\226\270 Style changes what a style means, and every "
  "paragraph in that style follows.",

  "File \342\226\270 Export as Presentation makes a slide show out of the "
  "headings, and View \342\226\270 Slide Show puts it on the screen.",

  "Double-click a word to select it, triple-click for the whole "
  "paragraph.  Ctrl+A selects everything.",

  "File \342\226\270 Revert goes back to the document as it was when you "
  "last saved it.",

  "Word42 writes RTF, Word (.docx), OpenDocument (.odt), AbiWord (.abw), "
  "web pages and PDF, and reads all of those and Word 97\342\200\2232003 "
  "documents besides.",

  "Format \342\226\270 Drop Cap sets the first letter of a paragraph "
  "large, dropped into the text beside it, the way a printed book opens "
  "a chapter.",
};

#define TIP_KEY "tip-of-the-day"
#define TIPS_AT_STARTUP "tips-at-startup"

typedef struct {
  GtkWidget *window;
  GtkWidget *text;
  GtkWidget *at_startup;
  int        which;
} TipBox;

static void
tip_free (gpointer data, GObject *where)
{
  (void) where;
  g_free (data);
}

static void
tip_set (TipBox *box)
{
  box->which = box->which % (int) G_N_ELEMENTS (TIPS);
  gtk_label_set_text (GTK_LABEL (box->text), TIPS[box->which]);
  w42_settings_set_int (TIP_KEY, box->which + 1);
}

static void
on_tip_next (GtkButton *button, gpointer data)
{
  TipBox *box = data;

  (void) button;
  box->which++;
  tip_set (box);
}

static void
on_tip_startup (GtkCheckButton *button, gpointer data)
{
  (void) data;
  w42_settings_set_bool (TIPS_AT_STARTUP,
                         gtk_check_button_get_active (button));
}

void
w42_tip_of_the_day_show (GtkWindow *parent, gboolean at_startup)
{
  TipBox *box;
  GtkWidget *content, *heading, *frame, *buttons, *next, *ok;
  GtkEventController *key = gtk_event_controller_key_new ();

  if (at_startup && !w42_settings_get_bool (TIPS_AT_STARTUP, TRUE))
    return;

  box = g_new0 (TipBox, 1);
  box->which = w42_settings_get_int (TIP_KEY, 0);

  box->window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (box->window), "Tip of the Day");
  gtk_window_set_transient_for (GTK_WINDOW (box->window), parent);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (box->window), TRUE);
  gtk_window_set_resizable (GTK_WINDOW (box->window), FALSE);
  gtk_widget_add_css_class (box->window, "w42");
  g_signal_connect (key, "key-pressed", G_CALLBACK (help_key), box->window);
  gtk_widget_add_controller (box->window, key);
  g_object_weak_ref (G_OBJECT (box->window), tip_free, box);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_add_css_class (content, "w42-dialog");
  gtk_widget_set_margin_start (content, 14);
  gtk_widget_set_margin_end (content, 14);
  gtk_widget_set_margin_top (content, 14);
  gtk_widget_set_margin_bottom (content, 14);
  gtk_window_set_child (GTK_WINDOW (box->window), content);

  heading = gtk_label_new (NULL);
  gtk_label_set_markup (GTK_LABEL (heading), "<b>Did you know...</b>");
  gtk_label_set_xalign (GTK_LABEL (heading), 0.0);
  gtk_box_append (GTK_BOX (content), heading);

  box->text = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (box->text), TRUE);
  gtk_label_set_xalign (GTK_LABEL (box->text), 0.0);
  gtk_label_set_yalign (GTK_LABEL (box->text), 0.0);
  gtk_widget_set_size_request (box->text, 420, 84);
  frame = gtk_frame_new (NULL);
  gtk_widget_set_margin_start (box->text, 10);
  gtk_widget_set_margin_end (box->text, 10);
  gtk_widget_set_margin_top (box->text, 10);
  gtk_widget_set_margin_bottom (box->text, 10);
  gtk_frame_set_child (GTK_FRAME (frame), box->text);
  gtk_box_append (GTK_BOX (content), frame);

  box->at_startup = gtk_check_button_new_with_mnemonic ("_Show tips when Word42 starts");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->at_startup),
                               w42_settings_get_bool (TIPS_AT_STARTUP, TRUE));
  g_signal_connect (box->at_startup, "toggled", G_CALLBACK (on_tip_startup), box);
  gtk_box_append (GTK_BOX (content), box->at_startup);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  next = gtk_button_new_with_mnemonic ("_Next Tip");
  ok = gtk_button_new_with_mnemonic ("_OK");
  gtk_widget_set_size_request (next, 92, 26);
  gtk_widget_set_size_request (ok, 92, 26);
  g_signal_connect (next, "clicked", G_CALLBACK (on_tip_next), box);
  g_signal_connect_swapped (ok, "clicked", G_CALLBACK (gtk_window_destroy), box->window);
  gtk_box_append (GTK_BOX (buttons), next);
  gtk_box_append (GTK_BOX (buttons), ok);
  gtk_box_append (GTK_BOX (content), buttons);
  gtk_window_set_default_widget (GTK_WINDOW (box->window), ok);

  tip_set (box);
  gtk_window_present (GTK_WINDOW (box->window));
}
