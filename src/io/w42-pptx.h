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
 * back into a document: each slide's title becomes a Heading 1 (a title
 * slide's, the Title style) and its body a run of paragraphs, bulleted
 * as the slide had them, which is what the outline view of a
 * presentation program shows.  Pictures come along both ways, and so do
 * the speaker's notes, as comments on the slide's title.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"

G_BEGIN_DECLS

/* A picture on a slide: the paragraphs under a heading that hold one. */
typedef struct {
  GBytes     *data;
  const char *format;    /* interned: "png", "jpeg", ... */
  int         pixel_w;
  int         pixel_h;
} W42SlidePicture;

/* One slide: a title, the lines under it, and their pictures.
 *
 * A line's level is its list level, or for a paragraph that is not in a
 * list, how far it is indented; a presentation shows that as bullets
 * under bullets.  A slide made from the Title style is a title slide,
 * its lines the subtitle.  The speaker's notes are the comments on the
 * slide's text: Insert > Comment on a heading is a note for the talk. */
typedef struct {
  char      *title;
  GPtrArray *lines;        /* char *, owned */
  GArray    *levels;       /* int per line, 0 outermost */
  GPtrArray *pictures;     /* W42SlidePicture *, owned */
  char      *notes;        /* NULL for none */
  int        level;        /* the heading level the title came from */
  gboolean   title_slide;
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
