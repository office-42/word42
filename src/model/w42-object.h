/* w42-object.h - the things in a document that are not text
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An object is a picture (for now) that sits in the text flow and occupies
 * one document position, the way a character does.  AbiWord's piece table
 * has an ObjectPiece for the same purpose.  The table holds the bytes as
 * loaded, so a JPEG saved is the same JPEG, not a recompression of it.
 */

#pragma once

#include <cairo.h>
#include <glib.h>

#include "w42-shape.h"

G_BEGIN_DECLS

typedef guint32 W42ObjectIdx;

#define W42_OBJECT_NONE ((W42ObjectIdx) G_MAXUINT32)

/* How the text treats a picture: as a character in the line, or as a
 * frame at the left or right of its paragraph with the text beside it,
 * or -- Word XP's other wrapping styles -- with the text above and below
 * it only, or running on under or over it. */
typedef enum {
  W42_WRAP_INLINE = 0,
  W42_WRAP_LEFT,              /* the picture at the left, text to its right */
  W42_WRAP_RIGHT,
  W42_WRAP_TOP_BOTTOM,        /* the text stops above it and goes on below */
  W42_WRAP_FRONT,             /* the text runs on under it: "in front of text" */
  W42_WRAP_BEHIND             /* the picture is painted under the text */
} W42Wrap;

typedef struct {
  GBytes          *data;      /* the picture file's bytes, as loaded; for a
                               * shape, a PNG of it for the formats that
                               * cannot say a shape */
  const char      *format;    /* interned: "png", "jpeg", ... */
  int              pixel_w;   /* the picture's own size */
  int              pixel_h;
  int              width;     /* the size it is shown at, in twips */
  int              height;
  W42Wrap          wrap;
  gboolean         positioned;  /* a wrapped object with a place of its own:
                                 * pos_x and pos_y twips from the left of the
                                 * column and the top of its paragraph, rather
                                 * than the column's edge and the first line */
  int              pos_x, pos_y;
  W42ShapeKind     shape;     /* what it is, when it is not a picture */
  double           line_pt;   /* a shape's outline: its width in points, 0 for
                               * none, and its colour */
  guint32          line_rgb;
  gboolean         filled;    /* and its fill */
  guint32          fill_rgb;
  const char      *text;      /* interned; the text set in the shape, or NULL */
  cairo_surface_t *surface;   /* decoded on first draw; a cache */
} W42Object;

typedef struct _W42ObjectTable W42ObjectTable;

W42ObjectTable   *w42_object_table_new  (void);
void              w42_object_table_free (W42ObjectTable *table);

/* Takes its own reference to `data`. */
W42ObjectIdx      w42_object_table_add  (W42ObjectTable *table,
                                         GBytes         *data,
                                         const char     *format,
                                         int             pixel_w,
                                         int             pixel_h,
                                         int             width,
                                         int             height);

const W42Object  *w42_object_table_get  (W42ObjectTable *table, W42ObjectIdx idx);
void              w42_object_table_set_wrap (W42ObjectTable *table, W42ObjectIdx idx, W42Wrap wrap);
/* Where a wrapped object sits, in twips from the column's left and its
 * paragraph's top; `positioned` off puts it back at the column's edge. */
void              w42_object_table_set_position (W42ObjectTable *table, W42ObjectIdx idx,
                                                 gboolean positioned, int x, int y);
/* Makes the object a shape: what it is, its outline and its fill, and
 * the text in it, which may be NULL. */
void              w42_object_table_set_shape (W42ObjectTable *table, W42ObjectIdx idx,
                                              W42ShapeKind kind, double line_pt, guint32 line_rgb,
                                              gboolean filled, guint32 fill_rgb, const char *text);
/* A copy of the object at another size: everything else carried over. */
W42ObjectIdx      w42_object_table_clone (W42ObjectTable *table, W42ObjectIdx idx,
                                          int width, int height);
guint             w42_object_table_size (W42ObjectTable *table);

/* The decoded picture, decoding it the first time.  Owned by the table. */
cairo_surface_t  *w42_object_surface    (W42ObjectTable *table, W42ObjectIdx idx);

G_END_DECLS
