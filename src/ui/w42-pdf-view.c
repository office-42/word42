/* w42-pdf-view.c - the pages of a PDF, one under another
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A scrollable widget of its own rather than a drawing area in a
 * viewport: a drawing area as tall as a thesis would be drawn whole
 * every time it changed.  This draws what is in sight.  Each page is
 * rendered by poppler once, at the size and the screen's scale it is
 * shown at, into a texture kept while the page is near the view; a page
 * scrolled far away gives its texture back.
 */

#include "w42-pdf-view.h"

#include <math.h>

#define GAP        12.0      /* round the pages and between them, in pixels */
#define KEEP_NEAR  8         /* pages either side of those in sight whose
                              * textures are kept */
#define ZOOM_MIN   0.1
#define ZOOM_MAX   8.0
/* A point is 1/72 inch; the screen is taken to be 96 pixels an inch, as
 * the page view takes it at 100%. */
#define PIXELS_PER_POINT (96.0 / 72.0)

typedef struct {
  double      width, height;    /* in points */
  GdkTexture *texture;          /* as last rendered, or NULL */
  double      rendered_at;      /* the pixels a point it was rendered at */
} Page;

struct _W42PdfView {
  GtkWidget            parent_instance;

  PopplerDocument     *doc;
  GArray              *pages;          /* Page */
  double              *above;          /* the pages' heights added up, in points:
                                        * above[i] is all of those before page i */
  double               widest;         /* in points */
  double               zoom;           /* 0: the width fitted */
  double               scale;          /* pixels a point, as laid out now */

  GtkAdjustment       *hadj, *vadj;
  GtkScrollablePolicy  hpolicy, vpolicy;

  int                  mark_page;      /* the band marked, from 1; 0 none */
  double               mark_top, mark_bottom;
  guint                mark_id;
};

enum {
  PROP_0,
  PROP_HADJUSTMENT,
  PROP_VADJUSTMENT,
  PROP_HSCROLL_POLICY,
  PROP_VSCROLL_POLICY,
  PROP_ZOOM,
};

enum {
  SIGNAL_POINT_ACTIVATED,
  SIGNAL_SCALE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE_WITH_CODE (W42PdfView, w42_pdf_view, GTK_TYPE_WIDGET,
                               G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, NULL))

/* ---- where things are ------------------------------------------------- */

static double
page_top (W42PdfView *self, guint i)
{
  return GAP * (i + 1) + self->scale * self->above[i];
}

static double
content_width (W42PdfView *self)
{
  return self->widest * self->scale + 2 * GAP;
}

static double
content_height (W42PdfView *self)
{
  guint n = self->pages != NULL ? self->pages->len : 0;

  return n == 0 ? 0 : GAP * (n + 1) + self->scale * self->above[n];
}

/* A page is centred in the width, or at the left margin when wider. */
static double
page_left (W42PdfView *self, guint i, double width)
{
  const Page *p = &g_array_index (self->pages, Page, i);

  return MAX (GAP, (MAX (width, content_width (self)) - p->width * self->scale) / 2);
}

static void
view_configure (W42PdfView *self)
{
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  int height = gtk_widget_get_height (GTK_WIDGET (self));
  double old = self->scale;
  double value;

  if (self->zoom > 0 || self->widest <= 0)
    self->scale = (self->zoom > 0 ? self->zoom : 1.0) * PIXELS_PER_POINT;
  else
    self->scale = MAX ((width - 2 * GAP) / self->widest, 0.05);

  if (fabs (old - self->scale) > 1e-9)
    g_signal_emit (self, signals[SIGNAL_SCALE_CHANGED], 0);
  if (self->vadj != NULL)
    {
      value = gtk_adjustment_get_value (self->vadj);
      /* The same place, at the new size. */
      if (old > 0 && fabs (old - self->scale) > 1e-9)
        value = value * self->scale / old;
      gtk_adjustment_configure (self->vadj, value, 0, MAX (content_height (self), height),
                                height * 0.1, height * 0.9, height);
    }
  if (self->hadj != NULL)
    {
      value = gtk_adjustment_get_value (self->hadj);
      if (old > 0 && fabs (old - self->scale) > 1e-9)
        value = value * self->scale / old;
      gtk_adjustment_configure (self->hadj, value, 0, MAX (content_width (self), width),
                                width * 0.1, width * 0.9, width);
    }
}

/* ---- drawing ---------------------------------------------------------- */

static GdkTexture *
render_page (PopplerDocument *doc, guint index, double scale)
{
  PopplerPage *page = poppler_document_get_page (doc, (int) index);
  double pw, ph;
  int w, h, stride;
  cairo_surface_t *surface;
  cairo_t *cr;
  GBytes *bytes;
  GdkTexture *texture;

  if (page == NULL)
    return NULL;
  poppler_page_get_size (page, &pw, &ph);
  w = MAX (1, (int) ceil (pw * scale));
  h = MAX (1, (int) ceil (ph * scale));
  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
  cr = cairo_create (surface);
  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_paint (cr);
  cairo_scale (cr, scale, scale);
  poppler_page_render (page, cr);
  cairo_destroy (cr);
  cairo_surface_flush (surface);
  g_object_unref (page);

  /* The texture reads the surface's own pixels, and lets it go when it
   * is done with them.  GDK's default format is cairo's. */
  stride = cairo_image_surface_get_stride (surface);
  bytes = g_bytes_new_with_free_func (cairo_image_surface_get_data (surface), (gsize) stride * h,
                                      (GDestroyNotify) cairo_surface_destroy, surface);
  texture = gdk_memory_texture_new (w, h, GDK_MEMORY_DEFAULT, bytes, (gsize) stride);
  g_bytes_unref (bytes);
  return texture;
}

static void
w42_pdf_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  W42PdfView *self = W42_PDF_VIEW (widget);
  double width = gtk_widget_get_width (widget), height = gtk_widget_get_height (widget);
  double x0 = self->hadj != NULL ? gtk_adjustment_get_value (self->hadj) : 0;
  double y0 = self->vadj != NULL ? gtk_adjustment_get_value (self->vadj) : 0;
  double sharp = self->scale * gtk_widget_get_scale_factor (widget);
  const GdkRGBA ground = { 0.50f, 0.50f, 0.50f, 1.0f };
  const GdkRGBA paper = { 1.0f, 1.0f, 1.0f, 1.0f };
  const GdkRGBA shade = { 0.0f, 0.0f, 0.0f, 0.35f };
  const GdkRGBA mark = { 1.0f, 0.80f, 0.0f, 0.35f };
  int first = -1, last = -1;

  gtk_snapshot_append_color (snapshot, &ground, &GRAPHENE_RECT_INIT (0, 0, width, height));
  if (self->doc == NULL || self->pages == NULL)
    return;

  for (guint i = 0; i < self->pages->len; i++)
    {
      Page *p = &g_array_index (self->pages, Page, i);
      double top = page_top (self, i) - y0;
      double left = page_left (self, i, width) - x0;
      double pw = p->width * self->scale, ph = p->height * self->scale;

      if (top > height || top + ph < 0)
        continue;
      if (first < 0)
        first = (int) i;
      last = (int) i;

      gtk_snapshot_append_color (snapshot, &shade, &GRAPHENE_RECT_INIT (left + 3, top + 3, pw, ph));
      gtk_snapshot_append_color (snapshot, &paper, &GRAPHENE_RECT_INIT (left, top, pw, ph));
      if (p->texture == NULL || fabs (p->rendered_at - sharp) > 1e-6)
        {
          g_clear_object (&p->texture);
          p->texture = render_page (self->doc, i, sharp);
          p->rendered_at = sharp;
        }
      if (p->texture != NULL)
        gtk_snapshot_append_texture (snapshot, p->texture, &GRAPHENE_RECT_INIT (left, top, pw, ph));
      if (self->mark_page == (int) i + 1)
        gtk_snapshot_append_color (snapshot, &mark,
                                   &GRAPHENE_RECT_INIT (left, top + self->mark_top * self->scale, pw,
                                                        MAX (2.0, (self->mark_bottom - self->mark_top) * self->scale)));
    }

  /* The pages far out of sight give their textures back. */
  for (guint i = 0; i < self->pages->len; i++)
    if (first < 0 || (int) i < first - KEEP_NEAR || (int) i > last + KEEP_NEAR)
      g_clear_object (&g_array_index (self->pages, Page, i).texture);
}

static void
w42_pdf_view_measure (GtkWidget *widget, GtkOrientation orientation, int for_size,
                      int *minimum, int *natural, int *minimum_baseline, int *natural_baseline)
{
  W42PdfView *self = W42_PDF_VIEW (widget);

  *minimum = 40;
  *natural = (int) (orientation == GTK_ORIENTATION_HORIZONTAL ? content_width (self)
                                                              : content_height (self));
  *natural = MAX (*natural, *minimum);
}

static void
w42_pdf_view_size_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  view_configure (W42_PDF_VIEW (widget));
}

/* ---- scrolling -------------------------------------------------------- */

static void
on_adjustment_value_changed (GtkAdjustment *adj, gpointer data)
{
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
view_set_adjustment (W42PdfView *self, GtkAdjustment **slot, GtkAdjustment *adj)
{
  if (adj != NULL && *slot == adj)
    return;
  if (*slot != NULL)
    {
      g_signal_handlers_disconnect_by_func (*slot, on_adjustment_value_changed, self);
      g_object_unref (*slot);
    }
  if (adj == NULL)
    adj = gtk_adjustment_new (0, 0, 0, 0, 0, 0);
  *slot = g_object_ref_sink (adj);
  g_signal_connect (adj, "value-changed", G_CALLBACK (on_adjustment_value_changed), self);
  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

/* Ctrl and the wheel zoom, as they do in the page view. */
static gboolean
on_scroll (GtkEventControllerScroll *controller, double dx, double dy, gpointer data)
{
  W42PdfView *self = data;
  GdkModifierType state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (controller));
  double zoom;

  if ((state & GDK_CONTROL_MASK) == 0 || dy == 0)
    return FALSE;
  zoom = self->scale / PIXELS_PER_POINT;
  w42_pdf_view_set_zoom (self, dy < 0 ? zoom * 1.1 : zoom / 1.1);
  return TRUE;
}

/* The page and the place on it under (wx, wy), in points. */
static gboolean
view_point (W42PdfView *self, double wx, double wy, int *page, double *px, double *py)
{
  double width = gtk_widget_get_width (GTK_WIDGET (self));
  double x = wx + (self->hadj != NULL ? gtk_adjustment_get_value (self->hadj) : 0);
  double y = wy + (self->vadj != NULL ? gtk_adjustment_get_value (self->vadj) : 0);

  if (self->pages == NULL || self->scale <= 0)
    return FALSE;
  for (guint i = 0; i < self->pages->len; i++)
    {
      const Page *p = &g_array_index (self->pages, Page, i);
      double top = page_top (self, i), left = page_left (self, i, width);

      if (y >= top && y <= top + p->height * self->scale &&
          x >= left && x <= left + p->width * self->scale)
        {
          *page = (int) i + 1;
          *px = (x - left) / self->scale;
          *py = (y - top) / self->scale;
          return TRUE;
        }
    }
  return FALSE;
}

static void
on_pressed (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  W42PdfView *self = data;
  int page;
  double px, py;

  if (n_press == 2 && view_point (self, x, y, &page, &px, &py))
    g_signal_emit (self, signals[SIGNAL_POINT_ACTIVATED], 0, page, px, py);
}

/* ---- the object ------------------------------------------------------- */

static void
view_clear_pages (W42PdfView *self)
{
  if (self->pages != NULL)
    {
      for (guint i = 0; i < self->pages->len; i++)
        g_clear_object (&g_array_index (self->pages, Page, i).texture);
      g_array_free (self->pages, TRUE);
      self->pages = NULL;
    }
  g_clear_pointer (&self->above, g_free);
  self->widest = 0;
}

static void
w42_pdf_view_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  W42PdfView *self = W42_PDF_VIEW (object);

  switch (prop_id)
    {
    case PROP_HADJUSTMENT:
      view_set_adjustment (self, &self->hadj, g_value_get_object (value));
      break;
    case PROP_VADJUSTMENT:
      view_set_adjustment (self, &self->vadj, g_value_get_object (value));
      break;
    case PROP_HSCROLL_POLICY:
      self->hpolicy = g_value_get_enum (value);
      gtk_widget_queue_resize (GTK_WIDGET (self));
      break;
    case PROP_VSCROLL_POLICY:
      self->vpolicy = g_value_get_enum (value);
      gtk_widget_queue_resize (GTK_WIDGET (self));
      break;
    case PROP_ZOOM:
      w42_pdf_view_set_zoom (self, g_value_get_double (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
w42_pdf_view_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  W42PdfView *self = W42_PDF_VIEW (object);

  switch (prop_id)
    {
    case PROP_HADJUSTMENT:
      g_value_set_object (value, self->hadj);
      break;
    case PROP_VADJUSTMENT:
      g_value_set_object (value, self->vadj);
      break;
    case PROP_HSCROLL_POLICY:
      g_value_set_enum (value, self->hpolicy);
      break;
    case PROP_VSCROLL_POLICY:
      g_value_set_enum (value, self->vpolicy);
      break;
    case PROP_ZOOM:
      g_value_set_double (value, self->zoom);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
w42_pdf_view_dispose (GObject *object)
{
  W42PdfView *self = W42_PDF_VIEW (object);

  if (self->mark_id != 0)
    {
      g_source_remove (self->mark_id);
      self->mark_id = 0;
    }
  view_clear_pages (self);
  g_clear_object (&self->doc);
  if (self->hadj != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->hadj, on_adjustment_value_changed, self);
      g_clear_object (&self->hadj);
    }
  if (self->vadj != NULL)
    {
      g_signal_handlers_disconnect_by_func (self->vadj, on_adjustment_value_changed, self);
      g_clear_object (&self->vadj);
    }
  G_OBJECT_CLASS (w42_pdf_view_parent_class)->dispose (object);
}

static void
w42_pdf_view_class_init (W42PdfViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->set_property = w42_pdf_view_set_property;
  object_class->get_property = w42_pdf_view_get_property;
  object_class->dispose = w42_pdf_view_dispose;
  widget_class->snapshot = w42_pdf_view_snapshot;
  widget_class->measure = w42_pdf_view_measure;
  widget_class->size_allocate = w42_pdf_view_size_allocate;

  g_object_class_override_property (object_class, PROP_HADJUSTMENT, "hadjustment");
  g_object_class_override_property (object_class, PROP_VADJUSTMENT, "vadjustment");
  g_object_class_override_property (object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
  g_object_class_override_property (object_class, PROP_VSCROLL_POLICY, "vscroll-policy");
  g_object_class_install_property (object_class, PROP_ZOOM,
    g_param_spec_double ("zoom", NULL, NULL, 0.0, ZOOM_MAX, 0.0,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS));

  signals[SIGNAL_POINT_ACTIVATED] =
    g_signal_new ("point-activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 3, G_TYPE_INT, G_TYPE_DOUBLE, G_TYPE_DOUBLE);
  signals[SIGNAL_SCALE_CHANGED] =
    g_signal_new ("scale-changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                  NULL, NULL, NULL, G_TYPE_NONE, 0);

  gtk_widget_class_set_css_name (widget_class, "w42pdfview");
}

static void
w42_pdf_view_init (W42PdfView *self)
{
  GtkEventController *scroll;
  GtkGesture *click;

  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
  scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), self);
  gtk_widget_add_controller (GTK_WIDGET (self), scroll);
  click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
}

GtkWidget *
w42_pdf_view_new (void)
{
  return g_object_new (W42_TYPE_PDF_VIEW, NULL);
}

void
w42_pdf_view_set_document (W42PdfView *self, PopplerDocument *doc)
{
  int n;

  g_return_if_fail (W42_IS_PDF_VIEW (self));

  view_clear_pages (self);
  g_set_object (&self->doc, doc);
  n = doc != NULL ? poppler_document_get_n_pages (doc) : 0;
  self->pages = g_array_sized_new (FALSE, TRUE, sizeof (Page), (guint) MAX (n, 0));
  self->above = g_new0 (double, n + 1);
  for (int i = 0; i < n; i++)
    {
      PopplerPage *page = poppler_document_get_page (doc, i);
      Page p = { 0 };

      if (page != NULL)
        {
          poppler_page_get_size (page, &p.width, &p.height);
          g_object_unref (page);
        }
      g_array_append_val (self->pages, p);
      self->above[i + 1] = self->above[i] + p.height;
      self->widest = MAX (self->widest, p.width);
    }
  /* The place scrolled to stays put: a new version of the document is
   * read where the old one was being read. */
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

int
w42_pdf_view_get_n_pages (W42PdfView *self)
{
  g_return_val_if_fail (W42_IS_PDF_VIEW (self), 0);
  return self->pages != NULL ? (int) self->pages->len : 0;
}

void
w42_pdf_view_set_zoom (W42PdfView *self, double zoom)
{
  g_return_if_fail (W42_IS_PDF_VIEW (self));

  zoom = zoom <= 0 ? 0 : CLAMP (zoom, ZOOM_MIN, ZOOM_MAX);
  if (fabs (zoom - self->zoom) < 1e-9)
    return;
  self->zoom = zoom;
  gtk_widget_queue_allocate (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
  g_object_notify (G_OBJECT (self), "zoom");
}

double
w42_pdf_view_get_zoom (W42PdfView *self)
{
  g_return_val_if_fail (W42_IS_PDF_VIEW (self), 0);
  return self->zoom;
}

double
w42_pdf_view_get_scale (W42PdfView *self)
{
  g_return_val_if_fail (W42_IS_PDF_VIEW (self), 1.0);
  return self->scale / PIXELS_PER_POINT;
}

static gboolean
on_mark_done (gpointer data)
{
  W42PdfView *self = data;

  self->mark_id = 0;
  self->mark_page = 0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
  return G_SOURCE_REMOVE;
}

void
w42_pdf_view_show (W42PdfView *self, int page, double top, double bottom, gboolean mark)
{
  double y_top, y_bottom, value, size;

  g_return_if_fail (W42_IS_PDF_VIEW (self));

  if (self->pages == NULL || page < 1 || (guint) page > self->pages->len || self->vadj == NULL)
    return;
  y_top = page_top (self, (guint) page - 1) + top * self->scale;
  y_bottom = page_top (self, (guint) page - 1) + bottom * self->scale;
  value = gtk_adjustment_get_value (self->vadj);
  size = gtk_adjustment_get_page_size (self->vadj);
  /* Only when out of sight, and then with a third of the view above it,
   * so that what led up to it can be read. */
  if (y_top < value || y_bottom > value + size)
    gtk_adjustment_set_value (self->vadj, y_top - size / 3);

  if (mark)
    {
      self->mark_page = page;
      self->mark_top = top;
      self->mark_bottom = bottom;
      if (self->mark_id != 0)
        g_source_remove (self->mark_id);
      self->mark_id = g_timeout_add (1500, on_mark_done, self);
    }
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
