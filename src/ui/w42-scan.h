/* w42-scan.h - Insert > Picture > From Scanner or Camera
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word XP asked the scanner through Windows Image Acquisition and put
 * what came back into the text as a picture.  word42 does the same: on
 * Windows through WIA's own dialog, which lists the scanners and cameras
 * and drives the one chosen; on Linux through SANE's scanimage; and on
 * macOS not at all, yet.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* TRUE when this build can ask a scanner for anything. */
gboolean w42_scan_available (void);

/* Runs the scanner's dialog and returns the picture as a file's bytes
 * (PNG or JPEG), with its format name interned; NULL when the user gave
 * it up, or with `error` set when it could not be done.  Blocks while
 * the scanner works, as Word did. */
GBytes  *w42_scan_acquire (GtkWindow *parent, const char **format, GError **error);

G_END_DECLS
