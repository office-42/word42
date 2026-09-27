/* w42-pdfsec.h - a PDF's password and its signature
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The password is PDF's standard security handler at its strongest:
 * AES with a 256-bit key, revision 6, the one PDF 2.0 made standard and
 * every reader since Acrobat X opens.  Word 97 offered two passwords,
 * one to open the document and one to modify it, and a PDF has the same
 * two: the user password, without which nothing in the file can be
 * read, and the owner password, without which a reader keeps to the
 * permissions the file sets -- here, printing and copying but no
 * changes.
 *
 * The signature is CMS, detached, over every byte of the file but the
 * signature itself, made with the certificate and private key in a
 * PKCS #12 file (.p12, .pfx): the PAdES baseline signature, with the
 * signing certificate named in the signed attributes as PAdES asks.
 *
 * Both go through GnuTLS.  A build without it writes PDFs with neither,
 * and says so when asked for one.
 */

#pragma once

#include <gio/gio.h>

#include "w42-pdffile.h"

G_BEGIN_DECLS

/* TRUE when Word42 was built with GnuTLS and can do either. */
gboolean      w42_pdf_security_available (void);

typedef struct _W42PdfCrypt W42PdfCrypt;

/* A key and its /Encrypt dictionary.  Either password may be NULL or
 * empty, but not both.  With a password to modify the file allows
 * printing and copying and no changes; without one, everything. */
W42PdfCrypt  *w42_pdf_crypt_new     (const char  *open_password,
                                     const char  *modify_password,
                                     GError     **error);
/* The /Encrypt dictionary, made afresh for each call. */
W42PdfObj    *w42_pdf_crypt_dict    (W42PdfCrypt *crypt);
/* A W42PdfEncryptFunc, with the W42PdfCrypt as its data. */
GBytes       *w42_pdf_crypt_encrypt (gpointer      crypt,
                                     const guint8 *data,
                                     gsize         len);
void          w42_pdf_crypt_free    (W42PdfCrypt *crypt);

typedef struct _W42PdfSigner W42PdfSigner;

/* Reads the certificate, its chain and the private key from a PKCS #12
 * file, and checks that the certificate may sign today. */
W42PdfSigner *w42_pdf_signer_new     (const char  *path,
                                      const char  *password,
                                      GError     **error);
/* The certificate's common name: who signed. */
const char   *w42_pdf_signer_name    (W42PdfSigner *signer);
/* How many bytes of signature to leave room for. */
gsize         w42_pdf_signer_reserve (W42PdfSigner *signer);
/* The CMS signature, DER, over `file` less the bytes from `hole_start`
 * to `hole_end`: the placeholder the signature goes in. */
GBytes       *w42_pdf_signer_sign    (W42PdfSigner *signer,
                                      GBytes       *file,
                                      gsize         hole_start,
                                      gsize         hole_end,
                                      GError      **error);
void          w42_pdf_signer_free    (W42PdfSigner *signer);

G_END_DECLS
