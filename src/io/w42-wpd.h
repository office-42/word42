/* w42-wpd.h - reading and writing WordPerfect documents
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WordPerfect 6 and every version since (.wpd) keep a document as a
 * packet index -- fonts, the text of headers and notes, the summary --
 * and a stream of characters and function codes.  This reads that, and
 * the WordPerfect 5.x files before it for their text and character
 * formatting, and writes WordPerfect 6.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"

G_BEGIN_DECLS

/* Whether `data` begins as a WordPerfect 5 or 6 document does, whatever
 * the file is called: DOS WordPerfect saved as .doc as often as not. */
gboolean w42_wpd_sniff (const guint8 *data, gsize len);

gboolean w42_wpd_load (W42PieceTable *pt, W42PageSetup *page,
                       GFile *file, GError **error);
gboolean w42_wpd_save (W42PieceTable *pt, const W42PageSetup *page,
                       GFile *file, GError **error);

G_END_DECLS
