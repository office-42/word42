/* w42-layout.h - turning a document into laid-out lines on numbered pages
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The layout engine formats the document at a fixed 96 dpi reference
 * resolution and hands back page-relative pixel coordinates.  Zoom is a cairo
 * scale applied at paint time, so a page breaks in the same place whatever
 * the zoom or the screen's DPI -- which is the whole point of a page-layout
 * view, and what Word 6 called Page Layout as opposed to Normal.
 */

#pragma once

#include <pango/pangocairo.h>

#include "w42-document.h"
#include "w42-spell.h"

G_BEGIN_DECLS

/* One laid-out line, positioned relative to the top-left corner of its page. */
typedef struct {
  int              page;
  double           x;           /* left edge of the line itself */
  double           origin_x;    /* left edge of the paragraph's text column;
                                 * Pango reports x-ranges relative to this */
  double           y;
  double           width;
  double           height;
  double           baseline;     /* from the top of the line box */
  int              block;        /* index into w42_layout_blocks() */
  guint            start_index;  /* byte offset into the block's text */
  guint            length;       /* bytes */
  PangoLayoutLine *line;         /* owned by the block's PangoLayout */

  /* A section number in front of a heading's first line, or NULL.  Owned by
   * the layout; drawn at prefix_x on the line's baseline. */
  PangoLayout     *prefix;
  double           prefix_x;
} W42LineBox;

typedef struct _W42Layout W42Layout;

W42Layout *w42_layout_new  (void);
void       w42_layout_free (W42Layout *self);

/* Normal view formats the document as one continuous galley: the text column
 * keeps its width, but nothing is broken into pages.  Page Layout view is
 * the same engine with the breaks left in. */
void       w42_layout_set_galley (W42Layout *self, gboolean galley);
/* View > Show Formatting Marks: spaces, tabs, line and paragraph ends
 * are painted in blue over the text. */
void       w42_layout_set_show_marks (W42Layout *self, gboolean show);
/* Table > Gridlines: the cells of a table with no rules shown faintly,
 * so that it can be seen while it is edited.  Not printed. */
void       w42_layout_set_gridlines (W42Layout *self, gboolean show);
gboolean   w42_layout_get_galley (W42Layout *self);

/* Re-formats everything.  Cheap enough for documents of the size Word 6 was
 * built for; incremental reformatting is the obvious next optimisation. */
void       w42_layout_build (W42Layout *self, W42Document *doc);

/* The same, from a piece table and a page setup without a document around
 * them, which is what the exporters have. */
void       w42_layout_build_pt (W42Layout          *self,
                                W42PieceTable      *pt,
                                const W42PageSetup *page);

int           w42_layout_n_pages       (W42Layout *self);
double        w42_layout_page_width    (W42Layout *self);   /* px */
double        w42_layout_page_height   (W42Layout *self);   /* px */
/* Words the dictionary does not know are underlined in red, for as long as
 * a dictionary is set.  None is set for printing.  The word the caret is in
 * is left alone: it is being typed, and is not wrong yet. */
void w42_layout_set_spell       (W42Layout *self, W42Spell *spell);
void w42_layout_set_spell_caret (W42Layout *self, gsize pos);

const GArray *w42_layout_lines         (W42Layout *self);   /* of W42LineBox */
GPtrArray    *w42_layout_blocks        (W42Layout *self);   /* of W42Block* */

/* A header or footer laid out for one page, in page-relative pixels. */
typedef struct {
  int          page;
  double       x;
  double       y;
  PangoLayout *layout;    /* owned by the W42Layout */
} W42Furniture;

const GArray *w42_layout_furniture (W42Layout *self);   /* of W42Furniture */

/* A cell's border, in page-relative pixels, and whose cell it is, so
 * that the view can tell which column edge the pointer is on. */
typedef struct {
  int    page;
  double x, y, w, h;
  int    table;
  int    col;
  gboolean borders;   /* the table is ruled */
  guint8 sides;       /* W42_BORDER_* bits: which of its sides are drawn */
} W42CellRect;

const GArray *w42_layout_cell_rects (W42Layout *self);  /* of W42CellRect */

/* A wrapped picture, placed beside its paragraph. */
typedef struct {
  int          page;
  double       x, y, w, h;
  W42ObjectIdx object;
  gsize        pos;       /* its anchor in the document */
} W42FloatBox;

const GArray *w42_layout_floats (W42Layout *self);      /* of W42FloatBox */

/* Paints the header and footer of one page, and the cell borders of any
 * table on it.  Headers and footers are nothing in Normal view, which has
 * no pages; borders are drawn in both views. */
void  w42_layout_draw_furniture (W42Layout *self, cairo_t *cr, int page);

/* Paragraph shading and borders for one page, drawn before its lines so
 * that the text sits on top.  Every painter calls this before the lines
 * and draw_furniture after them. */
void  w42_layout_draw_backdrop (W42Layout *self, cairo_t *cr, int page);

/* Paints one line -- its section number, if it has one, and its text -- at
 * the line's place on its page.  Every painter word42 has goes through this,
 * so the screen, the printer, the preview and the PDF agree. */
void  w42_layout_draw_line (W42Layout *self, cairo_t *cr, const W42LineBox *box);

/* The shaped paragraphs the layout keeps between passes are let go of:
 * for when the layout is given a different document, whose formatting
 * records are numbered its own way. */
void  w42_layout_forget_shaping (W42Layout *self);

/* What the last pass did: how many paragraphs it reused and how many it
 * had to shape.  For the checks, and for anyone measuring. */
void  w42_layout_shaping_counts (W42Layout *self, guint *reused, guint *shaped);

/* ---- Mapping between document positions and the page ------------------ */

/* Position of a block's text within the document, and back again. */
gsize w42_block_byte_to_pos (const W42Block *block, gsize byte);
gsize w42_block_pos_to_byte (const W42Block *block, gsize pos);

int   w42_layout_block_at_pos (W42Layout *self, gsize pos);

/* Caret rectangle for `pos`, in page-relative pixels.  FALSE if the layout is
 * empty. */
gboolean w42_layout_pos_to_caret (W42Layout *self,
                                  gsize      pos,
                                  int       *page,
                                  double    *x,
                                  double    *y,
                                  double    *height);

/* Nearest caret position to a point given in page-relative pixels. */
gsize w42_layout_point_to_pos (W42Layout *self, int page, double x, double y);

/* The rectangle a picture is shown in, in page coordinates, for the
 * handles.  FALSE when there is no picture at `pos`. */
gboolean w42_layout_object_rect (W42Layout *self, gsize pos, int *page,
                                 double *x, double *y,
                                 double *width, double *height);

/* Up/down arrow: `dir` is -1 or +1.  `want_x` carries the "sticky" column
 * across successive moves the way Word does; pass a negative value to take it
 * from the current position. */
gsize w42_layout_move_line (W42Layout *self, gsize pos, int dir, double *want_x);

gsize w42_layout_line_start (W42Layout *self, gsize pos);
gsize w42_layout_line_end   (W42Layout *self, gsize pos);

/* For the status bar: 1-based page, line-within-page and column. */
void  w42_layout_describe_pos (W42Layout *self,
                               gsize      pos,
                               int       *page,
                               int       *line,
                               int       *column);

G_END_DECLS
