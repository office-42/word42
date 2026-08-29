/* w42-zip.c - see w42-zip.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-zip.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* CRC-32, as zip wants it                                                 */
/* ---------------------------------------------------------------------- */

static guint32 crc_table[256];

static void
crc_init (void)
{
  static gboolean done = FALSE;

  if (done)
    return;
  for (guint32 n = 0; n < 256; n++)
    {
      guint32 c = n;

      for (int k = 0; k < 8; k++)
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      crc_table[n] = c;
    }
  done = TRUE;
}

static guint32
crc32_of (const guint8 *data, gsize len)
{
  guint32 c = 0xFFFFFFFFu;

  crc_init ();
  for (gsize i = 0; i < len; i++)
    c = crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------- */
/* Deflate through GLib                                                    */
/* ---------------------------------------------------------------------- */

static GBytes *
convert_all (GConverter *converter, const guint8 *in, gsize in_len, gsize max_out)
{
  GByteArray *out = g_byte_array_new ();
  guint8 buf[65536];
  gsize in_pos = 0;
  gboolean done = FALSE;

  while (!done)
    {
      gsize read = 0, written = 0;
      GConverterResult res;
      GError *err = NULL;

      res = g_converter_convert (converter, in + in_pos, in_len - in_pos,
                                 buf, sizeof buf,
                                 in_pos >= in_len ? G_CONVERTER_INPUT_AT_END : G_CONVERTER_NO_FLAGS,
                                 &read, &written, &err);
      if (res == G_CONVERTER_ERROR)
        {
          g_clear_error (&err);
          g_byte_array_free (out, TRUE);
          return NULL;
        }
      in_pos += read;
      if (max_out > 0 && out->len + written > max_out)
        {
          g_byte_array_free (out, TRUE);      /* more than the entry claims: broken */
          return NULL;
        }
      g_byte_array_append (out, buf, written);
      if (res == G_CONVERTER_FINISHED)
        done = TRUE;
      else if (read == 0 && written == 0 && in_pos >= in_len)
        done = TRUE;          /* nothing more can happen */
    }
  return g_byte_array_free_to_bytes (out);
}

static GBytes *
inflate_raw (const guint8 *in, gsize in_len, gsize max_out)
{
  GZlibDecompressor *dec = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW);
  GBytes *out = convert_all (G_CONVERTER (dec), in, in_len, max_out);

  g_object_unref (dec);
  return out;
}

static GBytes *
deflate_raw (const guint8 *in, gsize in_len)
{
  GZlibCompressor *comp = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_RAW, 6);
  GBytes *out = convert_all (G_CONVERTER (comp), in, in_len, 0);

  g_object_unref (comp);
  return out;
}

/* ---------------------------------------------------------------------- */
/* Reading                                                                 */
/* ---------------------------------------------------------------------- */

typedef struct {
  char   *name;
  guint16 method;
  guint32 crc;
  guint32 comp_size;
  guint32 size;
  guint32 local_offset;
} ZipEntry;

struct _W42Zip {
  GBytes *bytes;
  GArray *entries;     /* ZipEntry */
};

static guint16 rd16 (const guint8 *p) { return (guint16) (p[0] | (p[1] << 8)); }
static guint32 rd32 (const guint8 *p) { return (guint32) p[0] | ((guint32) p[1] << 8) | ((guint32) p[2] << 16) | ((guint32) p[3] << 24); }

static void
entry_clear (gpointer data)
{
  g_free (((ZipEntry *) data)->name);
}

W42Zip *
w42_zip_new_from_bytes (GBytes *bytes, GError **error)
{
  gsize len;
  const guint8 *d = g_bytes_get_data (bytes, &len);
  gsize eocd = 0;
  gboolean found = FALSE;
  guint32 n_entries, cd_offset, cd_size;
  W42Zip *zip;
  const guint8 *p, *end;

  /* The end-of-central-directory record is at the end, before a comment
   * of up to 64K. */
  if (len < 22)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Not a zip file.");
      return NULL;
    }
  for (gsize i = len - 22; ; i--)
    {
      if (rd32 (d + i) == 0x06054b50)
        {
          eocd = i;
          found = TRUE;
          break;
        }
      if (i == 0 || len - i > 65536 + 22)
        break;
    }
  if (!found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Not a zip file.");
      return NULL;
    }

  n_entries = rd16 (d + eocd + 10);
  cd_size   = rd32 (d + eocd + 12);
  cd_offset = rd32 (d + eocd + 16);
  if ((gsize) cd_offset + cd_size > len)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The zip file is cut short.");
      return NULL;
    }

  zip = g_new0 (W42Zip, 1);
  zip->bytes = g_bytes_ref (bytes);
  zip->entries = g_array_new (FALSE, TRUE, sizeof (ZipEntry));
  g_array_set_clear_func (zip->entries, entry_clear);

  p = d + cd_offset;
  end = d + cd_offset + cd_size;
  for (guint32 i = 0; i < n_entries && p + 46 <= end; i++)
    {
      ZipEntry e;
      guint16 name_len, extra_len, comment_len;

      if (rd32 (p) != 0x02014b50)
        break;
      e.method       = rd16 (p + 10);
      e.crc          = rd32 (p + 16);
      e.comp_size    = rd32 (p + 20);
      e.size         = rd32 (p + 24);
      name_len       = rd16 (p + 28);
      extra_len      = rd16 (p + 30);
      comment_len    = rd16 (p + 32);
      e.local_offset = rd32 (p + 42);
      if (p + 46 + name_len > end)
        break;
      e.name = g_strndup ((const char *) p + 46, name_len);
      g_array_append_val (zip->entries, e);
      p += 46 + name_len + extra_len + comment_len;
    }
  return zip;
}

W42Zip *
w42_zip_open (GFile *file, GError **error)
{
  GBytes *bytes = g_file_load_bytes (file, NULL, NULL, error);
  W42Zip *zip;

  if (bytes == NULL)
    return NULL;
  zip = w42_zip_new_from_bytes (bytes, error);
  g_bytes_unref (bytes);
  return zip;
}

void
w42_zip_free (W42Zip *zip)
{
  if (zip == NULL)
    return;
  g_array_free (zip->entries, TRUE);
  g_bytes_unref (zip->bytes);
  g_free (zip);
}

static const ZipEntry *
find_entry (W42Zip *zip, const char *name)
{
  for (guint i = 0; i < zip->entries->len; i++)
    {
      const ZipEntry *e = &g_array_index (zip->entries, ZipEntry, i);

      if (g_str_equal (e->name, name))
        return e;
    }
  return NULL;
}

gboolean
w42_zip_has (W42Zip *zip, const char *name)
{
  return zip != NULL && find_entry (zip, name) != NULL;
}

GBytes *
w42_zip_read (W42Zip *zip, const char *name)
{
  const ZipEntry *e;
  gsize len;
  const guint8 *d;
  gsize data_at;
  guint16 name_len, extra_len;

  g_return_val_if_fail (zip != NULL, NULL);

  e = find_entry (zip, name);
  if (e == NULL)
    return NULL;
  d = g_bytes_get_data (zip->bytes, &len);
  if ((gsize) e->local_offset + 30 > len || rd32 (d + e->local_offset) != 0x04034b50)
    return NULL;
  name_len  = rd16 (d + e->local_offset + 26);
  extra_len = rd16 (d + e->local_offset + 28);
  data_at = (gsize) e->local_offset + 30 + name_len + extra_len;
  if (data_at + e->comp_size > len)
    return NULL;

  if (e->method == 0)
    return g_bytes_new (d + data_at, e->comp_size);
  if (e->method == 8)
    /* The entry's own size bounds the output; an entry that claims none
     * still gets a ceiling, so a small file cannot unpack without end. */
    return inflate_raw (d + data_at, e->comp_size,
                        e->size > 0 ? MIN (e->size, 256u << 20) : 256u << 20);
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* Writing                                                                 */
/* ---------------------------------------------------------------------- */

typedef struct {
  char   *name;
  GBytes *data;       /* as stored in the file */
  guint16 method;
  guint32 crc;
  guint32 size;
  guint32 offset;
} WriteEntry;

struct _W42ZipWriter {
  GArray *entries;
};

static void
write_entry_clear (gpointer data)
{
  WriteEntry *e = data;

  g_free (e->name);
  g_bytes_unref (e->data);
}

W42ZipWriter *
w42_zip_writer_new (void)
{
  W42ZipWriter *w = g_new0 (W42ZipWriter, 1);

  w->entries = g_array_new (FALSE, TRUE, sizeof (WriteEntry));
  g_array_set_clear_func (w->entries, write_entry_clear);
  return w;
}

void
w42_zip_writer_add (W42ZipWriter *writer, const char *name, const void *data, gsize length)
{
  WriteEntry e;
  GBytes *packed;

  g_return_if_fail (writer != NULL && name != NULL);
  if (strlen (name) > 65535 || length > 0xFFFFFFF0u || writer->entries->len >= 65535)
    return;                                 /* beyond what plain zip can hold */

  e.name = g_strdup (name);
  e.crc = crc32_of (data, length);
  e.size = (guint32) length;
  packed = length > 0 ? deflate_raw (data, length) : NULL;
  if (packed != NULL && g_bytes_get_size (packed) < length)
    {
      e.data = packed;
      e.method = 8;
    }
  else
    {
      if (packed != NULL)
        g_bytes_unref (packed);
      e.data = g_bytes_new (data, length);
      e.method = 0;
    }
  e.offset = 0;
  g_array_append_val (writer->entries, e);
}

static void
put16 (GByteArray *out, guint16 v)
{
  guint8 b[2] = { (guint8) v, (guint8) (v >> 8) };
  g_byte_array_append (out, b, 2);
}

static void
put32 (GByteArray *out, guint32 v)
{
  guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
  g_byte_array_append (out, b, 4);
}

gboolean
w42_zip_writer_save (W42ZipWriter *writer, GFile *file, GError **error)
{
  GByteArray *out = g_byte_array_new ();
  guint32 cd_start, cd_size;
  gboolean ok;

  g_return_val_if_fail (writer != NULL, FALSE);

  /* Local headers and data. */
  for (guint i = 0; i < writer->entries->len; i++)
    {
      WriteEntry *e = &g_array_index (writer->entries, WriteEntry, i);
      gsize packed_len;
      const guint8 *packed = g_bytes_get_data (e->data, &packed_len);

      e->offset = out->len;
      put32 (out, 0x04034b50);
      put16 (out, 20);                 /* version needed */
      put16 (out, 0x0800);             /* flags: UTF-8 names */
      put16 (out, e->method);
      put16 (out, 0); put16 (out, 0x21);   /* time, date: 1980-01-01 */
      put32 (out, e->crc);
      put32 (out, (guint32) packed_len);
      put32 (out, e->size);
      put16 (out, (guint16) strlen (e->name));
      put16 (out, 0);
      g_byte_array_append (out, (const guint8 *) e->name, strlen (e->name));
      g_byte_array_append (out, packed, packed_len);
    }

  /* The central directory. */
  cd_start = out->len;
  for (guint i = 0; i < writer->entries->len; i++)
    {
      const WriteEntry *e = &g_array_index (writer->entries, WriteEntry, i);

      put32 (out, 0x02014b50);
      put16 (out, 20); put16 (out, 20);
      put16 (out, 0x0800);
      put16 (out, e->method);
      put16 (out, 0); put16 (out, 0x21);
      put32 (out, e->crc);
      put32 (out, (guint32) g_bytes_get_size (e->data));
      put32 (out, e->size);
      put16 (out, (guint16) strlen (e->name));
      put16 (out, 0); put16 (out, 0);
      put16 (out, 0); put16 (out, 0);
      put32 (out, 0);
      put32 (out, e->offset);
      g_byte_array_append (out, (const guint8 *) e->name, strlen (e->name));
    }
  cd_size = out->len - cd_start;

  put32 (out, 0x06054b50);
  put16 (out, 0); put16 (out, 0);
  put16 (out, (guint16) writer->entries->len);
  put16 (out, (guint16) writer->entries->len);
  put32 (out, cd_size);
  put32 (out, cd_start);
  put16 (out, 0);

  ok = g_file_replace_contents (file, (const char *) out->data, out->len, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);
  g_byte_array_free (out, TRUE);
  return ok;
}

void
w42_zip_writer_free (W42ZipWriter *writer)
{
  if (writer == NULL)
    return;
  g_array_free (writer->entries, TRUE);
  g_free (writer);
}
