/* w42-abw.h - AbiWord documents: .abw and gzipped .zabw, read and written
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AbiWord's XML: sections of paragraphs of character runs, formatting in
 * CSS-like "props" attributes, lists and styles in tables at the top,
 * pictures base64 in a data section at the end.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"
#include "w42-types.h"

G_BEGIN_DECLS

gboolean w42_abw_load (W42PieceTable *pt, W42PageSetup *page,
                       GFile *file, GError **error);
gboolean w42_abw_save (W42PieceTable *pt, const W42PageSetup *page,
                       GFile *file, GError **error);

G_END_DECLS
