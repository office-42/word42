/* w42-image.h - decoding pictures, and handing them to cairo
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * gdk-pixbuf does the decoding, which gives word42 every format it has a
 * loader for -- PNG, JPEG, GIF, BMP, TIFF, WebP, AVIF, SVG and the rest --
 * without word42 knowing anything about any of them.  The bytes of the file
 * are what the document keeps; the decoded picture is a cache.
 *
 * This lives in util/ rather than ui/ because the layout engine draws
 * pictures too, and the layout engine does not link GTK.  gdk-pixbuf is not
 * GTK.
 */

#pragma once

#include <cairo.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Reads the picture's size without keeping the decoded pixels.  `format`
 * receives gdk-pixbuf's name for the format ("png", "jpeg"...), interned. */
gboolean w42_image_probe (GBytes      *data,
                          int         *width,
                          int         *height,
                          const char **format);

/* Loads a file and probes it in one go. */
GBytes  *w42_image_load_file (GFile       *file,
                              int         *width,
                              int         *height,
                              const char **format,
                              GError     **error);

/* Decodes to a cairo surface, premultiplied ARGB32.  NULL if the bytes are
 * not a picture. */
cairo_surface_t *w42_image_surface (GBytes *data);

/* Re-encodes as PNG, for putting into formats that cannot carry the
 * original.  Returns NULL on failure. */
GBytes  *w42_image_to_png (GBytes *data);

/* The bytes to put in a container that can hold more than PNG -- a .docx or
 * an .odt.  A picture in a format the container carries is handed back
 * untouched, so that a JPEG stays the JPEG it was rather than trebling in
 * size as a PNG; anything else is re-encoded.  `ext` and `mime` receive the
 * file extension and the MIME type to file it under, both static.  The
 * caller owns the bytes.  NULL if the picture cannot be read at all. */
GBytes  *w42_image_for_container (GBytes      *data,
                                  const char **ext,
                                  const char **mime);

/* Encodes a cairo image surface as PNG, for pictures pulled out of a PDF. */
GBytes  *w42_image_surface_to_png (cairo_surface_t *surface);

G_END_DECLS
