/* w42-image.c - see w42-image.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-image.h"

#include <math.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <string.h>

/* A picture that would decode to more than 40 million pixels is scaled
 * down as it is read: a small file can claim a huge size. */
static void
image_size_prepared (GdkPixbufLoader *loader, int width, int height, gpointer data)
{
  const double limit = 40e6;

  (void) data;
  if (width > 0 && height > 0 && (double) width * height > limit)
    {
      /* Only some loaders scale while decoding; the rest would decode
       * the whole picture first.  Such a picture is refused instead. */
      g_object_set_data (G_OBJECT (loader), "w42-too-big", GINT_TO_POINTER (1));
      gdk_pixbuf_loader_set_size (loader, 1, 1);
    }
}

static GdkPixbufLoader *
image_loader_new (void)
{
  GdkPixbufLoader *loader = gdk_pixbuf_loader_new ();

  g_signal_connect (loader, "size-prepared", G_CALLBACK (image_size_prepared), NULL);
  return loader;
}

static GdkPixbuf *
decode (GBytes *data, const char **format)
{
  GdkPixbufLoader *loader;
  GdkPixbuf *pixbuf = NULL;
  gsize len = 0;
  const guint8 *bytes;

  if (data == NULL)
    return NULL;

  bytes = g_bytes_get_data (data, &len);
  if (bytes == NULL || len == 0)
    return NULL;      /* nothing to decode, and gdk-pixbuf shouts about it */
  loader = image_loader_new ();

  {
    gboolean written = gdk_pixbuf_loader_write (loader, bytes, len, NULL);
    gboolean closed = gdk_pixbuf_loader_close (loader, NULL);   /* once, whatever write did */

    if (written && closed && g_object_get_data (G_OBJECT (loader), "w42-too-big") == NULL)
      goto decoded;
    g_object_unref (loader);
    return NULL;
  }
 decoded:
    {
      pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
      if (pixbuf != NULL)
        {
          /* GIFs and the like come back as animations; the first frame is
           * the picture. */
          g_object_ref (pixbuf);

          if (format != NULL)
            {
              GdkPixbufFormat *f = gdk_pixbuf_loader_get_format (loader);
              char *name = f != NULL ? gdk_pixbuf_format_get_name (f) : NULL;
              *format = g_intern_string (name != NULL ? name : "unknown");
              g_free (name);
            }
        }
    }

  g_object_unref (loader);
  return pixbuf;
}

gboolean
w42_image_probe (GBytes *data, int *width, int *height, const char **format)
{
  GdkPixbuf *pixbuf = decode (data, format);

  if (pixbuf == NULL)
    return FALSE;

  if (width)  *width  = gdk_pixbuf_get_width (pixbuf);
  if (height) *height = gdk_pixbuf_get_height (pixbuf);

  g_object_unref (pixbuf);
  return TRUE;
}

GBytes *
w42_image_load_file (GFile       *file,
                     int         *width,
                     int         *height,
                     const char **format,
                     GError     **error)
{
  char *contents = NULL;
  gsize length = 0;
  GBytes *data;

  g_return_val_if_fail (G_IS_FILE (file), NULL);

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return NULL;

  data = g_bytes_new_take (contents, length);

  if (!w42_image_probe (data, width, height, format))
    {
      g_bytes_unref (data);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "That file is not a picture in any format Word42 "
                   "can read.");
      return NULL;
    }

  return data;
}

/* gdk-pixbuf keeps pixels as straight RGB(A) bytes; cairo wants premultiplied
 * BGRA in native byte order.  This is the conversion GTK does inside
 * gdk_cairo_set_source_pixbuf(), done here so that the layout engine can draw
 * without GTK. */
cairo_surface_t *
w42_image_surface (GBytes *data)
{
  GdkPixbuf *pixbuf = decode (data, NULL);
  cairo_surface_t *surface;
  int width, height, src_stride, dst_stride, channels;
  const guint8 *src;
  guint8 *dst;
  gboolean alpha;

  if (pixbuf == NULL)
    return NULL;

  width  = gdk_pixbuf_get_width (pixbuf);
  height = gdk_pixbuf_get_height (pixbuf);
  src_stride = gdk_pixbuf_get_rowstride (pixbuf);
  channels = gdk_pixbuf_get_n_channels (pixbuf);
  alpha = gdk_pixbuf_get_has_alpha (pixbuf);
  src = gdk_pixbuf_read_pixels (pixbuf);

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS)
    {
      cairo_surface_destroy (surface);
      g_object_unref (pixbuf);
      return NULL;
    }

  cairo_surface_flush (surface);
  dst = cairo_image_surface_get_data (surface);
  dst_stride = cairo_image_surface_get_stride (surface);

  for (int y = 0; y < height; y++)
    {
      const guint8 *s = src + y * src_stride;
      guint32 *d = (guint32 *) (dst + y * dst_stride);

      for (int x = 0; x < width; x++)
        {
          guint32 r = s[0], g = s[1], b = s[2];
          guint32 a = alpha ? s[3] : 255;

          if (a != 255)
            {
              r = (r * a + 127) / 255;
              g = (g * a + 127) / 255;
              b = (b * a + 127) / 255;
            }

          d[x] = (a << 24) | (r << 16) | (g << 8) | b;
          s += channels;
        }
    }

  cairo_surface_mark_dirty (surface);
  g_object_unref (pixbuf);

  return surface;
}

GBytes *
w42_image_to_png (GBytes *data)
{
  GdkPixbuf *pixbuf = decode (data, NULL);
  char *buffer = NULL;
  gsize length = 0;

  if (pixbuf == NULL)
    return NULL;

  if (!gdk_pixbuf_save_to_buffer (pixbuf, &buffer, &length, "png", NULL, NULL))
    {
      g_object_unref (pixbuf);
      return NULL;
    }

  g_object_unref (pixbuf);
  return g_bytes_new_take (buffer, length);
}

static cairo_status_t
append_to_bytes (void *closure, const unsigned char *data, unsigned int length)
{
  g_byte_array_append (closure, data, length);
  return CAIRO_STATUS_SUCCESS;
}

GBytes *
w42_image_surface_to_png (cairo_surface_t *surface)
{
  GByteArray *array;

  if (surface == NULL)
    return NULL;

  array = g_byte_array_new ();

  if (cairo_surface_write_to_png_stream (surface, append_to_bytes, array)
      != CAIRO_STATUS_SUCCESS)
    {
      g_byte_array_free (array, TRUE);
      return NULL;
    }

  return g_byte_array_free_to_bytes (array);
}
