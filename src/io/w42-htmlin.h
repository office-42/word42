/* w42-htmlin.h - reading a web page as a document
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A plain reading of HTML: headings, paragraphs, line breaks, bold, italic,
 * underline, strikeout, superscript and subscript, links, bulleted and
 * numbered lists, tables, pictures given as data URIs, and the little of
 * inline style that maps on to what word42 has -- font, size, colour,
 * alignment.  Scripts and stylesheets are skipped; everything else that is
 * not understood is read as its text.  No DOM, no CSS engine: enough to
 * bring in what word42 itself wrote, and most pages that are text.
 */

#pragma once

#include <gio/gio.h>

#include "w42-document.h"
#include "w42-piecetable.h"

G_BEGIN_DECLS

gboolean w42_html_import (W42PieceTable *pt,
                          W42PageSetup  *page,
                          GFile         *file,
                          GError       **error);

G_END_DECLS
