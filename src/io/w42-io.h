/* w42-io.h - reading and writing documents
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Plain text and RTF.  The importer/exporter pair is kept behind this narrow
 * interface so that Word 97 .doc -- the OLE2 compound file with its FIB, its
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
  W42_FORMAT_DOC,      /* Word 97's .doc; read only */
  W42_FORMAT_HTML,     /* read and written */
  W42_FORMAT_DOCX,     /* .docx, Word's XML format; read and written */
  W42_FORMAT_ABW,      /* AbiWord, plain or gzipped; read and written */
  W42_FORMAT_ODT,      /* OpenDocument text; read and written */
  W42_FORMAT_PPTX      /* slides: PowerPoint's presentation, read and written */
} W42Format;

W42Format w42_io_guess_format (GFile *file);

/* Whether a document read from `file` and written back to it comes out
 * as it went in.  Word 97 .doc is not written at all; a PDF, a web page
 * and a presentation are written as a rendering of the document, which
 * reading back does not undo.  A document from one of those is saved
 * somewhere else, and one saved to one of those has been exported. */
gboolean  w42_io_format_round_trips (GFile *file);

/* Clamps a page setup to what can be laid out; every reader's result
 * goes through it, and a dialog may use it too. */
void      w42_page_setup_sanitize (W42PageSetup *page);

/* `page` carries the document's geometry: RTF records it, plain text has
 * nowhere to put it.  Either may be NULL. */
gboolean  w42_io_load (W42PieceTable *pt, W42PageSetup *page,
                       GFile *file, GError **error);
gboolean  w42_io_save (W42PieceTable *pt, const W42PageSetup *page,
                       GFile *file, GError **error);

G_END_DECLS
