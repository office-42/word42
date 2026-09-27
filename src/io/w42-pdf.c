/* w42-pdf.c - see w42-pdf.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-pdf.h"

#include "w42-build.h"
#include "w42-image.h"
#include "w42-layout.h"
#include "w42-object.h"
#include "w42-odt.h"
#include "w42-pdffile.h"
#include "w42-pdfsec.h"

#include <cairo-pdf.h>
#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

#ifdef HAVE_POPPLER
#include <poppler.h>
#endif

/* The layout works in pixels at 96 dpi; PDF works in points. */
#define PX_TO_POINTS (72.0 / W42_LAYOUT_DPI)

/* What the document inside a PDF is called in its description, which is
 * how Word42 knows its own when it reads the PDF back.  A mark, not
 * text for a person, and so never translated. */
#define SOURCE_DESC "Word42 document"
#define SOURCE_MIME "application/vnd.oasis.opendocument.text"

/* The width of a signature's /ByteRange, filled in once the file is
 * written: four numbers of up to ten digits. */
#define BYTE_RANGE_WIDTH 36

G_DEFINE_QUARK (w42-pdf-error-quark, w42_pdf_error)

/* ====================================================================== */
/* Options                                                                 */
/* ====================================================================== */

W42PdfOptions *
w42_pdf_options_new (void)
{
  W42PdfOptions *o = g_new0 (W42PdfOptions, 1);

  o->compress = TRUE;
  return o;
}

static void
wipe (char **field)
{
  if (*field != NULL)
    {
      memset (*field, 0, strlen (*field));
      g_free (*field);
      *field = NULL;
    }
}

void
w42_pdf_options_set (char **field, const char *value)
{
  char *copy = value != NULL && *value != '\0' ? g_strdup (value) : NULL;

  wipe (field);
  *field = copy;
}

W42PdfOptions *
w42_pdf_options_copy (const W42PdfOptions *options)
{
  W42PdfOptions *o = g_new0 (W42PdfOptions, 1);

  *o = *options;
  o->open_password = g_strdup (options->open_password);
  o->modify_password = g_strdup (options->modify_password);
  o->certificate = g_strdup (options->certificate);
  o->certificate_password = g_strdup (options->certificate_password);
  o->reason = g_strdup (options->reason);
  o->location = g_strdup (options->location);
  o->contact = g_strdup (options->contact);
  return o;
}

void
w42_pdf_options_free (W42PdfOptions *options)
{
  if (options == NULL)
    return;
  wipe (&options->open_password);
  wipe (&options->modify_password);
  wipe (&options->certificate_password);
  g_free (options->certificate);
  g_free (options->reason);
  g_free (options->location);
  g_free (options->contact);
  g_free (options);
}

/* ====================================================================== */
/* Export                                                                  */
/* ====================================================================== */

static cairo_status_t
write_to_bytes (void *closure, const unsigned char *data, unsigned int length)
{
  g_byte_array_append (closure, data, length);
  return CAIRO_STATUS_SUCCESS;
}

/* The Summary Info goes into the PDF's own, which is what a reader shows
 * as the title of the window and a shop takes the book's name from. */
static void
pdf_metadata (cairo_surface_t *surface, W42PieceTable *pt)
{
  const W42DocInfo *info = w42_pt_get_info (pt);

  if (info->title != NULL && *info->title != '\0')
    cairo_pdf_surface_set_metadata (surface, CAIRO_PDF_METADATA_TITLE, info->title);
  if (info->author != NULL && *info->author != '\0')
    cairo_pdf_surface_set_metadata (surface, CAIRO_PDF_METADATA_AUTHOR, info->author);
  if (info->subject != NULL && *info->subject != '\0')
    cairo_pdf_surface_set_metadata (surface, CAIRO_PDF_METADATA_SUBJECT, info->subject);
  if (info->keywords != NULL && *info->keywords != '\0')
    cairo_pdf_surface_set_metadata (surface, CAIRO_PDF_METADATA_KEYWORDS, info->keywords);
  cairo_pdf_surface_set_metadata (surface, CAIRO_PDF_METADATA_CREATOR, "Word42");
}

/* The headings as the PDF's bookmarks, nested by their outline level, each
 * going to where its first line is: a book's parts and chapters in the
 * reader's side panel, as the Document Map shows them in Word42. */
static void
pdf_outline (cairo_surface_t *surface, W42PieceTable *pt, W42Layout *layout)
{
  const GArray *lines = w42_layout_lines (layout);
  GPtrArray *blocks = w42_layout_blocks (layout);
  W42StyleSheet *styles = w42_pt_stylesheet (pt);
  W42ApTable *aps = w42_pt_ap_table (pt);
  int parent[10] = { 0 };
  int last_block = -1;

  if (blocks == NULL)
    return;

  for (guint i = 0; i < lines->len; i++)
    {
      const W42LineBox *box = &g_array_index (lines, W42LineBox, i);
      const W42Block *block;
      const char *style;
      int level;
      GString *name;
      char *link;
      int id;

      if (box->block == last_block || box->block < 0 || (guint) box->block >= blocks->len)
        continue;
      last_block = box->block;
      block = g_ptr_array_index (blocks, box->block);
      if (block->note >= 0 || block->table >= 0)
        continue;
      style = w42_ap_table_get (aps, block->ap)->pa.style;
      level = style != NULL ? w42_stylesheet_outline (styles, style) : 0;
      if (level < 1 || level > 9)
        continue;

      /* The heading's text on one line: its line breaks, tabs and
       * pictures are nothing to a bookmark. */
      name = g_string_new (NULL);
      for (const char *c = block->text->str; *c != '\0'; c = g_utf8_next_char (c))
        {
          gunichar u = g_utf8_get_char (c);

          if (u == 0xFFFC || u == 0x00AD)
            continue;
          if (u == 0x2028 || u == '\t' || u == '\n' || u == '\v')
            u = ' ';
          if (u == ' ' && (name->len == 0 || name->str[name->len - 1] == ' '))
            continue;
          g_string_append_unichar (name, u);
        }
      while (name->len > 0 && name->str[name->len - 1] == ' ')
        g_string_truncate (name, name->len - 1);
      if (name->len == 0)
        {
          g_string_free (name, TRUE);
          continue;
        }

      /* Nested under the nearest shallower heading, as a document's
       * outline is. */
      {
        int up = CAIRO_PDF_OUTLINE_ROOT;

        for (int l = level - 1; l >= 1; l--)
          if (parent[l] != 0)
            {
              up = parent[l];
              break;
            }
        link = g_strdup_printf ("page=%d pos=[%g %g]", box->page + 1,
                                box->x * PX_TO_POINTS, box->y * PX_TO_POINTS);
        id = cairo_pdf_surface_add_outline (surface, up, name->str, link,
                                            level == 1 ? CAIRO_PDF_OUTLINE_FLAG_OPEN : 0);
        g_free (link);
      }
      parent[level] = id;
      for (int l = level + 1; l < 10; l++)
        parent[l] = 0;
      g_string_free (name, TRUE);
    }
}

/* The pages, as cairo writes them. */
static GBytes *
render_pages (W42PieceTable *pt, const W42PageSetup *page, int picture_ppi,
              GError **error)
{
  GByteArray *out = g_byte_array_new ();
  cairo_surface_t *surface;
  cairo_t *cr;
  W42Layout *layout;
  const GArray *lines;
  int n_pages;
  cairo_status_t status;

  /* Always paginated: exporting from Normal view must still give pages. */
  layout = w42_layout_new ();
  w42_layout_set_galley (layout, FALSE);
  w42_layout_set_picture_ppi (layout, picture_ppi);
  w42_layout_build_pt (layout, pt, page);

  surface = cairo_pdf_surface_create_for_stream (write_to_bytes, out,
                                                 page->width / 20.0,
                                                 page->height / 20.0);
  cr = cairo_create (surface);
  pdf_metadata (surface, pt);

  lines = w42_layout_lines (layout);
  n_pages = w42_layout_n_pages (layout);

  for (int p = 0; p < n_pages; p++)
    {
      cairo_save (cr);
      cairo_scale (cr, PX_TO_POINTS, PX_TO_POINTS);
      cairo_set_source_rgb (cr, 0, 0, 0);

      w42_layout_draw_backdrop (layout, cr, p);

      for (guint i = 0; i < lines->len; i++)
        {
          const W42LineBox *box = &g_array_index (lines, W42LineBox, i);

          if (box->page != p)
            continue;

          w42_layout_draw_line (layout, cr, box);
        }

      w42_layout_draw_furniture (layout, cr, p);

      cairo_restore (cr);
      cairo_show_page (cr);
    }

  status = cairo_status (cr);
  pdf_outline (surface, pt, layout);
  cairo_destroy (cr);
  /* Finishing writes most of the file -- the fonts, the page tree, the
   * cross-reference table -- so a failure shows here, not before. */
  cairo_surface_finish (surface);
  if (status == CAIRO_STATUS_SUCCESS)
    status = cairo_surface_status (surface);
  cairo_surface_destroy (surface);
  w42_layout_free (layout);

  if (status != CAIRO_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   /* Translators: %s is the reason, as the Cairo graphics
                    * library gives it, in English. */
                   _("Word42 could not write the PDF: %s"),
                   cairo_status_to_string (status));
      g_byte_array_free (out, TRUE);
      return NULL;
    }
  return g_byte_array_free_to_bytes (out);
}

/* The document itself goes inside the PDF, as an .odt: an attachment any
 * reader lists, marked as the source the pages were made from, which is
 * what Word42 opens when it is given the PDF back. */
static void
attach_source (W42PdfFile *f, GBytes *odt, W42PdfObj *mod_date, const char *name)
{
  W42PdfObj *catalog = w42_pdf_file_catalog (f);
  W42PdfObj *params = w42_pdf_dict (), *dict = w42_pdf_dict ();
  W42PdfObj *spec = w42_pdf_dict (), *ef = w42_pdf_dict ();
  W42PdfObj *names, *tree, *list;
  guint8 md5[16];
  gsize md5_len = sizeof md5;
  GChecksum *sum = g_checksum_new (G_CHECKSUM_MD5);
  guint file_num, spec_num;

  g_checksum_update (sum, g_bytes_get_data (odt, NULL), (gssize) g_bytes_get_size (odt));
  g_checksum_get_digest (sum, md5, &md5_len);
  g_checksum_free (sum);

  w42_pdf_dict_set (params, "Size", w42_pdf_int ((gint64) g_bytes_get_size (odt)));
  w42_pdf_dict_set (params, "ModDate", w42_pdf_string (mod_date->v.s.data, mod_date->v.s.len));
  w42_pdf_dict_set (params, "CheckSum", w42_pdf_string (md5, md5_len));
  w42_pdf_dict_set (dict, "Type", w42_pdf_name ("EmbeddedFile"));
  w42_pdf_dict_set (dict, "Subtype", w42_pdf_name (SOURCE_MIME));
  w42_pdf_dict_set (dict, "Params", params);
  file_num = w42_pdf_file_add (f, w42_pdf_stream (dict, g_bytes_ref (odt)));

  w42_pdf_dict_set (ef, "F", w42_pdf_ref (file_num));
  w42_pdf_dict_set (ef, "UF", w42_pdf_ref (file_num));
  w42_pdf_dict_set (spec, "Type", w42_pdf_name ("Filespec"));
  w42_pdf_dict_set (spec, "F", w42_pdf_text (name));
  w42_pdf_dict_set (spec, "UF", w42_pdf_text (name));
  w42_pdf_dict_set (spec, "Desc", w42_pdf_text (SOURCE_DESC));
  /* PDF 2.0's word for it, which PDF/A-3 readers show as such. */
  w42_pdf_dict_set (spec, "AFRelationship", w42_pdf_name ("Source"));
  w42_pdf_dict_set (spec, "EF", ef);
  spec_num = w42_pdf_file_add (f, spec);

  names = w42_pdf_file_dict_at (f, catalog, "Names");
  tree = w42_pdf_file_dict_at (f, names, "EmbeddedFiles");
  list = w42_pdf_file_array_at (f, tree, "Names");
  w42_pdf_array_add (list, w42_pdf_text (name));
  w42_pdf_array_add (list, w42_pdf_ref (spec_num));
  w42_pdf_array_add (w42_pdf_file_array_at (f, catalog, "AF"), w42_pdf_ref (spec_num));
}

/* An invisible signature field on the first page, whose value is the
 * signature: its /ByteRange and /Contents placeholders are filled in
 * once the rest of the file is written.  Returns the signature's
 * object, which has to be written where it can be found again. */
static guint
add_signature (W42PdfFile *f, W42PdfSigner *signer, const W42PdfOptions *options,
               GDateTime *now)
{
  W42PdfObj *catalog = w42_pdf_file_catalog (f);
  W42PdfObj *sig = w42_pdf_dict (), *widget = w42_pdf_dict ();
  W42PdfObj *build = w42_pdf_dict (), *app = w42_pdf_dict ();
  W42PdfObj *rect = w42_pdf_array (), *page, *form;
  GString *contents = g_string_new ("<");
  char *range;
  guint sig_num, widget_num, page_num = 0;

  for (gsize i = 0; i < 2 * w42_pdf_signer_reserve (signer); i++)
    g_string_append_c (contents, '0');
  g_string_append_c (contents, '>');
  range = g_strdup_printf ("[0 %-*s]", BYTE_RANGE_WIDTH - 4, "0 0 0");

  w42_pdf_dict_set (sig, "Type", w42_pdf_name ("Sig"));
  w42_pdf_dict_set (sig, "Filter", w42_pdf_name ("Adobe.PPKLite"));
  w42_pdf_dict_set (sig, "SubFilter", w42_pdf_name ("ETSI.CAdES.detached"));
  w42_pdf_dict_set (sig, "ByteRange", w42_pdf_raw (range, 1));
  w42_pdf_dict_set (sig, "Contents", w42_pdf_raw (contents->str, 2));
  w42_pdf_dict_set (sig, "M", w42_pdf_date (now));
  if (*w42_pdf_signer_name (signer) != '\0')
    w42_pdf_dict_set (sig, "Name", w42_pdf_text (w42_pdf_signer_name (signer)));
  if (options->reason != NULL)
    w42_pdf_dict_set (sig, "Reason", w42_pdf_text (options->reason));
  if (options->location != NULL)
    w42_pdf_dict_set (sig, "Location", w42_pdf_text (options->location));
  if (options->contact != NULL)
    w42_pdf_dict_set (sig, "ContactInfo", w42_pdf_text (options->contact));
  w42_pdf_dict_set (app, "Name", w42_pdf_name ("Word42"));
  w42_pdf_dict_set (app, "REx", w42_pdf_text (W42_VERSION));
  w42_pdf_dict_set (build, "App", app);
  w42_pdf_dict_set (sig, "Prop_Build", build);
  sig_num = w42_pdf_file_add (f, sig);
  g_string_free (contents, TRUE);
  g_free (range);

  page = w42_pdf_file_first_page (f, &page_num);
  for (int i = 0; i < 4; i++)
    w42_pdf_array_add (rect, w42_pdf_int (0));
  w42_pdf_dict_set (widget, "Type", w42_pdf_name ("Annot"));
  w42_pdf_dict_set (widget, "Subtype", w42_pdf_name ("Widget"));
  w42_pdf_dict_set (widget, "FT", w42_pdf_name ("Sig"));
  w42_pdf_dict_set (widget, "T", w42_pdf_text ("Signature1"));
  w42_pdf_dict_set (widget, "V", w42_pdf_ref (sig_num));
  w42_pdf_dict_set (widget, "Rect", rect);
  /* Printed and locked, as an invisible signature is. */
  w42_pdf_dict_set (widget, "F", w42_pdf_int (132));
  if (page != NULL)
    w42_pdf_dict_set (widget, "P", w42_pdf_ref (page_num));
  widget_num = w42_pdf_file_add (f, widget);
  if (page != NULL)
    w42_pdf_array_add (w42_pdf_file_array_at (f, page, "Annots"), w42_pdf_ref (widget_num));

  form = w42_pdf_file_dict_at (f, catalog, "AcroForm");
  w42_pdf_array_add (w42_pdf_file_array_at (f, form, "Fields"), w42_pdf_ref (widget_num));
  /* The document has signatures, and is to be changed only by adding
   * to the end of it. */
  w42_pdf_dict_set (form, "SigFlags", w42_pdf_int (3));
  return sig_num;
}

/* The signature, now that the file is final: the byte range written in,
 * then the CMS over everything but the placeholder, in hexadecimal in
 * the placeholder. */
static GBytes *
fill_signature (GBytes *written, GArray *placeholders, W42PdfSigner *signer,
                GError **error)
{
  const W42PdfPlaceholder *range = NULL, *contents = NULL;
  gsize len;
  guint8 *data;
  GBytes *file, *cms;
  char *text;
  static const char digits[] = "0123456789ABCDEF";

  for (guint i = 0; i < placeholders->len; i++)
    {
      const W42PdfPlaceholder *p = &g_array_index (placeholders, W42PdfPlaceholder, i);

      if (p->tag == 1)
        range = p;
      else if (p->tag == 2)
        contents = p;
    }
  if (range == NULL || contents == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Word42 could not sign the PDF."));
      g_bytes_unref (written);
      return NULL;
    }

  data = g_bytes_unref_to_data (written, &len);
  text = g_strdup_printf ("[0 %" G_GSIZE_FORMAT " %" G_GSIZE_FORMAT " %" G_GSIZE_FORMAT,
                          contents->offset, contents->offset + contents->length,
                          len - (contents->offset + contents->length));
  if (strlen (text) + 1 > range->length)
    {
      g_free (text);
      g_free (data);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("The PDF is too large to be signed."));
      return NULL;
    }
  memset (data + range->offset, ' ', range->length);
  memcpy (data + range->offset, text, strlen (text));
  data[range->offset + range->length - 1] = ']';
  g_free (text);

  file = g_bytes_new_take (data, len);
  cms = w42_pdf_signer_sign (signer, file, contents->offset,
                             contents->offset + contents->length, error);
  if (cms == NULL)
    {
      g_bytes_unref (file);
      return NULL;
    }
  if (2 * g_bytes_get_size (cms) + 2 > contents->length)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Word42 could not sign the PDF."));
      g_bytes_unref (cms);
      g_bytes_unref (file);
      return NULL;
    }

  data = g_bytes_unref_to_data (file, &len);
  {
    gsize n;
    const guint8 *sig = g_bytes_get_data (cms, &n);
    guint8 *hex = data + contents->offset + 1;

    for (gsize i = 0; i < n; i++)
      {
        hex[2 * i] = (guint8) digits[sig[i] >> 4];
        hex[2 * i + 1] = (guint8) digits[sig[i] & 15];
      }
  }
  g_bytes_unref (cms);
  return g_bytes_new_take (data, len);
}

/* What cairo wrote, finished as `options` ask: the document put inside,
 * a password, a signature, and packed tight or laid out plainly. */
static GBytes *
finish_pdf (GBytes *pages, W42PieceTable *pt, const W42PageSetup *page,
            GFile *file, const W42PdfOptions *options, GError **error)
{
  W42PdfFile *f;
  W42PdfWriteOptions wo;
  W42PdfCrypt *crypt = NULL;
  W42PdfSigner *signer = NULL;
  GArray *placeholders = NULL;
  GDateTime *now = g_date_time_new_now_local ();
  W42PdfObj *mod_date = w42_pdf_date (now);
  gboolean password = (options->open_password != NULL && *options->open_password != '\0') ||
                      (options->modify_password != NULL && *options->modify_password != '\0');
  GBytes *out = NULL;

  memset (&wo, 0, sizeof wo);
  wo.compress = options->compress;

  f = w42_pdf_file_parse (pages, error);
  if (f == NULL)
    goto done;

  /* When the file was last changed, in the PDF's own information and on
   * the document inside it: two dates that differ say some other
   * program has changed the PDF since Word42 wrote it. */
  {
    W42PdfObj *info = w42_pdf_file_resolve (f, w42_pdf_dict_get (w42_pdf_file_trailer (f), "Info"));

    if (info == NULL || info->kind != W42_PDF_DICT)
      {
        info = w42_pdf_dict ();
        w42_pdf_dict_set (w42_pdf_file_trailer (f), "Info",
                          w42_pdf_ref (w42_pdf_file_add (f, info)));
      }
    w42_pdf_dict_set (info, "ModDate", w42_pdf_string (mod_date->v.s.data, mod_date->v.s.len));
  }

  if (options->keep_document)
    {
      GBytes *odt = w42_odt_save_bytes (pt, page);
      char *base = g_file_get_basename (file);
      char *dot = strrchr (base, '.');
      char *name;

      if (dot != NULL && dot != base)
        *dot = '\0';
      name = g_strconcat (base, ".odt", NULL);
      if (odt == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       _("Word42 could not put the document in the PDF."));
          g_free (base);
          g_free (name);
          goto done;
        }
      attach_source (f, odt, mod_date, name);
      g_bytes_unref (odt);
      g_free (base);
      g_free (name);
    }

  if (password)
    {
      W42PdfObj *ext = w42_pdf_dict (), *adbe = w42_pdf_dict ();
      guint num;

      crypt = w42_pdf_crypt_new (options->open_password, options->modify_password, error);
      if (crypt == NULL)
        goto done;
      num = w42_pdf_file_add (f, w42_pdf_crypt_dict (crypt));
      w42_pdf_dict_set (w42_pdf_file_trailer (f), "Encrypt", w42_pdf_ref (num));
      /* AES-256 is PDF 2.0's, or 1.7's with Adobe's eighth extension. */
      w42_pdf_dict_set (adbe, "BaseVersion", w42_pdf_name ("1.7"));
      w42_pdf_dict_set (adbe, "ExtensionLevel", w42_pdf_int (8));
      w42_pdf_dict_set (ext, "ADBE", adbe);
      w42_pdf_dict_set (w42_pdf_file_catalog (f), "Extensions", ext);
      wo.encrypt = w42_pdf_crypt_encrypt;
      wo.encrypt_data = crypt;
      wo.encrypt_dict = num;
    }

  if (options->sign)
    {
      if (options->certificate == NULL)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       _("No certificate to sign the PDF with has been chosen."));
          goto done;
        }
      signer = w42_pdf_signer_new (options->certificate, options->certificate_password, error);
      if (signer == NULL)
        goto done;
      wo.unpacked = add_signature (f, signer, options, now);
      placeholders = g_array_new (FALSE, FALSE, sizeof (W42PdfPlaceholder));
    }

  out = w42_pdf_file_write (f, &wo, placeholders);
  if (signer != NULL)
    out = fill_signature (out, placeholders, signer, error);

done:
  if (placeholders != NULL)
    g_array_free (placeholders, TRUE);
  w42_pdf_signer_free (signer);
  w42_pdf_crypt_free (crypt);
  w42_pdf_file_free (f);
  w42_pdf_obj_free (mod_date);
  g_date_time_unref (now);
  return out;
}

gboolean
w42_pdf_export_with (W42PieceTable       *pt,
                     const W42PageSetup  *page,
                     GFile               *file,
                     const W42PdfOptions *options,
                     GError             **error)
{
  W42PdfOptions *plain = NULL;
  GBytes *pages, *out;
  GError *local = NULL;
  gboolean ok;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (page != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (options == NULL)
    options = plain = w42_pdf_options_new ();

  pages = render_pages (pt, page, options->picture_ppi, error);
  if (pages == NULL)
    {
      w42_pdf_options_free (plain);
      return FALSE;
    }

  out = finish_pdf (pages, pt, page, file, options, &local);
  /* Should cairo ever write something this cannot read back, the pages
   * are still a PDF, and one that asked for nothing more is written as
   * cairo made it. */
  if (out == NULL && !options->keep_document && !options->sign &&
      options->open_password == NULL && options->modify_password == NULL &&
      g_error_matches (local, G_IO_ERROR, G_IO_ERROR_INVALID_DATA))
    {
      g_clear_error (&local);
      out = g_bytes_ref (pages);
    }
  g_bytes_unref (pages);
  w42_pdf_options_free (plain);
  if (out == NULL)
    {
      g_propagate_error (error, local);
      return FALSE;
    }

  /* Written whole to a file beside it and put in its place, so that a
   * full disk or a failure leaves the file that was there. */
  ok = g_file_replace_contents (file, g_bytes_get_data (out, NULL), g_bytes_get_size (out),
                                NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, error);
  g_bytes_unref (out);
  return ok;
}

gboolean
w42_pdf_export (W42PieceTable      *pt,
                const W42PageSetup *page,
                GFile              *file,
                GError            **error)
{
  return w42_pdf_export_with (pt, page, file, NULL, error);
}

/* ====================================================================== */
/* Import                                                                  */
/* ====================================================================== */

gboolean
w42_pdf_import_available (void)
{
#ifdef HAVE_POPPLER
  return TRUE;
#else
  return FALSE;
#endif
}

#ifdef HAVE_POPPLER

/* ---- reading -------------------------------------------------------- */

/* A PDF names a font "ABCDEF+SegoeUI-Bold": a six-letter subset tag, the
 * family, and the style.  What the model wants is the family, spaced the
 * way a person writes it, and the style as two flags. */
static char *
pdf_family (const char *name, gboolean *bold, gboolean *italic)
{
  static const char *tails[] = {
    "PSMT", "MT", "PS", "Bold", "Italic", "Oblique", "Regular",
    "Black", "Light", "Medium", "SemiBold", "Semibold", "Book"
  };
  char *lower, *base;
  const char *p = name, *cut;
  GString *out;
  gboolean again = TRUE;

  *bold = *italic = FALSE;
  if (name == NULL || *name == '\0')
    return NULL;

  lower = g_ascii_strdown (name, -1);
  *bold = strstr (lower, "bold") != NULL || strstr (lower, "black") != NULL ||
          strstr (lower, "heavy") != NULL || strstr (lower, "semib") != NULL;
  *italic = strstr (lower, "italic") != NULL || strstr (lower, "oblique") != NULL;
  g_free (lower);

  if (strlen (p) > 7 && p[6] == '+')
    p += 7;

  cut = strpbrk (p, "-,");
  base = cut != NULL ? g_strndup (p, (gsize) (cut - p)) : g_strdup (p);

  /* A style tacked on to the family with no separator at all. */
  while (again)
    {
      again = FALSE;
      for (gsize i = 0; i < G_N_ELEMENTS (tails); i++)
        {
          gsize n = strlen (base), m = strlen (tails[i]);

          if (n > m && g_str_has_suffix (base, tails[i]))
            {
              base[n - m] = '\0';
              again = TRUE;
              break;
            }
        }
    }

  out = g_string_new (NULL);
  for (const char *q = base; *q != '\0'; q++)
    {
      if (q != base && g_ascii_isupper (*q) &&
          (g_ascii_islower (q[-1]) || g_ascii_isdigit (q[-1])))
        g_string_append_c (out, ' ');
      g_string_append_c (out, *q);
    }
  g_free (base);

  if (out->len == 0)
    {
      g_string_free (out, TRUE);
      return NULL;
    }
  return g_string_free (out, FALSE);
}

/* One line of a page, as poppler broke it, with where it sits. */
typedef struct {
  guint  start, end;        /* characters of the page's text */
  double x1, y1, x2, y2;
  int    column;
  gboolean rtl;             /* in a right-to-left script as a whole */
} PdfLine;

/* One span of a paragraph's text that shares its formatting. */
typedef struct {
  GString    *text;
  W42CharFmt  ch;
} PdfChunk;

/* The bands of a page's width that no character sits in.  Text either
 * side of one is read as its own column rather than line by line across
 * both, which is what a two-column CV or a newspaper page needs.  The
 * bands are found from the characters themselves and not from poppler's
 * lines, because a line is exactly the thing that runs across them: the
 * telephone number in the margin and the heading beside it come back as
 * one line, and it is the gutter that says they are two. */
static int
find_gutters (const char *text, const gsize *byte_of, gsize n_chars,
              const PopplerRectangle *rects, guint n_rects,
              double page_w, double *gutters, int max_gutters)
{
  int bins = (int) CLAMP (isfinite (page_w) ? page_w : 0.0, 32.0, 4096.0);
  guint8 *covered = g_new0 (guint8, bins + 1);
  int n = 0;

  for (gsize i = 0; i < n_chars && i < n_rects; i++)
    {
      const PopplerRectangle *r = &rects[i];
      gunichar c = g_utf8_get_char (text + byte_of[i]);
      int a, b;

      /* A space is whatever gap the typesetter left, so it says nothing
       * about where the text is.  A rectangle that is not a number says
       * even less, and CLAMP would let it straight through to the cast. */
      if (g_unichar_isspace (c) || !isfinite (r->x1) || !isfinite (r->x2) ||
          r->x2 <= r->x1)
        continue;
      a = (int) CLAMP (r->x1, 0.0, (double) bins);
      b = (int) CLAMP (r->x2 + 1.0, 0.0, (double) bins);
      for (int x = a; x < b; x++)
        covered[x] = 1;
    }

  for (int x = 1; x < bins && n < max_gutters; x++)
    {
      int run;

      if (covered[x])
        continue;
      run = x;
      while (run < bins && !covered[run])
        run++;
      /* A gutter is wide, and inside the page rather than the margin at
       * either edge. */
      if (run - x >= 8 && x > bins / 20 && run < bins - bins / 20)
        gutters[n++] = (x + run) / 2.0;
      x = run;
    }

  g_free (covered);
  return n;
}

/* Type set with its letters spaced out -- a heading in tracked capitals --
 * comes back with a space between every letter, because a space is what
 * the gap looks like.  A line that is nearly all single letters has its
 * letter gaps closed up again, keeping the wider ones that really are the
 * spaces between words. */
static void
close_tracking (const char *text, const gsize *byte_of,
                const PopplerRectangle *rects, guint n_rects,
                guint start, guint end, guint8 *drop)
{
  guint tokens = 0, singles = 0, run = 0;
  double lo = G_MAXDOUBLE, hi = 0.0, cut;

  for (guint k = start; k <= end; k++)
    {
      gunichar c = k < end ? g_utf8_get_char (text + byte_of[k]) : ' ';

      if (g_unichar_isspace (c))
        {
          if (run > 0)
            {
              tokens++;
              if (run == 1)
                singles++;
            }
          run = 0;
          if (k < end && k < n_rects && rects[k].x2 > rects[k].x1)
            {
              lo = MIN (lo, rects[k].x2 - rects[k].x1);
              hi = MAX (hi, rects[k].x2 - rects[k].x1);
            }
        }
      else
        run++;
    }

  /* Four letters at least, and seven in ten of the words one letter long. */
  if (tokens < 4 || singles * 10 < tokens * 7 || lo == G_MAXDOUBLE)
    return;

  /* Where the gaps come in two sizes, the small ones are the tracking and
   * the wide ones are the words; where they are all one size, they are all
   * tracking. */
  cut = (hi > lo * 1.8) ? (lo + hi) / 2.0 : hi + 1.0;
  for (guint k = start; k < end; k++)
    {
      gunichar c = g_utf8_get_char (text + byte_of[k]);

      if (g_unichar_isspace (c) && k < n_rects &&
          rects[k].x2 - rects[k].x1 < cut)
        drop[k] = 1;
    }
}

/* A line that opens with a bullet or a number is an item of its own, not
 * the tail of the item before it, however close the two lines sit. */
static gboolean
starts_item (const char *p)
{
  gunichar c = g_utf8_get_char (p);
  const char *q;

  if (c == 0x2022 || c == 0x25AA || c == 0x25CB || c == 0x25E6 ||
      c == 0x2023 || c == 0x2043 || c == 0x00B7 || c == '-' ||
      c == 0x2013 || c == 0x2014)
    return TRUE;

  /* "1." or "a)" and their like, at the head of the line. */
  q = p;
  if (g_ascii_isdigit (*q))
    {
      while (g_ascii_isdigit (*q))
        q++;
    }
  else if (g_ascii_isalpha (*q) && !g_ascii_isalpha (q[1]))
    q++;
  else
    return FALSE;
  if (*q != '.' && *q != ')')
    return FALSE;
  q++;
  return *q == '\0' || *q == ' ' || *q == '\t' || *q == '\n';
}

/* Which column an x falls in. */
static int
column_of (double x, const double *gutters, int n_gutters)
{
  int c = 0;

  for (int g = 0; g < n_gutters; g++)
    if (x > gutters[g])
      c = g + 1;
  return c;
}

static int
double_cmp (gconstpointer a, gconstpointer b)
{
  double p = *(const double *) a, q = *(const double *) b;

  return p < q ? -1 : p > q ? 1 : 0;
}

/* Of the lines' edges, sorted, the furthest out that another line comes
 * within a few points of; the furthest of all when none does. */
static double
agreed_edge (GArray *edges, gboolean right)
{
  guint n = edges->len;

  if (right)
    {
      for (guint i = n - 1; i > 0; i--)
        if (g_array_index (edges, double, i) - g_array_index (edges, double, i - 1) <= 3.0)
          return g_array_index (edges, double, i);
      return g_array_index (edges, double, n - 1);
    }
  for (guint i = 0; i + 1 < n; i++)
    if (g_array_index (edges, double, i + 1) - g_array_index (edges, double, i) <= 3.0)
      return g_array_index (edges, double, i);
  return g_array_index (edges, double, 0);
}

static int
line_cmp (gconstpointer a, gconstpointer b)
{
  const PdfLine *p = a, *q = b;

  if (p->column != q->column)
    return p->column - q->column;
  if (p->y1 < q->y1 - 0.5) return -1;
  if (p->y1 > q->y1 + 0.5) return 1;
  return p->x1 < q->x1 ? -1 : p->x1 > q->x1 ? 1 : 0;
}

static void
chunk_clear (gpointer data)
{
  PdfChunk *c = data;

  if (c->text != NULL)
    g_string_free (c->text, TRUE);
}

/* ---- what a page holds besides its text ------------------------------- */

/* A bookmark of the PDF's outline, which becomes a heading where its
 * words are found as a paragraph of their own on its page. */
typedef struct {
  char *key;           /* the title, as paragraph_key makes it */
  int   level;
} PdfHeading;

/* A web link on a page, in points from the page's top left. */
typedef struct {
  double      x1, y1, x2, y2;
  const char *uri;     /* interned */
} PdfLink;

/* A picture on a page, decoded, and where it sits. */
typedef struct {
  double      x1, y1, x2, y2;
  int         column;
  GBytes     *png;
  const char *format;
  int         pw, ph;
} PdfPicture;

/* Where the lines of the paragraph being gathered sit. */
typedef struct {
  int      n_lines;
  double   first_x1;       /* the first line's left edge */
  double   body_x1;        /* the leftmost of the other lines' */
  double   body_x1_max;    /* ... and the rightmost */
  double   min_x1;
  double   max_gap;        /* the widest space left of a line */
  double   col_left, col_right;
  int      full;           /* lines that reach the column's right edge */
  gboolean last_full;
  int      centred;        /* lines as far from one edge as from the other */
  int      rtl;            /* lines mostly in a right-to-left script */
  double   space_before;   /* points of gap above the first line */
} PdfGeom;

/* How much of a page's height, at its top and at its foot, a running
 * header or footer is looked for in. */
#define RUNNING_ZONE 0.08

/* The lines that repeat at the top or the foot of the pages -- a running
 * head, a page number -- with the numbers in them made one mark, so that
 * "Page 3 of 9" and "Page 4 of 9" are the same line. */
typedef struct {
  /* At the top and at the foot: the line on most pages, and a second
   * when a book alternates them between left and right pages. */
  char     *key[2][2];
  gboolean  done[2][2];     /* made the document's own */
} PdfRunning;

/* What reading the pages carries from one to the next. */
typedef struct {
  W42Builder  b;
  GArray     *chunks;         /* PdfChunk: the paragraph being gathered */
  PdfGeom     geom;
  int         page;           /* from 0 */
  gboolean    break_pending;  /* the next paragraph starts a new page */
  GPtrArray  *headings;       /* by page: a GPtrArray of PdfHeading, or NULL */
  int         justified;      /* paragraphs of three lines or more seen */
  int         ragged;         /* ... justified, and not */
  double      first_indent;   /* the last first-line indent found, points */
  gboolean    para_is_item;   /* the paragraph began with a bullet or number */
  PdfRunning  running;
  int         n_pages;
} PdfRead;

static void
heading_free (gpointer data)
{
  PdfHeading *h = data;

  g_free (h->key);
  g_free (h);
}

static void
headings_free (gpointer data)
{
  if (data != NULL)
    g_ptr_array_unref (data);
}

static void
picture_clear (gpointer data)
{
  PdfPicture *p = data;

  if (p->png != NULL)
    g_bytes_unref (p->png);
}

/* A paragraph's words as a bookmark is compared with them: spaces run
 * together, none at the ends, and case folded. */
static char *
paragraph_key (const char *text)
{
  GString *s = g_string_new (NULL);
  char *key;

  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isspace (c) || c == 0x00A0)
        {
          if (s->len > 0 && s->str[s->len - 1] != ' ')
            g_string_append_c (s, ' ');
        }
      else if (c != 0x00AD)
        g_string_append_unichar (s, c);
    }
  while (s->len > 0 && s->str[s->len - 1] == ' ')
    g_string_truncate (s, s->len - 1);
  key = g_utf8_casefold (s->str, -1);
  g_string_free (s, TRUE);
  return key;
}

/* The style of a heading of `level`: the sheet a document starts with has
 * three, and a deeper one is made from the third, a level down, so that
 * it is a heading of its own level in the outline it is written with. */
static const char *
heading_style (W42PieceTable *pt, int level)
{
  W42StyleSheet *sheet = w42_pt_stylesheet (pt);
  char *name = g_strdup_printf ("Heading %d", level);
  const W42Style *st;
  const char *found;

  if (w42_stylesheet_find (sheet, name) == NULL &&
      (st = w42_stylesheet_find (sheet, "Heading 3")) != NULL)
    {
      W42Style deeper = *st;

      deeper.name = g_intern_string (name);
      deeper.pa.style = deeper.name;
      deeper.outline = level;
      deeper.based_on = st->name;
      deeper.pa_own = 0;
      deeper.ch_own = 0;
      w42_stylesheet_set (sheet, &deeper);
    }
  found = g_intern_string (w42_stylesheet_find (sheet, name) != NULL ? name : "Heading 3");
  g_free (name);
  return found;
}

/* A line as the running header or footer is recognised by: spaces run
 * together, case folded, every run of digits one "#", and a line that is
 * a roman numeral and nothing else -- the front matter's page number --
 * "#" too. */
static char *
running_key (const char *text)
{
  char *key = paragraph_key (text);
  GString *s = g_string_new (NULL);
  gboolean roman = *key != '\0';

  for (const char *p = key; *p != '\0'; p++)
    if (strchr ("ivxlcdm", *p) == NULL)
      roman = FALSE;
  if (roman)
    {
      g_free (key);
      g_string_free (s, TRUE);
      return g_strdup ("#");
    }
  for (const char *p = key; *p != '\0'; p++)
    if (g_ascii_isdigit (*p))
      {
        if (s->len == 0 || s->str[s->len - 1] != '#')
          g_string_append_c (s, '#');
      }
    else
      g_string_append_c (s, *p);
  g_free (key);
  return g_string_free (s, FALSE);
}

/* A page number, and nothing else: "7", "- 7 -", "xii". */
static gboolean
bare_page_number (const char *key)
{
  const char *p = key;

  while (*p == '-' || *p == ' ' || *p == '(' || *p == '[')
    p++;
  if (*p != '#')
    return FALSE;
  p++;
  while (*p == '-' || *p == ' ' || *p == ')' || *p == ']')
    p++;
  return *p == '\0';
}

/* The first pass over the pages: what repeats in their top and bottom
 * margins, looked at through the text there alone. */
static void
find_running (PopplerDocument *document, PdfRunning *running)
{
  int n_pages = poppler_document_get_n_pages (document);
  GHashTable *counts[2];

  if (n_pages < 2)
    return;
  for (int z = 0; z < 2; z++)
    counts[z] = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (int i = 0; i < n_pages; i++)
    {
      PopplerPage *pp = poppler_document_get_page (document, i);
      double w = 0, h = 0;

      if (pp == NULL)
        continue;
      poppler_page_get_size (pp, &w, &h);
      for (int z = 0; z < 2 && isfinite (w) && isfinite (h); z++)
        {
          PopplerRectangle area;
          char *text;
          char **rows;
          GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);

          area.x1 = 0;
          area.x2 = w;
          area.y1 = z == 0 ? 0 : h * (1.0 - RUNNING_ZONE);
          area.y2 = z == 0 ? h * RUNNING_ZONE : h;
          text = poppler_page_get_text_for_area (pp, &area);
          rows = g_strsplit (text != NULL ? text : "", "\n", -1);
          for (int k = 0; rows[k] != NULL; k++)
            {
              char *key = running_key (rows[k]);

              if (*key == '\0' || g_hash_table_contains (seen, key))
                {
                  g_free (key);
                  continue;
                }
              g_hash_table_add (seen, key);
              g_hash_table_replace (counts[z], g_strdup (key),
                                    GINT_TO_POINTER (GPOINTER_TO_INT (g_hash_table_lookup (counts[z], key)) + 1));
            }
          g_hash_table_destroy (seen);
          g_strfreev (rows);
          g_free (text);
        }
      g_object_unref (pp);
    }

  /* The line on the most pages, and the one on the next most: each on
   * a quarter of the pages or more -- a book's left pages and its right
   * ones may each have their own -- and at least on two. */
  for (int z = 0; z < 2; z++)
    {
      int best[2] = { 0, 0 };

      for (int k = 0; k < 2; k++)
        {
          GHashTableIter it;
          gpointer key, value;

          g_hash_table_iter_init (&it, counts[z]);
          while (g_hash_table_iter_next (&it, &key, &value))
            if (GPOINTER_TO_INT (value) > best[k] &&
                (k == 0 || strcmp (key, running->key[z][0]) != 0))
              {
                best[k] = GPOINTER_TO_INT (value);
                g_free (running->key[z][k]);
                running->key[z][k] = g_strdup (key);
              }
          if (best[k] < MAX (2, n_pages / 4))
            {
              g_clear_pointer (&running->key[z][k], g_free);
              break;
            }
        }
      g_hash_table_destroy (counts[z]);
    }
}

/* Whether a line at the top or foot of the page is the running header or
 * footer, or a page number there; the first one found becomes the
 * document's, with its page number a {PAGE} field. */
static gboolean
drop_running (PdfRead *r, const char *line, double x1, double x2, double y_mid,
              double page_w, double page_h)
{
  int z = y_mid < page_h * RUNNING_ZONE ? 0 : y_mid > page_h * (1.0 - RUNNING_ZONE) ? 1 : -1;
  int k = -1;
  char *key;

  if (z < 0 || r->n_pages < 2)
    return FALSE;
  key = running_key (line);
  for (int i = 0; i < 2 && k < 0; i++)
    if (r->running.key[z][i] != NULL && strcmp (key, r->running.key[z][i]) == 0)
      k = i;
  if (k < 0 && bare_page_number (key))
    {
      /* Where the page number is what runs, a number there that is not
       * on the most pages -- "- 7 -" among "7"s -- is one too. */
      for (int i = 0; i < 2 && k < 0; i++)
        if (r->running.key[z][i] != NULL && bare_page_number (r->running.key[z][i]))
          k = i;
    }

  if (k >= 0 && !r->running.done[z][k])
    {
      gboolean alternate = r->running.key[z][1] != NULL;
      W42PageTextKind kind = alternate && (r->page + 1) % 2 == 0 ? W42_PAGE_TEXT_EVEN
                                                                 : W42_PAGE_TEXT_DEFAULT;
      GString *field = g_string_new (NULL);
      char *trimmed = g_strstrip (g_strdup (line));
      double centre = (x1 + x2) / 2.0;
      W42Align align = centre < page_w * 0.4 ? W42_ALIGN_LEFT
                     : centre > page_w * 0.6 ? W42_ALIGN_RIGHT : W42_ALIGN_CENTER;
      int runs = 0, first = -1;

      /* The number that changes from page to page is the page's; one
       * that is the count of pages is that. */
      for (const char *p = trimmed; *p != '\0'; )
        if (g_ascii_isdigit (*p))
          {
            long v = strtol (p, NULL, 10);

            while (g_ascii_isdigit (*p))
              p++;
            if (runs++ == 0)
              {
                first = (int) CLAMP (v, 0, 100000);
                g_string_append (field, "{PAGE}");
              }
            else if (v == r->n_pages)
              g_string_append (field, "{NUMPAGES}");
            else
              g_string_append_printf (field, "%ld", v);
          }
        else
          g_string_append_c (field, *p++);

      if (z == 0)
        w42_pt_set_header_kind (r->b.pt, kind, field->str, align);
      else
        w42_pt_set_footer_kind (r->b.pt, kind, field->str, align);
      if (kind == W42_PAGE_TEXT_EVEN)
        w42_pt_set_facing_pages (r->b.pt, TRUE);
      /* Numbered from the page the numbers were first seen on, and from
       * the number they began at there. */
      if (first >= 0 && first != r->page + 1 && !r->running.done[z][1 - k])
        w42_pt_set_page_numbering (r->b.pt, r->page + 1, first);
      r->running.done[z][k] = TRUE;
      g_string_free (field, TRUE);
      g_free (trimmed);
    }
  g_free (key);
  return k >= 0;
}

/* The words after a section's number, "1.2 " or "3 ", when there is one:
 * the heading and its bookmark may each have it or not. */
static const char *
skip_number (const char *key)
{
  const char *p = key;

  while (g_ascii_isdigit (*p) || *p == '.')
    p++;
  return p != key && *p == ' ' ? p + 1 : key;
}

/* Whether a line beginning with `c` may start a sentence: a capital, a
 * number, or the quotation mark or dash that opens a line of dialogue. */
static gboolean
starts_sentence (gunichar c)
{
  /* A letter of a script with no capitals -- Arabic, Hebrew, Chinese --
   * may begin one as well as a capital does. */
  gboolean caseless = g_unichar_isalpha (c) && !g_unichar_islower (c) &&
                      g_unichar_toupper (c) == c && g_unichar_tolower (c) == c;

  return caseless || g_unichar_isupper (c) || g_unichar_isdigit (c) ||
         c == 0x00AB || c == 0x201C || c == 0x201E || c == 0x2018 ||
         c == '"' || c == 0x2013 || c == 0x2014 || c == 0x00BF || c == 0x00A1;
}

/* Whether text ending with this ends a sentence: a stop, or a stop inside
 * a closing quotation mark or bracket. */
static gboolean
ends_sentence (const GString *text)
{
  const char *end = text->str + text->len;

  for (int k = 0; k < 3 && end > text->str; k++)
    {
      const char *prev = g_utf8_find_prev_char (text->str, end);
      gunichar c = g_utf8_get_char (prev);

      if (c == '.' || c == '!' || c == '?' || c == ':' || c == 0x2026)
        return TRUE;
      if (c != 0x00BB && c != 0x201D && c != 0x2019 && c != '"' && c != ')' && c != ' ')
        return FALSE;
      end = prev;
    }
  return FALSE;
}

/* The outline's level for a paragraph, 0 when it is not a bookmark's.
 * A bookmark points at its page, or at the foot of the one before when
 * the heading begins the next; a heading may carry its section's number
 * in front, which the bookmark need not. */
static int
find_heading (PdfRead *r, const char *text)
{
  char *key;
  const char *bare;
  int level = 0;

  if (r->headings == NULL || strlen (text) > 400)
    return 0;
  key = paragraph_key (text);
  bare = skip_number (key);

  for (int pg = r->page; pg >= MAX (r->page - 1, 0) && level == 0; pg--)
    {
      GPtrArray *on = (guint) pg < r->headings->len ? g_ptr_array_index (r->headings, pg) : NULL;

      for (guint i = 0; on != NULL && i < on->len; i++)
        {
          PdfHeading *h = g_ptr_array_index (on, i);

          if (strcmp (h->key, key) == 0 || strcmp (skip_number (h->key), bare) == 0)
            {
              level = h->level;
              g_ptr_array_remove_index (on, i);
              break;
            }
        }
    }
  g_free (key);
  return level;
}

/* The bookmarks, by the page each points to. */
static void
read_outline (PopplerDocument *document, PopplerIndexIter *iter, int depth,
              GPtrArray *by_page, guint *budget)
{
  int n_pages = poppler_document_get_n_pages (document);

  do
    {
      PopplerAction *action = poppler_index_iter_get_action (iter);
      PopplerIndexIter *child;

      if (*budget == 0)
        {
          poppler_action_free (action);
          return;
        }
      if (action != NULL && action->type == POPPLER_ACTION_GOTO_DEST &&
          action->goto_dest.title != NULL && action->goto_dest.dest != NULL)
        {
          PopplerDest *dest = action->goto_dest.dest;
          int page = -1;

          if (dest->type == POPPLER_DEST_NAMED)
            {
              PopplerDest *named = poppler_document_find_dest (document, dest->named_dest);

              if (named != NULL)
                {
                  page = named->page_num;
                  poppler_dest_free (named);
                }
            }
          else
            page = dest->page_num;

          if (page >= 1 && page <= n_pages)
            {
              PdfHeading *h = g_new0 (PdfHeading, 1);
              GPtrArray *on;

              h->key = paragraph_key (action->goto_dest.title);
              h->level = CLAMP (depth, 1, 9);
              if ((guint) page > by_page->len)
                g_ptr_array_set_size (by_page, (guint) page);
              on = g_ptr_array_index (by_page, page - 1);
              if (on == NULL)
                {
                  on = g_ptr_array_new_with_free_func (heading_free);
                  g_ptr_array_index (by_page, page - 1) = on;
                }
              if (*h->key != '\0')
                {
                  g_ptr_array_add (on, h);
                  (*budget)--;
                }
              else
                heading_free (h);
            }
        }
      poppler_action_free (action);

      child = poppler_index_iter_get_child (iter);
      if (child != NULL)
        {
          if (depth < 32)
            read_outline (document, child, depth + 1, by_page, budget);
          poppler_index_iter_free (child);
        }
    }
  while (poppler_index_iter_next (iter));
}

/* The page's links to the web.  A link to anything else -- a program to
 * run, a file on this machine -- is not a link a document should carry. */
static GArray *
page_links (PopplerPage *pp, double page_h)
{
  GArray *links = g_array_new (FALSE, FALSE, sizeof (PdfLink));
  GList *mappings = poppler_page_get_link_mapping (pp);

  for (GList *l = mappings; l != NULL; l = l->next)
    {
      PopplerLinkMapping *m = l->data;
      const char *uri;
      PdfLink link;

      if (m->action == NULL || m->action->type != POPPLER_ACTION_URI)
        continue;
      uri = m->action->uri.uri;
      if (uri == NULL || strlen (uri) > 2048 ||
          !(g_ascii_strncasecmp (uri, "http://", 7) == 0 ||
            g_ascii_strncasecmp (uri, "https://", 8) == 0 ||
            g_ascii_strncasecmp (uri, "mailto:", 7) == 0 ||
            g_ascii_strncasecmp (uri, "ftp://", 6) == 0) ||
          !g_utf8_validate (uri, -1, NULL))
        continue;
      /* The link's area is in PDF's own coordinates, from the foot of
       * the page; the text's are from the top. */
      link.x1 = MIN (m->area.x1, m->area.x2);
      link.x2 = MAX (m->area.x1, m->area.x2);
      link.y1 = page_h - MAX (m->area.y1, m->area.y2);
      link.y2 = page_h - MIN (m->area.y1, m->area.y2);
      link.uri = g_intern_string (uri);
      g_array_append_val (links, link);
    }
  poppler_page_free_link_mapping (mappings);
  return links;
}

static const char *
link_at (GArray *links, const PopplerRectangle *r)
{
  double x = (r->x1 + r->x2) / 2.0, y = (r->y1 + r->y2) / 2.0;

  for (guint i = 0; i < links->len; i++)
    {
      const PdfLink *l = &g_array_index (links, PdfLink, i);

      if (x >= l->x1 && x <= l->x2 && y >= l->y1 && y <= l->y2)
        return l->uri;
    }
  return NULL;
}

/* The pictures on the page, decoded, with where each is shown. */
static GArray *
page_pictures (PopplerPage *pp)
{
  GArray *pictures = g_array_new (FALSE, FALSE, sizeof (PdfPicture));
  GList *mappings = poppler_page_get_image_mapping (pp);

  g_array_set_clear_func (pictures, picture_clear);
  for (GList *l = mappings; l != NULL; l = l->next)
    {
      PopplerImageMapping *m = l->data;
      cairo_surface_t *surface = poppler_page_get_image (pp, m->image_id);
      PdfPicture pic;

      if (surface == NULL)
        continue;
      memset (&pic, 0, sizeof pic);
      pic.png = w42_image_surface_to_png (surface);
      cairo_surface_destroy (surface);
      if (pic.png == NULL)
        continue;
      if (!w42_image_probe (pic.png, &pic.pw, &pic.ph, &pic.format))
        {
          g_bytes_unref (pic.png);
          continue;
        }
      pic.x1 = MIN (m->area.x1, m->area.x2);
      pic.x2 = MAX (m->area.x1, m->area.x2);
      pic.y1 = MIN (m->area.y1, m->area.y2);
      pic.y2 = MAX (m->area.y1, m->area.y2);
      if (!isfinite (pic.x1) || !isfinite (pic.x2) || !isfinite (pic.y1) || !isfinite (pic.y2))
        pic.x1 = pic.x2 = pic.y1 = pic.y2 = 0.0;
      g_array_append_val (pictures, pic);
    }
  poppler_page_free_image_mapping (mappings);
  return pictures;
}

static int
picture_cmp (gconstpointer a, gconstpointer b)
{
  const PdfPicture *p = a, *q = b;

  if (p->column != q->column)
    return p->column - q->column;
  return p->y1 < q->y1 ? -1 : p->y1 > q->y1 ? 1 : 0;
}

/* ---- paragraphs --------------------------------------------------------- */

static void
geom_reset (PdfGeom *g)
{
  memset (g, 0, sizeof *g);
}

static void
geom_add_line (PdfGeom *g, const PdfLine *line, double col_left, double col_right)
{
  double measure = col_right - col_left;
  double gap_l = line->x1 - col_left, gap_r = col_right - line->x2;
  /* A justified line's last glyph stops a hair either side of the
   * measure, by its side bearing or a stop hung out in the margin. */
  gboolean full = gap_r <= 3.0;

  if (g->n_lines == 0)
    {
      g->first_x1 = line->x1;
      g->body_x1 = G_MAXDOUBLE;
      g->body_x1_max = -G_MAXDOUBLE;
      g->min_x1 = line->x1;
      g->col_left = col_left;
      g->col_right = col_right;
    }
  else
    {
      g->body_x1 = MIN (g->body_x1, line->x1);
      g->body_x1_max = MAX (g->body_x1_max, line->x1);
    }
  g->min_x1 = MIN (g->min_x1, line->x1);
  g->max_gap = MAX (g->max_gap, gap_l);
  if (full)
    g->full++;
  g->last_full = full;
  /* As far from the one edge as from the other: the widest line of a
   * centred block is as wide as the column, so that alone says nothing,
   * but it is centred too. */
  if (ABS (gap_l - gap_r) <= MAX (3.0, measure * 0.015))
    g->centred++;
  g->n_lines++;
}

/* How the paragraph was set, from where its lines sit in their column:
 * every line centred, every line against the right edge, every line but
 * the last filling the measure -- justified -- or none of those; and how
 * far in its first line and the rest begin. */
static void
shape_paragraph (PdfRead *r, const PdfGeom *g, W42ParaFmt *pa)
{
  double measure = g->col_right - g->col_left;
  int n = g->n_lines;
  int full_before_last = g->full - (g->last_full ? 1 : 0);
  double left = 0.0, first = 0.0;

  if (n == 0 || measure < 36.0)
    return;
  /* A right-to-left paragraph is set from the right: flush right is how
   * it begins, and what an indent is measured from. */
  if (g->rtl * 2 > n)
    {
      pa->rtl = 1;
      if (g->centred == n && g->max_gap > MAX (4.0, measure * 0.04))
        pa->align = W42_ALIGN_CENTER;
      else if (n >= 3 && full_before_last >= n - 1 && g->min_x1 - g->col_left < 3.0)
        pa->align = W42_ALIGN_JUSTIFY;
      else if (g->full == n || g->min_x1 - g->col_left > 3.0)
        pa->align = W42_ALIGN_LEFT;
      else
        pa->align = W42_ALIGN_RIGHT;
      return;
    }
  if (g->centred == n && g->max_gap > MAX (4.0, measure * 0.04))
    {
      pa->align = W42_ALIGN_CENTER;
      return;
    }
  if (g->full == n && g->min_x1 - g->col_left > measure * 0.15)
    {
      pa->align = W42_ALIGN_RIGHT;
      return;
    }
  /* Two lines are too few to tell justified from ragged by: they go the
   * way the document's longer paragraphs have gone. */
  if (n >= 3)
    {
      if (full_before_last >= n - 1)
        {
          pa->align = W42_ALIGN_JUSTIFY;
          r->justified++;
        }
      else
        r->ragged++;
    }
  else if (n == 2 && full_before_last == 1 && r->justified > r->ragged)
    pa->align = W42_ALIGN_JUSTIFY;

  if (n >= 2)
    {
      /* Lines that do not agree on where they begin -- verse, a
       * ragged left edge -- have no indent to speak of. */
      if (g->body_x1_max - g->body_x1 > 3.0)
        return;
      left = g->body_x1 - g->col_left;
      first = g->first_x1 - g->body_x1;
      if (first > 2.0)
        r->first_indent = first;
    }
  else
    {
      /* One line: indented as the paragraphs round it begin, or set in
       * from the margin as a whole. */
      double off = g->first_x1 - g->col_left;

      if (r->first_indent > 2.0 && ABS (off - r->first_indent) < 2.0)
        first = off;
      else
        left = off;
    }
  if (left < 2.0)
    left = 0.0;
  if (ABS (first) < 2.0)
    first = 0.0;
  left = MIN (left, measure * 0.75);
  first = CLAMP (first, -left, measure * 0.75);
  pa->indent_left = (int) (left * 20.0 + 0.5);
  pa->indent_first = (int) floor (first * 20.0 + 0.5);
}

/* The paragraph gathered so far goes into the document, run by run. */
static void
flush_para (PdfRead *r)
{
  W42Builder *b = &r->b;
  GString *all = g_string_new (NULL);
  gboolean any = FALSE;

  for (guint i = 0; i < r->chunks->len; i++)
    {
      PdfChunk *c = &g_array_index (r->chunks, PdfChunk, i);

      g_string_append_len (all, c->text->str, (gssize) c->text->len);
      for (const char *p = c->text->str; *p != '\0' && !any; p++)
        if (!g_ascii_isspace (*p))
          any = TRUE;
    }

  if (any)
    {
      int level;

      w42_builder_reset_para (b);
      shape_paragraph (r, &r->geom, &b->pa);
      if (r->geom.space_before > 0.0)
        b->pa.space_before = (int) CLAMP (r->geom.space_before * 20.0, 0.0, 1440.0);
      if (r->break_pending)
        {
          b->pa.page_break_before = 1;
          r->break_pending = FALSE;
        }
      level = find_heading (r, all->str);
      if (level > 0)
        {
          b->pa.style = heading_style (b->pt, level);
          b->pa.keep_next = 1;
        }
      for (guint i = 0; i < r->chunks->len; i++)
        {
          PdfChunk *c = &g_array_index (r->chunks, PdfChunk, i);

          if (c->text->len == 0)
            continue;
          b->ch = c->ch;
          w42_builder_text (b, c->text->str);
        }
      w42_builder_end_paragraph (b);
    }
  g_string_free (all, TRUE);
  g_array_set_size (r->chunks, 0);
  geom_reset (&r->geom);
}

/* A picture in a paragraph of its own, set to the side it sat on. */
static void
emit_picture (PdfRead *r, const PdfPicture *pic, double col_left, double col_right)
{
  W42Builder *b = &r->b;
  double shown_w = pic->x2 - pic->x1, shown_h = pic->y2 - pic->y1;
  double measure = col_right - col_left;

  w42_builder_reset_char (b);
  w42_builder_reset_para (b);
  if (measure > 36.0 && shown_w < measure * 0.9)
    {
      double centre = (pic->x1 + pic->x2) / 2.0;

      if (ABS (centre - (col_left + col_right) / 2.0) < measure * 0.05)
        b->pa.align = W42_ALIGN_CENTER;
      else if (col_right - pic->x2 < measure * 0.05 && pic->x1 - col_left > measure * 0.2)
        b->pa.align = W42_ALIGN_RIGHT;
    }
  if (r->break_pending)
    {
      b->pa.page_break_before = 1;
      r->break_pending = FALSE;
    }
  w42_builder_object (b, pic->png, pic->format, pic->pw, pic->ph,
                      (int) CLAMP (shown_w * 20.0, 0.0, 31680.0),
                      (int) CLAMP (shown_h * 20.0, 0.0, 31680.0));
  w42_builder_end_paragraph (b);
}

static gboolean
rtl_script (gunichar c)
{
  GUnicodeScript script = g_unichar_get_script (c);

  return script == G_UNICODE_SCRIPT_ARABIC || script == G_UNICODE_SCRIPT_HEBREW ||
         script == G_UNICODE_SCRIPT_SYRIAC || script == G_UNICODE_SCRIPT_THAANA ||
         script == G_UNICODE_SCRIPT_NKO;
}

/* Poppler gives a line with its right-to-left words each in an embedding
 * of its own, the word in the order it is read and the words in the
 * order they stand on the page, left to right.  The line's characters,
 * from `start` to `end`, in the order they are read: a run of those
 * words turned round, with the spaces between them, and in a line that
 * is right-to-left as a whole, everything -- a number, a Latin word and
 * a stop included, each whole.  `embedding` says which embedding each
 * character was in, 0 for none; without it the line is left as it is.
 * Returns whether the line is right-to-left as a whole. */
static gboolean
reading_order (const char *text, const gsize *byte_of, const guint *embedding,
               guint start, guint end, GArray *order)
{
  GArray *units;          /* guint pairs: first character, count */
  guint rtl = 0, ltr = 0;
  gboolean whole;

  g_array_set_size (order, 0);
  for (guint c = start; c < end; c++)
    g_array_append_val (order, c);
  if (embedding == NULL || end <= start)
    return FALSE;
  for (guint c = start; c < end; c++)
    {
      gunichar u = g_utf8_get_char (text + byte_of[c]);

      if (embedding[c] != 0 && rtl_script (u))
        rtl++;
      else if (embedding[c] == 0 && g_unichar_isalpha (u))
        ltr++;
    }
  if (rtl == 0)
    return FALSE;
  whole = rtl > ltr;

  /* The units that keep their order: an embedding, a word or number
   * outside one with the stops inside it ("12.5"), a lone character. */
  units = g_array_new (FALSE, FALSE, sizeof (guint));
  for (guint c = start; c < end; )
    {
      guint k = c + 1;

      if (embedding[c] != 0)
        while (k < end && embedding[k] == embedding[c])
          k++;
      else if (g_unichar_isalnum (g_utf8_get_char (text + byte_of[c])))
        for (guint m = c + 1; m < end && embedding[m] == 0; m++)
          {
            gunichar u = g_utf8_get_char (text + byte_of[m]);

            if (g_unichar_isalnum (u))
              k = m + 1;
            else if (g_unichar_isspace (u) || m + 1 >= end ||
                     !g_unichar_isalnum (g_utf8_get_char (text + byte_of[m + 1])))
              break;
          }
      {
        guint pair[2] = { c, k - c };

        g_array_append_vals (units, pair, 2);
      }
      c = k;
    }

  /* The stretches to turn round, unit by unit. */
  {
    guint n = units->len / 2;
    GArray *out = g_array_new (FALSE, FALSE, sizeof (guint));

    for (guint i = 0; i < n; )
      {
        guint a = i, b = i + 1;

        if (whole)
          b = n;
        else if (embedding[g_array_index (units, guint, 2 * i)] == 0)
          {
            for (guint m = 0; m < g_array_index (units, guint, 2 * i + 1); m++)
              {
                guint c = g_array_index (units, guint, 2 * i) + m;

                g_array_append_val (out, c);
              }
            i++;
            continue;
          }
        else
          {
            /* From this embedding to the last one before anything that
             * is not a space or another embedding. */
            for (guint j = i + 1; j < n; j++)
              {
                guint first = g_array_index (units, guint, 2 * j);

                if (embedding[first] != 0)
                  b = j + 1;
                else if (!g_unichar_isspace (g_utf8_get_char (text + byte_of[first])))
                  break;
              }
          }
        for (guint j = b; j > a; j--)
          for (guint m = 0; m < g_array_index (units, guint, 2 * (j - 1) + 1); m++)
            {
              guint c = g_array_index (units, guint, 2 * (j - 1)) + m;

              g_array_append_val (out, c);
            }
        i = b;
      }
    for (guint m = 0; m < out->len && m < order->len; m++)
      g_array_index (order, guint, m) = g_array_index (out, guint, m);
    g_array_free (out, TRUE);
  }
  g_array_free (units, TRUE);
  return whole;
}

/* The text of one page, laid out the way it looks rather than the way the
 * bytes happen to run: the lines are gathered with where they sit, sorted
 * into columns, and joined into paragraphs where the geometry and the
 * formatting say one carries on.  The pictures go in between the
 * paragraphs where they sat, and the web links stay links. */
static void
read_page (PopplerPage *pp, PdfRead *r, PopplerRectangle *text_box)
{
  char *text = poppler_page_get_text (pp);
  PopplerRectangle *rects = NULL;
  guint n_rects = 0;
  GList *attrs, *l;
  PopplerTextAttributes **at = NULL;
  GArray *lines, *pictures, *links;
  guint8 *drop = NULL;
  gsize n_chars, *byte_of = NULL;
  double page_w = 0, page_h = 0;
  double gutters[3];
  double col_left[4], col_right[4];
  int n_gutters;
  int prev_column = -1;
  const PdfLine *prev = NULL;
  guint next_pic = 0;
  GArray *order = NULL;
  guint *embedding = NULL;      /* the right-to-left embedding of each character */

  if (text_box != NULL)
    {
      text_box->x1 = text_box->y1 = G_MAXDOUBLE;
      text_box->x2 = text_box->y2 = -G_MAXDOUBLE;
    }
  poppler_page_get_size (pp, &page_w, &page_h);
  if (!isfinite (page_w) || !isfinite (page_h))
    page_w = page_h = 0.0;
  pictures = page_pictures (pp);

  /* A page of pictures and no text -- a scan, a plate -- is its pictures. */
  if (text == NULL || *text == '\0')
    {
      for (guint i = 0; i < pictures->len; i++)
        emit_picture (r, &g_array_index (pictures, PdfPicture, i),
                      page_w * 0.1, page_w * 0.9);
      g_array_free (pictures, TRUE);
      g_free (text);
      return;
    }

  if (!poppler_page_get_text_layout (pp, &rects, &n_rects))
    n_rects = 0;
  /* Poppler marks a right-to-left run in the text with the embedding
   * controls, which are not glyphs and have no boxes: every character
   * after the first of them would take the box of the one before.  Out
   * they go, when that is what makes the text and the boxes agree. */
  if ((glong) n_rects != g_utf8_strlen (text, -1))
    {
      GString *plain = g_string_sized_new (strlen (text));
      GArray *marks = g_array_new (FALSE, FALSE, sizeof (guint));
      guint within = 0, next = 0;

      for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
        {
          gunichar c = g_utf8_get_char (p);

          /* Which right-to-left embedding or isolate the character is
           * in, if any: they are what says how the line is read. */
          if (c == 0x202B || c == 0x202E || c == 0x2067)
            within = ++next;
          else if (c == 0x202A || c == 0x202D || c == 0x2066 || c == 0x2068 ||
                   c == 0x202C || c == 0x2069)
            within = 0;
          else
            {
              g_string_append_unichar (plain, c);
              g_array_append_val (marks, within);
            }
        }
      if ((glong) n_rects == g_utf8_strlen (plain->str, -1))
        {
          g_free (text);
          text = g_string_free (plain, FALSE);
          embedding = (guint *) g_array_free (marks, FALSE);
        }
      else
        {
          g_string_free (plain, TRUE);
          g_array_free (marks, TRUE);
        }
    }

  links = page_links (pp, page_h);
  for (gsize i = 0; i < G_N_ELEMENTS (col_right); i++)
    {
      col_left[i] = G_MAXDOUBLE;
      col_right[i] = -G_MAXDOUBLE;
    }
  n_chars = g_utf8_strlen (text, -1);
  byte_of = g_new (gsize, n_chars + 1);
  {
    const char *p = text;

    for (gsize i = 0; i <= n_chars; i++)
      {
        byte_of[i] = (gsize) (p - text);
        if (*p != '\0')
          p = g_utf8_next_char (p);
      }
  }

  /* The formatting of every character, from the spans poppler found. */
  at = g_new0 (PopplerTextAttributes *, n_chars + 1);
  attrs = poppler_page_get_text_attributes (pp);
  for (l = attrs; l != NULL; l = l->next)
    {
      PopplerTextAttributes *a = l->data;

      for (gint i = MAX (a->start_index, 0);
           i <= a->end_index && (gsize) i < n_chars; i++)
        at[i] = a;
    }

  /* Without the glyphs' boxes there is nothing to sort or to split on, so
   * the page comes back as poppler read it, line by line. */
  if (n_rects < n_chars)
    {
      char **row = g_strsplit (text, "\n", -1);

      for (guint k = 0; row[k] != NULL; k++)
        if (row[k][0] != '\0')
          {
            w42_builder_reset_char (&r->b);
            w42_builder_reset_para (&r->b);
            if (r->break_pending)
              {
                r->b.pa.page_break_before = 1;
                r->break_pending = FALSE;
              }
            w42_builder_text (&r->b, row[k]);
            w42_builder_end_paragraph (&r->b);
          }
      g_strfreev (row);
      for (guint i = 0; i < pictures->len; i++)
        emit_picture (r, &g_array_index (pictures, PdfPicture, i),
                      page_w * 0.1, page_w * 0.9);
      g_array_free (pictures, TRUE);
      g_array_free (links, TRUE);
      g_free (embedding);
      g_free (at);
      g_free (byte_of);
      poppler_page_free_text_attributes (attrs);
      g_free (rects);
      g_free (text);
      return;
    }

  n_gutters = find_gutters (text, byte_of, n_chars, rects, n_rects, page_w,
                            gutters, (int) G_N_ELEMENTS (gutters));

  /* The lines, with the box each one covers, broken where poppler's own
   * line runs across a gutter into the next column. */
  lines = g_array_new (FALSE, FALSE, sizeof (PdfLine));
  {
    gsize i = 0;

    while (i < n_chars)
      {
        gsize j = i, seg = i;
        PdfLine line;
        gboolean open = FALSE;

        while (j < n_chars && *(text + byte_of[j]) != '\n')
          j++;

        memset (&line, 0, sizeof line);
        for (gsize k = i; k <= j; k++)
          {
            const PopplerRectangle *rc = k < j && k < n_rects ? &rects[k] : NULL;
            gunichar c = k < j ? g_utf8_get_char (text + byte_of[k]) : ' ';
            int col;

            if (k == j || rc == NULL || rc->x2 <= rc->x1)
              {
                if (k == j && open)
                  {
                    line.start = (guint) seg;
                    line.end = (guint) k;
                    g_array_append_val (lines, line);
                  }
                continue;
              }
            if (g_unichar_isspace (c))
              continue;

            col = column_of ((rc->x1 + rc->x2) / 2.0, gutters, n_gutters);
            if (open && col != line.column)
              {
                /* The column changed part way along: what came before is
                 * a line of its own, and this starts another. */
                line.start = (guint) seg;
                line.end = (guint) k;
                g_array_append_val (lines, line);
                open = FALSE;
                seg = k;
              }
            if (!open)
              {
                open = TRUE;
                seg = k;
                line.column = col;
                line.x1 = rc->x1; line.y1 = rc->y1;
                line.x2 = rc->x2; line.y2 = rc->y2;
              }
            else
              {
                line.x1 = MIN (line.x1, rc->x1);
                line.y1 = MIN (line.y1, rc->y1);
                line.x2 = MAX (line.x2, rc->x2);
                line.y2 = MAX (line.y2, rc->y2);
              }
          }
        i = j + 1;
      }
  }

  g_array_sort (lines, line_cmp);

  /* The running header and footer, and the page numbers, are not the
   * text: they go, or become the document's own. */
  for (guint i = 0; i < lines->len; )
    {
      const PdfLine *pl = &g_array_index (lines, PdfLine, i);
      char *words = g_strndup (text + byte_of[pl->start], byte_of[pl->end] - byte_of[pl->start]);
      gboolean running = drop_running (r, words, pl->x1, pl->x2, (pl->y1 + pl->y2) / 2.0,
                                       page_w, page_h);

      g_free (words);
      if (running)
        g_array_remove_index (lines, i);
      else
        i++;
    }

  /* Where each column ends: a line that ran to it was wrapped, and one
   * that stopped short of it ended its paragraph.  Not the very widest
   * line's edges -- one stop hung out in the margin would move the edge
   * for all the rest -- but the furthest out that two lines agree on. */
  for (int c = 0; c < (int) G_N_ELEMENTS (col_right); c++)
    {
      GArray *lefts = g_array_new (FALSE, FALSE, sizeof (double));
      GArray *rights = g_array_new (FALSE, FALSE, sizeof (double));

      for (guint i = 0; i < lines->len; i++)
        {
          const PdfLine *pl = &g_array_index (lines, PdfLine, i);

          if (CLAMP (pl->column, 0, (int) G_N_ELEMENTS (col_right) - 1) != c)
            continue;
          g_array_append_val (lefts, pl->x1);
          g_array_append_val (rights, pl->x2);
        }
      if (lefts->len > 0)
        {
          g_array_sort (lefts, double_cmp);
          g_array_sort (rights, double_cmp);
          col_left[c] = agreed_edge (lefts, FALSE);
          col_right[c] = agreed_edge (rights, TRUE);
        }
      g_array_free (lefts, TRUE);
      g_array_free (rights, TRUE);
    }
  for (guint i = 0; i < lines->len && text_box != NULL; i++)
    {
      const PdfLine *pl = &g_array_index (lines, PdfLine, i);

      text_box->x1 = MIN (text_box->x1, pl->x1);
      text_box->y1 = MIN (text_box->y1, pl->y1);
      text_box->x2 = MAX (text_box->x2, pl->x2);
      text_box->y2 = MAX (text_box->y2, pl->y2);
    }

  /* Each picture in the column it sits in, in the order it comes down. */
  for (guint i = 0; i < pictures->len; i++)
    {
      PdfPicture *pic = &g_array_index (pictures, PdfPicture, i);

      pic->column = CLAMP (column_of ((pic->x1 + pic->x2) / 2.0, gutters, n_gutters),
                           0, (int) G_N_ELEMENTS (col_right) - 1);
    }
  g_array_sort (pictures, picture_cmp);

  drop = g_new0 (guint8, n_chars + 1);
  for (guint i = 0; i < lines->len; i++)
    {
      const PdfLine *pl = &g_array_index (lines, PdfLine, i);

      close_tracking (text, byte_of, rects, n_rects, pl->start, pl->end, drop);
    }
  order = g_array_new (FALSE, FALSE, sizeof (guint));
  for (guint i = 0; i < lines->len; i++)
    {
      PdfLine *pl = &g_array_index (lines, PdfLine, i);

      pl->rtl = reading_order (text, byte_of, embedding, pl->start, pl->end, order);
    }

  for (guint i = 0; i < lines->len; i++)
    {
      const PdfLine *cur = &g_array_index (lines, PdfLine, i);
      double height = cur->y2 - cur->y1;
      gboolean para = FALSE;
      int col = CLAMP (cur->column, 0, (int) G_N_ELEMENTS (col_right) - 1);
      double gap = prev != NULL && cur->column == prev_column ? cur->y1 - prev->y2 : 0.0;

      if (prev == NULL || cur->column != prev_column)
        para = TRUE;
      else
        {
          const PopplerTextAttributes *a = at[cur->start];
          const PopplerTextAttributes *pa = at[prev->start];
          /* A gap taller than half a line, a step in from the margin, or a
           * change of type: each of them ends a paragraph. */
          double measure = col_right[col] - col_left[col];
          /* The line before it stopped short of the column's edge, so it
           * was the end of something rather than a line that wrapped. */
          gboolean prev_short = measure > 1.0 &&
                                prev->x2 < col_right[col] - measure * 0.12;
          /* A bullet or a number that a PDF put on a line of its own
           * belongs to the item beside it. */
          gboolean prev_marker = prev->end - prev->start <= 3 &&
                                 starts_item (text + byte_of[prev->start]);

          if (gap > height * 0.6)
            para = TRUE;
          else if (cur->rtl != prev->rtl)
            para = TRUE;                  /* the other way of writing */
          else if (starts_item (text + byte_of[cur->start]))
            para = TRUE;
          else if (cur->x1 < prev->x1 - 6.0 && (r->geom.n_lines > 1 || prev_short))
            /* Back out to the margin -- but not from a first line that
             * was set in and ran to the edge: that is a paragraph's
             * first-line indent, and this is the paragraph going on. */
            para = TRUE;
          else if (cur->x1 > prev->x1 + 6.0 && !prev_marker &&
                   (prev_short || (prev->x2 < col_right[col] - 3.0 && !r->para_is_item)))
            /* A first line, set in, after one that stopped short of the
             * edge -- unless this is an item whose lines hang beside its
             * bullet. */
            para = TRUE;
          else if (a != NULL && pa != NULL &&
                   (ABS (a->font_size - pa->font_size) > 0.6 ||
                    g_strcmp0 (a->font_name, pa->font_name) != 0))
            para = TRUE;
          else if (r->geom.n_lines > 0 && ABS (cur->x2 - prev->x2) > 3.0 &&
                   r->geom.centred == r->geom.n_lines && prev_short &&
                   ABS ((cur->x1 - col_left[col]) - (col_right[col] - cur->x2)) > MAX (2.5, measure * 0.015))
            para = TRUE;                  /* centred lines, then one that is not */
          else
            {
              /* A sentence that ended, followed by one that begins. */
              const PdfChunk *last = r->chunks->len > 0
                ? &g_array_index (r->chunks, PdfChunk, r->chunks->len - 1) : NULL;

              if (last != NULL && last->text->len > 0 && !prev_marker &&
                  prev->x2 < col_right[col] - 3.0 &&
                  ends_sentence (last->text) &&
                  starts_sentence (g_utf8_get_char (text + byte_of[cur->start])))
                para = TRUE;
            }
        }

      if (para)
        {
          flush_para (r);
          r->para_is_item = starts_item (text + byte_of[cur->start]);
          /* The pictures that sat above this paragraph go in before it. */
          while (next_pic < pictures->len)
            {
              const PdfPicture *pic = &g_array_index (pictures, PdfPicture, next_pic);

              if (pic->column > cur->column ||
                  (pic->column == cur->column && pic->y1 >= cur->y1))
                break;
              emit_picture (r, pic, col_left[pic->column], col_right[pic->column]);
              next_pic++;
            }
          /* A gap wider than a line's leading is space above the
           * paragraph. */
          if (gap > height * 0.6)
            r->geom.space_before = MIN (gap - height * 0.25, 72.0);
        }
      else if (r->chunks->len > 0)
        {
          /* Joined to the line before it: a hyphen the typesetter put in
           * goes away, anything else takes a space. */
          PdfChunk *last = &g_array_index (r->chunks, PdfChunk, r->chunks->len - 1);

          if (last->text->len >= 2 &&
              last->text->str[last->text->len - 1] == '-' &&
              g_ascii_isalpha (last->text->str[last->text->len - 2]))
            g_string_truncate (last->text, last->text->len - 1);
          else if (last->text->len > 0 &&
                   last->text->str[last->text->len - 1] != ' ')
            g_string_append_c (last->text, ' ');
        }
      geom_add_line (&r->geom, cur, col_left[col], col_right[col]);

      /* The line's own text, in the order it is read, one chunk per span
       * of formatting. */
      if (reading_order (text, byte_of, embedding, cur->start, cur->end, order))
        r->geom.rtl++;
      for (guint oi = 0; oi < order->len; oi++)
        {
          guint c = g_array_index (order, guint, oi);
          PopplerTextAttributes *a = at[c];
          PdfChunk *last = r->chunks->len > 0
            ? &g_array_index (r->chunks, PdfChunk, r->chunks->len - 1) : NULL;
          W42CharFmt want;
          W42Fmt def;

          if (drop[c])
            continue;
          w42_fmt_init_default (&def);
          want = def.ch;
          if (a != NULL)
            {
              gboolean bold = FALSE, italic = FALSE;
              char *family = pdf_family (a->font_name, &bold, &italic);

              if (family != NULL)
                want.family = g_intern_string (family);
              g_free (family);
              want.bold = bold;
              want.italic = italic;
              want.underline = a->is_underlined ? W42_UNDERLINE_SINGLE
                                                : W42_UNDERLINE_NONE;
              if (a->font_size > 1.0)
                want.size = CLAMP ((int) (a->font_size * 2.0 + 0.5), 2, 3276);
              want.color = ((guint32) (a->color.red   >> 8) << 16) |
                           ((guint32) (a->color.green >> 8) << 8) |
                            (guint32) (a->color.blue  >> 8);
            }
          if (c < n_rects && links->len > 0)
            want.link = link_at (links, &rects[c]);

          if (last == NULL || memcmp (&last->ch, &want, sizeof want) != 0)
            {
              PdfChunk fresh;

              fresh.text = g_string_new (NULL);
              fresh.ch = want;
              g_array_append_val (r->chunks, fresh);
              last = &g_array_index (r->chunks, PdfChunk, r->chunks->len - 1);
            }
          g_string_append_len (last->text, text + byte_of[c],
                               (gssize) (byte_of[c + 1] - byte_of[c]));
        }

      prev = cur;
      prev_column = cur->column;
    }

  flush_para (r);
  for (; next_pic < pictures->len; next_pic++)
    {
      const PdfPicture *pic = &g_array_index (pictures, PdfPicture, next_pic);
      double left = col_left[pic->column], right = col_right[pic->column];

      if (left >= right)
        {
          left = page_w * 0.1;
          right = page_w * 0.9;
        }
      emit_picture (r, pic, left, right);
    }

  g_array_free (lines, TRUE);
  g_array_free (order, TRUE);
  g_free (embedding);
  g_array_free (pictures, TRUE);
  g_array_free (links, TRUE);
  g_free (drop);
  g_free (at);
  g_free (byte_of);
  poppler_page_free_text_attributes (attrs);
  g_free (rects);
  g_free (text);
}

/* The PDF's own title, author, subject and keywords are the document's
 * Summary Info. */
static void
read_info (PopplerDocument *document, W42PieceTable *pt)
{
  W42DocInfo info;
  char *title = poppler_document_get_title (document);
  char *author = poppler_document_get_author (document);
  char *subject = poppler_document_get_subject (document);
  char *keywords = poppler_document_get_keywords (document);

  memset (&info, 0, sizeof info);
  info.title = title;
  info.author = author;
  info.subject = subject;
  info.keywords = keywords;
  w42_pt_set_info (pt, &info);
  g_free (title);
  g_free (author);
  g_free (subject);
  g_free (keywords);
}

/* The pages, read for what is on them: any PDF, and one of Word42's
 * whose document inside has gone.  Each page after the first begins with
 * a page break, so that the pages come out where they were for as long
 * as the text still fits them. */
static void
read_pages (PopplerDocument *document, W42PieceTable *pt, W42PageSetup *page)
{
  PdfRead r;
  PopplerRectangle text_box = { 0, 0, 0, 0 };
  PopplerIndexIter *outline;
  int n_pages;

  memset (&r, 0, sizeof r);
  n_pages = poppler_document_get_n_pages (document);
  r.n_pages = n_pages;
  w42_builder_init (&r.b, pt);
  find_running (document, &r.running);
  r.chunks = g_array_new (FALSE, FALSE, sizeof (PdfChunk));
  g_array_set_clear_func (r.chunks, chunk_clear);
  read_info (document, pt);

  outline = poppler_index_iter_new (document);
  if (outline != NULL)
    {
      guint budget = 100000;

      r.headings = g_ptr_array_new_with_free_func (headings_free);
      read_outline (document, outline, 1, r.headings, &budget);
      poppler_index_iter_free (outline);
    }

  for (int i = 0; i < n_pages; i++)
    {
      PopplerPage *pp = poppler_document_get_page (document, i);

      if (pp == NULL)
        continue;
      r.page = i;
      r.break_pending = i > 0;

      if (i == 0 && page != NULL)
        {
          double w = 0, h = 0;

          /* The first page's size becomes the document's, so that the
           * pagination matches the original as closely as the text allows. */
          /* The size is the attacker's to declare, so it is clamped to the
           * range the other readers allow before the cast can overflow. */
          poppler_page_get_size (pp, &w, &h);
          if (isfinite (w) && isfinite (h) && w > 0 && h > 0)
            {
              page->width  = (int) CLAMP (w * 20.0, 720.0, 31680.0);
              page->height = (int) CLAMP (h * 20.0, 720.0, 31680.0);
            }
        }

      read_page (pp, &r, i == 0 ? &text_box : NULL);

      /* And the margins are where the first page's text sits on it. */
      if (i == 0 && page != NULL && text_box.x2 > text_box.x1 &&
          isfinite (text_box.x1) && isfinite (text_box.x2) &&
          isfinite (text_box.y1) && isfinite (text_box.y2))
        {
          double w = page->width / 20.0, h = page->height / 20.0;

          page->margin_left   = (int) CLAMP (text_box.x1 * 20.0, 0.0, w * 8.0);
          page->margin_right  = (int) CLAMP ((w - text_box.x2) * 20.0, 0.0, w * 8.0);
          page->margin_top    = (int) CLAMP (text_box.y1 * 20.0, 0.0, h * 8.0);
          page->margin_bottom = (int) CLAMP ((h - text_box.y2) * 20.0, 0.0, h * 8.0);
        }

      g_object_unref (pp);
    }

  w42_builder_finish (&r.b);
  g_array_free (r.chunks, TRUE);
  if (r.headings != NULL)
    g_ptr_array_free (r.headings, TRUE);
  for (int z = 0; z < 2; z++)
    for (int k = 0; k < 2; k++)
      g_free (r.running.key[z][k]);
}

/* ---- the document Word42 kept inside ---------------------------------- */

static gboolean
gather_bytes (const gchar *buf, gsize count, gpointer data, GError **error)
{
  GByteArray *bytes = data;

  /* A document is a few megabytes, and a gigabyte is not one. */
  if (bytes->len + count > (gsize) 1 << 30)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "too large");
      return FALSE;
    }
  g_byte_array_append (bytes, (const guint8 *) buf, (guint) count);
  return TRUE;
}

/* A PDF Word42 saved carries the document it was made from.  That is
 * read, as it was, rather than the pages: every style, table, note and
 * field back where it was. */
static gboolean
read_source (PopplerDocument *document, W42PieceTable *pt, W42PageSetup *page,
             W42PdfOptions *options)
{
  char *creator = poppler_document_get_creator (document);
  gboolean ours = creator != NULL && g_str_has_prefix (creator, "Word42");
  gboolean done = FALSE;
  GList *list;

  g_free (creator);
  if (!ours || !poppler_document_has_attachments (document))
    return FALSE;

  list = poppler_document_get_attachments (document);
  for (GList *l = list; l != NULL && !done; l = l->next)
    {
      PopplerAttachment *a = l->data;
      GByteArray *gathered;
      GBytes *bytes;
#if POPPLER_CHECK_VERSION (20, 9, 0)
      const char *name = poppler_attachment_get_name (a);
      const char *desc = poppler_attachment_get_description (a);
      const GString *checksum = poppler_attachment_get_checksum (a);
#else
      const char *name = a->name;
      const char *desc = a->description;
      const GString *checksum = a->checksum;
#endif

      if (g_strcmp0 (desc, SOURCE_DESC) != 0 || name == NULL ||
          !g_str_has_suffix (name, ".odt"))
        continue;
      gathered = g_byte_array_new ();
      if (!poppler_attachment_save_to_callback (a, gather_bytes, gathered, NULL))
        {
          g_byte_array_free (gathered, TRUE);
          continue;
        }
      bytes = g_byte_array_free_to_bytes (gathered);

      /* The checksum the file was written with says the document came
       * through whole. */
      if (checksum != NULL && checksum->len == 16)
        {
          GChecksum *sum = g_checksum_new (G_CHECKSUM_MD5);
          guint8 md5[16];
          gsize md5_len = sizeof md5;

          g_checksum_update (sum, g_bytes_get_data (bytes, NULL),
                             (gssize) g_bytes_get_size (bytes));
          g_checksum_get_digest (sum, md5, &md5_len);
          g_checksum_free (sum);
          if (memcmp (md5, checksum->str, 16) != 0)
            {
              g_bytes_unref (bytes);
              continue;
            }
        }
      done = w42_odt_load_bytes (pt, page, bytes, NULL);
      g_bytes_unref (bytes);

#if POPPLER_CHECK_VERSION (20, 9, 0)
      /* The PDF's own date of change and the document's are written as
       * one; another program that changed the PDF changed only its. */
      if (done && options != NULL)
        {
          GDateTime *changed = poppler_document_get_modification_date_time (document);
          GDateTime *kept = poppler_attachment_get_mtime (a);

          if (changed != NULL && kept != NULL &&
              ABS (g_date_time_difference (changed, kept)) > 2 * G_TIME_SPAN_SECOND)
            options->source_stale = TRUE;
          if (changed != NULL)
            g_date_time_unref (changed);
        }
#endif
    }
  g_list_free_full (list, g_object_unref);
  return done;
}

static PopplerDocument *
open_document (GFile *file, const char *password, GError **error)
{
  GError *local = NULL;
  PopplerDocument *document = poppler_document_new_from_gfile (file, password, NULL, &local);

  if (document == NULL)
    {
      if (g_error_matches (local, POPPLER_ERROR, POPPLER_ERROR_ENCRYPTED))
        {
          g_set_error_literal (error, W42_PDF_ERROR, W42_PDF_ERROR_PASSWORD,
                               password != NULL ? _("The password is not correct.")
                                                : _("The PDF is protected with a password."));
          g_error_free (local);
        }
      else
        g_propagate_error (error, local);
    }
  return document;
}

gboolean
w42_pdf_probe (GFile *file, const char *password, gboolean *restricted, GError **error)
{
  PopplerDocument *document;

  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (restricted != NULL)
    *restricted = FALSE;
  document = open_document (file, password != NULL && *password != '\0' ? password : NULL, error);
  if (document == NULL)
    return FALSE;
  if (restricted != NULL)
    *restricted = (poppler_document_get_permissions (document) &
                   POPPLER_PERMISSIONS_OK_TO_MODIFY) == 0;
  g_object_unref (document);
  return TRUE;
}

gboolean
w42_pdf_import_with (W42PieceTable *pt,
                     W42PageSetup  *page,
                     GFile         *file,
                     W42PdfOptions *options,
                     GError       **error)
{
  PopplerDocument *document;
  const char *password = NULL;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (options != NULL)
    {
      /* The password to modify, when there is one, opens everything the
       * password to open does, and lifts the file's restrictions. */
      password = options->modify_password != NULL ? options->modify_password
                                                  : options->open_password;
      options->read_source = FALSE;
      options->source_stale = FALSE;
      options->restricted = FALSE;
      options->n_signatures = 0;
    }

  document = open_document (file, password, error);
  if (document == NULL)
    return FALSE;

  if (options != NULL)
    {
      options->restricted = (poppler_document_get_permissions (document) &
                             POPPLER_PERMISSIONS_OK_TO_MODIFY) == 0;
#if POPPLER_CHECK_VERSION (21, 12, 0)
      options->n_signatures = poppler_document_get_n_signatures (document);
#endif
    }

  if (read_source (document, pt, page, options))
    {
      if (options != NULL)
        options->read_source = TRUE;
    }
  else
    read_pages (document, pt, page);

  g_object_unref (document);
  w42_pt_clear_undo (pt);
  return TRUE;
}

#else  /* !HAVE_POPPLER */

gboolean
w42_pdf_probe (GFile *file, const char *password, gboolean *restricted, GError **error)
{
  (void) file; (void) password; (void) error;
  if (restricted != NULL)
    *restricted = FALSE;
  return TRUE;
}

gboolean
w42_pdf_import_with (W42PieceTable *pt,
                     W42PageSetup  *page,
                     GFile         *file,
                     W42PdfOptions *options,
                     GError       **error)
{
  (void) pt; (void) page; (void) file; (void) options;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               /* Translators: poppler is the name of a program library;
                * keep it as it is. */
               _("This build of Word42 cannot read PDF files. "
                 "It was built without poppler."));
  return FALSE;
}

#endif

gboolean
w42_pdf_import (W42PieceTable *pt,
                W42PageSetup  *page,
                GFile         *file,
                GError       **error)
{
  return w42_pdf_import_with (pt, page, file, NULL, error);
}
