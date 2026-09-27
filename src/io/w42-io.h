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

#include "w42-pdf.h"
#include "w42-piecetable.h"

G_BEGIN_DECLS

typedef enum {
  W42_FORMAT_UNKNOWN = 0,
  W42_FORMAT_TEXT,
  W42_FORMAT_RTF,
  W42_FORMAT_PDF,      /* written always, with the document inside it;
                        * read when built with poppler */
  W42_FORMAT_DOC,      /* Word 97's .doc, the default for Save */
  W42_FORMAT_HTML,     /* read and written */
  W42_FORMAT_DOCX,     /* .docx, Word's XML format; read and written */
  W42_FORMAT_ABW,      /* AbiWord, plain or gzipped; read and written */
  W42_FORMAT_ODT,      /* OpenDocument text; read and written */
  W42_FORMAT_PPTX,     /* slides: PowerPoint's presentation, read and written */
  W42_FORMAT_EPUB,     /* an e-book; written only */
  W42_FORMAT_WPD,      /* WordPerfect: 5 and 6 on read, 6 written */
  W42_FORMAT_LATEX     /* LaTeX source; written only */
} W42Format;

W42Format w42_io_guess_format (GFile *file);

/* Whether a document read from `file` and written back to it comes out
 * as it went in.  A web page, a presentation and an e-book are written
 * as a rendering of the document, which reading back does not undo --
 * an e-book is not read back at all.  A document from one of those is
 * saved somewhere else, and one saved to one of those has been
 * exported.  A PDF round trips: Word42 saves the document inside the
 * pages it writes, and reads that back.  One from anywhere else is read
 * from its pages, and the first save makes it one of Word42's. */
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

/* The same for a PDF with a password, compressed or signed: `pdf` says
 * what a PDF is to be opened with and receives what was found in it,
 * or says how one is written.  NULL is the defaults -- a PDF saved with
 * the document inside it, compressed, nothing else. */
gboolean  w42_io_load_with (W42PieceTable *pt, W42PageSetup *page,
                            GFile *file, W42PdfOptions *pdf, GError **error);
gboolean  w42_io_save_with (W42PieceTable *pt, const W42PageSetup *page,
                            GFile *file, const W42PdfOptions *pdf,
                            GError **error);

G_END_DECLS
