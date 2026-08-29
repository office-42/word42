/* w42-odt.h - OpenDocument text: .odt, read and written
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The zip holds a mimetype, content.xml, styles.xml, the pictures and a
 * manifest.  Formatting lives in named and automatic styles that the text
 * refers to by name; the reader resolves the parent chain into word42's
 * own formats, the writer makes one automatic style per distinct format.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"
#include "w42-types.h"

G_BEGIN_DECLS

gboolean w42_odt_load (W42PieceTable *pt, W42PageSetup *page,
                       GFile *file, GError **error);
gboolean w42_odt_save (W42PieceTable *pt, const W42PageSetup *page,
                       GFile *file, GError **error);

G_END_DECLS
