/* w42-io.c - see w42-io.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-io.h"

#include "w42-doc.h"
#include "w42-epub.h"
#include "w42-html.h"
#include "w42-htmlin.h"
#include "w42-docx.h"
#include "w42-pptx.h"
#include "w42-abw.h"
#include "w42-odt.h"
#include "w42-pdf.h"
#include "w42-rtf.h"
#include "w42-wpd.h"
#include "w42-latex.h"
#include "w42-mathtex.h"

#include <string.h>
#include <glib/gi18n.h>

W42Format
w42_io_guess_format (GFile *file)
{
  char *name;
  W42Format format = W42_FORMAT_TEXT;

  g_return_val_if_fail (G_IS_FILE (file), W42_FORMAT_UNKNOWN);

  name = g_file_get_basename (file);
  if (name == NULL)
    return W42_FORMAT_UNKNOWN;
  /* Windows files come in any case. */
  {
    char *lower = g_ascii_strdown (name, -1);
    g_free (name);
    name = lower;
  }

  if (g_str_has_suffix (name, ".rtf"))
    format = W42_FORMAT_RTF;
  else if (g_str_has_suffix (name, ".pdf"))
    format = W42_FORMAT_PDF;
  else if (g_str_has_suffix (name, ".doc"))
    format = W42_FORMAT_DOC;
  else if (g_str_has_suffix (name, ".html") || g_str_has_suffix (name, ".htm"))
    format = W42_FORMAT_HTML;
  else if (g_str_has_suffix (name, ".docx"))
    format = W42_FORMAT_DOCX;
  else if (g_str_has_suffix (name, ".abw") || g_str_has_suffix (name, ".zabw"))
    format = W42_FORMAT_ABW;
  else if (g_str_has_suffix (name, ".odt"))
    format = W42_FORMAT_ODT;
  else if (g_str_has_suffix (name, ".pptx") || g_str_has_suffix (name, ".ppsx"))
    format = W42_FORMAT_PPTX;
  else if (g_str_has_suffix (name, ".epub"))
    format = W42_FORMAT_EPUB;
  else if (g_str_has_suffix (name, ".wpd") || g_str_has_suffix (name, ".wp") ||
           g_str_has_suffix (name, ".wp5") || g_str_has_suffix (name, ".wp6") ||
           g_str_has_suffix (name, ".wp7"))
    format = W42_FORMAT_WPD;
  else if (g_str_has_suffix (name, ".tex") || g_str_has_suffix (name, ".latex"))
    format = W42_FORMAT_LATEX;

  g_free (name);
  return format;
}

gboolean
w42_io_format_round_trips (GFile *file)
{
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  switch (w42_io_guess_format (file))
    {
    case W42_FORMAT_LATEX:
    case W42_FORMAT_HTML:
    case W42_FORMAT_PPTX:
    case W42_FORMAT_EPUB:
      return FALSE;
    default:
      return TRUE;
    }
}

/* Whatever a file said about its page, the page is one that can be laid
 * out: a sheet between an inch and seventy inches a side, margins that
 * leave at least an inch of text between them, and up to six columns.
 * Every reader's geometry comes through here, so no format has to clamp
 * for itself. */
void
w42_page_setup_sanitize (W42PageSetup *page)
{
  if (page == NULL)
    return;
  if (page->width <= 0 || page->height <= 0)
    {
      page->width = 12240;
      page->height = 15840;
    }
  page->width  = CLAMP (page->width, 1440, 100800);
  page->height = CLAMP (page->height, 1440, 100800);
  page->margin_left   = CLAMP (page->margin_left, 0, page->width / 2 - 720);
  page->margin_right  = CLAMP (page->margin_right, 0, page->width / 2 - 720);
  page->margin_top    = CLAMP (page->margin_top, 0, page->height / 2 - 720);
  page->margin_bottom = CLAMP (page->margin_bottom, 0, page->height / 2 - 720);
  page->columns    = CLAMP (page->columns, 0, 6);
  page->column_gap = CLAMP (page->column_gap, 0, page->width / 2);
  /* The page border stays on the paper, short of the middle. */
  page->border_space = CLAMP (page->border_space, 0, MIN (page->width, page->height) / 4);
  if (page->border_style > 3)
    page->border_style = 0;
}

/* Windows-1252's 0x80 to 0x9F.  Five of them are not assigned, and
 * iconv refuses a file that has one; they stand as U+FFFD instead. */
static const gunichar CP1252_HIGH[32] = {
  0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
  0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
  0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178
};

static char *
cp1252_to_utf8 (const char *bytes, gsize length)
{
  GString *out = g_string_sized_new (length + length / 4);

  for (gsize i = 0; i < length; i++)
    {
      guchar c = (guchar) bytes[i];

      if (c >= 0x80 && c <= 0x9F)
        g_string_append_unichar (out, CP1252_HIGH[c - 0x80]);
      else
        g_string_append_unichar (out, c);
    }
  return g_string_free (out, FALSE);
}

/* A text file's bytes as UTF-8: a byte-order mark says UTF-8 or UTF-16
 * and is not text; otherwise UTF-8 if the bytes are, then Word 97's
 * Windows-1252, then the locale's.  What is left is made fit for a
 * paragraph: a form feed ends one, a vertical tab is a line break, and
 * the other control characters -- a NUL above all, which would end the
 * text there -- are not text at all. */
static char *
text_to_utf8 (const char *contents, gsize length)
{
  char *utf8 = NULL;
  gsize utf8_len = 0;
  GString *clean;

  if (length >= 2 && ((guchar) contents[0] == 0xFF || (guchar) contents[0] == 0xFE) &&
      (guchar) contents[1] == ((guchar) contents[0] ^ 0x01))
    {
      /* An odd byte at the end is half a character: dropped.  A NUL
       * comes through as one, and is dropped below with the rest. */
      gsize n = (length - 2) & ~(gsize) 1;

      utf8 = g_convert (contents + 2, n, "UTF-8",
                        (guchar) contents[0] == 0xFF ? "UTF-16LE" : "UTF-16BE",
                        NULL, &utf8_len, NULL);
    }
  else
    {
      /* A NUL is not text, and would make UTF-8 look like something
       * else: it goes before the bytes are judged. */
      char *bytes = g_malloc (length + 1);
      gsize n = 0;

      if (length >= 3 && memcmp (contents, "\357\273\277", 3) == 0)
        {
          contents += 3;
          length -= 3;
        }
      for (gsize i = 0; i < length; i++)
        if (contents[i] != '\0')
          bytes[n++] = contents[i];
      bytes[n] = '\0';
      if (g_utf8_validate_len (bytes, n, NULL))
        utf8 = g_strndup (bytes, n);
      else
        {
          utf8 = g_convert (bytes, n, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL);
          if (utf8 == NULL)
            utf8 = g_locale_to_utf8 (bytes, n, NULL, NULL, NULL);
          if (utf8 == NULL)
            utf8 = cp1252_to_utf8 (bytes, n);
        }
      g_free (bytes);
    }
  if (utf8 == NULL)
    return NULL;
  if (utf8_len == 0)
    utf8_len = strlen (utf8);

  clean = g_string_sized_new (utf8_len);
  for (const char *p = utf8; p < utf8 + utf8_len; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == '\f')
        g_string_append_c (clean, '\n');
      else if (c == '\v')
        g_string_append_unichar (clean, 0x2028);
      else if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7F)
        continue;
      else
        g_string_append_len (clean, p, g_utf8_next_char (p) - p);
    }
  g_free (utf8);
  return g_string_free (clean, FALSE);
}

gboolean
w42_io_load (W42PieceTable *pt, W42PageSetup *page, GFile *file, GError **error)
{
  return w42_io_load_with (pt, page, file, NULL, error);
}

gboolean
w42_io_load_with (W42PieceTable *pt, W42PageSetup *page, GFile *file,
                  W42PdfOptions *pdf, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  char *utf8 = NULL;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  /* An e-book is XHTML in a zip, which as text would be a screenful of
   * binary; and it is refused before anything of the window's page is
   * touched, since nothing is going to be loaded. */
  if (w42_io_guess_format (file) == W42_FORMAT_EPUB)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   _("Word42 writes EPUB but does not read it."));
      return FALSE;
    }
  if (w42_io_guess_format (file) == W42_FORMAT_LATEX)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   _("Word42 writes LaTeX but does not read it."));
      return FALSE;
    }

  /* A file says what colour its page is and whether it has a border, or
   * says nothing; either way the page it is read into does not keep the
   * last document's, since a window's document is loaded into again. */
  if (page != NULL)
    {
      page->has_background = 0;
      page->background = 0;
      page->has_border = 0;
      page->border_style = 0;
      page->border_width = 0;
      page->border_space = 0;
      page->border_color = 0;
    }

  {
    gboolean ok = FALSE, handled = TRUE;
    W42Format format = w42_io_guess_format (file);

    /* A WordPerfect document is one whatever it is called: DOS
     * WordPerfect's were as often .doc as anything. */
    {
      GFileInputStream *in = g_file_read (file, NULL, NULL);
      guint8 head[16];
      gsize got = 0;

      if (in != NULL)
        {
          g_input_stream_read_all (G_INPUT_STREAM (in), head, sizeof head, &got, NULL, NULL);
          g_object_unref (in);
          if (w42_wpd_sniff (head, got))
            format = W42_FORMAT_WPD;
        }
    }

    switch (format)
      {
      case W42_FORMAT_RTF:  ok = w42_rtf_load (pt, page, file, error); break;
      case W42_FORMAT_PDF:  ok = w42_pdf_import_with (pt, page, file, pdf, error); break;
      case W42_FORMAT_DOC:  ok = w42_doc_load (pt, page, file, error); break;
      case W42_FORMAT_HTML: ok = w42_html_import (pt, page, file, error); break;
      case W42_FORMAT_DOCX: ok = w42_docx_load (pt, page, file, error); break;
      case W42_FORMAT_ABW:  ok = w42_abw_load (pt, page, file, error); break;
      case W42_FORMAT_ODT:  ok = w42_odt_load (pt, page, file, error); break;
      case W42_FORMAT_PPTX: ok = w42_pptx_load (pt, page, file, error); break;
      case W42_FORMAT_WPD:  ok = w42_wpd_load (pt, page, file, error); break;
      default: handled = FALSE; break;
      }
    if (handled)
      {
        if (ok)
          w42_page_setup_sanitize (page);
        return ok;
      }
  }

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return FALSE;

  utf8 = text_to_utf8 (contents, length);
  g_free (contents);

  if (utf8 == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   _("The file is not text in any encoding Word42 recognises."));
      return FALSE;
    }

  w42_pt_load_text (pt, utf8);
  g_free (utf8);
  w42_page_setup_sanitize (page);

  return TRUE;
}

gboolean
w42_io_save (W42PieceTable *pt, const W42PageSetup *page,
             GFile *file, GError **error)
{
  return w42_io_save_with (pt, page, file, NULL, error);
}

/* An equation in a text file: as it reads, x = (−b ± √(b^2 − 4ac))/(2a). */
static char *
equation_text (W42ObjectIdx idx, gpointer data)
{
  const W42Object *object = w42_object_table_get (w42_pt_object_table (data), idx);
  W42MathNode *root;
  char *text;

  if (object == NULL || object->mathml == NULL ||
      (root = w42_math_parse (object->mathml, -1, NULL)) == NULL)
    return NULL;
  text = w42_mathml_to_text (root);
  w42_math_node_free (root);
  return text;
}

gboolean
w42_io_save_with (W42PieceTable *pt, const W42PageSetup *page,
                  GFile *file, const W42PdfOptions *pdf, GError **error)
{
  char *text;
  gsize first;
  gboolean ok;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  switch (w42_io_guess_format (file))
    {
    case W42_FORMAT_RTF:
      return w42_rtf_save (pt, page, file, error);
    case W42_FORMAT_PDF:
      if (pdf == NULL)
        {
          W42PdfOptions *saved = w42_pdf_options_new ();

          saved->keep_document = TRUE;
          ok = w42_pdf_export_with (pt, page, file, saved, error);
          w42_pdf_options_free (saved);
          return ok;
        }
      return w42_pdf_export_with (pt, page, file, pdf, error);
    case W42_FORMAT_HTML:
      return w42_html_export (pt, page, file, error);
    case W42_FORMAT_EPUB:
      return w42_epub_export (pt, page, file, error);
    case W42_FORMAT_DOCX:
      return w42_docx_save (pt, page, file, error);
    case W42_FORMAT_ABW:
      return w42_abw_save (pt, page, file, error);
    case W42_FORMAT_PPTX:
      return w42_pptx_save (pt, page, file, error);
    case W42_FORMAT_ODT:
      return w42_odt_save (pt, page, file, error);
    case W42_FORMAT_DOC:
      return w42_doc_save (pt, page, file, error);
    case W42_FORMAT_WPD:
      return w42_wpd_save (pt, page, file, error);
    case W42_FORMAT_LATEX:
      return w42_latex_export (pt, page, file, error);
    default:
      break;
    }

  first = w42_pt_first_caret_pos (pt);
  text = w42_pt_get_text_with (pt, first, w42_pt_length (pt) - first, equation_text, pt);

  /* A line break inside a paragraph is U+2028 to the model and a new
   * line to a text file; left as it is, other editors show a box.  In
   * one pass: searched for from the top and the rest moved up for each,
   * a file that is all line breaks took minutes to save. */
  {
    char *to = text;

    for (const char *from = text; *from != '\0'; )
      if (strncmp (from, "\342\200\250", 3) == 0)
        {
          *to++ = '\n';
          from += 3;
        }
      else
        *to++ = *from++;
    *to = '\0';
  }

  ok = g_file_replace_contents (file, text, strlen (text), NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);

  g_free (text);
  return ok;
}
