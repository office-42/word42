/* w42-pdf.c - see w42-pdf.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-pdf.h"

#include "w42-image.h"
#include "w42-layout.h"
#include "w42-object.h"

#include <cairo-pdf.h>
#include <string.h>

#ifdef HAVE_POPPLER
#include <poppler.h>
#endif

/* The layout works in pixels at 96 dpi; PDF works in points. */
#define PX_TO_POINTS (72.0 / W42_LAYOUT_DPI)

/* ====================================================================== */
/* Export                                                                  */
/* ====================================================================== */

static cairo_status_t
write_to_stream (void *closure, const unsigned char *data, unsigned int length)
{
  GOutputStream *stream = closure;
  gsize written = 0;

  if (!g_output_stream_write_all (stream, data, length, &written, NULL, NULL))
    return CAIRO_STATUS_WRITE_ERROR;

  return CAIRO_STATUS_SUCCESS;
}

gboolean
w42_pdf_export (W42PieceTable      *pt,
                const W42PageSetup *page,
                GFile              *file,
                GError            **error)
{
  GFileOutputStream *stream;
  cairo_surface_t *surface;
  cairo_t *cr;
  W42Layout *layout;
  const GArray *lines;
  int n_pages;
  gboolean ok = TRUE;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (page != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  stream = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
  if (stream == NULL)
    return FALSE;

  /* Always paginated: exporting from Normal view must still give pages. */
  layout = w42_layout_new ();
  w42_layout_set_galley (layout, FALSE);
  w42_layout_build_pt (layout, pt, page);

  surface = cairo_pdf_surface_create_for_stream (write_to_stream, stream,
                                                 page->width / 20.0,
                                                 page->height / 20.0);
  cr = cairo_create (surface);

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

  if (cairo_status (cr) != CAIRO_STATUS_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Word42 could not write the PDF: %s",
                   cairo_status_to_string (cairo_status (cr)));
      ok = FALSE;
    }

  cairo_destroy (cr);
  cairo_surface_finish (surface);
  cairo_surface_destroy (surface);
  w42_layout_free (layout);

  if (!g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, ok ? error : NULL))
    ok = FALSE;

  g_object_unref (stream);
  return ok;
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

/* Poppler gives back the page's text with a newline at the end of every
 * line.  Most of those are line breaks the typesetter chose, not paragraph
 * breaks the author chose, and the two have to be told apart or every line
 * of the PDF becomes a paragraph of its own.  The rule: a line that ends a
 * sentence and is followed by one starting with a capital or a digit is a
 * paragraph boundary; a blank line always is; anything else is a wrap. */
static void
append_page_text (GString *out, const char *text)
{
  char **lines = g_strsplit (text, "\n", -1);
  gboolean at_start = (out->len == 0 || out->str[out->len - 1] == '\n');

  for (guint i = 0; lines[i] != NULL; i++)
    {
      char *line = g_strstrip (lines[i]);
      gsize len = strlen (line);
      gboolean boundary;

      if (len == 0)
        {
          /* A blank line: end the paragraph, if one is open. */
          if (out->len > 0 && out->str[out->len - 1] != '\n')
            g_string_append_c (out, '\n');
          at_start = TRUE;
          continue;
        }

      if (at_start)
        {
          g_string_append (out, line);
          at_start = FALSE;
          continue;
        }

      {
        char last = out->str[out->len - 1];
        gunichar first = g_utf8_get_char (line);

        boundary = (last == '.' || last == '!' || last == '?' ||
                    last == ':') &&
                   (g_unichar_isupper (first) || g_unichar_isdigit (first));
      }

      /* A hyphen at the line end is a word broken by the typesetter. */
      if (!boundary && out->str[out->len - 1] == '-' && out->len >= 2 &&
          g_ascii_isalpha (out->str[out->len - 2]))
        {
          g_string_truncate (out, out->len - 1);
          g_string_append (out, line);
          continue;
        }

      g_string_append_c (out, boundary ? '\n' : ' ');
      g_string_append (out, line);
    }

  g_strfreev (lines);

  if (out->len > 0 && out->str[out->len - 1] != '\n')
    g_string_append_c (out, '\n');
}

gboolean
w42_pdf_import (W42PieceTable *pt,
                W42PageSetup  *page,
                GFile         *file,
                GError       **error)
{
  PopplerDocument *document;
  GString *text;
  int n_pages;
  W42ObjectTable *objects;
  W42ApIdx ap;
  GArray *pictures;      /* W42ObjectIdx, in page order */

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  document = poppler_document_new_from_gfile (file, NULL, NULL, error);
  if (document == NULL)
    return FALSE;

  n_pages = poppler_document_get_n_pages (document);
  text = g_string_new (NULL);
  objects = w42_pt_object_table (pt);
  pictures = g_array_new (FALSE, FALSE, sizeof (W42ObjectIdx));

  for (int i = 0; i < n_pages; i++)
    {
      PopplerPage *pp = poppler_document_get_page (document, i);
      char *page_text;
      GList *mappings, *l;

      if (pp == NULL)
        continue;

      if (i == 0 && page != NULL)
        {
          double w = 0, h = 0;

          /* The first page's size becomes the document's, so that the
           * pagination matches the original as closely as the text allows. */
          poppler_page_get_size (pp, &w, &h);
          if (w > 0 && h > 0)
            {
              page->width  = (int) (w * 20.0);
              page->height = (int) (h * 20.0);
            }
        }

      page_text = poppler_page_get_text (pp);
      if (page_text != NULL)
        {
          append_page_text (text, page_text);
          g_free (page_text);
        }

      /* The pictures on the page, each re-encoded as PNG and appended as a
       * paragraph of its own after the page's text.  Their position on the
       * page is not something a text flow can represent. */
      mappings = poppler_page_get_image_mapping (pp);
      for (l = mappings; l != NULL; l = l->next)
        {
          PopplerImageMapping *m = l->data;
          cairo_surface_t *surface = poppler_page_get_image (pp, m->image_id);
          GBytes *png;

          if (surface == NULL)
            continue;

          png = w42_image_surface_to_png (surface);
          cairo_surface_destroy (surface);

          if (png != NULL)
            {
              int pw = 0, ph = 0;
              const char *format = NULL;

              if (w42_image_probe (png, &pw, &ph, &format))
                {
                  double shown_w = m->area.x2 - m->area.x1;
                  double shown_h = m->area.y2 - m->area.y1;
                  W42ObjectIdx idx;

                  idx = w42_object_table_add (objects, png, format, pw, ph,
                                              (int) (shown_w * 20.0),
                                              (int) (shown_h * 20.0));
                  g_array_append_val (pictures, idx);
                }
              g_bytes_unref (png);
            }
        }
      poppler_page_free_image_mapping (mappings);

      g_object_unref (pp);
    }

  g_object_unref (document);

  w42_pt_load_text (pt, text->str);
  g_string_free (text, TRUE);

  /* Pictures go at the end, each in its own paragraph. */
  ap = w42_ap_table_default (w42_pt_ap_table (pt));
  for (guint i = 0; i < pictures->len; i++)
    {
      gsize end = w42_pt_length (pt);

      w42_pt_insert_block (pt, end, ap);
      w42_pt_insert_object (pt, end + 1,
                            g_array_index (pictures, W42ObjectIdx, i), ap);
    }
  g_array_free (pictures, TRUE);

  w42_pt_clear_undo (pt);
  return TRUE;
}

#else  /* !HAVE_POPPLER */

gboolean
w42_pdf_import (W42PieceTable *pt,
                W42PageSetup  *page,
                GFile         *file,
                GError       **error)
{
  (void) pt; (void) page; (void) file;

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
               "This build of Word42 cannot read PDF files. "
               "It was built without poppler.");
  return FALSE;
}

#endif
