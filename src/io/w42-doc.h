/* w42-doc.h - reading Word .doc files
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 97 through 2003 wrote a .doc as an OLE2 compound file holding a
 * WordDocument stream and a Table stream.  Inside are a File Information
 * Block, a piece table much like word42's own, "formatted disk pages" of
 * character and paragraph properties, and a stylesheet.  This reads text,
 * paragraph and character formatting, styles by their built-in identity,
 * tables, PNG and JPEG pictures, the first section's page setup and its
 * header and footer, and footnotes.  Fields keep their result.  A Word 6 or 95 file, whose insides are
 * laid out differently, yields its text and paragraphs and nothing more.
 * Nothing here writes .doc; RTF is the way out.
 */

#pragma once

#include <gio/gio.h>

#include "w42-document.h"
#include "w42-piecetable.h"

G_BEGIN_DECLS

gboolean w42_doc_load (W42PieceTable *pt,
                       W42PageSetup  *page,
                       GFile         *file,
                       GError       **error);

G_END_DECLS
