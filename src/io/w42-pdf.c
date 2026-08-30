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
  int bins = (int) CLAMP (page_w, 32.0, 4096.0);
  guint8 *covered = g_new0 (guint8, bins + 1);
  int n = 0;

  for (gsize i = 0; i < n_chars && i < n_rects; i++)
    {
      const PopplerRectangle *r = &rects[i];
      gunichar c = g_utf8_get_char (text + byte_of[i]);
      int a, b;

      /* A space is whatever gap the typesetter left, so it says nothing
       * about where the text is. */
      if (g_unichar_isspace (c) || r->x2 <= r->x1)
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

/* The paragraph gathered so far goes into the document, run by run. */
static void
flush_para (W42Builder *b, GArray *chunks, W42Align align)
{
  gboolean any = FALSE;

  for (guint i = 0; i < chunks->len; i++)
    if (g_array_index (chunks, PdfChunk, i).text->len > 0)
      any = TRUE;

  if (any)
    {
      b->pa.align = align;
      for (guint i = 0; i < chunks->len; i++)
        {
          PdfChunk *c = &g_array_index (chunks, PdfChunk, i);

          if (c->text->len == 0)
            continue;
          b->ch = c->ch;
          w42_builder_text (b, c->text->str);
        }
      w42_builder_end_paragraph (b);
    }
  g_array_set_size (chunks, 0);
}

/* The text of one page, laid out the way it looks rather than the way the
 * bytes happen to run: the lines are gathered with where they sit, sorted
 * into columns, and joined into paragraphs where the geometry and the
 * formatting say one carries on. */
static void
read_page (PopplerPage *pp, W42Builder *b, GArray *chunks,
           PopplerRectangle *text_box)
{
  char *text = poppler_page_get_text (pp);
  PopplerRectangle *rects = NULL;
  guint n_rects = 0;
  GList *attrs, *l;
  PopplerTextAttributes **at = NULL;
  GArray *lines;
  guint8 *drop = NULL;
  gsize n_chars, *byte_of = NULL;
  double page_w = 0, page_h = 0;
  double gutters[3];
  double col_left[4], col_right[4];
  int n_gutters;
  int prev_column = -1;
  const PdfLine *prev = NULL;

  if (text_box != NULL)
    {
      text_box->x1 = text_box->y1 = G_MAXDOUBLE;
      text_box->x2 = text_box->y2 = -G_MAXDOUBLE;
    }
  if (text == NULL || *text == '\0')
    {
      g_free (text);
      return;
    }

  poppler_page_get_size (pp, &page_w, &page_h);
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

  if (!poppler_page_get_text_layout (pp, &rects, &n_rects))
    n_rects = 0;

  /* Without the glyphs' boxes there is nothing to sort or to split on, so
   * the page comes back as poppler read it, line by line. */
  if (n_rects < n_chars)
    {
      char **row = g_strsplit (text, "\n", -1);

      for (guint r = 0; row[r] != NULL; r++)
        if (row[r][0] != '\0')
          {
            w42_builder_reset_char (b);
            w42_builder_reset_para (b);
            w42_builder_text (b, row[r]);
            w42_builder_end_paragraph (b);
          }
      g_strfreev (row);
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
            const PopplerRectangle *r = k < j && k < n_rects ? &rects[k] : NULL;
            gunichar c = k < j ? g_utf8_get_char (text + byte_of[k]) : ' ';
            int col;

            if (k == j || r == NULL || r->x2 <= r->x1)
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

            col = column_of ((r->x1 + r->x2) / 2.0, gutters, n_gutters);
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
                line.x1 = r->x1; line.y1 = r->y1;
                line.x2 = r->x2; line.y2 = r->y2;
              }
            else
              {
                line.x1 = MIN (line.x1, r->x1);
                line.y1 = MIN (line.y1, r->y1);
                line.x2 = MAX (line.x2, r->x2);
                line.y2 = MAX (line.y2, r->y2);
              }
          }
        i = j + 1;
      }
  }

  g_array_sort (lines, line_cmp);

  {
    /* Where each column ends: a line that ran to it was wrapped, and one
     * that stopped short of it ended its paragraph. */
    for (guint i = 0; i < lines->len; i++)
      {
        const PdfLine *pl = &g_array_index (lines, PdfLine, i);
        int c = CLAMP (pl->column, 0, (int) G_N_ELEMENTS (col_right) - 1);

        col_left[c] = MIN (col_left[c], pl->x1);
        col_right[c] = MAX (col_right[c], pl->x2);
        if (text_box != NULL)
          {
            text_box->x1 = MIN (text_box->x1, pl->x1);
            text_box->y1 = MIN (text_box->y1, pl->y1);
            text_box->x2 = MAX (text_box->x2, pl->x2);
            text_box->y2 = MAX (text_box->y2, pl->y2);
          }
      }
  }

  drop = g_new0 (guint8, n_chars + 1);
  for (guint i = 0; i < lines->len; i++)
    {
      const PdfLine *pl = &g_array_index (lines, PdfLine, i);

      close_tracking (text, byte_of, rects, n_rects, pl->start, pl->end, drop);
    }

  for (guint i = 0; i < lines->len; i++)
    {
      const PdfLine *cur = &g_array_index (lines, PdfLine, i);
      double height = cur->y2 - cur->y1;
      gboolean para = FALSE;

      if (prev == NULL || cur->column != prev_column)
        para = TRUE;
      else
        {
          double gap = cur->y1 - prev->y2;
          const PopplerTextAttributes *a = at[cur->start];
          const PopplerTextAttributes *pa = at[prev->start];


          /* A gap taller than half a line, a step in from the margin, or a
           * change of type: each of them ends a paragraph. */
          int col = CLAMP (cur->column, 0, (int) G_N_ELEMENTS (col_right) - 1);
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
          else if (starts_item (text + byte_of[cur->start]))
            para = TRUE;
          else if (cur->x1 < prev->x1 - 6.0)
            para = TRUE;                  /* back out to the margin */
          else if (cur->x1 > prev->x1 + 6.0 && prev_short && !prev_marker)
            para = TRUE;                  /* a first line, indented */
          else if (a != NULL && pa != NULL &&
                   (ABS (a->font_size - pa->font_size) > 0.6 ||
                    g_strcmp0 (a->font_name, pa->font_name) != 0))
            para = TRUE;
          else
            {
              /* A sentence that ended, followed by one that begins. */
              const PdfChunk *last = chunks->len > 0
                ? &g_array_index (chunks, PdfChunk, chunks->len - 1) : NULL;

              if (last != NULL && last->text->len > 0 && !prev_marker)
                {
                  char end = last->text->str[last->text->len - 1];
                  gunichar c = g_utf8_get_char (text + byte_of[cur->start]);

                  if ((end == '.' || end == '!' || end == '?' || end == ':') &&
                      (g_unichar_isupper (c) || g_unichar_isdigit (c)))
                    para = TRUE;
                }
            }
        }

      if (para)
        flush_para (b, chunks, W42_ALIGN_LEFT);
      else if (chunks->len > 0)
        {
          /* Joined to the line before it: a hyphen the typesetter put in
           * goes away, anything else takes a space. */
          PdfChunk *last = &g_array_index (chunks, PdfChunk, chunks->len - 1);

          if (last->text->len >= 2 &&
              last->text->str[last->text->len - 1] == '-' &&
              g_ascii_isalpha (last->text->str[last->text->len - 2]))
            g_string_truncate (last->text, last->text->len - 1);
          else if (last->text->len > 0 &&
                   last->text->str[last->text->len - 1] != ' ')
            g_string_append_c (last->text, ' ');
        }

      /* The line's own text, one chunk per span of formatting. */
      for (guint c = cur->start; c < cur->end; c++)
        {
          PopplerTextAttributes *a = at[c];
          PdfChunk *last = chunks->len > 0
            ? &g_array_index (chunks, PdfChunk, chunks->len - 1) : NULL;
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

          if (last == NULL || memcmp (&last->ch, &want, sizeof want) != 0)
            {
              PdfChunk fresh;

              fresh.text = g_string_new (NULL);
              fresh.ch = want;
              g_array_append_val (chunks, fresh);
              last = &g_array_index (chunks, PdfChunk, chunks->len - 1);
            }
          g_string_append_len (last->text, text + byte_of[c],
                               (gssize) (byte_of[c + 1] - byte_of[c]));
        }

      prev = cur;
      prev_column = cur->column;
    }

  flush_para (b, chunks, W42_ALIGN_LEFT);

  g_array_free (lines, TRUE);
  g_free (drop);
  g_free (at);
  g_free (byte_of);
  poppler_page_free_text_attributes (attrs);
  g_free (rects);
  g_free (text);
}

gboolean
w42_pdf_import (W42PieceTable *pt,
                W42PageSetup  *page,
                GFile         *file,
                GError       **error)
{
  PopplerDocument *document;
  W42Builder b;
  GArray *chunks;
  PopplerRectangle text_box = { 0, 0, 0, 0 };
  int n_pages;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  document = poppler_document_new_from_gfile (file, NULL, NULL, error);
  if (document == NULL)
    return FALSE;

  n_pages = poppler_document_get_n_pages (document);
  w42_builder_init (&b, pt);
  chunks = g_array_new (FALSE, FALSE, sizeof (PdfChunk));
  g_array_set_clear_func (chunks, chunk_clear);

  for (int i = 0; i < n_pages; i++)
    {
      PopplerPage *pp = poppler_document_get_page (document, i);
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

      read_page (pp, &b, chunks, i == 0 ? &text_box : NULL);

      /* And the margins are where the first page's text sits on it. */
      if (i == 0 && page != NULL && text_box.x2 > text_box.x1)
        {
          double w = page->width / 20.0, h = page->height / 20.0;

          page->margin_left   = (int) CLAMP (text_box.x1 * 20.0, 0.0, w * 8.0);
          page->margin_right  = (int) CLAMP ((w - text_box.x2) * 20.0, 0.0, w * 8.0);
          page->margin_top    = (int) CLAMP (text_box.y1 * 20.0, 0.0, h * 8.0);
          page->margin_bottom = (int) CLAMP ((h - text_box.y2) * 20.0, 0.0, h * 8.0);
        }

      /* The pictures on the page, each in a paragraph of its own after the
       * page's text.  Where they sat on the page is not something a text
       * flow can say. */
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

                  w42_builder_reset_char (&b);
                  w42_builder_reset_para (&b);
                  w42_builder_object (&b, png, format, pw, ph,
                                      (int) (ABS (shown_w) * 20.0),
                                      (int) (ABS (shown_h) * 20.0));
                  w42_builder_end_paragraph (&b);
                }
              g_bytes_unref (png);
            }
        }
      poppler_page_free_image_mapping (mappings);

      g_object_unref (pp);
    }

  w42_builder_finish (&b);
  g_array_free (chunks, TRUE);
  g_object_unref (document);

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
