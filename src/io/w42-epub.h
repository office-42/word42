/* w42-epub.h - writing a document as an e-book
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * EPUB 3: a zip holding XHTML, one file per chapter, a stylesheet made
 * from the document's own body and heading formatting, the pictures as
 * files of their own, and the package document, navigation document and
 * NCX that tell a reader what is in it and in what order.  An e-book
 * reader reflows the text to its screen and the reader's chosen type, so
 * what is written is what the document is -- headings, paragraphs,
 * emphasis, lists, tables, pictures, notes -- rather than what its pages
 * look like.  Nothing reads EPUB back: it is a way out, as PDF is.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"
#include "w42-types.h"

G_BEGIN_DECLS

/* Writes the document as an EPUB 3 e-book: a new chapter file at every
 * top-level heading, the text before the first heading in a file of its
 * own, footnotes and endnotes at the end of the chapter that refers to
 * them, and a table of contents from the headings.  The title, author,
 * subject and comments of File > Summary Info become the book's
 * metadata.  `page` gives the width pictures are measured against; it
 * may be NULL. */
gboolean w42_epub_export (W42PieceTable      *pt,
                          const W42PageSetup *page,
                          GFile              *file,
                          GError            **error);

G_END_DECLS
