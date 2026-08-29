/* w42-rtf.h - Rich Text Format, read and written
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RTF is the first format worth having after plain text: it is documented,
 * it is text, and it expresses everything word42's model holds -- character
 * runs, paragraph properties, fonts and colours.  Word and AbiWord both read
 * and write it, so it is also the shortest route to exchanging documents
 * with either.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"

G_BEGIN_DECLS

gboolean w42_rtf_save (W42PieceTable      *pt,
                       const W42PageSetup *page,
                       GFile              *file,
                       GError            **error);

/* Reads `file` into `pt`, replacing whatever was there.  Page geometry found
 * in the file is written to `page` when it is not NULL. */
gboolean w42_rtf_load (W42PieceTable *pt,
                       W42PageSetup  *page,
                       GFile         *file,
                       GError       **error);

G_END_DECLS
