/* w42-shape.h - Insert > Drawing: lines and boxes on the page
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 6's Drawing toolbar put lines, rectangles and ellipses on the page,
 * and Word XP's AutoShapes gave them fills, outlines and text.  word42
 * keeps a shape as what it is and draws it with Cairo wherever the page
 * is painted, so it stays sharp in print and in PDF; and it keeps a PNG
 * of it too, for the formats that can only say "picture".
 */

#pragma once

#include <glib.h>
#include <cairo.h>

G_BEGIN_DECLS

/* What a drawing object is.  W42_SHAPE_PICTURE is not a shape at all but
 * a picture, which is what an object is unless it says otherwise. */
typedef enum {
  W42_SHAPE_PICTURE = 0,
  W42_SHAPE_LINE,
  W42_SHAPE_ARROW,
  W42_SHAPE_RECTANGLE,
  W42_SHAPE_ROUNDED_RECTANGLE,
  W42_SHAPE_ELLIPSE,
  W42_SHAPE_KINDS
} W42ShapeKind;

/* Draws the shape into the box from (0, 0) to (width, height) in the
 * context's current units, with a line `line_pt` points wide in
 * `line_rgb` (0xRRGGBB; 0 points is no line) and, when `filled`, filled
 * with `fill_rgb`.  A line runs from the top left to the bottom right
 * corner; an arrow points that way too.  `text`, if any, is set in the
 * middle of the shape in `font`, a Pango description such as "Times New
 * Roman 10", or the default when NULL. */
void    w42_shape_draw (cairo_t *cr, W42ShapeKind kind, double width, double height,
                        double line_pt, guint32 line_rgb,
                        gboolean filled, guint32 fill_rgb,
                        const char *text, const char *font);

/* A PNG of the shape, `width` by `height` pixels at 96 dpi, for the
 * formats that cannot say a shape. */
GBytes *w42_shape_render (W42ShapeKind kind, int width, int height,
                          double line_pt, guint32 line_rgb,
                          gboolean filled, guint32 fill_rgb,
                          const char *text);

G_END_DECLS
