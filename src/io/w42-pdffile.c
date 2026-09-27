/* w42-pdffile.c - see w42-pdffile.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-pdffile.h"

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

/* How deep arrays and dictionaries may nest, and how many cross-reference
 * sections one file may chain: far past anything cairo writes, and short
 * of the stack. */
#define MAX_DEPTH    64
#define MAX_SECTIONS 64

/* Objects to one object stream: enough to pack well, few enough that a
 * reader finding one object does not inflate the whole file for it. */
#define OBJSTM_SIZE  100

/* ====================================================================== */
/* Objects                                                                 */
/* ====================================================================== */

static W42PdfObj *
obj_new (W42PdfKind kind)
{
  W42PdfObj *o = g_new0 (W42PdfObj, 1);

  o->kind = kind;
  return o;
}

W42PdfObj *
w42_pdf_null (void)
{
  return obj_new (W42_PDF_NULL);
}

W42PdfObj *
w42_pdf_bool (gboolean b)
{
  W42PdfObj *o = obj_new (W42_PDF_BOOL);

  o->v.b = b != FALSE;
  return o;
}

W42PdfObj *
w42_pdf_int (gint64 i)
{
  W42PdfObj *o = obj_new (W42_PDF_INT);

  o->v.i = i;
  return o;
}

W42PdfObj *
w42_pdf_real (double r)
{
  W42PdfObj *o = obj_new (W42_PDF_REAL);
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  char *end;

  /* Not the locale's decimal comma, and never an exponent, which PDF
   * does not have. */
  if (!isfinite (r) || fabs (r) > 1e9)
    r = 0.0;
  g_ascii_formatd (buf, sizeof buf, "%.5f", r);
  end = buf + strlen (buf);
  while (end > buf && end[-1] == '0')
    *--end = '\0';
  if (end > buf && end[-1] == '.')
    *--end = '\0';
  if (buf[0] == '\0' || strcmp (buf, "-0") == 0)
    strcpy (buf, "0");
  o->v.real = g_strdup (buf);
  return o;
}

static W42PdfObj *
bytes_obj (W42PdfKind kind, const void *data, gsize len)
{
  W42PdfObj *o = obj_new (kind);

  /* One byte more than asked, so that a name is also a C string. */
  o->v.s.data = g_malloc (len + 1);
  if (len > 0)
    memcpy (o->v.s.data, data, len);
  o->v.s.data[len] = '\0';
  o->v.s.len = len;
  return o;
}

W42PdfObj *
w42_pdf_name (const char *name)
{
  return bytes_obj (W42_PDF_NAME, name, strlen (name));
}

W42PdfObj *
w42_pdf_string (const void *data, gsize len)
{
  return bytes_obj (W42_PDF_STRING, data, len);
}

W42PdfObj *
w42_pdf_text (const char *utf8)
{
  gboolean ascii = TRUE;
  gunichar2 *utf16;
  glong n = 0;
  guint8 *be;
  W42PdfObj *o;

  if (utf8 == NULL)
    utf8 = "";
  for (const char *p = utf8; *p != '\0'; p++)
    if ((guchar) *p >= 0x80)
      ascii = FALSE;
  if (ascii)
    return w42_pdf_string (utf8, strlen (utf8));

  utf16 = g_utf8_to_utf16 (utf8, -1, NULL, &n, NULL);
  if (utf16 == NULL)
    return w42_pdf_string ("", 0);
  be = g_malloc (2 + 2 * (gsize) n);
  be[0] = 0xFE;
  be[1] = 0xFF;
  for (glong i = 0; i < n; i++)
    {
      be[2 + 2 * i] = (guint8) (utf16[i] >> 8);
      be[3 + 2 * i] = (guint8) utf16[i];
    }
  o = w42_pdf_string (be, 2 + 2 * (gsize) n);
  g_free (be);
  g_free (utf16);
  return o;
}

W42PdfObj *
w42_pdf_date (GDateTime *when)
{
  char *stamp = g_date_time_format (when, "D:%Y%m%d%H%M%S");
  GTimeSpan offset = g_date_time_get_utc_offset (when) / G_TIME_SPAN_MINUTE;
  char *full;
  W42PdfObj *o;

  if (offset == 0)
    full = g_strconcat (stamp, "Z", NULL);
  else
    full = g_strdup_printf ("%s%c%02d'%02d'", stamp, offset < 0 ? '-' : '+',
                            (int) (ABS (offset) / 60), (int) (ABS (offset) % 60));
  o = w42_pdf_string (full, strlen (full));
  g_free (full);
  g_free (stamp);
  return o;
}

W42PdfObj *
w42_pdf_ref (guint num)
{
  W42PdfObj *o = obj_new (W42_PDF_REF);

  o->v.ref.num = num;
  o->v.ref.gen = 0;
  return o;
}

W42PdfObj *
w42_pdf_raw (const char *text, int tag)
{
  W42PdfObj *o = obj_new (W42_PDF_RAW);

  o->v.raw.text = g_strdup (text);
  o->v.raw.tag = tag;
  return o;
}

W42PdfObj *
w42_pdf_array (void)
{
  W42PdfObj *o = obj_new (W42_PDF_ARRAY);

  o->v.array = g_ptr_array_new_with_free_func ((GDestroyNotify) w42_pdf_obj_free);
  return o;
}

W42PdfObj *
w42_pdf_dict (void)
{
  W42PdfObj *o = obj_new (W42_PDF_DICT);

  o->v.dict = g_ptr_array_new ();
  return o;
}

W42PdfObj *
w42_pdf_stream (W42PdfObj *dict, GBytes *data)
{
  W42PdfObj *o = obj_new (W42_PDF_STREAM);

  g_return_val_if_fail (dict != NULL && dict->kind == W42_PDF_DICT, o);
  o->v.stream.dict = dict;
  o->v.stream.data = data;
  return o;
}

void
w42_pdf_obj_free (W42PdfObj *obj)
{
  if (obj == NULL)
    return;
  switch (obj->kind)
    {
    case W42_PDF_REAL:
      g_free (obj->v.real);
      break;
    case W42_PDF_STRING:
    case W42_PDF_NAME:
      g_free (obj->v.s.data);
      break;
    case W42_PDF_ARRAY:
      g_ptr_array_free (obj->v.array, TRUE);
      break;
    case W42_PDF_DICT:
      for (guint i = 0; i + 1 < obj->v.dict->len; i += 2)
        {
          g_free (g_ptr_array_index (obj->v.dict, i));
          w42_pdf_obj_free (g_ptr_array_index (obj->v.dict, i + 1));
        }
      g_ptr_array_free (obj->v.dict, TRUE);
      break;
    case W42_PDF_STREAM:
      w42_pdf_obj_free (obj->v.stream.dict);
      if (obj->v.stream.data != NULL)
        g_bytes_unref (obj->v.stream.data);
      break;
    case W42_PDF_RAW:
      g_free (obj->v.raw.text);
      break;
    default:
      break;
    }
  g_free (obj);
}

void
w42_pdf_array_add (W42PdfObj *array, W42PdfObj *item)
{
  g_return_if_fail (array != NULL && array->kind == W42_PDF_ARRAY);
  g_ptr_array_add (array->v.array, item);
}

void
w42_pdf_dict_set (W42PdfObj *dict, const char *key, W42PdfObj *value)
{
  if (dict != NULL && dict->kind == W42_PDF_STREAM)
    dict = dict->v.stream.dict;
  g_return_if_fail (dict != NULL && dict->kind == W42_PDF_DICT);

  for (guint i = 0; i + 1 < dict->v.dict->len; i += 2)
    if (strcmp (g_ptr_array_index (dict->v.dict, i), key) == 0)
      {
        w42_pdf_obj_free (g_ptr_array_index (dict->v.dict, i + 1));
        if (value != NULL)
          g_ptr_array_index (dict->v.dict, i + 1) = value;
        else
          {
            g_free (g_ptr_array_index (dict->v.dict, i));
            g_ptr_array_remove_range (dict->v.dict, i, 2);
          }
        return;
      }
  if (value != NULL)
    {
      g_ptr_array_add (dict->v.dict, g_strdup (key));
      g_ptr_array_add (dict->v.dict, value);
    }
}

W42PdfObj *
w42_pdf_dict_get (W42PdfObj *dict, const char *key)
{
  if (dict != NULL && dict->kind == W42_PDF_STREAM)
    dict = dict->v.stream.dict;
  if (dict == NULL || dict->kind != W42_PDF_DICT)
    return NULL;
  for (guint i = 0; i + 1 < dict->v.dict->len; i += 2)
    if (strcmp (g_ptr_array_index (dict->v.dict, i), key) == 0)
      return g_ptr_array_index (dict->v.dict, i + 1);
  return NULL;
}

gboolean
w42_pdf_is_name (W42PdfObj *obj, const char *name)
{
  return obj != NULL && obj->kind == W42_PDF_NAME &&
         strcmp ((const char *) obj->v.s.data, name) == 0;
}

/* ====================================================================== */
/* Flate                                                                   */
/* ====================================================================== */

static GBytes *
convert_all (GConverter *converter, const guint8 *in, gsize in_len, gsize max_out)
{
  GByteArray *out = g_byte_array_new ();
  guint8 *buf = g_malloc (65536);
  gsize in_pos = 0;

  for (;;)
    {
      gsize read = 0, written = 0;
      GConverterResult res;

      res = g_converter_convert (converter, in + in_pos, in_len - in_pos,
                                 buf, 65536,
                                 in_pos >= in_len ? G_CONVERTER_INPUT_AT_END : G_CONVERTER_NO_FLAGS,
                                 &read, &written, NULL);
      if (res == G_CONVERTER_ERROR || (max_out > 0 && out->len + written > max_out))
        {
          g_free (buf);
          g_byte_array_free (out, TRUE);
          return NULL;
        }
      in_pos += read;
      g_byte_array_append (out, buf, (guint) written);
      if (res == G_CONVERTER_FINISHED ||
          (read == 0 && written == 0 && in_pos >= in_len))
        break;
    }
  g_free (buf);
  return g_byte_array_free_to_bytes (out);
}

/* What a stream may inflate to: a page of a few megabytes of pixels is
 * the most cairo writes, and a stream a thousand times its own size is
 * not a picture. */
static gsize
inflate_budget (gsize in_len)
{
  if (in_len > G_MAXSIZE / 1024)
    return G_MAXSIZE;
  return MAX ((gsize) 64 << 20, in_len * 1024);
}

static GBytes *
inflate (const guint8 *in, gsize in_len)
{
  GZlibDecompressor *dec = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_ZLIB);
  GBytes *out = convert_all (G_CONVERTER (dec), in, in_len, inflate_budget (in_len));

  g_object_unref (dec);
  return out;
}

static GBytes *
deflate_best (const guint8 *in, gsize in_len)
{
  GZlibCompressor *comp = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_ZLIB, 9);
  GBytes *out = convert_all (G_CONVERTER (comp), in, in_len, 0);

  g_object_unref (comp);
  return out;
}

/* The PNG predictors a cross-reference stream is usually written with:
 * each row a filter byte and the row, told from the row above. */
static GBytes *
unpredict (GBytes *data, W42PdfObj *parms)
{
  W42PdfObj *p = w42_pdf_dict_get (parms, "Predictor");
  W42PdfObj *c = w42_pdf_dict_get (parms, "Columns");
  W42PdfObj *k = w42_pdf_dict_get (parms, "Colors");
  W42PdfObj *b = w42_pdf_dict_get (parms, "BitsPerComponent");
  gint64 predictor = p != NULL && p->kind == W42_PDF_INT ? p->v.i : 1;
  gint64 columns = c != NULL && c->kind == W42_PDF_INT ? c->v.i : 1;
  gint64 colors = k != NULL && k->kind == W42_PDF_INT ? k->v.i : 1;
  gint64 bpc = b != NULL && b->kind == W42_PDF_INT ? b->v.i : 8;
  gsize len, row, bpp, rows;
  const guint8 *in;
  guint8 *out, *prev;

  if (predictor < 10)
    return predictor == 1 ? g_bytes_ref (data) : NULL;
  if (columns < 1 || columns > 1 << 20 || colors < 1 || colors > 32 ||
      (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16))
    return NULL;

  in = g_bytes_get_data (data, &len);
  row = (gsize) ((columns * colors * bpc + 7) / 8);
  bpp = MAX ((gsize) 1, (gsize) (colors * bpc / 8));
  rows = len / (row + 1);
  out = g_malloc0 (rows * row + 1);
  prev = g_malloc0 (row);
  for (gsize r = 0; r < rows; r++)
    {
      const guint8 *src = in + r * (row + 1);
      guint8 *dst = out + r * row;
      guint8 filter = src[0];

      src++;
      for (gsize i = 0; i < row; i++)
        {
          int left = i >= bpp ? dst[i - bpp] : 0;
          int up = prev[i];
          int corner = i >= bpp ? prev[i - bpp] : 0;
          int v = src[i];

          switch (filter)
            {
            case 1: v += left; break;
            case 2: v += up; break;
            case 3: v += (left + up) / 2; break;
            case 4:
              {
                int pa = ABS (up - corner), pb = ABS (left - corner);
                int pc = ABS (left + up - 2 * corner);

                v += (pa <= pb && pa <= pc) ? left : pb <= pc ? up : corner;
              }
              break;
            default: break;
            }
          dst[i] = (guint8) v;
        }
      memcpy (prev, dst, row);
    }
  g_free (prev);
  return g_bytes_new_take (out, rows * row);
}

GBytes *
w42_pdf_stream_decoded (W42PdfObj *stream)
{
  W42PdfObj *filter, *parms;
  GBytes *inflated, *out;
  gsize len;
  const guint8 *d;

  if (stream == NULL || stream->kind != W42_PDF_STREAM || stream->v.stream.data == NULL)
    return NULL;
  filter = w42_pdf_dict_get (stream, "Filter");
  parms = w42_pdf_dict_get (stream, "DecodeParms");
  if (filter != NULL && filter->kind == W42_PDF_ARRAY)
    {
      if (filter->v.array->len > 1)
        return NULL;
      filter = filter->v.array->len == 1 ? g_ptr_array_index (filter->v.array, 0) : NULL;
    }
  if (parms != NULL && parms->kind == W42_PDF_ARRAY)
    parms = parms->v.array->len == 1 ? g_ptr_array_index (parms->v.array, 0) : NULL;
  if (filter == NULL)
    return g_bytes_ref (stream->v.stream.data);
  if (!w42_pdf_is_name (filter, "FlateDecode"))
    return NULL;

  d = g_bytes_get_data (stream->v.stream.data, &len);
  inflated = inflate (d, len);
  if (inflated == NULL || parms == NULL || parms->kind != W42_PDF_DICT)
    return inflated;
  out = unpredict (inflated, parms);
  g_bytes_unref (inflated);
  return out;
}

/* ====================================================================== */
/* Reading                                                                 */
/* ====================================================================== */

typedef struct {
  const guint8 *d;
  gsize         len;
  gsize         pos;
} Lex;

static gboolean
is_ws (guint8 c)
{
  return c == 0 || c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

static gboolean
is_delim (guint8 c)
{
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
         c == '{' || c == '}' || c == '/' || c == '%';
}

static gboolean
is_regular (guint8 c)
{
  return !is_ws (c) && !is_delim (c);
}

static void
skip_ws (Lex *l)
{
  while (l->pos < l->len)
    {
      guint8 c = l->d[l->pos];

      if (c == '%')
        while (l->pos < l->len && l->d[l->pos] != '\n' && l->d[l->pos] != '\r')
          l->pos++;
      else if (is_ws (c))
        l->pos++;
      else
        break;
    }
}

static gboolean
at_keyword (Lex *l, const char *kw)
{
  gsize n = strlen (kw);

  skip_ws (l);
  return l->pos + n <= l->len && memcmp (l->d + l->pos, kw, n) == 0 &&
         (l->pos + n == l->len || !is_regular (l->d[l->pos + n]));
}

static gboolean
read_uint (Lex *l, guint64 *out)
{
  guint64 v = 0;
  gsize p;

  skip_ws (l);
  p = l->pos;
  if (p >= l->len || !g_ascii_isdigit (l->d[p]))
    return FALSE;
  while (p < l->len && g_ascii_isdigit (l->d[p]))
    {
      if (v > (G_MAXUINT64 - 9) / 10)
        return FALSE;
      v = v * 10 + (guint64) (l->d[p] - '0');
      p++;
    }
  if (p < l->len && is_regular (l->d[p]))
    return FALSE;
  l->pos = p;
  *out = v;
  return TRUE;
}

static W42PdfObj *parse_object (Lex *l, int depth);

static W42PdfObj *
parse_name (Lex *l)
{
  GString *s = g_string_new (NULL);
  W42PdfObj *o;

  l->pos++;
  while (l->pos < l->len && is_regular (l->d[l->pos]))
    {
      guint8 c = l->d[l->pos];

      if (c == '#' && l->pos + 2 < l->len &&
          g_ascii_isxdigit (l->d[l->pos + 1]) && g_ascii_isxdigit (l->d[l->pos + 2]))
        {
          c = (guint8) ((g_ascii_xdigit_value (l->d[l->pos + 1]) << 4) |
                        g_ascii_xdigit_value (l->d[l->pos + 2]));
          l->pos += 3;
        }
      else
        l->pos++;
      /* A NUL would cut the name short as a C string: not a name. */
      if (c != 0)
        g_string_append_c (s, (char) c);
    }
  o = bytes_obj (W42_PDF_NAME, s->str, s->len);
  g_string_free (s, TRUE);
  return o;
}

static W42PdfObj *
parse_literal (Lex *l)
{
  GByteArray *b = g_byte_array_new ();
  int nest = 1;
  W42PdfObj *o;

  l->pos++;
  while (l->pos < l->len)
    {
      guint8 c = l->d[l->pos++];

      if (c == '(')
        nest++;
      else if (c == ')' && --nest == 0)
        {
          o = w42_pdf_string (b->data, b->len);
          g_byte_array_free (b, TRUE);
          return o;
        }
      else if (c == '\\')
        {
          if (l->pos >= l->len)
            break;
          c = l->d[l->pos++];
          switch (c)
            {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '\r':
              /* A backslash at the end of a line joins it to the next. */
              if (l->pos < l->len && l->d[l->pos] == '\n')
                l->pos++;
              continue;
            case '\n':
              continue;
            default:
              if (c >= '0' && c <= '7')
                {
                  int v = c - '0';

                  for (int k = 0; k < 2 && l->pos < l->len &&
                                  l->d[l->pos] >= '0' && l->d[l->pos] <= '7'; k++)
                    v = v * 8 + (l->d[l->pos++] - '0');
                  c = (guint8) v;
                }
              break;
            }
        }
      else if (c == '\r')
        {
          c = '\n';
          if (l->pos < l->len && l->d[l->pos] == '\n')
            l->pos++;
        }
      g_byte_array_append (b, &c, 1);
    }
  g_byte_array_free (b, TRUE);
  return NULL;
}

static W42PdfObj *
parse_hex (Lex *l)
{
  GByteArray *b = g_byte_array_new ();
  int high = -1;
  W42PdfObj *o;

  l->pos++;
  while (l->pos < l->len)
    {
      guint8 c = l->d[l->pos++];
      int v;

      if (c == '>')
        {
          if (high >= 0)
            {
              guint8 last = (guint8) (high << 4);

              g_byte_array_append (b, &last, 1);
            }
          o = w42_pdf_string (b->data, b->len);
          g_byte_array_free (b, TRUE);
          return o;
        }
      if (is_ws (c))
        continue;
      v = g_ascii_xdigit_value (c);
      if (v < 0)
        break;
      if (high < 0)
        high = v;
      else
        {
          guint8 byte = (guint8) ((high << 4) | v);

          g_byte_array_append (b, &byte, 1);
          high = -1;
        }
    }
  g_byte_array_free (b, TRUE);
  return NULL;
}

static W42PdfObj *
parse_number (Lex *l)
{
  gsize start = l->pos, n = 0;
  gboolean real = FALSE;
  char *text;
  W42PdfObj *o;

  while (start + n < l->len && is_regular (l->d[start + n]))
    {
      guint8 c = l->d[start + n];

      if (c == '.')
        real = TRUE;
      else if (!g_ascii_isdigit (c) && !((c == '+' || c == '-') && n == 0))
        return NULL;
      n++;
    }
  if (n == 0 || n > 64)
    return NULL;
  text = g_strndup ((const char *) l->d + start, n);
  l->pos = start + n;

  if (real)
    {
      o = obj_new (W42_PDF_REAL);
      /* Kept as written, less a sign PDF allows and some readers do
       * not: "+.5" is ".5". */
      o->v.real = text[0] == '+' ? g_strdup (text + 1) : g_strdup (text);
      if (o->v.real[0] == '\0' || strcmp (o->v.real, "-") == 0 ||
          strcmp (o->v.real, ".") == 0 || strcmp (o->v.real, "-.") == 0)
        {
          g_free (o->v.real);
          o->v.real = g_strdup ("0");
        }
      g_free (text);
      return o;
    }

  o = w42_pdf_int (g_ascii_strtoll (text, NULL, 10));
  g_free (text);

  /* Two numbers and an R are a reference. */
  if (o->v.i >= 0 && o->v.i <= G_MAXINT)
    {
      gsize back = l->pos;
      guint64 gen;

      if (read_uint (l, &gen) && gen <= 65535 && at_keyword (l, "R"))
        {
          gint64 num = o->v.i;

          l->pos++;
          o->kind = W42_PDF_REF;
          o->v.ref.num = (guint) num;
          o->v.ref.gen = (guint) gen;
          return o;
        }
      l->pos = back;
    }
  return o;
}

static W42PdfObj *
parse_object (Lex *l, int depth)
{
  guint8 c;

  if (depth > MAX_DEPTH)
    return NULL;
  skip_ws (l);
  if (l->pos >= l->len)
    return NULL;
  c = l->d[l->pos];

  if (c == '/')
    return parse_name (l);
  if (c == '(')
    return parse_literal (l);
  if (c == '<' && l->pos + 1 < l->len && l->d[l->pos + 1] == '<')
    {
      W42PdfObj *dict = w42_pdf_dict ();

      l->pos += 2;
      for (;;)
        {
          W42PdfObj *key, *value;

          skip_ws (l);
          if (l->pos + 1 < l->len && l->d[l->pos] == '>' && l->d[l->pos + 1] == '>')
            {
              l->pos += 2;
              return dict;
            }
          if (l->pos >= l->len || l->d[l->pos] != '/')
            break;
          key = parse_name (l);
          value = parse_object (l, depth + 1);
          if (value == NULL)
            {
              w42_pdf_obj_free (key);
              break;
            }
          w42_pdf_dict_set (dict, (const char *) key->v.s.data, value);
          w42_pdf_obj_free (key);
        }
      w42_pdf_obj_free (dict);
      return NULL;
    }
  if (c == '<')
    return parse_hex (l);
  if (c == '[')
    {
      W42PdfObj *array = w42_pdf_array ();

      l->pos++;
      for (;;)
        {
          W42PdfObj *item;

          skip_ws (l);
          if (l->pos < l->len && l->d[l->pos] == ']')
            {
              l->pos++;
              return array;
            }
          item = parse_object (l, depth + 1);
          if (item == NULL)
            break;
          w42_pdf_array_add (array, item);
        }
      w42_pdf_obj_free (array);
      return NULL;
    }
  if (g_ascii_isdigit (c) || c == '+' || c == '-' || c == '.')
    return parse_number (l);
  if (at_keyword (l, "true"))
    {
      l->pos += 4;
      return w42_pdf_bool (TRUE);
    }
  if (at_keyword (l, "false"))
    {
      l->pos += 5;
      return w42_pdf_bool (FALSE);
    }
  if (at_keyword (l, "null"))
    {
      l->pos += 4;
      return w42_pdf_null ();
    }
  return NULL;
}

/* ---- the file ---------------------------------------------------------- */

typedef struct {
  guint8  type;        /* 0 none, 1 at an offset, 2 in an object stream */
  guint64 a;           /* the offset, or the object stream's number */
  guint64 b;           /* the generation, or the index in that stream */
} XrefEntry;

typedef struct {
  GBytes  *data;       /* the object stream inflated */
  gsize    first;      /* where its objects begin */
  GArray  *offsets;    /* guint64 pairs: number, offset */
} ObjStm;

struct _W42PdfFile {
  GBytes     *bytes;
  const guint8 *d;
  gsize       len;
  GArray     *xref;      /* XrefEntry by object number, while reading */
  GPtrArray  *objects;   /* W42PdfObj by number; NULL where there is none */
  GByteArray *loading;   /* 1 while an object is being read: no loops */
  GHashTable *objstms;   /* number -> ObjStm, while reading */
  W42PdfObj  *trailer;
};

static void
objstm_free (gpointer data)
{
  ObjStm *s = data;

  g_bytes_unref (s->data);
  g_array_free (s->offsets, TRUE);
  g_free (s);
}

static gsize
find_bytes (const guint8 *d, gsize len, gsize from, const char *what)
{
  gsize n = strlen (what);

  for (gsize i = from; i + n <= len; i++)
    if (d[i] == (guint8) what[0] && memcmp (d + i, what, n) == 0)
      return i;
  return G_MAXSIZE;
}

static W42PdfObj *load_object (W42PdfFile *f, guint num);

static void
xref_set (W42PdfFile *f, guint64 num, guint8 type, guint64 a, guint64 b)
{
  XrefEntry *e;

  /* Numbers beyond this are not objects any file needs. */
  if (num >= 8388608)
    return;
  if (num >= f->xref->len)
    g_array_set_size (f->xref, (guint) num + 1);
  e = &g_array_index (f->xref, XrefEntry, num);
  /* The newest section is read first, and says what the object is now. */
  if (e->type != 0)
    return;
  e->type = type;
  e->a = a;
  e->b = b;
}

/* An object as written in the file: "12 0 obj ... endobj", with its
 * stream if it has one. */
static W42PdfObj *
parse_indirect (W42PdfFile *f, gsize offset, guint expect)
{
  Lex l = { f->d, f->len, offset };
  guint64 num, gen;
  W42PdfObj *obj, *length;
  gsize start, end = G_MAXSIZE;
  gint64 n = -1;

  if (!read_uint (&l, &num) || !read_uint (&l, &gen) || !at_keyword (&l, "obj"))
    return NULL;
  if (expect != G_MAXUINT && num != expect)
    return NULL;
  l.pos += 3;
  obj = parse_object (&l, 0);
  if (obj == NULL || obj->kind != W42_PDF_DICT || !at_keyword (&l, "stream"))
    return obj;

  l.pos += 6;
  if (l.pos < l.len && l.d[l.pos] == '\r')
    l.pos++;
  if (l.pos < l.len && l.d[l.pos] == '\n')
    l.pos++;
  start = l.pos;

  length = w42_pdf_dict_get (obj, "Length");
  if (length != NULL && length->kind == W42_PDF_REF)
    length = load_object (f, length->v.ref.num);
  if (length != NULL && length->kind == W42_PDF_INT)
    n = length->v.i;
  if (n >= 0 && (guint64) n <= f->len - start)
    {
      Lex after = { f->d, f->len, start + (gsize) n };

      if (at_keyword (&after, "endstream"))
        end = start + (gsize) n;
    }
  if (end == G_MAXSIZE)
    {
      /* The length is wrong or missing: the data runs to "endstream",
       * less the line end before it. */
      end = find_bytes (f->d, f->len, start, "endstream");
      if (end == G_MAXSIZE)
        {
          w42_pdf_obj_free (obj);
          return NULL;
        }
      if (end > start && f->d[end - 1] == '\n')
        end--;
      if (end > start && f->d[end - 1] == '\r')
        end--;
    }
  return w42_pdf_stream (obj, g_bytes_new_from_bytes (f->bytes, start, end - start));
}

static ObjStm *
load_objstm (W42PdfFile *f, guint num)
{
  ObjStm *s = g_hash_table_lookup (f->objstms, GUINT_TO_POINTER (num));
  W42PdfObj *stream, *n, *first;
  Lex l;

  if (s != NULL)
    return s;
  stream = load_object (f, num);
  n = w42_pdf_dict_get (stream, "N");
  first = w42_pdf_dict_get (stream, "First");
  if (stream == NULL || stream->kind != W42_PDF_STREAM ||
      n == NULL || n->kind != W42_PDF_INT || n->v.i < 0 || n->v.i > 1000000 ||
      first == NULL || first->kind != W42_PDF_INT || first->v.i < 0)
    return NULL;

  s = g_new0 (ObjStm, 1);
  s->data = w42_pdf_stream_decoded (stream);
  s->offsets = g_array_new (FALSE, FALSE, sizeof (guint64));
  s->first = (gsize) first->v.i;
  if (s->data == NULL)
    {
      g_array_free (s->offsets, TRUE);
      g_free (s);
      return NULL;
    }
  l.d = g_bytes_get_data (s->data, &l.len);
  l.pos = 0;
  for (gint64 i = 0; i < n->v.i; i++)
    {
      guint64 pair[2];

      if (!read_uint (&l, &pair[0]) || !read_uint (&l, &pair[1]))
        break;
      g_array_append_vals (s->offsets, pair, 2);
    }
  g_hash_table_insert (f->objstms, GUINT_TO_POINTER (num), s);
  return s;
}

static W42PdfObj *
load_object (W42PdfFile *f, guint num)
{
  XrefEntry *e;
  W42PdfObj *obj = NULL;

  if (num < f->objects->len && g_ptr_array_index (f->objects, num) != NULL)
    return g_ptr_array_index (f->objects, num);
  /* Not while the sections are still being read: a cross-reference
   * stream's length has to be written out in full. */
  if (f->loading == NULL || num >= f->xref->len || f->loading->data[num])
    return NULL;
  e = &g_array_index (f->xref, XrefEntry, num);
  f->loading->data[num] = 1;

  if (e->type == 1 && e->a < f->len)
    obj = parse_indirect (f, (gsize) e->a, num);
  else if (e->type == 2 && e->a <= G_MAXUINT)
    {
      ObjStm *s = load_objstm (f, (guint) e->a);

      if (s != NULL && e->b < s->offsets->len / 2 &&
          g_array_index (s->offsets, guint64, 2 * e->b) == num)
        {
          Lex l;

          l.d = g_bytes_get_data (s->data, &l.len);
          l.pos = s->first + (gsize) g_array_index (s->offsets, guint64, 2 * e->b + 1);
          if (l.pos < l.len)
            obj = parse_object (&l, 0);
        }
    }

  f->loading->data[num] = 0;
  if (obj != NULL)
    {
      if (num >= f->objects->len)
        g_ptr_array_set_size (f->objects, num + 1);
      g_ptr_array_index (f->objects, num) = obj;
    }
  return obj;
}

static gboolean
read_xref_stream (W42PdfFile *f, gsize offset, W42PdfObj **dict_out)
{
  W42PdfObj *stream = parse_indirect (f, offset, G_MAXUINT);
  W42PdfObj *w, *index, *size;
  GBytes *data;
  const guint8 *d;
  gsize len, pos = 0;
  guint64 widths[3];
  gsize row = 0;

  if (stream == NULL || stream->kind != W42_PDF_STREAM ||
      !w42_pdf_is_name (w42_pdf_dict_get (stream, "Type"), "XRef"))
    {
      w42_pdf_obj_free (stream);
      return FALSE;
    }
  w = w42_pdf_dict_get (stream, "W");
  size = w42_pdf_dict_get (stream, "Size");
  data = w42_pdf_stream_decoded (stream);
  if (w == NULL || w->kind != W42_PDF_ARRAY || w->v.array->len != 3 ||
      size == NULL || size->kind != W42_PDF_INT || data == NULL)
    {
      if (data != NULL)
        g_bytes_unref (data);
      w42_pdf_obj_free (stream);
      return FALSE;
    }
  for (int i = 0; i < 3; i++)
    {
      W42PdfObj *wi = g_ptr_array_index (w->v.array, i);

      widths[i] = wi->kind == W42_PDF_INT && wi->v.i >= 0 && wi->v.i <= 8 ? (guint64) wi->v.i : 9;
      if (widths[i] > 8)
        {
          g_bytes_unref (data);
          w42_pdf_obj_free (stream);
          return FALSE;
        }
      row += (gsize) widths[i];
    }

  d = g_bytes_get_data (data, &len);
  index = w42_pdf_dict_get (stream, "Index");
  {
    GArray *ranges = g_array_new (FALSE, FALSE, sizeof (gint64));

    if (index != NULL && index->kind == W42_PDF_ARRAY)
      for (guint i = 0; i < index->v.array->len; i++)
        {
          W42PdfObj *v = g_ptr_array_index (index->v.array, i);
          gint64 n = v->kind == W42_PDF_INT ? v->v.i : -1;

          g_array_append_val (ranges, n);
        }
    else
      {
        gint64 zero = 0;

        g_array_append_val (ranges, zero);
        g_array_append_val (ranges, size->v.i);
      }

    for (guint r = 0; r + 1 < ranges->len && row > 0; r += 2)
      {
        gint64 start = g_array_index (ranges, gint64, r);
        gint64 count = g_array_index (ranges, gint64, r + 1);

        if (start < 0 || count < 0)
          break;
        for (gint64 k = 0; k < count && pos + row <= len; k++)
          {
            guint64 field[3] = { 1, 0, 0 };

            for (int i = 0; i < 3; i++)
              {
                if (widths[i] == 0)
                  continue;
                field[i] = 0;
                for (guint64 j = 0; j < widths[i]; j++)
                  field[i] = (field[i] << 8) | d[pos++];
              }
            if (field[0] == 1 || field[0] == 2)
              xref_set (f, (guint64) (start + k), (guint8) field[0], field[1], field[2]);
          }
      }
    g_array_free (ranges, TRUE);
  }
  g_bytes_unref (data);

  *dict_out = stream->v.stream.dict;
  stream->v.stream.dict = w42_pdf_dict ();
  w42_pdf_obj_free (stream);
  return TRUE;
}

/* One cross-reference section and its trailer.  The trailer comes back
 * for the caller to follow /Prev and to keep. */
static W42PdfObj *
read_xref_section (W42PdfFile *f, gsize offset)
{
  Lex l = { f->d, f->len, offset };
  W42PdfObj *trailer = NULL;

  if (!at_keyword (&l, "xref"))
    return read_xref_stream (f, offset, &trailer) ? trailer : NULL;

  l.pos += 4;
  for (;;)
    {
      guint64 start, count;

      if (at_keyword (&l, "trailer"))
        break;
      if (!read_uint (&l, &start) || !read_uint (&l, &count))
        return NULL;
      for (guint64 i = 0; i < count; i++)
        {
          guint64 off, gen;

          if (!read_uint (&l, &off) || !read_uint (&l, &gen))
            return NULL;
          skip_ws (&l);
          if (l.pos >= l.len)
            return NULL;
          if (l.d[l.pos] == 'n')
            xref_set (f, start + i, 1, off, gen);
          l.pos++;
        }
    }
  l.pos += 7;
  trailer = parse_object (&l, 0);
  if (trailer != NULL && trailer->kind != W42_PDF_DICT)
    {
      w42_pdf_obj_free (trailer);
      return NULL;
    }
  /* A file written for both old readers and new keeps some objects in a
   * cross-reference stream that the table leaves out. */
  if (trailer != NULL)
    {
      W42PdfObj *hidden = w42_pdf_dict_get (trailer, "XRefStm"), *extra = NULL;

      if (hidden != NULL && hidden->kind == W42_PDF_INT && hidden->v.i > 0 &&
          (guint64) hidden->v.i < f->len &&
          read_xref_stream (f, (gsize) hidden->v.i, &extra))
        w42_pdf_obj_free (extra);
    }
  return trailer;
}

W42PdfFile *
w42_pdf_file_parse (GBytes *bytes, GError **error)
{
  W42PdfFile *f;
  gsize tail, at;
  guint64 offset;
  GArray *seen;
  gboolean ok = FALSE;

  g_return_val_if_fail (bytes != NULL, NULL);

  f = g_new0 (W42PdfFile, 1);
  f->bytes = g_bytes_ref (bytes);
  f->d = g_bytes_get_data (bytes, &f->len);
  f->xref = g_array_new (FALSE, TRUE, sizeof (XrefEntry));
  f->objects = g_ptr_array_new_with_free_func ((GDestroyNotify) w42_pdf_obj_free);
  f->objstms = g_hash_table_new_full (NULL, NULL, NULL, objstm_free);

  /* "startxref", near the end, says where the newest section is. */
  tail = f->len > 2048 ? f->len - 2048 : 0;
  at = G_MAXSIZE;
  for (gsize p = tail; p != G_MAXSIZE; )
    {
      gsize next = find_bytes (f->d, f->len, p, "startxref");

      if (next == G_MAXSIZE)
        break;
      at = next;
      p = next + 1;
    }
  if (at != G_MAXSIZE)
    {
      Lex l = { f->d, f->len, at + 9 };

      if (read_uint (&l, &offset) && offset < f->len)
        ok = TRUE;
    }

  seen = g_array_new (FALSE, FALSE, sizeof (guint64));
  while (ok)
    {
      W42PdfObj *trailer, *prev;
      gboolean again = FALSE;

      for (guint i = 0; i < seen->len; i++)
        if (g_array_index (seen, guint64, i) == offset)
          again = TRUE;
      if (again || seen->len >= MAX_SECTIONS)
        break;
      g_array_append_val (seen, offset);

      trailer = read_xref_section (f, (gsize) offset);
      if (trailer == NULL)
        {
          ok = f->trailer != NULL;
          break;
        }
      prev = w42_pdf_dict_get (trailer, "Prev");
      offset = prev != NULL && prev->kind == W42_PDF_INT && prev->v.i >= 0 ? (guint64) prev->v.i : f->len;
      if (f->trailer == NULL)
        f->trailer = trailer;
      else
        w42_pdf_obj_free (trailer);
      if (offset >= f->len)
        break;
    }
  g_array_free (seen, TRUE);

  if (ok && f->trailer != NULL)
    {
      f->loading = g_byte_array_sized_new (f->xref->len);
      g_byte_array_set_size (f->loading, f->xref->len);
      memset (f->loading->data, 0, f->loading->len);
      for (guint num = 1; num < f->xref->len; num++)
        load_object (f, num);

      /* The file's own scaffolding goes: the writer makes its own. */
      for (guint num = 0; num < f->objects->len; num++)
        {
          W42PdfObj *o = g_ptr_array_index (f->objects, num);
          W42PdfObj *type = w42_pdf_dict_get (o, "Type");

          if (o != NULL && o->kind == W42_PDF_STREAM &&
              (w42_pdf_is_name (type, "ObjStm") || w42_pdf_is_name (type, "XRef")))
            {
              w42_pdf_obj_free (o);
              g_ptr_array_index (f->objects, num) = NULL;
            }
        }
      g_byte_array_free (f->loading, TRUE);
      f->loading = NULL;
    }
  g_hash_table_destroy (f->objstms);
  f->objstms = NULL;
  g_array_free (f->xref, TRUE);
  f->xref = NULL;

  if (!ok || f->trailer == NULL || w42_pdf_file_catalog (f) == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   _("The PDF could not be read back to be finished."));
      w42_pdf_file_free (f);
      return NULL;
    }
  return f;
}

void
w42_pdf_file_free (W42PdfFile *file)
{
  if (file == NULL)
    return;
  g_ptr_array_free (file->objects, TRUE);
  w42_pdf_obj_free (file->trailer);
  if (file->objstms != NULL)
    g_hash_table_destroy (file->objstms);
  if (file->xref != NULL)
    g_array_free (file->xref, TRUE);
  if (file->loading != NULL)
    g_byte_array_free (file->loading, TRUE);
  g_bytes_unref (file->bytes);
  g_free (file);
}

W42PdfObj *
w42_pdf_file_trailer (W42PdfFile *file)
{
  return file->trailer;
}

W42PdfObj *
w42_pdf_file_get (W42PdfFile *file, guint num)
{
  return num < file->objects->len ? g_ptr_array_index (file->objects, num) : NULL;
}

W42PdfObj *
w42_pdf_file_resolve (W42PdfFile *file, W42PdfObj *obj)
{
  if (obj != NULL && obj->kind == W42_PDF_REF)
    return w42_pdf_file_get (file, obj->v.ref.num);
  return obj;
}

guint
w42_pdf_file_add (W42PdfFile *file, W42PdfObj *obj)
{
  guint num = MAX (file->objects->len, 1u);

  g_ptr_array_set_size (file->objects, num + 1);
  g_ptr_array_index (file->objects, num) = obj;
  return num;
}

static W42PdfObj *
file_kind_at (W42PdfFile *file, W42PdfObj *dict, const char *key, W42PdfKind kind)
{
  W42PdfObj *value = w42_pdf_file_resolve (file, w42_pdf_dict_get (dict, key));

  if (value != NULL && value->kind == kind)
    return value;
  value = kind == W42_PDF_DICT ? w42_pdf_dict () : w42_pdf_array ();
  w42_pdf_dict_set (dict, key, value);
  return value;
}

W42PdfObj *
w42_pdf_file_dict_at (W42PdfFile *file, W42PdfObj *dict, const char *key)
{
  return file_kind_at (file, dict, key, W42_PDF_DICT);
}

W42PdfObj *
w42_pdf_file_array_at (W42PdfFile *file, W42PdfObj *dict, const char *key)
{
  return file_kind_at (file, dict, key, W42_PDF_ARRAY);
}

W42PdfObj *
w42_pdf_file_catalog (W42PdfFile *file)
{
  W42PdfObj *root = w42_pdf_file_resolve (file, w42_pdf_dict_get (file->trailer, "Root"));

  return root != NULL && root->kind == W42_PDF_DICT ? root : NULL;
}

W42PdfObj *
w42_pdf_file_first_page (W42PdfFile *file, guint *num)
{
  W42PdfObj *catalog = w42_pdf_file_catalog (file);
  W42PdfObj *ref = w42_pdf_dict_get (catalog, "Pages");

  for (int depth = 0; depth < MAX_DEPTH && ref != NULL && ref->kind == W42_PDF_REF; depth++)
    {
      W42PdfObj *node = w42_pdf_file_get (file, ref->v.ref.num);
      W42PdfObj *kids;

      if (node == NULL || node->kind != W42_PDF_DICT)
        return NULL;
      if (w42_pdf_is_name (w42_pdf_dict_get (node, "Type"), "Page"))
        {
          if (num != NULL)
            *num = ref->v.ref.num;
          return node;
        }
      kids = w42_pdf_file_resolve (file, w42_pdf_dict_get (node, "Kids"));
      if (kids == NULL || kids->kind != W42_PDF_ARRAY || kids->v.array->len == 0)
        return NULL;
      ref = g_ptr_array_index (kids->v.array, 0);
    }
  return NULL;
}

/* ====================================================================== */
/* Writing                                                                 */
/* ====================================================================== */

typedef struct {
  guint8  type;          /* 1 at an offset, 2 in an object stream */
  guint64 a, b;
} OutEntry;

typedef struct {
  W42PdfFile               *f;
  const W42PdfWriteOptions *o;
  guint      *canon;       /* old number -> the one it is a copy of */
  guint      *renum;       /* old number -> new; 0 for one left out */
  guint       n_new;
  GByteArray *out;
  GArray     *entries;     /* OutEntry by new number */
  GArray     *placeholders;
  gboolean    encrypting;  /* the object being written has its strings encrypted */
  gsize       base;        /* where the text being made will land; G_MAXSIZE
                            * inside an object stream */
} Writer;

static void
put_name (GString *s, const guint8 *name, gsize len)
{
  g_string_append_c (s, '/');
  for (gsize i = 0; i < len; i++)
    {
      guint8 c = name[i];

      if (c < 0x21 || c > 0x7E || c == '#' || is_delim (c))
        g_string_append_printf (s, "#%02X", c);
      else
        g_string_append_c (s, (char) c);
    }
}

static void
put_hex (GString *s, const guint8 *data, gsize len)
{
  static const char digits[] = "0123456789ABCDEF";

  g_string_append_c (s, '<');
  for (gsize i = 0; i < len; i++)
    {
      g_string_append_c (s, digits[data[i] >> 4]);
      g_string_append_c (s, digits[data[i] & 15]);
    }
  g_string_append_c (s, '>');
}

static void
put_string (GString *s, const guint8 *data, gsize len)
{
  gsize odd = 0;

  for (gsize i = 0; i < len; i++)
    if (data[i] < 0x20 || data[i] > 0x7E)
      odd++;
  /* Mostly bytes that are not text -- UTF-16, a key -- read better and
   * cost less in hexadecimal. */
  if (odd * 4 > len)
    {
      put_hex (s, data, len);
      return;
    }
  g_string_append_c (s, '(');
  for (gsize i = 0; i < len; i++)
    {
      guint8 c = data[i];

      if (c == '(' || c == ')' || c == '\\')
        {
          g_string_append_c (s, '\\');
          g_string_append_c (s, (char) c);
        }
      else if (c < 0x20 || c > 0x7E)
        g_string_append_printf (s, "\\%03o", c);
      else
        g_string_append_c (s, (char) c);
    }
  g_string_append_c (s, ')');
}

static guint
new_number (Writer *w, guint old)
{
  if (old >= w->f->objects->len)
    return 0;
  return w->renum[w->canon[old]];
}

static void
put_obj (Writer *w, GString *s, W42PdfObj *o, gboolean stream_dict)
{
  switch (o->kind)
    {
    case W42_PDF_NULL:
      g_string_append (s, "null");
      break;
    case W42_PDF_BOOL:
      g_string_append (s, o->v.b ? "true" : "false");
      break;
    case W42_PDF_INT:
      g_string_append_printf (s, "%" G_GINT64_FORMAT, o->v.i);
      break;
    case W42_PDF_REAL:
      g_string_append (s, o->v.real);
      break;
    case W42_PDF_NAME:
      put_name (s, o->v.s.data, o->v.s.len);
      break;
    case W42_PDF_STRING:
      if (w->encrypting)
        {
          GBytes *enc = w->o->encrypt (w->o->encrypt_data, o->v.s.data, o->v.s.len);
          gsize n;
          const guint8 *d = g_bytes_get_data (enc, &n);

          put_hex (s, d, n);
          g_bytes_unref (enc);
        }
      else
        put_string (s, o->v.s.data, o->v.s.len);
      break;
    case W42_PDF_ARRAY:
      g_string_append_c (s, '[');
      for (guint i = 0; i < o->v.array->len; i++)
        {
          if (i > 0)
            g_string_append_c (s, ' ');
          put_obj (w, s, g_ptr_array_index (o->v.array, i), FALSE);
        }
      g_string_append_c (s, ']');
      break;
    case W42_PDF_DICT:
      g_string_append (s, "<<");
      for (guint i = 0; i + 1 < o->v.dict->len; i += 2)
        {
          const char *key = g_ptr_array_index (o->v.dict, i);

          /* A stream's length is the writer's to say. */
          if (stream_dict && strcmp (key, "Length") == 0)
            continue;
          g_string_append_c (s, ' ');
          put_name (s, (const guint8 *) key, strlen (key));
          g_string_append_c (s, ' ');
          put_obj (w, s, g_ptr_array_index (o->v.dict, i + 1), FALSE);
        }
      g_string_append (s, " >>");
      break;
    case W42_PDF_REF:
      {
        guint num = new_number (w, o->v.ref.num);

        if (num == 0)
          g_string_append (s, "null");
        else
          g_string_append_printf (s, "%u 0 R", num);
      }
      break;
    case W42_PDF_RAW:
      if (w->placeholders != NULL && w->base != G_MAXSIZE)
        {
          W42PdfPlaceholder p;

          p.tag = o->v.raw.tag;
          p.offset = w->base + s->len;
          p.length = strlen (o->v.raw.text);
          g_array_append_val (w->placeholders, p);
        }
      g_string_append (s, o->v.raw.text);
      break;
    case W42_PDF_STREAM:
    default:
      g_string_append (s, "null");
      break;
    }
}

/* Marks what can be reached from `num`, following references through a
 * stack rather than the C one: an outline's items and a long document's
 * pages are chains thousands long. */
static void
mark_refs (Writer *w, W42PdfObj *o, GArray *stack, gboolean stream_dict)
{
  switch (o->kind)
    {
    case W42_PDF_REF:
      if (o->v.ref.num < w->f->objects->len)
        {
          guint target = w->canon[o->v.ref.num];

          if (w->renum[target] == 0 && g_ptr_array_index (w->f->objects, target) != NULL)
            {
              w->renum[target] = 1;
              g_array_append_val (stack, target);
            }
        }
      break;
    case W42_PDF_ARRAY:
      for (guint i = 0; i < o->v.array->len; i++)
        mark_refs (w, g_ptr_array_index (o->v.array, i), stack, FALSE);
      break;
    case W42_PDF_DICT:
      for (guint i = 0; i + 1 < o->v.dict->len; i += 2)
        if (!stream_dict || strcmp (g_ptr_array_index (o->v.dict, i), "Length") != 0)
          mark_refs (w, g_ptr_array_index (o->v.dict, i + 1), stack, FALSE);
      break;
    case W42_PDF_STREAM:
      mark_refs (w, o->v.stream.dict, stack, TRUE);
      break;
    default:
      break;
    }
}

/* A stream packed as tightly as Flate will: one written plain is
 * deflated, and one cairo deflated at its default level is deflated
 * again at the best -- whichever comes out smaller is kept. */
static void
recompress (W42PdfObj *stream)
{
  W42PdfObj *filter = w42_pdf_dict_get (stream, "Filter");
  W42PdfObj *type = w42_pdf_dict_get (stream, "Type");
  gsize len;
  const guint8 *d = g_bytes_get_data (stream->v.stream.data, &len);
  GBytes *plain = NULL, *packed;

  /* XMP is meant to be found by programs that do not read PDF. */
  if (w42_pdf_is_name (type, "Metadata"))
    return;
  if (filter == NULL)
    plain = g_bytes_ref (stream->v.stream.data);
  else if (w42_pdf_dict_get (stream, "DecodeParms") == NULL &&
           (w42_pdf_is_name (filter, "FlateDecode") ||
            (filter->kind == W42_PDF_ARRAY && filter->v.array->len == 1 &&
             w42_pdf_is_name (g_ptr_array_index (filter->v.array, 0), "FlateDecode"))))
    plain = inflate (d, len);
  if (plain == NULL || len < 64)
    {
      if (plain != NULL)
        g_bytes_unref (plain);
      return;
    }

  packed = deflate_best (g_bytes_get_data (plain, NULL), g_bytes_get_size (plain));
  if (packed != NULL && g_bytes_get_size (packed) + 16 < len)
    {
      g_bytes_unref (stream->v.stream.data);
      stream->v.stream.data = g_bytes_ref (packed);
      w42_pdf_dict_set (stream, "Filter", w42_pdf_name ("FlateDecode"));
    }
  if (packed != NULL)
    g_bytes_unref (packed);
  g_bytes_unref (plain);
}

/* Streams that are byte for byte the same as one before them -- a
 * picture set twice that cairo embedded twice -- are made one. */
static void
find_copies (Writer *w)
{
  GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *objects = w->f->objects;
  GArray *placeholders = w->placeholders;

  /* Written with the old numbers, which is what makes two streams the
   * same: the same bytes pointing at the same things. */
  w->placeholders = NULL;
  for (guint i = 0; i < objects->len; i++)
    w->renum[i] = i;

  for (guint num = 1; num < objects->len; num++)
    {
      W42PdfObj *o = g_ptr_array_index (objects, num);
      GChecksum *sum;
      GString *text;
      gpointer first;
      gsize len;
      const guint8 *d;
      char *key;

      if (o == NULL || o->kind != W42_PDF_STREAM || num == w->o->unpacked)
        continue;
      text = g_string_new (NULL);
      w->canon[num] = num;
      put_obj (w, text, o->v.stream.dict, TRUE);
      d = g_bytes_get_data (o->v.stream.data, &len);
      sum = g_checksum_new (G_CHECKSUM_SHA256);
      g_checksum_update (sum, (const guchar *) text->str, (gssize) text->len);
      g_checksum_update (sum, (const guchar *) "\0", 1);
      if (len > 0)
        g_checksum_update (sum, d, (gssize) len);
      key = g_strdup (g_checksum_get_string (sum));
      g_checksum_free (sum);
      g_string_free (text, TRUE);

      if (g_hash_table_lookup_extended (seen, key, NULL, &first))
        {
          w->canon[num] = GPOINTER_TO_UINT (first);
          g_free (key);
        }
      else
        g_hash_table_insert (seen, key, GUINT_TO_POINTER (num));
    }
  g_hash_table_destroy (seen);
  memset (w->renum, 0, sizeof (guint) * objects->len);
  w->placeholders = placeholders;
}

static void
put_entry (Writer *w, guint num, guint8 type, guint64 a, guint64 b)
{
  OutEntry e = { type, a, b };

  if (num >= w->entries->len)
    g_array_set_size (w->entries, num + 1);
  g_array_index (w->entries, OutEntry, num) = e;
}

static void
out_append (Writer *w, const void *data, gsize len)
{
  g_byte_array_append (w->out, data, (guint) len);
}

static void
out_string (Writer *w, GString *s)
{
  out_append (w, s->str, s->len);
}

/* A stream as an object of its own: its dictionary with the length of
 * the bytes as they are written, encrypted or not. */
static void
write_stream (Writer *w, guint num, W42PdfObj *dict, GBytes *data, gboolean encrypt)
{
  GBytes *body = NULL;
  gsize len;
  const guint8 *d;
  GString *s = g_string_new (NULL);

  if (encrypt && w->o->encrypt != NULL)
    {
      d = g_bytes_get_data (data, &len);
      body = w->o->encrypt (w->o->encrypt_data, d, len);
    }
  else
    body = g_bytes_ref (data);
  d = g_bytes_get_data (body, &len);

  put_entry (w, num, 1, w->out->len, 0);
  g_string_append_printf (s, "%u 0 obj\n", num);
  w->base = w->out->len;
  put_obj (w, s, dict, TRUE);
  /* The length goes in before the closing of the dictionary. */
  g_string_truncate (s, s->len - 2);
  g_string_append_printf (s, "/Length %" G_GSIZE_FORMAT " >>\nstream\n", len);
  out_string (w, s);
  out_append (w, d, len);
  out_append (w, "\nendstream\nendobj\n", 18);
  g_string_free (s, TRUE);
  g_bytes_unref (body);
}

static void
flush_objstm (Writer *w, GString *head, GString *body, GArray *members)
{
  guint num;
  W42PdfObj *dict;
  GString *all;
  GBytes *packed;

  if (members->len == 0)
    return;
  num = ++w->n_new;
  for (guint i = 0; i < members->len; i++)
    put_entry (w, g_array_index (members, guint, i), 2, num, i);

  all = g_string_new_len (head->str, (gssize) head->len);
  g_string_append_len (all, body->str, (gssize) body->len);
  packed = deflate_best ((const guint8 *) all->str, all->len);
  dict = w42_pdf_dict ();
  w42_pdf_dict_set (dict, "Type", w42_pdf_name ("ObjStm"));
  w42_pdf_dict_set (dict, "N", w42_pdf_int (members->len));
  w42_pdf_dict_set (dict, "First", w42_pdf_int ((gint64) head->len));
  w42_pdf_dict_set (dict, "Filter", w42_pdf_name ("FlateDecode"));
  w->encrypting = FALSE;
  write_stream (w, num, dict, packed, TRUE);
  w42_pdf_obj_free (dict);
  g_bytes_unref (packed);
  g_string_free (all, TRUE);

  g_string_truncate (head, 0);
  g_string_truncate (body, 0);
  g_array_set_size (members, 0);
}

/* The trailer's references and identity, for the table or the stream. */
static void
put_trailer_keys (Writer *w, GString *s)
{
  static const char *const keys[] = { "Root", "Info", "Encrypt", "ID" };

  w->encrypting = FALSE;
  for (gsize i = 0; i < G_N_ELEMENTS (keys); i++)
    {
      W42PdfObj *v = w42_pdf_dict_get (w->f->trailer, keys[i]);

      if (v == NULL)
        continue;
      g_string_append_printf (s, " /%s ", keys[i]);
      put_obj (w, s, v, FALSE);
    }
}

GBytes *
w42_pdf_file_write (W42PdfFile               *file,
                    const W42PdfWriteOptions *options,
                    GArray                   *placeholders)
{
  Writer w;
  GArray *stack;
  guint n_old = file->objects->len;
  GString *s, *head, *body;
  GArray *members;
  static const char *const roots[] = { "Root", "Info", "Encrypt" };

  memset (&w, 0, sizeof w);
  w.f = file;
  w.o = options;
  w.canon = g_new0 (guint, n_old + 1);
  w.renum = g_new0 (guint, n_old + 1);
  w.out = g_byte_array_new ();
  w.entries = g_array_new (FALSE, TRUE, sizeof (OutEntry));
  w.placeholders = placeholders;

  for (guint i = 0; i < n_old; i++)
    w.canon[i] = i;
  if (options->compress)
    find_copies (&w);

  /* Every file gets an identity: two strings a reader may use to tell
   * one version of the file from another. */
  if (w42_pdf_dict_get (file->trailer, "ID") == NULL)
    {
      GChecksum *sum = g_checksum_new (G_CHECKSUM_MD5);
      gint64 now = g_get_real_time ();
      guint32 noise[4] = { g_random_int (), g_random_int (), g_random_int (), g_random_int () };
      guint8 id[16];
      gsize id_len = sizeof id;
      W42PdfObj *ids = w42_pdf_array ();

      g_checksum_update (sum, (const guchar *) &now, sizeof now);
      g_checksum_update (sum, (const guchar *) noise, sizeof noise);
      g_checksum_update (sum, g_bytes_get_data (file->bytes, NULL),
                         (gssize) MIN (g_bytes_get_size (file->bytes), (gsize) 65536));
      g_checksum_get_digest (sum, id, &id_len);
      g_checksum_free (sum);
      w42_pdf_array_add (ids, w42_pdf_string (id, id_len));
      w42_pdf_array_add (ids, w42_pdf_string (id, id_len));
      w42_pdf_dict_set (file->trailer, "ID", ids);
    }

  /* What the trailer can reach is the document; the rest -- the lengths
   * cairo wrote as objects of their own, the copies -- is left out. */
  stack = g_array_new (FALSE, FALSE, sizeof (guint));
  for (gsize i = 0; i < G_N_ELEMENTS (roots); i++)
    {
      W42PdfObj *v = w42_pdf_dict_get (file->trailer, roots[i]);

      if (v != NULL)
        mark_refs (&w, v, stack, FALSE);
    }
  while (stack->len > 0)
    {
      guint num = g_array_index (stack, guint, stack->len - 1);

      g_array_set_size (stack, stack->len - 1);
      mark_refs (&w, g_ptr_array_index (file->objects, num), stack, FALSE);
    }
  g_array_free (stack, TRUE);
  for (guint num = 1; num < n_old; num++)
    if (w.renum[num] != 0 && w.canon[num] == num)
      w.renum[num] = ++w.n_new;

  out_append (&w, "%PDF-1.7\n%\342\343\317\323\n", 15);
  s = g_string_new (NULL);
  head = g_string_new (NULL);
  body = g_string_new (NULL);
  members = g_array_new (FALSE, FALSE, sizeof (guint));

  for (guint num = 1; num < n_old; num++)
    {
      W42PdfObj *o = g_ptr_array_index (file->objects, num);
      guint nn = w.canon[num] == num ? w.renum[num] : 0;
      gboolean special = num == options->encrypt_dict || num == options->unpacked;

      if (nn == 0 || o == NULL)
        continue;
      w.encrypting = options->encrypt != NULL && num != options->encrypt_dict;

      if (o->kind == W42_PDF_STREAM)
        {
          if (options->compress)
            recompress (o);
          write_stream (&w, nn, o->v.stream.dict, o->v.stream.data, w.encrypting);
        }
      else if (options->compress && !special)
        {
          /* Inside an object stream a string is not encrypted on its
           * own: the stream round it is. */
          w.encrypting = FALSE;
          w.base = G_MAXSIZE;
          g_string_append_printf (head, "%u %" G_GSIZE_FORMAT " ", nn, body->len);
          put_obj (&w, body, o, FALSE);
          g_string_append_c (body, '\n');
          g_array_append_val (members, nn);
          if (members->len >= OBJSTM_SIZE)
            flush_objstm (&w, head, body, members);
        }
      else
        {
          put_entry (&w, nn, 1, w.out->len, 0);
          g_string_printf (s, "%u 0 obj\n", nn);
          w.base = w.out->len;
          put_obj (&w, s, o, FALSE);
          g_string_append (s, "\nendobj\n");
          out_string (&w, s);
        }
    }
  flush_objstm (&w, head, body, members);

  if (options->compress)
    {
      guint xref_num = ++w.n_new;
      gsize xref_at = w.out->len;
      guint64 widest = MAX ((guint64) xref_at, (guint64) w.n_new);
      int width = 1;
      GByteArray *rows = g_byte_array_new ();
      GBytes *packed;

      while (width < 8 && (widest >> (8 * width)) != 0)
        width++;
      put_entry (&w, xref_num, 1, xref_at, 0);
      for (guint num = 0; num <= w.n_new; num++)
        {
          OutEntry e = num < w.entries->len ? g_array_index (w.entries, OutEntry, num)
                                            : (OutEntry) { 0, 0, 0 };
          guint8 row[1 + 8 + 2];
          guint64 b = e.type == 0 ? (num == 0 ? 65535 : 0) : e.b;

          row[0] = e.type;
          for (int i = 0; i < width; i++)
            row[1 + i] = (guint8) (e.a >> (8 * (width - 1 - i)));
          row[1 + width] = (guint8) (b >> 8);
          row[2 + width] = (guint8) b;
          g_byte_array_append (rows, row, (guint) (3 + width));
        }
      packed = deflate_best (rows->data, rows->len);
      g_byte_array_free (rows, TRUE);

      g_string_printf (s, "%u 0 obj\n<< /Type /XRef /Size %u /W [1 %d 2]", xref_num,
                       w.n_new + 1, width);
      put_trailer_keys (&w, s);
      g_string_append_printf (s, " /Filter /FlateDecode /Length %" G_GSIZE_FORMAT " >>\nstream\n",
                              g_bytes_get_size (packed));
      out_string (&w, s);
      out_append (&w, g_bytes_get_data (packed, NULL), g_bytes_get_size (packed));
      g_string_printf (s, "\nendstream\nendobj\nstartxref\n%" G_GSIZE_FORMAT "\n%%%%EOF\n", xref_at);
      out_string (&w, s);
      g_bytes_unref (packed);
    }
  else
    {
      gsize xref_at = w.out->len;

      g_string_printf (s, "xref\n0 %u\n0000000000 65535 f\r\n", w.n_new + 1);
      for (guint num = 1; num <= w.n_new; num++)
        {
          OutEntry e = num < w.entries->len ? g_array_index (w.entries, OutEntry, num)
                                            : (OutEntry) { 0, 0, 0 };

          if (e.type == 1)
            g_string_append_printf (s, "%010" G_GUINT64_FORMAT " 00000 n\r\n", e.a);
          else
            g_string_append (s, "0000000000 00000 f\r\n");
        }
      g_string_append_printf (s, "trailer\n<< /Size %u", w.n_new + 1);
      put_trailer_keys (&w, s);
      g_string_append_printf (s, " >>\nstartxref\n%" G_GSIZE_FORMAT "\n%%%%EOF\n", xref_at);
      out_string (&w, s);
    }

  g_string_free (s, TRUE);
  g_string_free (head, TRUE);
  g_string_free (body, TRUE);
  g_array_free (members, TRUE);
  g_array_free (w.entries, TRUE);
  g_free (w.canon);
  g_free (w.renum);
  return g_byte_array_free_to_bytes (w.out);
}
