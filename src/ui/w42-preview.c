/* w42-preview.c - see w42-preview.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-preview.h"

#include "w42-layout.h"
#include "w42-print.h"

#include <math.h>

#define PAGE_GAP 24.0

static const double ZOOMS[] = { 0.25, 0.35, 0.5, 0.65, 0.8, 1.0, 1.25, 1.5, 2.0 };

struct _W42Preview {
  GtkWindow    parent_instance;

  W42Document *doc;
  GtkWindow   *owner;
  W42Layout   *layout;
  gulong       changed_id;

  GtkWidget   *area;
  GtkWidget   *scrolled;
  GtkWidget   *page_label;
  GtkWidget   *zoom_label;
  guint        zoom_index;
};

G_DEFINE_FINAL_TYPE (W42Preview, w42_preview, GTK_TYPE_WINDOW)

static double
preview_zoom (W42Preview *self)
{
  return ZOOMS[self->zoom_index];
}

static void
preview_resize (W42Preview *self)
{
  double zoom = preview_zoom (self);
  double w = w42_layout_page_width (self->layout) * zoom + 2 * PAGE_GAP;
  double h = PAGE_GAP + w42_layout_n_pages (self->layout) *
             (w42_layout_page_height (self->layout) * zoom + PAGE_GAP);

  gtk_drawing_area_set_content_width (GTK_DRAWING_AREA (self->area), (int) ceil (w));
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (self->area), (int) ceil (h));
  gtk_widget_queue_draw (self->area);
}

static void
preview_rebuild (W42Preview *self)
{
  w42_layout_build (self->layout, self->doc);
  preview_resize (self);
}

static void
update_page_label (W42Preview *self)
{
  GtkAdjustment *vadj =
    gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled));
  double zoom = preview_zoom (self);
  double page_h = w42_layout_page_height (self->layout) * zoom + PAGE_GAP;
  double middle = gtk_adjustment_get_value (vadj) +
                  gtk_adjustment_get_page_size (vadj) / 2.0;
  int n = w42_layout_n_pages (self->layout);
  int page = CLAMP ((int) floor ((middle - PAGE_GAP) / page_h), 0, n - 1);
  char text[64];

  g_snprintf (text, sizeof text, "Page %d of %d", page + 1, n);
  gtk_label_set_text (GTK_LABEL (self->page_label), text);

  g_snprintf (text, sizeof text, "%d%%", (int) (zoom * 100 + 0.5));
  gtk_label_set_text (GTK_LABEL (self->zoom_label), text);
}

static void
draw_preview (GtkDrawingArea *area, cairo_t *cr, int width, int height,
              gpointer data)
{
  W42Preview *self = data;
  double zoom = preview_zoom (self);
  const GArray *lines = w42_layout_lines (self->layout);
  double page_w = w42_layout_page_width (self->layout) * zoom;
  double page_h = w42_layout_page_height (self->layout) * zoom;
  double ox = MAX (PAGE_GAP, (width - page_w) / 2.0);
  int n = w42_layout_n_pages (self->layout);

  /* `height` is the whole run of pages, not the part on screen, so the
   * band actually being drawn is what the pages are culled against:
   * without it every page in the document is laid out again on every
   * scroll tick. */
  double top = 0, bottom = height;

  (void) area;

  {
    double x1, y1, x2, y2;

    cairo_clip_extents (cr, &x1, &y1, &x2, &y2);
    if (y2 > y1)
      {
        top = y1;
        bottom = y2;
      }
  }

  cairo_set_source_rgb (cr, 0.50, 0.50, 0.50);
  cairo_paint (cr);

  for (int p = 0; p < n; p++)
    {
      double oy = PAGE_GAP + p * (page_h + PAGE_GAP);

      if (oy > bottom || oy + page_h < top)
        continue;

      cairo_set_source_rgba (cr, 0, 0, 0, 0.35);
      cairo_rectangle (cr, ox + 3, oy + 3, page_w, page_h);
      cairo_fill (cr);

      {
        /* The paper is white unless Format > Background says otherwise. */
        const W42PageSetup *setup = w42_document_page_setup (self->doc);

        if (setup != NULL && setup->has_background)
          cairo_set_source_rgb (cr, ((setup->background >> 16) & 0xFF) / 255.0,
                                ((setup->background >> 8) & 0xFF) / 255.0,
                                (setup->background & 0xFF) / 255.0);
        else
          cairo_set_source_rgb (cr, 1, 1, 1);
      }
      cairo_rectangle (cr, ox, oy, page_w, page_h);
      cairo_fill_preserve (cr);
      cairo_set_source_rgb (cr, 0.2, 0.2, 0.2);
      cairo_set_line_width (cr, 1.0);
      cairo_stroke (cr);

      cairo_save (cr);
      cairo_rectangle (cr, ox, oy, page_w, page_h);
      cairo_clip (cr);
      cairo_translate (cr, ox, oy);
      cairo_scale (cr, zoom, zoom);
      cairo_set_source_rgb (cr, 0, 0, 0);

      w42_layout_draw_backdrop (self->layout, cr, p);

      for (guint i = 0; i < lines->len; i++)
        {
          const W42LineBox *box = &g_array_index (lines, W42LineBox, i);

          if (box->page != p)
            continue;

          w42_layout_draw_line (self->layout, cr, box);
        }

      w42_layout_draw_furniture (self->layout, cr, p);

      cairo_restore (cr);
    }
}

static void
set_zoom_index (W42Preview *self, int index)
{
  self->zoom_index = CLAMP (index, 0, (int) G_N_ELEMENTS (ZOOMS) - 1);
  preview_resize (self);
  update_page_label (self);
}

static void
on_zoom_in (GtkButton *b, gpointer data)
{
  (void) b;
  set_zoom_index (data, (int) W42_PREVIEW (data)->zoom_index + 1);
}

static void
on_zoom_out (GtkButton *b, gpointer data)
{
  (void) b;
  set_zoom_index (data, (int) W42_PREVIEW (data)->zoom_index - 1);
}

static void
on_print (GtkButton *b, gpointer data)
{
  W42Preview *self = data;

  (void) b;
  w42_print_document (self->owner, self->doc, FALSE);
}

static void
on_close (GtkButton *b, gpointer data)
{
  (void) b;
  gtk_window_close (GTK_WINDOW (data));
}

static void
on_scrolled (GtkAdjustment *adj, gpointer data)
{
  (void) adj;
  update_page_label (data);
}

static void
on_doc_changed (W42Document *doc, gpointer data)
{
  (void) doc;
  preview_rebuild (data);
  update_page_label (data);
}

static gboolean
on_key (GtkEventControllerKey *c, guint keyval, guint keycode,
        GdkModifierType state, gpointer data)
{
  W42Preview *self = data;

  (void) c; (void) keycode; (void) state;

  switch (keyval)
    {
    case GDK_KEY_Escape:
      gtk_window_close (GTK_WINDOW (self));
      return GDK_EVENT_STOP;
    case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
      set_zoom_index (self, (int) self->zoom_index + 1);
      return GDK_EVENT_STOP;
    case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
      set_zoom_index (self, (int) self->zoom_index - 1);
      return GDK_EVENT_STOP;
    default:
      return GDK_EVENT_PROPAGATE;
    }
}

static GtkWidget *
bar_button (const char *label, GCallback cb, gpointer data)
{
  GtkWidget *button = gtk_button_new_with_mnemonic (label);

  gtk_widget_set_focusable (button, FALSE);
  g_signal_connect (button, "clicked", cb, data);
  return button;
}

static void
w42_preview_dispose (GObject *object)
{
  W42Preview *self = W42_PREVIEW (object);

  if (self->doc != NULL && self->changed_id != 0)
    {
      g_signal_handler_disconnect (self->doc, self->changed_id);
      self->changed_id = 0;
    }

  g_clear_object (&self->doc);
  g_clear_pointer (&self->layout, w42_layout_free);

  G_OBJECT_CLASS (w42_preview_parent_class)->dispose (object);
}

static void
w42_preview_class_init (W42PreviewClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = w42_preview_dispose;
}

static void
w42_preview_init (W42Preview *self)
{
  GtkWidget *box, *bar;
  GtkEventController *key;

  self->layout = w42_layout_new ();
  w42_layout_set_galley (self->layout, FALSE);
  self->zoom_index = 3;      /* 65%: a whole Letter page in a 700px window */

  gtk_window_set_title (GTK_WINDOW (self), "Print Preview");
  gtk_window_set_default_size (GTK_WINDOW (self), 760, 820);
  gtk_widget_add_css_class (GTK_WIDGET (self), "w42");

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child (GTK_WINDOW (self), box);

  /* Word 6's preview bar: Print, zoom, page count, Close. */
  bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (bar, "w42-toolbar");
  gtk_box_append (GTK_BOX (bar), bar_button ("_Print...", G_CALLBACK (on_print), self));
  gtk_box_append (GTK_BOX (bar), bar_button ("Zoom _Out", G_CALLBACK (on_zoom_out), self));
  self->zoom_label = gtk_label_new ("65%");
  gtk_widget_set_size_request (self->zoom_label, 44, -1);
  gtk_box_append (GTK_BOX (bar), self->zoom_label);
  gtk_box_append (GTK_BOX (bar), bar_button ("Zoom _In", G_CALLBACK (on_zoom_in), self));
  self->page_label = gtk_label_new ("");
  gtk_widget_set_hexpand (self->page_label, TRUE);
  gtk_box_append (GTK_BOX (bar), self->page_label);
  gtk_box_append (GTK_BOX (bar), bar_button ("_Close", G_CALLBACK (on_close), self));
  gtk_box_append (GTK_BOX (box), bar);

  self->scrolled = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (self->scrolled, TRUE);
  gtk_box_append (GTK_BOX (box), self->scrolled);

  self->area = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (self->area), draw_preview,
                                  self, NULL);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scrolled), self->area);

  g_signal_connect (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled)),
                    "value-changed", G_CALLBACK (on_scrolled), self);

  key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), key);
}

GtkWidget *
w42_preview_new (GtkWindow *parent, W42Document *doc)
{
  W42Preview *self;

  g_return_val_if_fail (W42_IS_DOCUMENT (doc), NULL);

  self = g_object_new (W42_TYPE_PREVIEW, NULL);
  self->doc = g_object_ref (doc);
  self->owner = parent;

  gtk_window_set_transient_for (GTK_WINDOW (self), parent);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (self), TRUE);

  /* The preview follows the document: edit in the main window and the
   * pages here re-flow. */
  self->changed_id = g_signal_connect (doc, "changed",
                                       G_CALLBACK (on_doc_changed), self);

  preview_rebuild (self);
  update_page_label (self);

  return GTK_WIDGET (self);
}
