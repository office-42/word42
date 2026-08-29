/* w42-zip.h - the zip container .docx lives in
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Enough of PKZIP for Office files: stored and deflated entries, read by
 * the central directory and written with GLib's raw deflate.  No zip64,
 * no encryption, no spanning.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _W42Zip W42Zip;
typedef struct _W42ZipWriter W42ZipWriter;

/* Reading.  The archive is loaded whole. */
W42Zip *w42_zip_open  (GFile *file, GError **error);
W42Zip *w42_zip_new_from_bytes (GBytes *bytes, GError **error);
void    w42_zip_free  (W42Zip *zip);
gboolean w42_zip_has  (W42Zip *zip, const char *name);
/* The entry's contents, inflated; NULL if absent or broken. */
GBytes *w42_zip_read  (W42Zip *zip, const char *name);

/* Writing.  Entries go in the order added; each is deflated unless that
 * makes it bigger. */
W42ZipWriter *w42_zip_writer_new  (void);
void          w42_zip_writer_add  (W42ZipWriter *writer, const char *name,
                                   const void *data, gsize length);
gboolean      w42_zip_writer_save (W42ZipWriter *writer, GFile *file, GError **error);
void          w42_zip_writer_free (W42ZipWriter *writer);

G_END_DECLS
