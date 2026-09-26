/* w42-slideshow.c - see w42-slideshow.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-slideshow.h"

#include "w42-image.h"
#include "w42-pptx.h"

#include <glib/gi18n.h>
#include <pango/pangocairo.h>

typedef enum {
  BLANK_NONE,
  BLANK_BLACK,            /* B or . : the screen goes black, as PowerPoint's */
  BLANK_WHITE             /* W or , : or white */
} Blank;

typedef struct {
  GtkWidget *window;
  GtkWidget *area;
  GPtrArray *slides;      /* W42Slide * */
  GHashTable *surfaces;   /* W42SlidePicture * -> cairo_surface_t *, decoded once */
  guint      at;          /* which slide is up */
  gboolean   ended;       /* the black screen after the last one */
  Blank      blank;
  int        typed;       /* a slide number being typed, to go to with Enter */
} Show;

static void
show_free (gpointer data, GObject *gone)
{
  Show *show = data;

  (void) gone;
  g_hash_table_destroy (show->surfaces);
  w42_slides_free (show->slides);
  g_free (show);
}

/* The stage: 16:9, as the slides are written, centred, as much of the
 * window as fits, so that what is shown is what a projector would show. */
static void
stage (int width, int height, double *x, double *y, double *w, double *h)
{
  double want = 16.0 / 9.0;
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

/* Sets a paragraph of text, wrapped to `width`, and says how tall it came
 * out.  With `cr` NULL it only measures. */
static double
set_text (cairo_t *cr, PangoContext *pango, const char *text, double size_px,
          gboolean bold, PangoAlignment align, double x, double y, double width)
{
  PangoLayout *layout = pango_layout_new (pango);
  PangoFontDescription *desc = pango_font_description_new ();
  int w = 0, h = 0;

  pango_font_description_set_family (desc, "Arial");
  pango_font_description_set_absolute_size (desc, size_px * PANGO_SCALE);
  pango_font_description_set_weight (desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description (layout, desc);
  pango_font_description_free (desc);

  pango_layout_set_width (layout, (int) (MAX (width, 1.0) * PANGO_SCALE));
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_alignment (layout, align);
  pango_layout_set_text (layout, text, -1);
  pango_layout_get_pixel_size (layout, &w, &h);

  if (cr != NULL)
    {
      cairo_move_to (cr, x, y);
      pango_cairo_show_layout (cr, layout);
    }
  g_object_unref (layout);
  return h;
}

/* A level's bullet and its type, a step smaller each level down, as the
 * written slides have them. */
static const char *
level_bullet (int level)
{
  static const char *const bullets[] = { "\342\200\242", "\342\200\223", "\342\200\242",
                                         "\342\200\223", "\302\273" };

  return bullets[CLAMP (level, 0, 4)];
}

static double
level_size (int level)
{
  static const double sizes[] = { 1.0, 0.86, 0.72, 0.64, 0.64 };

  return sizes[CLAMP (level, 0, 4)];
}

/* The bullets, from `y` down in the box: how tall they come out at this
 * size.  With `cr` NULL it only measures, which is how the size that
 * fits is found. */
static double
set_lines (cairo_t *cr, PangoContext *pango, const W42Slide *slide, double size,
           double x, double y, double width)
{
  double top = y;

  for (guint i = 0; i < slide->lines->len; i++)
    {
      int level = g_array_index (slide->levels, int, i);
      double px = size * level_size (level);
      double indent = level * size * 1.4;
      double bullet_w = px * 1.1;
      double h;

      if (cr != NULL)
        set_text (cr, pango, level_bullet (level), px, FALSE, PANGO_ALIGN_LEFT,
                  x + indent, y, bullet_w);
      h = set_text (cr, pango, g_ptr_array_index (slide->lines, i), px, FALSE, PANGO_ALIGN_LEFT,
                    x + indent + bullet_w, y, width - indent - bullet_w);
      y += h + px * 0.35;
    }
  return y - top;
}

static cairo_surface_t *
picture_surface (Show *show, const W42SlidePicture *pic)
{
  cairo_surface_t *surface = g_hash_table_lookup (show->surfaces, pic);

  if (surface == NULL && !g_hash_table_contains (show->surfaces, pic))
    {
      surface = w42_image_surface (pic->data);
      g_hash_table_insert (show->surfaces, (gpointer) pic, surface);
    }
  return surface;
}

/* The pictures side by side in the box, each as large as its share lets
 * it be without changing shape. */
static void
set_pictures (cairo_t *cr, Show *show, const W42Slide *slide,
              double bx, double by, double bw, double bh)
{
  guint n = slide->pictures->len;
  double gap = n > 1 ? bw * 0.03 : 0;
  double cell = (bw - gap * (n - 1)) / MAX (n, 1);

  for (guint i = 0; i < n; i++)
    {
      const W42SlidePicture *pic = g_ptr_array_index (slide->pictures, i);
      cairo_surface_t *surface = picture_surface (show, pic);
      double iw, ih, scale, w, h, x, y;

      if (surface == NULL)
        continue;
      iw = cairo_image_surface_get_width (surface);
      ih = cairo_image_surface_get_height (surface);
      if (iw <= 0 || ih <= 0)
        continue;
      scale = MIN (cell / iw, bh / ih);
      w = iw * scale;
      h = ih * scale;
      x = bx + i * (cell + gap) + (cell - w) / 2;
      y = by + (bh - h) / 2;

      cairo_save (cr);
      cairo_translate (cr, x, y);
      cairo_scale (cr, scale, scale);
      cairo_set_source_surface (cr, surface, 0, 0);
      cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
      cairo_paint (cr);
      cairo_restore (cr);
    }
}

/* The title slide: the title large, a little above the middle, and the
 * lines under it as the subtitle, all centred. */
static void
draw_title_slide (cairo_t *cr, PangoContext *pango, const W42Slide *slide,
                  double sx, double sy, double sw, double sh)
{
  double margin = sw * 0.10;
  double title_size = sh * 0.11;
  double title_h = set_text (NULL, pango, slide->title, title_size, TRUE, PANGO_ALIGN_CENTER,
                             0, 0, sw - 2 * margin);
  double y;

  while (title_h > sh * 0.45 && title_size > sh * 0.04)
    {
      title_size *= 0.9;
      title_h = set_text (NULL, pango, slide->title, title_size, TRUE, PANGO_ALIGN_CENTER,
                          0, 0, sw - 2 * margin);
    }
  y = sy + sh * 0.52 - title_h;

  cairo_set_source_rgb (cr, 0.0, 0.0, 0.35);
  set_text (cr, pango, slide->title, title_size, TRUE, PANGO_ALIGN_CENTER,
            sx + margin, y, sw - 2 * margin);
  y = sy + sh * 0.58;

  cairo_set_source_rgb (cr, 0.3, 0.3, 0.3);
  for (guint i = 0; i < slide->lines->len && y < sy + sh * 0.92; i++)
    y += set_text (cr, pango, g_ptr_array_index (slide->lines, i), sh * 0.05, FALSE,
                   PANGO_ALIGN_CENTER, sx + margin, y, sw - 2 * margin) + sh * 0.015;
}

static void
draw_content_slide (cairo_t *cr, PangoContext *pango, Show *show, const W42Slide *slide,
                    double sx, double sy, double sw, double sh)
{
  double margin = sw * 0.07;
  double y = sy + sh * 0.07, title_h = 0;
  double box_x = sx + margin, box_w = sw - 2 * margin, box_h;
  gboolean pictures = slide->pictures->len > 0, lines = slide->lines->len > 0;

  if (*slide->title != '\0')
    {
      cairo_set_source_rgb (cr, 0.0, 0.0, 0.35);
      title_h = set_text (cr, pango, slide->title, sh * 0.085, TRUE, PANGO_ALIGN_LEFT,
                          box_x, y, box_w);
      y += title_h + sh * 0.035;

      /* A rule under the title, as the plainest of slides has always had. */
      cairo_set_source_rgb (cr, 0.75, 0.75, 0.75);
      cairo_rectangle (cr, box_x, y - sh * 0.018, box_w, MAX (sh * 0.004, 1.0));
      cairo_fill (cr);
    }
  box_h = sy + sh - sh * 0.08 - y;

  /* With pictures, the text takes the left half and they the right. */
  if (pictures)
    set_pictures (cr, show, slide, lines ? box_x + box_w * 0.52 : box_x, y,
                  lines ? box_w * 0.48 : box_w, box_h);

  if (lines)
    {
      double width = pictures ? box_w * 0.48 : box_w;
      double size = sh * 0.052;

      /* Too much to fit: smaller type, as PowerPoint shrinks text on
       * overflow, down to a size that can still be read at the back. */
      for (int i = 0; i < 8 && set_lines (NULL, pango, slide, size, box_x, y, width) > box_h; i++)
        size *= 0.9;

      cairo_set_source_rgb (cr, 0.1, 0.1, 0.1);
      set_lines (cr, pango, slide, size, box_x, y, width);
    }
}

static void
draw_slide (GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
{
  Show *show = data;
  PangoContext *pango = pango_cairo_create_context (cr);
  double sx, sy, sw, sh;
  const W42Slide *slide;

  (void) area;

  /* Behind the stage, black: a room is dark. */
  cairo_set_source_rgb (cr, 0.05, 0.05, 0.05);
  cairo_paint (cr);

  if (show->blank != BLANK_NONE)
    {
      double c = show->blank == BLANK_WHITE ? 1.0 : 0.0;

      cairo_set_source_rgb (cr, c, c, c);
      cairo_paint (cr);
      g_object_unref (pango);
      return;
    }

  if (show->ended || show->slides->len == 0)
    {
      /* Translators: Escape is the key on the keyboard. */
      const char *done = _("End of the show.  Escape closes it.");

      cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
      set_text (cr, pango, done, MAX (height / 40.0, 12.0), FALSE, PANGO_ALIGN_LEFT,
                width / 8.0, height / 2.0, width * 0.75);
      g_object_unref (pango);
      return;
    }

  stage (width, height, &sx, &sy, &sw, &sh);
  slide = g_ptr_array_index (show->slides, MIN (show->at, show->slides->len - 1));

  cairo_set_source_rgb (cr, 1, 1, 1);
  cairo_rectangle (cr, sx, sy, sw, sh);
  cairo_fill (cr);

  if (slide->title_slide && slide->pictures->len == 0)
    draw_title_slide (cr, pango, slide, sx, sy, sw, sh);
  else
    draw_content_slide (cr, pango, show, slide, sx, sy, sw, sh);

  /* Which slide this is, faintly, in the corner. */
  {
    char *count = g_strdup_printf ("%u / %u", show->at + 1, show->slides->len);

    cairo_set_source_rgb (cr, 0.6, 0.6, 0.6);
    set_text (cr, pango, count, sh * 0.025, FALSE, PANGO_ALIGN_RIGHT,
              sx + sw * 0.80, sy + sh * 0.95, sw * 0.17);
    g_free (count);
  }
  g_object_unref (pango);
}

static void
go (Show *show, int by)
{
  show->blank = BLANK_NONE;
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

static void
go_to (Show *show, guint index)
{
  show->at = show->slides->len > 0 ? MIN (index, show->slides->len - 1) : 0;
  show->ended = FALSE;
  show->blank = BLANK_NONE;
  gtk_widget_queue_draw (show->area);
}

static gboolean
on_key (GtkEventControllerKey *controller, guint keyval, guint keycode,
        GdkModifierType state, gpointer data)
{
  Show *show = data;
  gunichar c = gdk_keyval_to_unicode (keyval);
  int typed = show->typed;

  (void) controller; (void) keycode; (void) state;

  /* A slide number, typed and then Enter, goes to that slide, as in
   * PowerPoint.  Any other key forgets it. */
  show->typed = 0;
  if (c >= '0' && c <= '9')
    {
      show->typed = MIN (typed * 10 + (int) (c - '0'), 99999);
      return GDK_EVENT_STOP;
    }

  switch (keyval)
    {
    case GDK_KEY_Escape:
      if (show->blank != BLANK_NONE)
        {
          show->blank = BLANK_NONE;
          gtk_widget_queue_draw (show->area);
          return GDK_EVENT_STOP;
        }
      gtk_window_destroy (GTK_WINDOW (show->window));
      return GDK_EVENT_STOP;

    case GDK_KEY_q:
    case GDK_KEY_Q:
      gtk_window_destroy (GTK_WINDOW (show->window));
      return GDK_EVENT_STOP;

    case GDK_KEY_b:
    case GDK_KEY_B:
    case GDK_KEY_period:
      show->blank = show->blank == BLANK_BLACK ? BLANK_NONE : BLANK_BLACK;
      gtk_widget_queue_draw (show->area);
      return GDK_EVENT_STOP;

    case GDK_KEY_w:
    case GDK_KEY_W:
    case GDK_KEY_comma:
      show->blank = show->blank == BLANK_WHITE ? BLANK_NONE : BLANK_WHITE;
      gtk_widget_queue_draw (show->area);
      return GDK_EVENT_STOP;

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      if (typed > 0)
        {
          go_to (show, (guint) typed - 1);
          return GDK_EVENT_STOP;
        }
      go (show, +1);
      return GDK_EVENT_STOP;

    case GDK_KEY_space:
    case GDK_KEY_Right:
    case GDK_KEY_Down:
    case GDK_KEY_Page_Down:
    case GDK_KEY_n:
    case GDK_KEY_N:
      go (show, +1);
      return GDK_EVENT_STOP;

    case GDK_KEY_BackSpace:
    case GDK_KEY_Left:
    case GDK_KEY_Up:
    case GDK_KEY_Page_Up:
    case GDK_KEY_p:
    case GDK_KEY_P:
      go (show, -1);
      return GDK_EVENT_STOP;

    case GDK_KEY_Home:
      go_to (show, 0);
      return GDK_EVENT_STOP;

    case GDK_KEY_End:
      go_to (show, show->slides->len > 0 ? show->slides->len - 1 : 0);
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

static gboolean
on_scroll (GtkEventControllerScroll *controller, double dx, double dy, gpointer data)
{
  (void) controller; (void) dx;
  if (dy != 0)
    go (data, dy > 0 ? +1 : -1);
  return TRUE;
}

static void
surface_free (gpointer data)
{
  if (data != NULL)
    cairo_surface_destroy (data);
}

void
w42_slideshow_show (GtkWindow *parent, W42View *view)
{
  Show *show;
  GtkEventController *keys, *scroll;
  GtkGesture *click;
  W42PieceTable *pt;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;
  pt = w42_document_pt (w42_view_get_document (view));

  show = g_new0 (Show, 1);
  show->slides = w42_slides_from_document (pt);
  show->surfaces = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, surface_free);

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
  gtk_window_set_title (GTK_WINDOW (show->window), _("Slide Show"));
  if (parent != NULL)
    {
      gtk_window_set_transient_for (GTK_WINDOW (show->window), parent);
      gtk_window_set_destroy_with_parent (GTK_WINDOW (show->window), TRUE);
    }
  gtk_window_set_default_size (GTK_WINDOW (show->window), 1280, 720);
  g_object_weak_ref (G_OBJECT (show->window), show_free, show);

  show->area = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (show->area), draw_slide, show, NULL);
  /* No pointer over the slides: the audience is looking at them. */
  gtk_widget_set_cursor_from_name (show->area, "none");
  gtk_window_set_child (GTK_WINDOW (show->window), show->area);

  keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key), show);
  gtk_widget_add_controller (show->window, keys);

  click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);   /* any button */
  g_signal_connect (click, "pressed", G_CALLBACK (on_click), show);
  gtk_widget_add_controller (show->area, GTK_EVENT_CONTROLLER (click));

  scroll = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
                                            GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
  g_signal_connect (scroll, "scroll", G_CALLBACK (on_scroll), show);
  gtk_widget_add_controller (show->area, scroll);

  gtk_window_fullscreen (GTK_WINDOW (show->window));
  gtk_window_present (GTK_WINDOW (show->window));
  gtk_widget_grab_focus (show->area);
}
