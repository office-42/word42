/* w42-slideshow.c - see w42-slideshow.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-slideshow.h"

#include "w42-pptx.h"

#include <pango/pangocairo.h>

typedef struct {
  GtkWidget *window;
  GtkWidget *area;
  GPtrArray *slides;      /* W42Slide * */
  guint      at;          /* which slide is up */
  gboolean   ended;       /* the black screen after the last one */
} Show;

static void
show_free (gpointer data, GObject *gone)
{
  Show *show = data;

  (void) gone;
  w42_slides_free (show->slides);
  g_free (show);
}

/* The stage: 4:3, centred, as much of the window as fits, so that what is
 * shown is what a projector would show. */
static void
stage (int width, int height, double *x, double *y, double *w, double *h)
{
  double want = 4.0 / 3.0;
  double have = height > 0 ? (double) width / height : want;

  if (have > want)
    {
      *h = height;
      *w = *h * want;
    }
  else
    {
      *w = width;
      *h = *w / want;
    }
  *x = (width - *w) / 2.0;
  *y = (height - *h) / 2.0;
}

static void
draw_text (cairo_t *cr, const char *text, const char *family, double size_px,
           gboolean bold, double x, double y, double width, double *height_out)
{
  PangoLayout *layout = pango_cairo_create_layout (cr);
  PangoFontDescription *desc = pango_font_description_new ();
  int w = 0, h = 0;

  pango_font_description_set_family (desc, family);
  pango_font_description_set_absolute_size (desc, size_px * PANGO_SCALE);
  pango_font_description_set_weight (desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description (layout, desc);
  pango_font_description_free (desc);

  pango_layout_set_width (layout, (int) (width * PANGO_SCALE));
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_text (layout, text, -1);

  cairo_move_to (cr, x, y);
  pango_cairo_show_layout (cr, layout);
  pango_layout_get_pixel_size (layout, &w, &h);
  if (height_out != NULL)
    *height_out = h;
  g_object_unref (layout);
}

static void
draw_slide (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  Show *show = data;
  double sx, sy, sw, sh;
  const W42Slide *slide;
  double margin, y, title_h = 0;

  (void) area;

  /* Behind the stage, black: a room is dark. */
  cairo_set_source_rgb (cr, 0.05, 0.05, 0.05);
  cairo_paint (cr);

  if (show->ended || show->slides->len == 0)
    {
      const char *done = "End of the show.  Escape closes it.";

      cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
      draw_text (cr, done, "Arial", MAX (height / 40.0, 12.0), FALSE,
                 width / 8.0, height / 2.0, width * 0.75, NULL);
      return;
    }

  stage (width, height, &sx, &sy, &sw, &sh);
  slide = g_ptr_array_index (show->slides, MIN (show->at, show->slides->len - 1));

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_rectangle (cr, sx, sy, sw, sh);
  cairo_fill (cr);

  margin = sw * 0.08;
  y = sy + sh * 0.10;

  cairo_set_source_rgb (cr, 0.0, 0.0, 0.35);
  draw_text (cr, slide->title, "Arial", sh * 0.10, TRUE,
             sx + margin, y, sw - 2 * margin, &title_h);
  y += title_h + sh * 0.06;

  /* A rule under the title, as the plainest of slides has always had. */
  cairo_set_source_rgb (cr, 0.75, 0.75, 0.75);
  cairo_rectangle (cr, sx + margin, y - sh * 0.03, sw - 2 * margin, MAX (sh * 0.004, 1.0));
  cairo_fill (cr);

  cairo_set_source_rgb (cr, 0.1, 0.1, 0.1);
  for (guint i = 0; i < slide->lines->len && y < sy + sh - sh * 0.08; i++)
    {
      const char *line = g_ptr_array_index (slide->lines, i);
      char *bulleted = g_strconcat ("\342\200\242  ", line, NULL);
      double h = 0;

      draw_text (cr, bulleted, "Arial", sh * 0.055, FALSE,
                 sx + margin * 1.2, y, sw - 2.4 * margin, &h);
      y += h + sh * 0.02;
      g_free (bulleted);
    }

  /* Which slide this is, faintly, in the corner. */
  {
    char *count = g_strdup_printf ("%u / %u", show->at + 1, show->slides->len);

    cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
    draw_text (cr, count, "Arial", sh * 0.03, FALSE,
               sx + sw - margin - sw * 0.10, sy + sh - margin * 0.7, sw * 0.10, NULL);
    g_free (count);
  }
}

static void
go (Show *show, int by)
{
  if (show->ended && by < 0)
    {
      show->ended = FALSE;
    }
  else if (by > 0 && show->at + 1 >= show->slides->len)
    {
      if (show->ended)
        {
          gtk_window_destroy (GTK_WINDOW (show->window));
          return;
        }
      show->ended = TRUE;
    }
  else if (by > 0)
    show->at++;
  else if (by < 0 && show->at > 0)
    show->at--;

  gtk_widget_queue_draw (show->area);
}

static gboolean
on_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
        GdkModifierType state, gpointer data)
{
  Show *show = data;

  (void) controller; (void) keycode; (void) state;

  switch (keyval)
    {
    case GDK_KEY_Escape:
    case GDK_KEY_q:
    case GDK_KEY_Q:
      gtk_window_destroy (GTK_WINDOW (show->window));
      return GDK_EVENT_STOP;

    case GDK_KEY_space:
    case GDK_KEY_Right:
    case GDK_KEY_Down:
    case GDK_KEY_Page_Down:
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      go (show, +1);
      return GDK_EVENT_STOP;

    case GDK_KEY_BackSpace:
    case GDK_KEY_Left:
    case GDK_KEY_Up:
    case GDK_KEY_Page_Up:
      go (show, -1);
      return GDK_EVENT_STOP;

    case GDK_KEY_Home:
      show->at = 0;
      show->ended = FALSE;
      gtk_widget_queue_draw (show->area);
      return GDK_EVENT_STOP;

    case GDK_KEY_End:
      show->at = show->slides->len > 0 ? show->slides->len - 1 : 0;
      show->ended = FALSE;
      gtk_widget_queue_draw (show->area);
      return GDK_EVENT_STOP;

    default:
      break;
    }
  return GDK_EVENT_PROPAGATE;
}

static void
on_click (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  Show *show = data;

  (void) n_press; (void) x; (void) y;
  go (show, gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture)) == GDK_BUTTON_SECONDARY
      ? -1 : +1);
}

void
w42_slideshow_show (GtkWindow *parent, W42View *view)
{
  Show *show;
  GtkEventController *keys;
  GtkGesture *click;
  W42PieceTable *pt;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;
  pt = w42_document_pt (w42_view_get_document (view));

  show = g_new0 (Show, 1);
  show->slides = w42_slides_from_document (pt);

  /* Start on the slide the caret is in, so that a talk can be picked up
   * where it was left. */
  {
    GPtrArray *blocks = w42_pt_snapshot_blocks (pt);
    gsize caret = w42_view_get_caret (view);
    guint seen = 0;
    W42StyleSheet *styles = w42_pt_stylesheet (pt);

    for (guint b = 0; b < blocks->len; b++)
      {
        const W42Block *block = g_ptr_array_index (blocks, b);
        const W42ParaFmt *pa = &w42_ap_table_get (w42_pt_ap_table (pt), block->ap)->pa;
        int outline = pa->style != NULL ? w42_stylesheet_outline (styles, pa->style) : 0;

        if (outline == 0 && pa->style != NULL && g_ascii_strcasecmp (pa->style, "Title") == 0)
          outline = 1;
        if (block->note >= 0 || block->table >= 0)
          continue;
        if (outline > 0 && block->text->len > 0)
          {
            if (block->start_pos > caret)
              break;
            if (seen + 1 < show->slides->len + 1)
              seen++;
          }
      }
    show->at = seen > 0 ? seen - 1 : 0;
    if (show->at >= show->slides->len)
      show->at = show->slides->len > 0 ? show->slides->len - 1 : 0;
    g_ptr_array_free (blocks, TRUE);
  }

  show->window = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (show->window), "Slide Show");
  if (parent != NULL)
    {
      gtk_window_set_transient_for (GTK_WINDOW (show->window), parent);
      gtk_window_set_destroy_with_parent (GTK_WINDOW (show->window), TRUE);
    }
  gtk_window_set_default_size (GTK_WINDOW (show->window), 1024, 768);
  g_object_weak_ref (G_OBJECT (show->window), show_free, show);

  show->area = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (show->area), draw_slide, show, NULL);
  gtk_window_set_child (GTK_WINDOW (show->window), show->area);

  keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), show);
  gtk_widget_add_controller (show->window, keys);

  click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);   /* any button */
  g_signal_connect (click, "pressed", G_CALLBACK (on_click), show);
  gtk_widget_add_controller (show->area, GTK_EVENT_CONTROLLER (click));

  gtk_window_fullscreen (GTK_WINDOW (show->window));
  gtk_window_present (GTK_WINDOW (show->window));
  gtk_widget_grab_focus (show->area);
}
