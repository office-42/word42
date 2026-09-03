/* w42-shape.c - see w42-shape.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-shape.h"

#include <math.h>
#include <cairo.h>

#include "w42-image.h"

static void
set_rgb (cairo_t *cr, guint32 rgb)
{
  cairo_set_source_rgb (cr, ((rgb >> 16) & 0xff) / 255.0,
                            ((rgb >> 8) & 0xff) / 255.0,
                            (rgb & 0xff) / 255.0);
}

GBytes *
w42_shape_render (W42ShapeKind kind, int width, int height,
                  double line_pt, guint32 line_rgb,
                  gboolean filled, guint32 fill_rgb)
{
  cairo_surface_t *surface;
  cairo_t *cr;
  double lw = MAX (line_pt, 0.25) * 96.0 / 72.0;   /* points to pixels */
  double half = lw / 2.0;
  GBytes *png;

  width  = CLAMP (width, 2, 10000);
  height = CLAMP (height, 2, 10000);

  /* A line thicker than the shape leaves it nothing to be drawn round:
   * the ellipse below would have no radius at all, and Cairo draws a
   * degenerate path as nothing whatever, so the picture would come out
   * blank with no error anywhere.  Half the shorter side is as thick as
   * a line can usefully be. */
  lw = MIN (lw, MAX (MIN (width, height) / 2.0, 1.0));
  half = lw / 2.0;

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  cr = cairo_create (surface);
  cairo_set_line_width (cr, lw);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

  switch (kind)
    {
    case W42_SHAPE_LINE:
    case W42_SHAPE_ARROW:
      {
        double x0 = half, y0 = half, x1 = width - half, y1 = height - half;

        /* A line that is nearly flat or nearly upright is drawn straight,
         * which is what a one-pixel-high box asks for. */
        if (height <= lw * 2) y0 = y1 = height / 2.0;
        if (width <= lw * 2)  x0 = x1 = width / 2.0;

        set_rgb (cr, line_rgb);
        cairo_move_to (cr, x0, y0);
        cairo_line_to (cr, x1, y1);
        cairo_stroke (cr);

        if (kind == W42_SHAPE_ARROW)
          {
            double angle = atan2 (y1 - y0, x1 - x0);
            double size = MAX (lw * 4.0, 8.0);

            cairo_move_to (cr, x1, y1);
            cairo_line_to (cr, x1 - size * cos (angle - 0.45), y1 - size * sin (angle - 0.45));
            cairo_line_to (cr, x1 - size * cos (angle + 0.45), y1 - size * sin (angle + 0.45));
            cairo_close_path (cr);
            cairo_fill (cr);
          }
        break;
      }

    case W42_SHAPE_RECTANGLE:
    case W42_SHAPE_ROUNDED_RECTANGLE:
      {
        double x = half, y = half, w = width - lw, h = height - lw;

        if (kind == W42_SHAPE_ROUNDED_RECTANGLE)
          {
            double r = MIN (MIN (w, h) / 4.0, 24.0);

            cairo_new_sub_path (cr);
            cairo_arc (cr, x + w - r, y + r, r, -G_PI / 2, 0);
            cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI / 2);
            cairo_arc (cr, x + r, y + h - r, r, G_PI / 2, G_PI);
            cairo_arc (cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
            cairo_close_path (cr);
          }
        else
          cairo_rectangle (cr, x, y, w, h);

        if (filled)
          {
            set_rgb (cr, fill_rgb);
            cairo_fill_preserve (cr);
          }
        set_rgb (cr, line_rgb);
        cairo_stroke (cr);
        break;
      }

    case W42_SHAPE_ELLIPSE:
      cairo_save (cr);
      cairo_translate (cr, width / 2.0, height / 2.0);
      /* A line as thick as the shape leaves nothing to scale by, and a
       * zero scale puts the context into an error it never comes out of:
       * every later call does nothing and the picture comes out blank. */
      cairo_scale (cr, MAX ((width - lw) / 2.0, 0.5), MAX ((height - lw) / 2.0, 0.5));
      cairo_arc (cr, 0, 0, 1.0, 0, 2 * G_PI);
      cairo_restore (cr);
      if (filled)
        {
          set_rgb (cr, fill_rgb);
          cairo_fill_preserve (cr);
        }
      set_rgb (cr, line_rgb);
      cairo_stroke (cr);
      break;
    }

  cairo_destroy (cr);
  cairo_surface_flush (surface);
  png = w42_image_surface_to_png (surface);
  cairo_surface_destroy (surface);
  return png;
}
