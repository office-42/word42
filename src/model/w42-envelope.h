/* w42-envelope.h - envelopes and sheets of labels
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 6's Tools menu made an envelope out of an address and a sheet of
 * labels out of a line of text.  These make the same, as documents:
 * an envelope is a page of the envelope's size with the two addresses
 * on it, and a sheet of labels is a table with a cell per label and no
 * rules.  The sizes are named by what they measure rather than by any
 * maker's catalogue number.
 */

#pragma once

#include "w42-piecetable.h"

G_BEGIN_DECLS

typedef struct {
  const char *name;     /* "Envelope #10 (9 1/2 x 4 1/8 in)" */
  int         width;    /* twips, the long way: an envelope is fed sideways */
  int         height;
} W42EnvelopeSize;

typedef struct {
  const char *name;     /* "24 per sheet (63.5 x 33.9 mm)" */
  int         page_width;
  int         page_height;
  int         across;   /* labels per row */
  int         down;     /* rows of labels */
  int         label_width;
  int         label_height;
  int         margin_left;
  int         margin_top;
} W42LabelSheet;

const W42EnvelopeSize *w42_envelope_sizes (int *n);
const W42LabelSheet   *w42_label_sheets   (int *n);

/* Makes an envelope of `size` in `pt`, and says in `page` what paper it
 * wants.  Either address may be NULL.  Whatever was in `pt` is
 * replaced. */
void w42_envelope_make (W42PieceTable *pt, W42PageSetup *page, int size,
                        const char *delivery, const char *sender);

/* And a sheet of labels: `text` in every one, or in the first only when
 * `same` is FALSE.  Whatever was in `pt` is replaced. */
void w42_labels_make (W42PieceTable *pt, W42PageSetup *page, int sheet,
                      const char *text, gboolean same);

G_END_DECLS
