/* w42-math.h - equations: reading MathML, and setting it as type
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An equation in a document is MathML, the W3C's markup for mathematics,
 * which browsers show, EPUB carries and OpenDocument keeps its formulas
 * in.  This reads it into a tree and sets the tree the way TeX sets
 * mathematics -- italic letters, upright numbers and names, space round
 * relations and operators, scripts smaller and raised, fractions on the
 * axis, radicals and fences grown to what they hold -- with Pango for the
 * glyphs and Cairo for the rules, so that it draws on the screen, on the
 * printer and into a PDF alike.
 *
 * Units are points throughout.  A formula's box has a width, a height
 * above its baseline and a depth below it, and sits on the baseline of
 * the line it is in as a character does.
 */

#pragma once

#include <glib.h>
#include <cairo.h>

G_BEGIN_DECLS

#define W42_MATHML_NS "http://www.w3.org/1998/Math/MathML"

/* ---- The tree ------------------------------------------------------- */

typedef struct _W42MathNode W42MathNode;

struct _W42MathNode {
  const char  *name;       /* interned, without a namespace prefix: "mi" */
  char       **attrs;      /* name, value, name, value, ..., NULL */
  GPtrArray   *children;   /* W42MathNode* */
  char        *text;       /* a token's text, whitespace trimmed and folded;
                            * an annotation's, as it was; else NULL */
};

/* Reads MathML: a <math> element, with or without its namespace or a
 * prefix on it, and the entities MathML names (&alpha;, &InvisibleTimes;)
 * as well as XML's.  NULL, with `error` set, for what is not MathML. */
W42MathNode *w42_math_parse (const char *mathml, gssize len, GError **error);
void         w42_math_node_free (W42MathNode *node);

W42MathNode *w42_math_node_new (const char *name, const char *text);
void         w42_math_node_add  (W42MathNode *parent, W42MathNode *child);
void         w42_math_node_set_attr (W42MathNode *node, const char *name, const char *value);
const char  *w42_math_node_attr (const W42MathNode *node, const char *name);

/* The tree as MathML: `<math xmlns="...">` and no prefixes.  Free with
 * g_free. */
char        *w42_math_node_to_string (const W42MathNode *node);

/* Whether the XML's root element is a MathML <math>, with or without a
 * prefix: what a file's part or data item is, before it is read as one. */
gboolean     w42_math_is_mathml (const char *xml, gsize len);

/* Parse and write out again: the one form the document keeps, whatever a
 * file wrote.  NULL for what is not MathML. */
char        *w42_math_canonical (const char *mathml, gssize len);

/* The text of the annotation in `encoding` ("application/x-tex") the
 * formula carries in its <semantics>, or NULL.  Not a copy. */
const char  *w42_math_annotation (const W42MathNode *root, const char *encoding);

/* Whether the formula is set on a line of its own: <math display="block">. */
gboolean     w42_math_is_display (const W42MathNode *root);

/* ---- Setting it ----------------------------------------------------- */

typedef struct _W42MathBox W42MathBox;

/* Sets the formula at `size` points.  Never fails: what it does not know
 * it sets as a row of what is in it. */
W42MathBox  *w42_math_box_new  (const W42MathNode *root, double size);
void         w42_math_box_free (W42MathBox *box);
void         w42_math_box_extents (const W42MathBox *box, double *width,
                                   double *ascent, double *descent);
/* Draws the formula with its baseline's left end at (x, y), in the
 * context's source colour where the formula does not name one. */
void         w42_math_box_draw (const W42MathBox *box, cairo_t *cr, double x, double y);

/* Draws the formula to fill the box at (x, y), `w` by `h`, as
 * w42_math_measure measured it, scaled to it: the way a formula is
 * shown at whatever size its box has been given. */
void         w42_math_box_draw_in (const W42MathBox *box, cairo_t *cr,
                                   double x, double y, double w, double h);

/* The size a formula comes to at `size` points, in twips: its width, its
 * whole height and the part of that below the baseline.  FALSE for what
 * is not MathML. */
gboolean     w42_math_measure (const char *mathml, double size,
                               int *width, int *height, int *descent);

/* A PNG of the formula at `size` points, drawn at `ppi` pixels to the
 * inch on a clear ground, for the formats that can only say "picture".
 * `pixel_w` and `pixel_h` receive its size.  NULL for what is not
 * MathML. */
GBytes      *w42_math_render_png (const char *mathml, double size, int ppi,
                                  int *pixel_w, int *pixel_h);

G_END_DECLS
