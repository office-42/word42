/* w42-html.h - writing a document as a web page
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One self-contained HTML file: headings by their style, paragraphs with
 * their alignment and indents, runs with their font, size, weight, slant,
 * underline, colour and links, bullets and numbers as lists, tables as
 * tables, pictures embedded as data URIs, footnotes as superscript links to
 * the notes at the end.  Nothing reads HTML back: it is a way out, as PDF
 * is.
 */

#pragma once

#include <gio/gio.h>

#include "w42-document.h"
#include "w42-piecetable.h"

G_BEGIN_DECLS

gboolean w42_html_export (W42PieceTable      *pt,
                          const W42PageSetup *page,
                          GFile              *file,
                          GError            **error);

G_END_DECLS
