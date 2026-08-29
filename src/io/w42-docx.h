/* w42-docx.h - Word 2007 and later: .docx, read and written
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WordprocessingML in a zip: document.xml with its styles, numbering,
 * footnotes, headers and footers, relationships and media.  Read with
 * GMarkup, written by hand.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"
#include "w42-types.h"

G_BEGIN_DECLS

gboolean w42_docx_load (W42PieceTable *pt, W42PageSetup *page,
                        GFile *file, GError **error);
gboolean w42_docx_save (W42PieceTable *pt, const W42PageSetup *page,
                        GFile *file, GError **error);

G_END_DECLS
