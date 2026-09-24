/* w42-io.c - see w42-io.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-io.h"

#include "w42-doc.h"
#include "w42-html.h"
#include "w42-htmlin.h"
#include "w42-docx.h"
#include "w42-pptx.h"
#include "w42-abw.h"
#include "w42-odt.h"
#include "w42-pdf.h"
#include "w42-rtf.h"

#include <string.h>

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

  g_free (name);
  return format;
}

gboolean
w42_io_format_round_trips (GFile *file)
{
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  switch (w42_io_guess_format (file))
    {
    case W42_FORMAT_DOC:
    case W42_FORMAT_PDF:
    case W42_FORMAT_HTML:
    case W42_FORMAT_PPTX:
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

gboolean
w42_io_load (W42PieceTable *pt, W42PageSetup *page, GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  char *utf8 = NULL;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

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

    switch (w42_io_guess_format (file))
      {
      case W42_FORMAT_RTF:  ok = w42_rtf_load (pt, page, file, error); break;
      case W42_FORMAT_PDF:  ok = w42_pdf_import (pt, page, file, error); break;
      case W42_FORMAT_DOC:  ok = w42_doc_load (pt, page, file, error); break;
      case W42_FORMAT_HTML: ok = w42_html_import (pt, page, file, error); break;
      case W42_FORMAT_DOCX: ok = w42_docx_load (pt, page, file, error); break;
      case W42_FORMAT_ABW:  ok = w42_abw_load (pt, page, file, error); break;
      case W42_FORMAT_ODT:  ok = w42_odt_load (pt, page, file, error); break;
      case W42_FORMAT_PPTX: ok = w42_pptx_load (pt, page, file, error); break;
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

  if (g_utf8_validate (contents, length, NULL))
    {
      utf8 = g_strndup (contents, length);
    }
  else
    {
      /* Word 97 wrote Windows-1252, so that is the sensible fallback for a
       * text file that is not valid UTF-8. */
      utf8 = g_convert (contents, length, "UTF-8", "WINDOWS-1252",
                        NULL, NULL, NULL);
      if (utf8 == NULL)
        utf8 = g_locale_to_utf8 (contents, length, NULL, NULL, NULL);
    }

  g_free (contents);

  if (utf8 == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "The file is not text in any encoding Word42 recognises.");
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
      return w42_pdf_export (pt, page, file, error);
    case W42_FORMAT_HTML:
      return w42_html_export (pt, page, file, error);
    case W42_FORMAT_DOCX:
      return w42_docx_save (pt, page, file, error);
    case W42_FORMAT_ABW:
      return w42_abw_save (pt, page, file, error);
    case W42_FORMAT_PPTX:
      return w42_pptx_save (pt, page, file, error);
    case W42_FORMAT_ODT:
      return w42_odt_save (pt, page, file, error);
    case W42_FORMAT_DOC:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Word42 reads Word .doc files but does not write them. "
                   "Save as RTF or .docx, which other word processors read.");
      return FALSE;
    default:
      break;
    }

  first = w42_pt_first_caret_pos (pt);
  text = w42_pt_get_text (pt, first, w42_pt_length (pt) - first);

  ok = g_file_replace_contents (file, text, strlen (text), NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);

  g_free (text);
  return ok;
}
