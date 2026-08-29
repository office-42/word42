/* w42-shape.h - Insert > Drawing: lines and boxes as pictures
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 6's Drawing toolbar put lines, rectangles and ellipses on the page.
 * word42 draws them with Cairo into a PNG and puts that in the text as a
 * picture, so they resize with the handles, print, and go through RTF,
 * HTML and PDF like any picture does.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  W42_SHAPE_LINE,
  W42_SHAPE_ARROW,
  W42_SHAPE_RECTANGLE,
  W42_SHAPE_ROUNDED_RECTANGLE,
  W42_SHAPE_ELLIPSE
} W42ShapeKind;

/* A PNG of the shape, `width` by `height` pixels at 96 dpi, drawn with a
 * line `line_pt` points wide in `line_rgb` (0xRRGGBB) and, when `filled`,
 * filled with `fill_rgb`.  A line runs from the top left to the bottom
 * right corner; an arrow points that way too. */
GBytes *w42_shape_render (W42ShapeKind kind, int width, int height,
                          double line_pt, guint32 line_rgb,
                          gboolean filled, guint32 fill_rgb);

G_END_DECLS
