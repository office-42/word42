/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Table AutoFormat: a handful of ready-made looks for a table, and the
 * one call that puts one of them on.  The looks are Word42's own -- a
 * plain one, ruled ones, a banded one -- not any other program's. */
#pragma once

#include "w42-piecetable.h"

G_BEGIN_DECLS

/* How a format rules its cells. */
typedef enum {
  W42_TF_RULES_NONE = 0,   /* nothing ruled */
  W42_TF_RULES_GRID,       /* every cell */
  W42_TF_RULES_BOX         /* round the table, and under the heading row */
} W42TfRules;

typedef struct {
  const char *name;
  const char *hint;             /* one line, for the dialog */
  guint8      rules;            /* W42TfRules */
  guint8      head_shading;     /* percent of black behind the first row */
  guint8      band_shading;     /* and behind every other row after it */
  guint8      head_bold;
  guint8      head_italic;
  guint8      first_col_bold;
} W42TableFormat;

/* The formats, in the order the dialog lists them. */
const W42TableFormat *w42_table_formats (int *n);

/* Puts `fmt` on the table, as one undo step.  `heading` and
 * `first_column` are the dialog's switches: with them off the parts of
 * the format that single out the first row or column are left out.
 * FALSE when there is no such table. */
gboolean w42_pt_table_autoformat (W42PieceTable        *pt,
                                  int                   table,
                                  const W42TableFormat *fmt,
                                  gboolean              heading,
                                  gboolean              first_column);

G_END_DECLS
