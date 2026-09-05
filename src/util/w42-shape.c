/* w42-shape.c - see w42-shape.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-shape.h"

#include <math.h>
#include <cairo.h>
#include <pango/pangocairo.h>

#include "w42-image.h"

static void
set_rgb (cairo_t *cr, guint32 rgb)
{
  cairo_set_source_rgb (cr, ((rgb >> 16) & 0xff) / 255.0,
                            ((rgb >> 8) & 0xff) / 255.0,
                            (rgb & 0xff) / 255.0);
}

void
w42_shape_draw (cairo_t *cr, W42ShapeKind kind, double width, double height,
                double line_pt, guint32 line_rgb,
                gboolean filled, guint32 fill_rgb,
                const char *text, const char *font)
{
  double lw = line_pt * 96.0 / 72.0;   /* points to pixels */
  gboolean stroked = line_pt > 0.0;
  double half;

  g_return_if_fail (cr != NULL);

  width  = MAX (width, 1.0);
  height = MAX (height, 1.0);

  /* A line thicker than the shape leaves it nothing to be drawn round:
   * the ellipse below would have no radius at all, and Cairo draws a
   * degenerate path as nothing whatever.  Half the shorter side is as
   * thick as a line can usefully be; and a line or an arrow with no
   * outline has nothing to show, so it keeps a hairline. */
  if (!stroked && (kind == W42_SHAPE_LINE || kind == W42_SHAPE_ARROW))
    {
      stroked = TRUE;
      lw = 1.0;
    }
  lw = CLAMP (lw, stroked ? 0.75 : 0.0, MAX (MIN (width, height) / 2.0, 1.0));
  half = lw / 2.0;

  cairo_save (cr);
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
            if (stroked)
              cairo_fill_preserve (cr);
            else
              cairo_fill (cr);
          }
        if (stroked)
          {
            set_rgb (cr, line_rgb);
            cairo_stroke (cr);
          }
        else
          cairo_new_path (cr);
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
          if (stroked)
            cairo_fill_preserve (cr);
          else
            cairo_fill (cr);
        }
      if (stroked)
        {
          set_rgb (cr, line_rgb);
          cairo_stroke (cr);
        }
      else
        cairo_new_path (cr);
      break;

    default:
      break;
    }

  /* The text, set in the middle, wrapped to the shape's width less a
   * margin, as Word set a text box's. */
  if (text != NULL && *text != '\0' && kind != W42_SHAPE_LINE && kind != W42_SHAPE_ARROW)
    {
      PangoLayout *layout = pango_cairo_create_layout (cr);
      PangoFontDescription *desc = pango_font_description_from_string (font != NULL ? font : "Times New Roman, 10");
      double inset = MAX (lw, 1.0) + 96.0 * 0.1;   /* a tenth of an inch in */
      int lh;

      pango_layout_set_font_description (layout, desc);
      pango_layout_set_text (layout, text, -1);
      pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
      pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);
      pango_layout_set_width (layout, (int) (MAX (width - 2 * inset, 8.0) * PANGO_SCALE));
      pango_layout_get_pixel_size (layout, NULL, &lh);
      cairo_save (cr);
      cairo_rectangle (cr, half, half, MAX (width - lw, 0.0), MAX (height - lw, 0.0));
      cairo_clip (cr);
      cairo_set_source_rgb (cr, 0, 0, 0);
      cairo_move_to (cr, inset, MAX ((height - lh) / 2.0, half));
      pango_cairo_show_layout (cr, layout);
      cairo_restore (cr);
      pango_font_description_free (desc);
      g_object_unref (layout);
    }

  cairo_restore (cr);
}

GBytes *
w42_shape_render (W42ShapeKind kind, int width, int height,
                  double line_pt, guint32 line_rgb,
                  gboolean filled, guint32 fill_rgb,
                  const char *text)
{
  cairo_surface_t *surface;
  cairo_t *cr;
  GBytes *png;

  width  = CLAMP (width, 2, 10000);
  height = CLAMP (height, 2, 10000);

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  cr = cairo_create (surface);
  w42_shape_draw (cr, kind, width, height, line_pt, line_rgb, filled, fill_rgb, text, NULL);
  cairo_destroy (cr);
  cairo_surface_flush (surface);
  png = w42_image_surface_to_png (surface);
  cairo_surface_destroy (surface);
  return png;
}
