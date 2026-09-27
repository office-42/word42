/* w42-document.h - a piece table plus the things a window needs to know
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

#include "w42-pdf.h"
#include "w42-piecetable.h"

G_BEGIN_DECLS

#define W42_TYPE_DOCUMENT (w42_document_get_type ())
G_DECLARE_FINAL_TYPE (W42Document, w42_document, W42, DOCUMENT, GObject)

W42Document   *w42_document_new        (void);

W42PieceTable *w42_document_pt         (W42Document *self);
W42ApTable    *w42_document_ap_table   (W42Document *self);
const W42PageSetup *w42_document_page_setup (W42Document *self);
void  w42_document_set_page_setup (W42Document *self, const W42PageSetup *page);

/* Emits ::changed, which is what makes the view re-lay-out and repaint. */
void      w42_document_touch           (W42Document *self);

gboolean  w42_document_get_modified    (W42Document *self);
void      w42_document_set_modified    (W42Document *self, gboolean modified);
void      w42_document_mark_unsaved    (W42Document *self);
/* Whether the undo history stands where it stood when the document was
 * last marked clean: undo has taken it back to the saved text. */
gboolean  w42_document_at_saved_state  (W42Document *self);

GFile    *w42_document_get_file        (W42Document *self);
void      w42_document_set_file        (W42Document *self, GFile *file);
/* "Document1" for an unsaved document, otherwise the basename. */
char     *w42_document_get_title       (W42Document *self);
/* The name an unsaved document goes by instead of "Document1", until
 * it has a file: a version opened from File > Versions is named after
 * the document it is a version of.  NULL goes back to "Document1". */
void      w42_document_set_title       (W42Document *self, const char *title);

gboolean  w42_document_load            (W42Document *self, GFile *file, GError **error);
gboolean  w42_document_save            (W42Document *self, GFile *file, GError **error);

/* As w42_document_load, trying the passwords when the file is a PDF that
 * has them; either may be NULL.  A password missing or wrong is
 * W42_PDF_ERROR_PASSWORD. */
gboolean  w42_document_load_with       (W42Document *self, GFile *file,
                                        const char *password,
                                        const char *modify_password,
                                        GError **error);

/* How the document is written when it is written as a PDF -- its
 * passwords, compression, signature -- and what reading one found.
 * Owned by the document, and never NULL.  Loading a file keeps the
 * settings that are the writer's taste and clears what belonged to the
 * last file: its passwords, and the signing. */
W42PdfOptions *w42_document_pdf_options (W42Document *self);

G_END_DECLS
