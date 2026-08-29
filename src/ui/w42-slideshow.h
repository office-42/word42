/* w42-slideshow.h - View > Slide Show: the document's outline, presented
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A talk is a document read aloud.  This shows one heading and the lines
 * under it at a time, on the whole screen, in type large enough for a
 * room: the space bar or the arrow keys move on, Escape ends it.
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-view.h"

G_BEGIN_DECLS

/* Opens the show on the slide the caret is in. */
void w42_slideshow_show (GtkWindow *parent, W42View *view);

G_END_DECLS
