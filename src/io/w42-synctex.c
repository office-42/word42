/* w42-synctex.c - where TeX set each line of a source
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The file is text, gzipped: a preamble naming the inputs and the unit,
 * then a record a line.  "{3" and "}3" open and close page 3; "(" and "["
 * open an hbox and a vbox, ")" and "]" close them; "h" and "v" are empty
 * boxes; "x", "k", "g" and "$" are points -- a character, a kern, glue,
 * mathematics.  Each record but the closing ones reads
 *
 *     tag,line:x,y[:width,height,depth]
 *
 * with the tag naming an input and the place in scaled points from the
 * page's top left.  A line of type is an hbox, so a point is placed on
 * the page by its own place and put in its line by the hbox around it.
 */

#include "w42-synctex.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Scaled points in a PDF point: 65536 to TeX's point, 72.27 of those to
 * the inch, 72 PDF points to the inch. */
#define SP_PER_BP 65781.76

/* How far past a line with nothing set on it to look for one that has. */
#define LINES_AHEAD 12

typedef struct {
  guint line;
  int   page;
  float x, y;             /* the record's own place */
  float top, bottom;      /* the line of type it is in */
} Record;

struct _W42SyncTex {
  GArray     *records;    /* Record, in the order they were set */
  GHashTable *first;      /* line -> index + 1 of its first record */
};

typedef struct {
  float    top, bottom;
  gboolean hbox;
} Box;

/* "tag,line:x,y" and what may follow, into `v`; how many numbers. */
static int
record_numbers (const char *p, long v[7])
{
  int n = 0;

  while (n < 7)
    {
      char *end;

      v[n] = strtol (p, &end, 10);
      if (end == p)
        break;
      n++;
      if (*end != ',' && *end != ':')
        break;
      p = end + 1;
    }
  return n;
}

static gboolean
is_source (const char *path, const char *source)
{
  const char *slash = strrchr (path, '/');
  const char *back = strrchr (path, '\\');
  const char *base = slash != NULL && (back == NULL || slash > back) ? slash + 1
                   : back != NULL ? back + 1 : path;

  return g_str_equal (base, source);
}

W42SyncTex *
w42_synctex_load (const char *path, const char *source, GError **error)
{
  GFile *file = g_file_new_for_path (path);
  GInputStream *raw = G_INPUT_STREAM (g_file_read (file, NULL, error));
  GInputStream *in;
  GDataInputStream *data;
  GHashTable *tags = g_hash_table_new (g_direct_hash, g_direct_equal);
  GArray *boxes = g_array_new (FALSE, FALSE, sizeof (Box));
  W42SyncTex *sync;
  double unit = 1.0, magnification = 1000.0, x_offset = 0.0, y_offset = 0.0, scale = 0.0;
  gboolean content = FALSE;
  int page = 0;
  char *line;
  gsize len;
  GError *read_error = NULL;

  g_object_unref (file);
  if (raw == NULL)
    {
      g_hash_table_destroy (tags);
      g_array_free (boxes, TRUE);
      return NULL;
    }
  if (g_str_has_suffix (path, ".gz"))
    {
      GZlibDecompressor *gunzip = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);

      in = g_converter_input_stream_new (raw, G_CONVERTER (gunzip));
      g_object_unref (gunzip);
      g_object_unref (raw);
    }
  else
    in = raw;
  data = g_data_input_stream_new (in);
  g_object_unref (in);

  sync = g_new0 (W42SyncTex, 1);
  sync->records = g_array_new (FALSE, FALSE, sizeof (Record));
  sync->first = g_hash_table_new (g_direct_hash, g_direct_equal);

  while ((line = g_data_input_stream_read_line (data, &len, NULL, &read_error)) != NULL)
    {
      char kind = line[0];

      if (g_str_has_prefix (line, "Input:"))
        {
          char *end;
          long tag = strtol (line + 6, &end, 10);

          if (*end == ':' && is_source (end + 1, source))
            g_hash_table_add (tags, GINT_TO_POINTER ((int) tag));
        }
      else if (!content)
        {
          if (g_str_has_prefix (line, "Unit:"))
            unit = g_ascii_strtod (line + 5, NULL);
          else if (g_str_has_prefix (line, "Magnification:"))
            magnification = g_ascii_strtod (line + 14, NULL);
          else if (g_str_has_prefix (line, "X Offset:"))
            x_offset = g_ascii_strtod (line + 9, NULL);
          else if (g_str_has_prefix (line, "Y Offset:"))
            y_offset = g_ascii_strtod (line + 9, NULL);
          else if (g_str_has_prefix (line, "Content:"))
            {
              content = TRUE;
              if (unit <= 0)
                unit = 1.0;
              if (magnification <= 0)
                magnification = 1000.0;
              scale = unit / SP_PER_BP * magnification / 1000.0;
              x_offset *= unit / SP_PER_BP;
              y_offset *= unit / SP_PER_BP;
            }
        }
      else if (kind == '{')
        {
          page = atoi (line + 1);
          g_array_set_size (boxes, 0);
        }
      else if (kind == '}')
        page = 0;
      else if (kind == ')' || kind == ']')
        {
          if (boxes->len > 0)
            g_array_set_size (boxes, boxes->len - 1);
        }
      else if (page > 0 && kind != '\0' && strchr ("([hvxkg$", kind) != NULL)
        {
          long v[7];
          int n = record_numbers (line + 1, v);
          float x, y, height = 0, depth = 0;

          if (n < 4)
            {
              g_free (line);
              continue;
            }
          x = (float) (v[2] * scale + x_offset);
          y = (float) (v[3] * scale + y_offset);
          if (n >= 7)
            {
              height = (float) (v[5] * scale);
              depth = (float) (v[6] * scale);
            }
          if (kind == '(' || kind == '[')
            {
              Box box = { y - height, y + depth, kind == '(' };

              g_array_append_val (boxes, box);
            }
          if (kind != '[' && kind != 'v' && v[1] > 0 &&
              g_hash_table_contains (tags, GINT_TO_POINTER ((int) v[0])))
            {
              Record r = { (guint) v[1], page, x, y, y - height, y + depth };

              /* A point is placed in the line of type around it. */
              for (guint b = boxes->len; b-- > 0 && kind != '(' && kind != 'h'; )
                {
                  const Box *box = &g_array_index (boxes, Box, b);

                  if (box->hbox && box->bottom > box->top)
                    {
                      r.top = box->top;
                      r.bottom = box->bottom;
                      break;
                    }
                }
              if (!g_hash_table_contains (sync->first, GUINT_TO_POINTER (r.line)))
                g_hash_table_insert (sync->first, GUINT_TO_POINTER (r.line),
                                     GUINT_TO_POINTER (sync->records->len + 1));
              g_array_append_val (sync->records, r);
            }
        }
      g_free (line);
    }

  g_object_unref (data);
  g_hash_table_destroy (tags);
  g_array_free (boxes, TRUE);
  if (read_error != NULL)
    {
      g_propagate_error (error, read_error);
      w42_synctex_free (sync);
      return NULL;
    }
  return sync;
}

void
w42_synctex_free (W42SyncTex *sync)
{
  if (sync == NULL)
    return;
  g_array_free (sync->records, TRUE);
  g_hash_table_destroy (sync->first);
  g_free (sync);
}

gboolean
w42_synctex_forward (W42SyncTex *sync, guint line, int *page, double *top, double *bottom)
{
  g_return_val_if_fail (sync != NULL, FALSE);

  for (guint l = line; l < line + LINES_AHEAD; l++)
    {
      guint at = GPOINTER_TO_UINT (g_hash_table_lookup (sync->first, GUINT_TO_POINTER (l)));
      const Record *r;

      if (at == 0)
        continue;
      r = &g_array_index (sync->records, Record, at - 1);
      *page = r->page;
      if (r->bottom > r->top)
        {
          *top = r->top;
          *bottom = r->bottom;
        }
      else
        {
          /* A point with no line around it: about a line's height of it. */
          *top = r->y - 9.0;
          *bottom = r->y + 3.0;
        }
      return TRUE;
    }
  return FALSE;
}

guint
w42_synctex_inverse (W42SyncTex *sync, int page, double x, double y)
{
  guint best = 0;
  double best_dy = G_MAXDOUBLE, best_dx = G_MAXDOUBLE;

  g_return_val_if_fail (sync != NULL, 0);

  /* The line of type the point is in, or the nearest above or below it;
   * in that line, the record nearest across. */
  for (guint i = 0; i < sync->records->len; i++)
    {
      const Record *r = &g_array_index (sync->records, Record, i);
      double top = r->bottom > r->top ? r->top : r->y - 9.0;
      double bottom = r->bottom > r->top ? r->bottom : r->y + 3.0;
      double dy, dx;

      if (r->page != page)
        continue;
      dy = y < top ? top - y : y > bottom ? y - bottom : 0.0;
      dx = fabs (x - r->x);
      if (dy < best_dy - 0.5 || (dy <= best_dy + 0.5 && dx < best_dx))
        {
          best = r->line;
          best_dy = dy;
          best_dx = dx;
        }
    }
  return best;
}
