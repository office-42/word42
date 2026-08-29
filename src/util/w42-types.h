/* w42-types.h - basic types and unit conversions shared across word42
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.  See the LICENSE file for the full text.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Word 6 measured everything in twips (twentieths of a point, 1/1440 inch)
 * and so do we.  Layout happens at a fixed 96 dpi reference resolution; the
 * zoom factor is applied as a cairo scale at paint time, so a document lays
 * out identically at every zoom level. */
#define W42_TWIPS_PER_INCH 1440.0
#define W42_LAYOUT_DPI       96.0
#define W42_TWIPS_PER_PX   (W42_TWIPS_PER_INCH / W42_LAYOUT_DPI)   /* 15.0 */

static inline double w42_twips_to_px (double twips) { return twips / W42_TWIPS_PER_PX; }
static inline double w42_px_to_twips (double px)    { return px   * W42_TWIPS_PER_PX; }

/* Font sizes are stored in half-points, again following Word's own file
 * formats: 20 == 10pt.  Pango wants points. */
static inline double w42_halfpt_to_pt (int halfpt) { return halfpt / 2.0; }

/* Page geometry, in twips.  Word 6 called this Page Setup. */
typedef struct {
  int width;
  int height;
  int margin_left;
  int margin_right;
  int margin_top;
  int margin_bottom;
  int columns;          /* newspaper columns; 0 or 1 is one */
  int column_gap;       /* twips between them; 0 means a half inch */
  guint8  has_background;  /* the page has a colour of its own */
  guint32 background;      /* 0x00RRGGBB, when it has */
} W42PageSetup;

static inline int w42_page_columns (const W42PageSetup *page)
{
  return page != NULL && page->columns > 1 ? MIN (page->columns, 6) : 1;
}
static inline int w42_page_column_gap (const W42PageSetup *page)
{
  return page != NULL && page->column_gap > 0 ? page->column_gap : 720;
}

typedef enum {
  W42_ALIGN_LEFT = 0,
  W42_ALIGN_CENTER,
  W42_ALIGN_RIGHT,
  W42_ALIGN_JUSTIFY
} W42Align;

G_END_DECLS
