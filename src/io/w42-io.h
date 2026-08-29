/* w42-io.h - reading and writing documents
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Plain text and RTF.  The importer/exporter pair is kept behind this narrow
 * interface so that Word 6 .doc -- the OLE2 compound file with its FIB, its
 * own piece table and its character/paragraph property bins -- can be added
 * as a further backend without the model noticing.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"

G_BEGIN_DECLS

typedef enum {
  W42_FORMAT_UNKNOWN = 0,
  W42_FORMAT_TEXT,
  W42_FORMAT_RTF,
  W42_FORMAT_PDF,      /* written always; read when built with poppler */
  W42_FORMAT_DOC,      /* Word 97-2003; read only */
  W42_FORMAT_HTML,     /* read and written */
  W42_FORMAT_DOCX,     /* Word 2007 and later; read and written */
  W42_FORMAT_ABW,      /* AbiWord, plain or gzipped; read and written */
  W42_FORMAT_ODT,      /* OpenDocument text; read and written */
  W42_FORMAT_PPTX      /* slides: PowerPoint's presentation, read and written */
} W42Format;

W42Format w42_io_guess_format (GFile *file);

/* `page` carries the document's geometry: RTF records it, plain text has
 * nowhere to put it.  Either may be NULL. */
gboolean  w42_io_load (W42PieceTable *pt, W42PageSetup *page,
                       GFile *file, GError **error);
gboolean  w42_io_save (W42PieceTable *pt, const W42PageSetup *page,
                       GFile *file, GError **error);

G_END_DECLS
