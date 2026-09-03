/* w42-ruler.c - see w42-ruler.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ruler is a drawing area with three gestures on it.  It shows the
 * current paragraph's indents and tab stops and lets them be dragged; a
 * click on the empty band sets a tab stop of the kind shown in the little
 * box at the left, which a click on the box cycles, as Word 6's did.
 * Nothing changes in the document until the button goes up.
 */

#include "w42-ruler.h"

#include "w42-settings.h"

#include <math.h>

#define RULER_HEIGHT 20
#define TYPE_BOX     16          /* the tab-kind box at the far left */
#define GRAB         5.0         /* px either side of a marker */
#define SNAP         90          /* twips: a sixteenth of an inch */

typedef enum {
  DRAG_NONE,
  DRAG_FIRST,      /* first-line indent */
  DRAG_LEFT,       /* left indent, first line going with it */
  DRAG_RIGHT,      /* right indent */
  DRAG_TAB         /* a tab stop, by index */
} DragKind;

typedef struct {
  W42View    *view;
  GtkWidget  *area;

  W42TabKind  new_kind;    /* what a click on the band sets */

  DragKind    drag;
  int         drag_index;  /* which tab stop */
  double      drag_x;      /* widget px, where the marker is now */
  double      drag_y;      /* below the band means "take it away" */
  gboolean    drag_new;    /* the tab stop was made by this press */
} Ruler;

/* ---------------------------------------------------------------------- */
/* Geometry                                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
  double left, column, page_left, page_width, zoom;
} Column;

static gboolean
ruler_column (Ruler *self, Column *c)
{
  if (w42_view_get_document (self->view) == NULL)
    return FALSE;

  c->zoom = w42_view_get_zoom (self->view);
  w42_view_get_text_column (self->view, &c->left, &c->column,
                            &c->page_left, &c->page_width);
  return c->column > 0;
}

static double
twips_to_x (const Column *c, int twips)
{
  return c->left + twips / 1440.0 * W42_LAYOUT_DPI * c->zoom;
}

/* The width of the text column, in twips: what the indents are measured
 * against, and the far side of where a marker may be put. */
static int
column_twips (const Column *c)
{
  return (int) lround (c->column / (W42_LAYOUT_DPI * c->zoom) * 1440.0);
}

static int
x_to_twips (const Column *c, double x)
{
  double twips = (x - c->left) / (W42_LAYOUT_DPI * c->zoom) * 1440.0;
  int snapped = (int) lround (twips / SNAP) * SNAP;

  return snapped;
}

/* ---------------------------------------------------------------------- */
/* Drawing                                                                 */
/* ---------------------------------------------------------------------- */

static void
draw_triangle (cairo_t *cr, double x, double y, gboolean point_down)
{
  cairo_move_to (cr, x - 4, point_down ? y : y + 5);
  cairo_line_to (cr, x + 4, point_down ? y : y + 5);
  cairo_line_to (cr, x, point_down ? y + 5 : y);
  cairo_close_path (cr);
  cairo_fill (cr);
}

/* A tab stop's mark: L for left, the mirror for right, an inverted T for
 * centre, with a dot after it for decimal. */
static void
draw_tab_mark (cairo_t *cr, double x, double y, W42TabKind kind)
{
  x = floor (x) + 0.5;
  cairo_set_line_width (cr, 2.0);
  cairo_move_to (cr, x, y - 6);
  cairo_line_to (cr, x, y);
  cairo_stroke (cr);
  cairo_set_line_width (cr, 2.0);
  switch (kind)
    {
    case W42_TAB_LEFT:
      cairo_move_to (cr, x - 1, y - 1);  cairo_line_to (cr, x + 5, y - 1);
      break;
    case W42_TAB_RIGHT:
      cairo_move_to (cr, x - 5, y - 1);  cairo_line_to (cr, x + 1, y - 1);
      break;
    case W42_TAB_CENTER:
    case W42_TAB_DECIMAL:
    default:
      cairo_move_to (cr, x - 4, y - 1);  cairo_line_to (cr, x + 4, y - 1);
      break;
    }
  cairo_stroke (cr);
  if (kind == W42_TAB_DECIMAL)
    {
      cairo_rectangle (cr, x + 4, y - 6, 2, 2);
      cairo_fill (cr);
    }
}

static void
ruler_draw (GtkDrawingArea *area,
            cairo_t        *cr,
            int             width,
            int             height,
            gpointer        data)
{
  Ruler *self = data;
  Column c;
  W42ParaFmt pa;
  double top = 3, band = height - 8;
  PangoLayout *layout;
  PangoFontDescription *font;

  (void) area;

  /* Silver face, as the chrome around it. */
  cairo_set_source_rgb (cr, 0.753, 0.753, 0.753);
  cairo_paint (cr);

  if (!ruler_column (self, &c))
    return;

  w42_view_get_para_fmt (self->view, &pa);

  /* The tab-kind box: sunken, with the mark a click on the band would set. */
  cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
  cairo_rectangle (cr, 2, top, TYPE_BOX - 2, band);
  cairo_fill (cr);
  cairo_set_line_width (cr, 1.0);
  cairo_set_source_rgb (cr, 0.50, 0.50, 0.50);
  cairo_move_to (cr, 2.5, top + band);
  cairo_line_to (cr, 2.5, top + 0.5);
  cairo_line_to (cr, TYPE_BOX, top + 0.5);
  cairo_stroke (cr);
  cairo_set_source_rgb (cr, 0.05, 0.05, 0.05);
  draw_tab_mark (cr, 2 + (TYPE_BOX - 2) / 2.0 - 1, top + band - 3, self->new_kind);

  /* The margins are the darker part of the band, the text column the light
   * strip, both sunk into the face. */
  cairo_set_source_rgb (cr, 0.60, 0.60, 0.60);
  cairo_rectangle (cr, c.page_left, top, c.page_width, band);
  cairo_fill (cr);

  cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
  cairo_rectangle (cr, c.left, top, c.column, band);
  cairo_fill (cr);

  cairo_set_line_width (cr, 1.0);
  cairo_set_source_rgb (cr, 0.50, 0.50, 0.50);
  cairo_move_to (cr, c.page_left + 0.5, top + band);
  cairo_line_to (cr, c.page_left + 0.5, top + 0.5);
  cairo_line_to (cr, c.page_left + c.page_width, top + 0.5);
  cairo_stroke (cr);
  cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
  cairo_move_to (cr, c.page_left + c.page_width - 0.5, top);
  cairo_line_to (cr, c.page_left + c.page_width - 0.5, top + band - 0.5);
  cairo_line_to (cr, c.page_left, top + band - 0.5);
  cairo_stroke (cr);

  /* Numbers every inch with ticks at the quarters -- or every centimetre
   * with a tick at the half -- measured from the left text margin the way
   * Word does. */
  layout = pango_cairo_create_layout (cr);
  font = pango_font_description_from_string ("Sans 7");
  pango_layout_set_font_description (layout, font);
  pango_font_description_free (font);

  cairo_set_source_rgb (cr, 0.05, 0.05, 0.05);

  {
    gboolean cm = w42_settings_get_units () == W42_UNITS_CM;
    double unit_px = (cm ? W42_LAYOUT_DPI / 2.54 : W42_LAYOUT_DPI) * c.zoom;
    int per_unit = cm ? 2 : 4;

    for (int tick = 0; ; tick++)
      {
        double x = c.left + ((double) tick / per_unit) * unit_px;

        if (x > c.left + c.column + 1 || x > width)
          break;

        if (tick % per_unit == 0)
          {
            if (tick > 0)
              {
                char label[16];
                int tw = 0, th = 0;

                g_snprintf (label, sizeof label, "%d", tick / per_unit);
                pango_layout_set_text (layout, label, -1);
                pango_layout_get_pixel_size (layout, &tw, &th);

                cairo_move_to (cr, x - tw / 2.0, (height - th) / 2.0);
                pango_cairo_show_layout (cr, layout);
              }
          }
        else if (tick % 2 == 0 || cm)
          {
            cairo_rectangle (cr, floor (x), height / 2.0 - 2.5, 1, 5);
            cairo_fill (cr);
          }
        else
          {
            cairo_rectangle (cr, floor (x), height / 2.0 - 1.0, 1, 2);
            cairo_fill (cr);
          }
      }
  }

  g_object_unref (layout);

  /* Default tab stops every half inch: faint ticks along the bottom. */
  cairo_set_source_rgb (cr, 0.45, 0.45, 0.45);
  for (int t = 720; ; t += 720)
    {
      double x = twips_to_x (&c, t);

      if (x >= c.left + c.column)
        break;
      cairo_rectangle (cr, floor (x), top + band - 2, 1, 2);
      cairo_fill (cr);
    }

  /* The paragraph's own tab stops. */
  cairo_set_source_rgb (cr, 0.05, 0.05, 0.05);
  for (int i = 0; i < pa.n_tabs; i++)
    {
      double x;
      gboolean going = self->drag == DRAG_TAB && self->drag_index == i;

      if (going && self->drag_y > RULER_HEIGHT + 4)
        continue;         /* being pulled off */

      x = going ? self->drag_x : twips_to_x (&c, pa.tab_pos[i]);
      draw_tab_mark (cr, x, top + band - 1, W42_TAB_KIND (pa.tab_kind[i]));
    }

  /* The indent markers: first line at the top, left and right at the
   * bottom, where the paragraph really has them. */
  {
    double first = twips_to_x (&c, pa.indent_left + pa.indent_first);
    double left  = twips_to_x (&c, pa.indent_left);
    double right = c.left + c.column - (pa.indent_right / 1440.0) * W42_LAYOUT_DPI * c.zoom;

    if (self->drag == DRAG_FIRST) first = self->drag_x;
    if (self->drag == DRAG_LEFT)
      {
        first += self->drag_x - left;
        left = self->drag_x;
      }
    if (self->drag == DRAG_RIGHT) right = self->drag_x;

    cairo_set_source_rgb (cr, 0.35, 0.35, 0.35);
    draw_triangle (cr, first, top, TRUE);
    draw_triangle (cr, left, top + band - 5, FALSE);
    draw_triangle (cr, right, top + band - 5, FALSE);
  }

  /* While something drags, a guide line shows where it will land. */
  if (self->drag != DRAG_NONE && !(self->drag == DRAG_TAB && self->drag_y > RULER_HEIGHT + 4))
    {
      cairo_set_source_rgb (cr, 0.0, 0.0, 0.5);
      cairo_rectangle (cr, floor (self->drag_x), top, 1, band);
      cairo_fill (cr);
    }
}

/* ---------------------------------------------------------------------- */
/* Hit testing and dragging                                                */
/* ---------------------------------------------------------------------- */

static DragKind
hit_test (Ruler *self, double x, double y, int *index)
{
  Column c;
  W42ParaFmt pa;
  double top = 3, band = RULER_HEIGHT - 8;

  if (!ruler_column (self, &c))
    return DRAG_NONE;

  w42_view_get_para_fmt (self->view, &pa);

  /* Tab stops first: they are small and sit on the indent markers' row. */
  for (int i = 0; i < pa.n_tabs; i++)
    if (fabs (x - twips_to_x (&c, pa.tab_pos[i])) <= GRAB && y > top + band / 2)
      {
        *index = i;
        return DRAG_TAB;
      }

  if (y <= top + band / 2)
    {
      if (fabs (x - twips_to_x (&c, pa.indent_left + pa.indent_first)) <= GRAB)
        return DRAG_FIRST;
    }
  else
    {
      double right = c.left + c.column - (pa.indent_right / 1440.0) * W42_LAYOUT_DPI * c.zoom;

      if (fabs (x - twips_to_x (&c, pa.indent_left)) <= GRAB)
        return DRAG_LEFT;
      if (fabs (x - right) <= GRAB)
        return DRAG_RIGHT;
    }

  return DRAG_NONE;
}

static void
on_drag_begin (GtkGestureDrag *gesture, double x, double y, gpointer data)
{
  Ruler *self = data;
  Column c;
  int index = 0;

  if (!ruler_column (self, &c))
    {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }

  /* The box at the left cycles the kind of tab a click sets. */
  if (x < TYPE_BOX)
    {
      self->new_kind = (W42TabKind) ((self->new_kind + 1) % 4);
      gtk_widget_queue_draw (self->area);
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }

  self->drag = hit_test (self, x, y, &index);
  self->drag_index = index;
  self->drag_x = x;
  self->drag_y = y;
  self->drag_new = FALSE;

  /* Nothing under the pointer: a click on the column sets a tab stop,
   * which is then dragged like any other until the button goes up. */
  if (self->drag == DRAG_NONE)
    {
      W42ParaFmt pa;
      int twips = x_to_twips (&c, x);

      if (x < c.left || x > c.left + c.column || twips <= 0)
        {
          gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
          return;
        }

      w42_view_get_para_fmt (self->view, &pa);
      w42_para_fmt_set_tab (&pa, twips, self->new_kind);
      w42_view_apply_para_fmt (self->view, W42_PARA_TABS, &pa);

      self->drag_index = -1;
      for (int i = 0; i < pa.n_tabs; i++)
        if (pa.tab_pos[i] == twips)
          self->drag_index = i;
      if (self->drag_index < 0)
        {
          /* No room for another stop: nothing to drag. */
          gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
          return;
        }
      self->drag = DRAG_TAB;
      self->drag_new = TRUE;
      self->drag_x = twips_to_x (&c, twips);
    }

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_widget_queue_draw (self->area);
}

static void
on_drag_update (GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
  Ruler *self = data;
  double x0 = 0, y0 = 0;

  if (self->drag == DRAG_NONE)
    return;

  gtk_gesture_drag_get_start_point (gesture, &x0, &y0);
  self->drag_x = x0 + dx;
  self->drag_y = y0 + dy;
  gtk_widget_queue_draw (self->area);
}

static void
on_drag_end (GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
  Ruler *self = data;
  Column c;
  W42ParaFmt pa;
  double x0 = 0, y0 = 0, x, y;
  DragKind drag = self->drag;

  self->drag = DRAG_NONE;

  if (drag == DRAG_NONE || !ruler_column (self, &c))
    {
      gtk_widget_queue_draw (self->area);
      return;
    }

  gtk_gesture_drag_get_start_point (gesture, &x0, &y0);
  x = x0 + dx;
  y = y0 + dy;

  w42_view_get_para_fmt (self->view, &pa);

  switch (drag)
    {
    case DRAG_FIRST:
      {
        int width = column_twips (&c);
        /* The first line starts somewhere in the column: as far left as
         * the left margin, and not past the right indent. */
        int first = CLAMP (x_to_twips (&c, x), 0,
                           MAX (width - pa.indent_right - 360, 0));

        pa.indent_first = first - pa.indent_left;
        w42_view_apply_para_fmt (self->view, W42_PARA_INDENT_FIRST, &pa);
      }
      break;

    case DRAG_LEFT:
      {
        int width = column_twips (&c);
        int left = CLAMP (x_to_twips (&c, x), 0, MAX (width - pa.indent_right - 360, 0));
        /* The first line keeps its distance from the left indent. */
        pa.indent_left = left;
        w42_view_apply_para_fmt (self->view, W42_PARA_INDENT_LEFT, &pa);
      }
      break;

    case DRAG_RIGHT:
      {
        int width = column_twips (&c);
        int right = width - x_to_twips (&c, x);

        pa.indent_right = CLAMP (right, 0, MAX (width - pa.indent_left - 360, 0));
        w42_view_apply_para_fmt (self->view, W42_PARA_INDENT_RIGHT, &pa);
      }
      break;

    case DRAG_TAB:
      if (self->drag_index < pa.n_tabs)
        {
          int old = pa.tab_pos[self->drag_index];
          W42TabKind kind = W42_TAB_KIND (pa.tab_kind[self->drag_index]);
          W42TabLeader leader = W42_TAB_LEADER (pa.tab_kind[self->drag_index]);
          int twips = x_to_twips (&c, x);

          w42_para_fmt_clear_tab (&pa, old);
          /* Pulled below the ruler, or off the column, it is gone. */
          if (y <= RULER_HEIGHT + 4 && twips > 0 && x <= c.left + c.column)
            w42_para_fmt_set_tab_leader (&pa, twips, kind, leader);
          if (!(self->drag_new && dx == 0 && dy == 0))
            w42_view_apply_para_fmt (self->view, W42_PARA_TABS, &pa);
        }
      break;

    default:
      break;
    }

  gtk_widget_grab_focus (GTK_WIDGET (self->view));
  gtk_widget_queue_draw (self->area);
}

/* ---------------------------------------------------------------------- */

static void
on_view_state_changed (W42View *view, gpointer data)
{
  (void) view;
  gtk_widget_queue_draw (GTK_WIDGET (data));
}

static void
ruler_free (gpointer data)
{
  g_free (data);
}

GtkWidget *
w42_ruler_new (W42View *view)
{
  GtkWidget *area = gtk_drawing_area_new ();
  Ruler *self;
  GtkGesture *drag;

  g_return_val_if_fail (W42_IS_VIEW (view), area);

  self = g_new0 (Ruler, 1);
  self->view = view;
  self->area = area;
  self->new_kind = W42_TAB_LEFT;
  self->drag = DRAG_NONE;
  g_object_set_data_full (G_OBJECT (area), "w42-ruler", self, ruler_free);

  gtk_widget_set_size_request (area, -1, RULER_HEIGHT);
  gtk_widget_add_css_class (area, "w42-ruler");
  gtk_widget_set_cursor_from_name (area, "default");

  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), ruler_draw,
                                  self, NULL);

  drag = gtk_gesture_drag_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
  g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
  g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
  g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
  gtk_widget_add_controller (area, GTK_EVENT_CONTROLLER (drag));

  g_signal_connect_object (view, "state-changed",
                           G_CALLBACK (on_view_state_changed), area, 0);

  return area;
}

void
w42_ruler_set_view (GtkWidget *ruler, W42View *view)
{
  Ruler *self = g_object_get_data (G_OBJECT (ruler), "w42-ruler");

  g_return_if_fail (self != NULL);
  g_return_if_fail (W42_IS_VIEW (view));

  if (self->view == view)
    return;

  g_signal_handlers_disconnect_by_func (self->view,
                                        G_CALLBACK (on_view_state_changed),
                                        ruler);
  self->view = view;
  g_signal_connect_object (view, "state-changed",
                           G_CALLBACK (on_view_state_changed), ruler, 0);
  gtk_widget_queue_draw (ruler);
}
