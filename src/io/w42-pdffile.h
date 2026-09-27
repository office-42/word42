/* w42-pdffile.h - a PDF's objects, read back and written out again
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cairo writes a PDF that says what the pages look like, and says it
 * well: the fonts subset, the pictures, the outline.  What it has no way
 * to say is everything else a PDF may carry -- a file inside it, a
 * password, a signature -- and it packs the file as tightly as its own
 * version knows how.  This reads cairo's PDF back as objects, lets those
 * things be added, and writes the whole of it out again: compressed into
 * object streams or laid out plainly, encrypted or not, with room held
 * open for a signature that can only be made once the rest of the file
 * is final.
 *
 * It reads the PDFs Word42 itself has just made.  The parser handles
 * what any writer may use -- a classic cross-reference table or a
 * compressed one, object streams, lengths given as references -- and
 * checks every bound, but a PDF from anywhere else is read by poppler.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum {
  W42_PDF_NULL = 0,
  W42_PDF_BOOL,
  W42_PDF_INT,
  W42_PDF_REAL,
  W42_PDF_STRING,
  W42_PDF_NAME,
  W42_PDF_ARRAY,
  W42_PDF_DICT,
  W42_PDF_REF,
  W42_PDF_STREAM,
  W42_PDF_RAW         /* text written as it is, and where it went noted:
                       * a signature's placeholders */
} W42PdfKind;

typedef struct _W42PdfObj W42PdfObj;

struct _W42PdfObj {
  W42PdfKind kind;
  union {
    gboolean  b;
    gint64    i;
    char     *real;                          /* as written: PDF has no exponents */
    struct { guint8 *data; gsize len; } s;   /* a string's bytes, a name's text */
    GPtrArray *array;                        /* of W42PdfObj */
    GPtrArray *dict;                         /* key, value, key, value... */
    struct { guint num, gen; } ref;
    struct { W42PdfObj *dict; GBytes *data; } stream;   /* data as filtered */
    struct { char *text; int tag; } raw;
  } v;
};

W42PdfObj  *w42_pdf_null   (void);
W42PdfObj  *w42_pdf_bool   (gboolean b);
W42PdfObj  *w42_pdf_int    (gint64 i);
W42PdfObj  *w42_pdf_real   (double r);
W42PdfObj  *w42_pdf_name   (const char *name);
W42PdfObj  *w42_pdf_string (const void *data, gsize len);
/* A text string: ASCII as it is, anything else as UTF-16 with its mark. */
W42PdfObj  *w42_pdf_text   (const char *utf8);
/* A date as PDF writes one: D:20260927143000+02'00'. */
W42PdfObj  *w42_pdf_date   (GDateTime *when);
W42PdfObj  *w42_pdf_ref    (guint num);
W42PdfObj  *w42_pdf_raw    (const char *text, int tag);
W42PdfObj  *w42_pdf_array  (void);
W42PdfObj  *w42_pdf_dict   (void);
/* Takes both; `data` is the stream's bytes as its /Filter leaves them. */
W42PdfObj  *w42_pdf_stream (W42PdfObj *dict, GBytes *data);
void        w42_pdf_obj_free (W42PdfObj *obj);

void        w42_pdf_array_add (W42PdfObj *array, W42PdfObj *item);
/* Takes `value`, replacing what the key had; NULL takes the key out. */
void        w42_pdf_dict_set  (W42PdfObj *dict, const char *key, W42PdfObj *value);
/* The value as it stands, a reference not followed; NULL when absent.
 * A stream is looked up in its dictionary. */
W42PdfObj  *w42_pdf_dict_get  (W42PdfObj *dict, const char *key);
gboolean    w42_pdf_is_name   (W42PdfObj *obj, const char *name);

typedef struct _W42PdfFile W42PdfFile;

W42PdfFile *w42_pdf_file_parse (GBytes *bytes, GError **error);
void        w42_pdf_file_free  (W42PdfFile *file);

/* The trailer's dictionary: /Root, /Info and what goes with them. */
W42PdfObj  *w42_pdf_file_trailer (W42PdfFile *file);
/* An object by number, or `obj` itself when it is not a reference; NULL
 * for one that is not there. */
W42PdfObj  *w42_pdf_file_get     (W42PdfFile *file, guint num);
W42PdfObj  *w42_pdf_file_resolve (W42PdfFile *file, W42PdfObj *obj);
/* Takes `obj` as a new object of the file, and returns its number. */
guint       w42_pdf_file_add     (W42PdfFile *file, W42PdfObj *obj);
/* A dictionary the file holds at `key` of `dict`, directly or through a
 * reference, made there empty if it is missing. */
W42PdfObj  *w42_pdf_file_dict_at (W42PdfFile *file, W42PdfObj *dict, const char *key);
/* The same for an array. */
W42PdfObj  *w42_pdf_file_array_at (W42PdfFile *file, W42PdfObj *dict, const char *key);
/* The document catalogue, and the first page with its number. */
W42PdfObj  *w42_pdf_file_catalog    (W42PdfFile *file);
W42PdfObj  *w42_pdf_file_first_page (W42PdfFile *file, guint *num);

/* A stream's bytes with its filters undone, where they are Flate or
 * none; NULL for any other. */
GBytes     *w42_pdf_stream_decoded (W42PdfObj *stream);

/* Encrypting one string or stream: the bytes in, the bytes out. */
typedef GBytes *(*W42PdfEncryptFunc) (gpointer data, const guint8 *bytes, gsize len);

typedef struct {
  /* Object streams and a compressed cross-reference stream, every
   * stream deflated as far as it will go, and a stream that is the same
   * as another written once: the smallest file.  Otherwise the objects
   * are written one after another with a plain table, as PDF 1.4 did. */
  gboolean          compress;
  /* When set, every string and stream is encrypted with it but those
   * of `encrypt_dict`, the cross-reference stream's and the RAW text. */
  W42PdfEncryptFunc encrypt;
  gpointer          encrypt_data;
  guint             encrypt_dict;
  /* An object written as an object of its own even when compressing, so
   * that its bytes can be found in the file afterwards: a signature's. */
  guint             unpacked;
} W42PdfWriteOptions;

/* Where a RAW object's text went in the file written. */
typedef struct {
  int   tag;
  gsize offset;
  gsize length;
} W42PdfPlaceholder;

/* The whole file.  Objects nothing refers to are left out and the rest
 * numbered afresh, and `placeholders`, when given, receives a
 * W42PdfPlaceholder for every RAW object written. */
GBytes     *w42_pdf_file_write (W42PdfFile               *file,
                                const W42PdfWriteOptions *options,
                                GArray                   *placeholders);

G_END_DECLS
