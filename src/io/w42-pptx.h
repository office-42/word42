/* w42-pptx.h - slides: PowerPoint's .pptx, read and written
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A word processor is not a presentation program, and Word42 does not
 * pretend to be one.  What it has is an outline: a document's headings and
 * the paragraphs under them are a talk waiting to be given.  So a slide
 * here is a heading and the lines that follow it, and that is what is
 * written to and read from a presentation file.
 *
 * Writing turns the document's outline into slides.  Reading turns a deck
 * back into a document: each slide's title becomes a Heading 1 and its
 * body a run of paragraphs, which is what the outline view of a
 * presentation program shows.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"

G_BEGIN_DECLS

/* One slide: a title and the lines under it. */
typedef struct {
  char   *title;
  GPtrArray *lines;      /* char *, owned */
  int     level;         /* the heading level the title came from */
} W42Slide;

/* The document's outline as slides.  Free with w42_slides_free. */
GPtrArray *w42_slides_from_document (W42PieceTable *pt);
void       w42_slides_free (GPtrArray *slides);

gboolean w42_pptx_save (W42PieceTable      *pt,
                        const W42PageSetup *page,
                        GFile              *file,
                        GError            **error);

gboolean w42_pptx_load (W42PieceTable *pt,
                        W42PageSetup  *page,
                        GFile         *file,
                        GError       **error);

G_END_DECLS
