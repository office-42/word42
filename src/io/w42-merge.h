/* w42-merge.h - mail merge: one copy of the document per row of a table
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The data source is a CSV file whose first row names the fields.  A field
 * stands in the document as its name between chevrons, «Name», which Word 6
 * showed for its MERGEFIELD.  Merging makes a document with one copy of the
 * main document per row, each field replaced by the row's value, the copies
 * on pages of their own.
 */

#pragma once

#include <gio/gio.h>

#include "w42-document.h"
#include "w42-piecetable.h"

G_BEGIN_DECLS

typedef struct {
  char  **fields;      /* the header row */
  GPtrArray *rows;     /* char** per row, one string per field */
} W42MergeSource;

W42MergeSource *w42_merge_source_load (GFile *file, GError **error);
void            w42_merge_source_free (W42MergeSource *source);

/* The text a field is written as in the document. */
char *w42_merge_field_text (const char *name);

/* Writes the merged copies as RTF to `out`, one per row, page breaks
 * between.  The main document is read from `pt`. */
gboolean w42_merge_to_file (W42PieceTable        *pt,
                            const W42PageSetup   *page,
                            const W42MergeSource *source,
                            GFile                *out,
                            GError              **error);

G_END_DECLS
