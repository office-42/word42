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
 * emphasis, figures and their captions, the table of contents -- and
 * LaTeX decides the typography.
 *
 * Mathematics is written in the text as LaTeX writes it, and goes over
 * as mathematics: $...$ and \(...\) in the line, $$...$$ and \[...\]
 * displayed, and the amsmath environments -- equation, align, gather and
 * their kin -- whole.  A dollar sign is taken to open mathematics only
 * with no space after it, and to close it only with no space before it
 * and no digit after, as Pandoc takes it, so that "$5 to $10" stays
 * money.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"

G_BEGIN_DECLS

/* Writes `file`, and each picture as "<name>-fig<N>.<ext>" in the same
 * folder. */
gboolean w42_latex_export (W42PieceTable *pt, const W42PageSetup *page,
                           GFile *file, GError **error);

/* Where a stretch of the document's text went in the source: a position
 * in the document, and the line of the .tex its text starts on.  The
 * source breaks a long paragraph over several lines, at spaces, so that
 * the lines SyncTeX follows through to the PDF narrow down to a part of
 * a paragraph rather than the whole of it. */
typedef struct {
  gsize pos;
  guint line;
} W42LatexAnchor;

/* As w42_latex_export, and *anchors -- when `anchors` is not NULL -- is
 * given the anchors, in the order of their positions.  g_array_unref. */
gboolean w42_latex_export_anchored (W42PieceTable *pt, const W42PageSetup *page,
                                    GFile *file, GArray **anchors, GError **error);

G_END_DECLS
