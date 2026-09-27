/* w42-latex.h - writing documents as LaTeX
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LaTeX sets type as scientific papers and theses are set: Latin Modern,
 * justified with hyphenation, sections numbered.  This writes a document
 * as a LaTeX source that any engine compiles -- pdfLaTeX, XeLaTeX,
 * LuaLaTeX or Tectonic -- with its pictures as files beside it.  The
 * document's structure goes over -- headings, lists, tables, notes,
 * emphasis -- and LaTeX decides the typography.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"

G_BEGIN_DECLS

/* Writes `file`, and each picture as "<name>-fig<N>.<ext>" in the same
 * folder. */
gboolean w42_latex_export (W42PieceTable *pt, const W42PageSetup *page,
                           GFile *file, GError **error);

G_END_DECLS
