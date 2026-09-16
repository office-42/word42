/* w42-docmap.c - View > Document Map: the headings in a pane at the left
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 97's Document Map was a pane down the left of the window listing
 * every heading, indented by its level, with the one the caret was under
 * shown selected; clicking a heading went there.  This is that.  The list
 * is built from the same snapshot of the paragraphs the layout uses and
 * the same outline levels the table of contents uses, so the three agree
 * about what a heading is.
 */

#include "w42-docmap.h"

#include "w42-document.h"
#include "w42-style.h"

typedef struct {
  GtkWidget   *list;            /* the GtkListBox */
  W42View     *view;
  W42Document *doc;             /* the document the rows were built from */
  gulong       doc_changed_id;
  guint        rebuild_id;      /* a rebuild waiting for typing to pause */
  gboolean     stale;           /* the text changed while the map was hidden */
  gboolean     syncing;         /* a row is being selected from the caret,
                                 * not by the pointer: do not move the caret */
  GArray      *starts;          /* gsize per row: the heading's BLOCK strux */
} DocMap;

static void docmap_schedule (GtkWidget *map);

static DocMap *
docmap_of (GtkWidget *map)
{
  return g_object_get_data (G_OBJECT (map), "w42-docmap");
}

/* The heading the caret is under is the last one that starts at or before
 * it; -1 when the caret is above the first. */
static int
docmap_row_for_caret (DocMap *self, gsize caret)
{
  int found = -1;

  for (guint i = 0; i < self->starts->len; i++)
    {
      if (g_array_index (self->starts, gsize, i) > caret)
        break;
      found = (int) i;
    }
  return found;
}

static void
docmap_show_caret (GtkWidget *map)
{
  DocMap *self = docmap_of (map);
  int row;

  if (self->view == NULL || self->starts->len == 0)
    return;

  row = docmap_row_for_caret (self, w42_view_get_caret (self->view));
  self->syncing = TRUE;
  if (row < 0)
    gtk_list_box_unselect_all (GTK_LIST_BOX (self->list));
  else
    {
      GtkListBoxRow *r = gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->list), row);

      if (r != NULL && !gtk_list_box_row_is_selected (r))
        gtk_list_box_select_row (GTK_LIST_BOX (self->list), r);
    }
  self->syncing = FALSE;
}

static void
docmap_clear (DocMap *self)
{
  GtkListBoxRow *row;

  while ((row = gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->list), 0)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (self->list), GTK_WIDGET (row));
  g_array_set_size (self->starts, 0);
}

static gboolean
docmap_rebuild (gpointer data)
{
  GtkWidget *map = data;
  DocMap *self = docmap_of (map);
  W42PieceTable *pt;
  W42StyleSheet *styles;
  W42ApTable *aps;
  GPtrArray *blocks;
  gboolean numbering;
  int counters[10] = { 0 };

  self->rebuild_id = 0;

  /* A hidden map is not worth keeping up to date keystroke by keystroke;
   * it is built when it is shown. */
  if (!gtk_widget_get_visible (map))
    {
      self->stale = TRUE;
      return G_SOURCE_REMOVE;
    }
  self->stale = FALSE;

  docmap_clear (self);
  if (self->view == NULL || self->doc == NULL)
    return G_SOURCE_REMOVE;

  pt = w42_document_pt (self->doc);
  styles = w42_pt_stylesheet (pt);
  aps = w42_pt_ap_table (pt);
  numbering = w42_stylesheet_get_number_headings (styles);
  blocks = w42_pt_snapshot_blocks (pt);

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42Fmt *fmt = w42_ap_table_get (aps, block->ap);
      const char *style = fmt->pa.style;
      int level = style != NULL ? w42_stylesheet_outline (styles, style) : 0;
      GString *text;
      GtkWidget *label;
      GtkWidget *row;

      /* Notes and the insides of tables are not part of the outline, and
       * the Title is its top, as the slide show and the outline files
       * have it. */
      if (block->note >= 0 || block->table >= 0)
        continue;
      if (level == 0 && style != NULL && g_ascii_strcasecmp (style, "Title") == 0)
        level = 1;
      if (level <= 0 || level >= 10)
        continue;

      /* The counters run over empty headings too, as the layout's do, so
       * the numbers here are the numbers on the page. */
      counters[level]++;
      for (int l = level + 1; l < 10; l++)
        counters[l] = 0;

      text = g_string_new (NULL);
      if (numbering)
        {
          for (int l = 1; l <= level; l++)
            g_string_append_printf (text, l > 1 ? ".%d" : "%d", counters[l]);
          g_string_append_c (text, ' ');
        }
      for (const char *p = block->text->str; *p; p = g_utf8_next_char (p))
        {
          gunichar c = g_utf8_get_char (p);

          if (c == 0xFFFC)
            continue;
          g_string_append_unichar (text, c == '\t' ? ' ' : c);
        }
      if (g_strstrip (text->str)[0] == '\0')
        {
          g_string_free (text, TRUE);
          continue;
        }

      label = gtk_label_new (text->str);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_margin_start (label, 6 + (level - 1) * 14);
      gtk_widget_set_margin_end (label, 6);
      gtk_widget_set_margin_top (label, 2);
      gtk_widget_set_margin_bottom (label, 2);
      gtk_widget_set_tooltip_text (label, text->str);

      row = gtk_list_box_row_new ();
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
      gtk_list_box_append (GTK_LIST_BOX (self->list), row);
      g_array_append_val (self->starts, block->start_pos);
      g_string_free (text, TRUE);
    }

  g_ptr_array_free (blocks, TRUE);
  docmap_show_caret (map);
  return G_SOURCE_REMOVE;
}

/* A rebuild waits a moment, so that typing in a heading does not remake
 * the list at every character. */
static void
docmap_schedule (GtkWidget *map)
{
  DocMap *self = docmap_of (map);

  if (self->rebuild_id != 0)
    return;
  self->rebuild_id = g_timeout_add (250, docmap_rebuild, map);
}

static void
on_document_changed (W42Document *doc, gpointer data)
{
  docmap_schedule (data);
}

static void
docmap_watch_document (GtkWidget *map, W42Document *doc)
{
  DocMap *self = docmap_of (map);

  if (self->doc == doc)
    return;
  if (self->doc != NULL && self->doc_changed_id != 0)
    g_signal_handler_disconnect (self->doc, self->doc_changed_id);
  self->doc_changed_id = 0;
  g_set_object (&self->doc, doc);
  if (doc != NULL)
    self->doc_changed_id = g_signal_connect (doc, "changed",
                                             G_CALLBACK (on_document_changed), map);
  docmap_schedule (map);
}

static void
on_view_state_changed (W42View *view, gpointer data)
{
  GtkWidget *map = data;
  DocMap *self = docmap_of (map);

  /* The view may have been given another document since. */
  docmap_watch_document (map, w42_view_get_document (view));
  if (self->rebuild_id == 0 && !self->stale)
    docmap_show_caret (map);
}

static void
on_row_selected (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  GtkWidget *map = data;
  DocMap *self = docmap_of (map);
  int index;
  gsize pos;

  if (self->syncing || row == NULL || self->view == NULL)
    return;
  index = gtk_list_box_row_get_index (row);
  if (index < 0 || (guint) index >= self->starts->len)
    return;

  /* The heading's text starts just after its paragraph mark. */
  pos = g_array_index (self->starts, gsize, index) + 1;
  w42_view_select_range (self->view, pos, pos);
  gtk_widget_grab_focus (GTK_WIDGET (self->view));
}

static void
on_map_visible (GObject *map, GParamSpec *pspec, gpointer data)
{
  DocMap *self = docmap_of (GTK_WIDGET (map));

  if (gtk_widget_get_visible (GTK_WIDGET (map)) && self->stale)
    docmap_schedule (GTK_WIDGET (map));
}

static void
docmap_free (gpointer data)
{
  DocMap *self = data;

  if (self->rebuild_id != 0)
    g_source_remove (self->rebuild_id);
  if (self->doc != NULL && self->doc_changed_id != 0)
    g_signal_handler_disconnect (self->doc, self->doc_changed_id);
  g_clear_object (&self->doc);
  g_array_free (self->starts, TRUE);
  g_free (self);
}

GtkWidget *
w42_docmap_new (W42View *view)
{
  DocMap *self = g_new0 (DocMap, 1);
  GtkWidget *map;

  self->starts = g_array_new (FALSE, FALSE, sizeof (gsize));
  self->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->list), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (self->list), TRUE);
  gtk_widget_add_css_class (self->list, "w42-docmap");

  map = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (map),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (map), self->list);
  gtk_widget_set_size_request (map, 160, -1);
  g_object_set_data_full (G_OBJECT (map), "w42-docmap", self, docmap_free);

  g_signal_connect (self->list, "row-selected", G_CALLBACK (on_row_selected), map);
  g_signal_connect (map, "notify::visible", G_CALLBACK (on_map_visible), NULL);

  w42_docmap_set_view (map, view);
  return map;
}

void
w42_docmap_set_view (GtkWidget *map, W42View *view)
{
  DocMap *self = docmap_of (map);

  g_return_if_fail (self != NULL);
  g_return_if_fail (view == NULL || W42_IS_VIEW (view));

  if (self->view == view)
    return;
  if (self->view != NULL)
    g_signal_handlers_disconnect_by_func (self->view,
                                          G_CALLBACK (on_view_state_changed),
                                          map);
  self->view = view;
  if (view != NULL)
    {
      g_signal_connect_object (view, "state-changed",
                               G_CALLBACK (on_view_state_changed), map, 0);
      docmap_watch_document (map, w42_view_get_document (view));
    }
  docmap_schedule (map);
}
