/* w42-compare.h - Tools > Track Changes > Compare Documents
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 97 compared the open document with an earlier version of it and
 * marked the differences as changes: what the open document has that the
 * earlier one had not is shown inserted, and what the earlier one had is
 * put back where it stood, shown deleted.  The result is the document as
 * it would look had every change since been made with Highlight Changes
 * on, and Accept All and Reject All work on it as on any other.
 */

#pragma once

#include "w42-piecetable.h"

G_BEGIN_DECLS

/* Marks in `pt` how it differs from `original`, as one undo step.
 * Returns how many changes were marked; 0 when the two say the same. */
int w42_pt_compare (W42PieceTable *pt, W42PieceTable *original);

G_END_DECLS
