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

/* Word XP's zoom box offered these beside the percentages. */
typedef enum {
  FIT_NONE = 0,
  FIT_PAGE_WIDTH,
  FIT_WHOLE_PAGE
} FitMode;

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
  GtkWidget   *zoom_drop;
  guint        zoom_index;
  FitMode      fit;
  int          columns;        /* pages across: One Page or Multiple Pages */
  gboolean     magnified;      /* the magnifier: a click took the page to 100% */
  guint        zoom_before;    /* and this is what it was */
  FitMode      fit_before;
  int          columns_before;
};

G_DEFINE_FINAL_TYPE (W42Preview, w42_preview, GTK_TYPE_WINDOW)

/* The window's viewport, less the scrollbar, for the fitting zooms. */
static void
viewport_size (W42Preview *self, double *w, double *h)
{
  int vw = gtk_widget_get_width (self->scrolled);
  int vh = gtk_widget_get_height (self->scrolled);

  *w = vw > 0 ? vw - 20.0 : 720.0;
  *h = vh > 0 ? vh : 700.0;
}

static double
preview_zoom (W42Preview *self)
{
  double vw, vh, pw = w42_layout_page_width (self->layout), ph = w42_layout_page_height (self->layout);
  double zoom;

  if (self->fit == FIT_NONE)
    return ZOOMS[self->zoom_index];
  viewport_size (self, &vw, &vh);
  zoom = (vw - (self->columns + 1) * PAGE_GAP) / (pw * self->columns);
  if (self->fit == FIT_WHOLE_PAGE)
    zoom = MIN (zoom, (vh - 2 * PAGE_GAP) / ph);
  return CLAMP (zoom, 0.1, 4.0);
}

/* The rows the pages fall into, `columns` across. */
static int
preview_rows (W42Preview *self)
{
  int n = w42_layout_n_pages (self->layout);

  return (n + self->columns - 1) / self->columns;
}

static void
preview_resize (W42Preview *self)
{
  double zoom = preview_zoom (self);
  double w = self->columns * (w42_layout_page_width (self->layout) * zoom + PAGE_GAP) + PAGE_GAP;
  double h = PAGE_GAP + preview_rows (self) *
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
  int row = CLAMP ((int) floor ((middle - PAGE_GAP) / page_h), 0, MAX (preview_rows (self) - 1, 0));
  int page = CLAMP (row * self->columns, 0, n - 1);
  char text[64];

  if (self->columns > 1 && row * self->columns + self->columns <= n)
    g_snprintf (text, sizeof text, "Pages %d-%d of %d", page + 1, page + self->columns, n);
  else
    g_snprintf (text, sizeof text, "Page %d of %d", page + 1, n);
  gtk_label_set_text (GTK_LABEL (self->page_label), text);

  g_snprintf (text, sizeof text, "%d%%", (int) (zoom * 100 + 0.5));
  gtk_label_set_text (GTK_LABEL (self->zoom_label), text);
}

/* The page under a point of the drawing area, and where on it. */
static int
page_at (W42Preview *self, double x, double y, double *page_x, double *page_y)
{
  double zoom = preview_zoom (self);
  double page_w = w42_layout_page_width (self->layout) * zoom;
  double page_h = w42_layout_page_height (self->layout) * zoom;
  int width = gtk_widget_get_width (self->area);
  double row_w = self->columns * (page_w + PAGE_GAP) - PAGE_GAP;
  double ox = MAX (PAGE_GAP, (width - row_w) / 2.0);
  int col = (int) floor ((x - ox) / (page_w + PAGE_GAP));
  int row = (int) floor ((y - PAGE_GAP) / (page_h + PAGE_GAP));
  int page;

  if (col < 0 || col >= self->columns || row < 0)
    return -1;
  page = row * self->columns + col;
  if (page >= w42_layout_n_pages (self->layout))
    return -1;
  if (page_x != NULL)
    *page_x = (x - ox - col * (page_w + PAGE_GAP)) / zoom;
  if (page_y != NULL)
    *page_y = (y - PAGE_GAP - row * (page_h + PAGE_GAP)) / zoom;
  return page;
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
  double row_w = self->columns * (page_w + PAGE_GAP) - PAGE_GAP;
  double ox0 = MAX (PAGE_GAP, (width - row_w) / 2.0);
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
      double oy = PAGE_GAP + (p / self->columns) * (page_h + PAGE_GAP);
      double ox = ox0 + (p % self->columns) * (page_w + PAGE_GAP);

      if (oy > bottom || oy + page_h < top)
        continue;

      cairo_set_source_rgba (cr, 0, 0, 0, 0.35);
      cairo_rectangle (cr, ox + 3, oy + 3, page_w, page_h);
      cairo_fill (cr);

      {
        /* The paper is white unless Format > Background says otherwise. */
        const W42PageSetup *setup = self->doc != NULL
                                      ? w42_document_page_setup (self->doc) : NULL;

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
  self->fit = FIT_NONE;
  self->magnified = FALSE;
  preview_resize (self);
  update_page_label (self);
}

/* The nearest step to the zoom in force, for stepping from a fit. */
static int
nearest_zoom_index (W42Preview *self)
{
  double zoom = preview_zoom (self);
  int best = 0;

  for (guint i = 0; i < G_N_ELEMENTS (ZOOMS); i++)
    if (fabs (ZOOMS[i] - zoom) < fabs (ZOOMS[best] - zoom))
      best = (int) i;
  return best;
}

static void
on_zoom_in (GtkButton *b, gpointer data)
{
  W42Preview *self = data;

  (void) b;
  set_zoom_index (self, self->fit == FIT_NONE ? (int) self->zoom_index + 1 : nearest_zoom_index (self) + 1);
}

static void
on_zoom_out (GtkButton *b, gpointer data)
{
  W42Preview *self = data;

  (void) b;
  set_zoom_index (self, self->fit == FIT_NONE ? (int) self->zoom_index - 1 : nearest_zoom_index (self) - 1);
}

/* The zoom box: the percentages, then Page Width, Whole Page and Two
 * Pages, as Word XP's preview offered. */
static const char *const ZOOM_CHOICES[] = {
  "25%", "35%", "50%", "65%", "80%", "100%", "125%", "150%", "200%",
  "Page Width", "Whole Page", "Two Pages", NULL
};

static void
on_zoom_chosen (GObject *drop, GParamSpec *pspec, gpointer data)
{
  W42Preview *self = data;
  guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (drop));

  (void) pspec;
  self->magnified = FALSE;
  if (i < G_N_ELEMENTS (ZOOMS))
    {
      self->columns = 1;
      set_zoom_index (self, (int) i);
      return;
    }
  self->fit = i == G_N_ELEMENTS (ZOOMS) ? FIT_PAGE_WIDTH : FIT_WHOLE_PAGE;
  self->columns = i == G_N_ELEMENTS (ZOOMS) + 2 ? 2 : 1;
  preview_resize (self);
  update_page_label (self);
}

static void
on_one_page (GtkButton *b, gpointer data)
{
  W42Preview *self = data;

  (void) b;
  self->columns = 1;
  self->fit = FIT_WHOLE_PAGE;
  self->magnified = FALSE;
  preview_resize (self);
  update_page_label (self);
}

static void
on_multiple_pages (GtkButton *b, gpointer data)
{
  W42Preview *self = data;

  (void) b;
  /* Two across, then three, then back to two. */
  self->columns = self->columns >= 3 ? 2 : self->columns + 1;
  if (self->columns < 2)
    self->columns = 2;
  self->fit = FIT_WHOLE_PAGE;
  self->magnified = FALSE;
  preview_resize (self);
  update_page_label (self);
}

/* Scrolls so that page `page` is at the top. */
static void
go_to_page (W42Preview *self, int page)
{
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled));
  double page_h = w42_layout_page_height (self->layout) * preview_zoom (self) + PAGE_GAP;
  int n = w42_layout_n_pages (self->layout);

  page = CLAMP (page, 0, n - 1);
  gtk_adjustment_set_value (vadj, (page / self->columns) * page_h);
  update_page_label (self);
}

static int
current_page (W42Preview *self)
{
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled));
  double page_h = w42_layout_page_height (self->layout) * preview_zoom (self) + PAGE_GAP;
  double middle = gtk_adjustment_get_value (vadj) + gtk_adjustment_get_page_size (vadj) / 2.0;

  return CLAMP ((int) floor ((middle - PAGE_GAP) / page_h), 0, MAX (preview_rows (self) - 1, 0)) * self->columns;
}

static void
on_prev_page (GtkButton *b, gpointer data)
{
  W42Preview *self = data;

  (void) b;
  go_to_page (self, current_page (self) - self->columns);
}

static void
on_next_page (GtkButton *b, gpointer data)
{
  W42Preview *self = data;

  (void) b;
  go_to_page (self, current_page (self) + self->columns);
}

/* The magnifier: a click on a page takes it to 100% around the point
 * clicked; another puts the view back as it was. */
static void
on_page_clicked (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  W42Preview *self = data;
  double px = 0, py = 0;
  int page;

  (void) gesture; (void) n_press;
  page = page_at (self, x, y, &px, &py);
  if (page < 0)
    return;
  if (self->magnified)
    {
      self->magnified = FALSE;
      self->zoom_index = self->zoom_before;
      self->fit = self->fit_before;
      self->columns = self->columns_before;
      preview_resize (self);
      go_to_page (self, page);
      return;
    }
  self->zoom_before = self->zoom_index;
  self->fit_before = self->fit;
  self->columns_before = self->columns;
  self->magnified = TRUE;
  self->fit = FIT_NONE;
  self->columns = 1;
  self->zoom_index = 5;                        /* 100% */
  preview_resize (self);
  {
    /* Centred on the point clicked. */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled));
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (self->scrolled));
    double page_h = w42_layout_page_height (self->layout) + PAGE_GAP;
    double top = PAGE_GAP + page * page_h + py - gtk_adjustment_get_page_size (vadj) / 2.0;
    double left = PAGE_GAP + px - gtk_adjustment_get_page_size (hadj) / 2.0;

    gtk_adjustment_set_value (vadj, MAX (top, 0.0));
    gtk_adjustment_set_value (hadj, MAX (left, 0.0));
  }
  update_page_label (self);
}

static void
on_viewport_resized (GObject *adj, GParamSpec *pspec, gpointer data)
{
  W42Preview *self = data;

  (void) adj; (void) pspec;
  if (self->fit != FIT_NONE)
    {
      preview_resize (self);
      update_page_label (self);
    }
}

static void
on_print (GtkButton *b, gpointer data)
{
  W42Preview *self = data;

  (void) b;
  w42_print_document (self->owner, self->doc, FALSE, NULL);
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
    case GDK_KEY_Page_Up:
      on_prev_page (NULL, self);
      return GDK_EVENT_STOP;
    case GDK_KEY_Page_Down:
      on_next_page (NULL, self);
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
  self->fit = FIT_WHOLE_PAGE;
  self->columns = 1;

  gtk_window_set_title (GTK_WINDOW (self), "Print Preview");
  gtk_window_set_default_size (GTK_WINDOW (self), 760, 820);
  gtk_widget_add_css_class (GTK_WIDGET (self), "w42");

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child (GTK_WINDOW (self), box);

  /* Word XP's preview bar: Print, the magnifier (a click on the page),
   * One Page, Multiple Pages, the zoom box, the page count, Close. */
  bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (bar, "w42-toolbar");
  gtk_box_append (GTK_BOX (bar), bar_button ("_Print...", G_CALLBACK (on_print), self));
  gtk_box_append (GTK_BOX (bar), bar_button ("_One Page", G_CALLBACK (on_one_page), self));
  gtk_box_append (GTK_BOX (bar), bar_button ("_Multiple Pages", G_CALLBACK (on_multiple_pages), self));
  gtk_box_append (GTK_BOX (bar), bar_button ("Zoom _Out", G_CALLBACK (on_zoom_out), self));
  self->zoom_label = gtk_label_new ("65%");
  gtk_widget_set_size_request (self->zoom_label, 44, -1);
  gtk_box_append (GTK_BOX (bar), self->zoom_label);
  gtk_box_append (GTK_BOX (bar), bar_button ("Zoom _In", G_CALLBACK (on_zoom_in), self));
  self->zoom_drop = gtk_drop_down_new_from_strings (ZOOM_CHOICES);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (self->zoom_drop), G_N_ELEMENTS (ZOOMS) + 1);
  gtk_widget_set_focusable (self->zoom_drop, FALSE);
  g_signal_connect (self->zoom_drop, "notify::selected", G_CALLBACK (on_zoom_chosen), self);
  gtk_box_append (GTK_BOX (bar), self->zoom_drop);
  gtk_box_append (GTK_BOX (bar), bar_button ("Pre_vious", G_CALLBACK (on_prev_page), self));
  gtk_box_append (GTK_BOX (bar), bar_button ("_Next", G_CALLBACK (on_next_page), self));
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
  /* The fitting zooms follow the window's size. */
  g_signal_connect (gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (self->scrolled)),
                    "notify::page-size", G_CALLBACK (on_viewport_resized), self);
  g_signal_connect (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scrolled)),
                    "notify::page-size", G_CALLBACK (on_viewport_resized), self);
  {
    GtkGesture *click = gtk_gesture_click_new ();

    g_signal_connect (click, "released", G_CALLBACK (on_page_clicked), self);
    gtk_widget_add_controller (self->area, GTK_EVENT_CONTROLLER (click));
    gtk_widget_set_cursor_from_name (self->area, "zoom-in");
  }

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
