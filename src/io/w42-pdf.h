/* w42-pdf.h - writing a document out as PDF, and reading one in
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Export goes through cairo's PDF surface with the same layout engine and
 * the same painting code the screen and the printer use, so the pages come
 * out where the screen said they would.  What cairo wrote is then read
 * back as objects (w42-pdffile.h) and finished: packed into object
 * streams, the editable document put inside it when it is to be the
 * document's own file, encrypted, signed.
 *
 * Import reads a PDF Word42 saved as the document it keeps inside, as it
 * was; any other goes through poppler, when it was available at build
 * time.  A PDF is a picture of a document rather than the document, so
 * what comes back from one is the text and the pictures, with the
 * paragraphs, their alignment, the headings and the links guessed from
 * where everything sits, which is all any program can do with one.
 */

#pragma once

#include <gio/gio.h>

#include "w42-piecetable.h"

G_BEGIN_DECLS

#define W42_PDF_ERROR (w42_pdf_error_quark ())
GQuark w42_pdf_error_quark (void);

typedef enum {
  /* The PDF needs a password to be opened, or the one given is wrong. */
  W42_PDF_ERROR_PASSWORD
} W42PdfError;

/* The picture resolutions Word offered to compress pictures to: for
 * printing, for a screen, for sending by e-mail.  0 keeps them. */
#define W42_PDF_PPI_PRINT  220
#define W42_PDF_PPI_SCREEN 150
#define W42_PDF_PPI_EMAIL   96

typedef struct {
  /* How a PDF is written. */
  char     *open_password;       /* Word 97's Password to open: the user
                                  * password, without which nothing in the
                                  * file can be read */
  char     *modify_password;     /* Password to modify: the owner password,
                                  * without which a reader allows no changes */
  gboolean  keep_document;       /* the document goes inside, as .odt, so
                                  * that Word42 opens the PDF as it was */
  gboolean  compress;            /* object streams, everything deflated as
                                  * far as it goes, copies written once */
  int       picture_ppi;         /* pictures at most this many pixels to
                                  * the inch; 0 keeps them as they are */
  gboolean  sign;                /* signed with the certificate below */
  char     *certificate;         /* a PKCS #12 file: .p12, .pfx */
  char     *certificate_password;
  char     *reason;              /* why it was signed, where, and how to
                                  * reach the signer; each may be NULL */
  char     *location;
  char     *contact;

  /* What reading a PDF found. */
  gboolean  read_source;         /* it was Word42's, read as it was saved */
  gboolean  source_stale;        /* ... though something else has changed
                                  * the PDF since */
  gboolean  restricted;          /* it allows no changes without its
                                  * password to modify */
  int       n_signatures;        /* signatures saving it again will void */
} W42PdfOptions;

/* Compressed, nothing else. */
W42PdfOptions *w42_pdf_options_new  (void);
W42PdfOptions *w42_pdf_options_copy (const W42PdfOptions *options);
/* Wipes the passwords before the memory goes back. */
void           w42_pdf_options_free (W42PdfOptions *options);
void           w42_pdf_options_set  (char **field, const char *value);

/* Writes the document as a PDF with `options`, or as File > Print to
 * file does when they are NULL: compressed, and nothing else. */
gboolean w42_pdf_export_with (W42PieceTable       *pt,
                              const W42PageSetup  *page,
                              GFile               *file,
                              const W42PdfOptions *options,
                              GError             **error);
gboolean w42_pdf_export      (W42PieceTable      *pt,
                              const W42PageSetup *page,
                              GFile              *file,
                              GError            **error);

/* TRUE when word42 was built with poppler and can read PDF at all. */
gboolean w42_pdf_import_available (void);

/* What opening `file` with `password` (NULL for none) would meet, found
 * without reading a page of it: W42_PDF_ERROR_PASSWORD when the password
 * is missing or wrong, and otherwise whether it allows no changes
 * without its password to modify.  Asked before a window gives itself to
 * the file, so that it can ask for the passwords first. */
gboolean w42_pdf_probe (GFile      *file,
                        const char *password,
                        gboolean   *restricted,
                        GError    **error);

/* Reads `file`, trying `options->open_password` when it asks for one
 * (options may be NULL), and fills in what it found.  A password that
 * is missing or wrong is W42_PDF_ERROR_PASSWORD. */
gboolean w42_pdf_import_with (W42PieceTable *pt,
                              W42PageSetup  *page,
                              GFile         *file,
                              W42PdfOptions *options,
                              GError       **error);
gboolean w42_pdf_import      (W42PieceTable *pt,
                              W42PageSetup  *page,
                              GFile         *file,
                              GError       **error);

G_END_DECLS
