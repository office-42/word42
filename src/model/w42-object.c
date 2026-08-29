/* w42-object.c - see w42-object.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-object.h"

#include "w42-image.h"

struct _W42ObjectTable {
  GPtrArray *objects;    /* W42Object* */
};

static void
object_free (gpointer data)
{
  W42Object *object = data;

  g_clear_pointer (&object->data, g_bytes_unref);
  g_clear_pointer (&object->surface, cairo_surface_destroy);
  g_free (object);
}

W42ObjectTable *
w42_object_table_new (void)
{
  W42ObjectTable *table = g_new0 (W42ObjectTable, 1);

  table->objects = g_ptr_array_new_with_free_func (object_free);
  return table;
}

void
w42_object_table_free (W42ObjectTable *table)
{
  if (table == NULL)
    return;

  g_ptr_array_free (table->objects, TRUE);
  g_free (table);
}

W42ObjectIdx
w42_object_table_add (W42ObjectTable *table,
                      GBytes         *data,
                      const char     *format,
                      int             pixel_w,
                      int             pixel_h,
                      int             width,
                      int             height)
{
  W42Object *object;

  g_return_val_if_fail (table != NULL, W42_OBJECT_NONE);
  g_return_val_if_fail (data != NULL, W42_OBJECT_NONE);

  object = g_new0 (W42Object, 1);
  object->data    = g_bytes_ref (data);
  object->format  = g_intern_string (format != NULL ? format : "unknown");
  object->pixel_w = pixel_w;
  object->pixel_h = pixel_h;
  object->width   = width;
  object->height  = height;

  g_ptr_array_add (table->objects, object);
  return (W42ObjectIdx) (table->objects->len - 1);
}

const W42Object *
w42_object_table_get (W42ObjectTable *table, W42ObjectIdx idx)
{
  g_return_val_if_fail (table != NULL, NULL);

  if (idx >= table->objects->len)
    return NULL;

  return g_ptr_array_index (table->objects, idx);
}

guint
w42_object_table_size (W42ObjectTable *table)
{
  g_return_val_if_fail (table != NULL, 0);
  return table->objects->len;
}

cairo_surface_t *
w42_object_surface (W42ObjectTable *table, W42ObjectIdx idx)
{
  W42Object *object;

  g_return_val_if_fail (table != NULL, NULL);

  if (idx >= table->objects->len)
    return NULL;

  object = g_ptr_array_index (table->objects, idx);

  if (object->surface == NULL)
    object->surface = w42_image_surface (object->data);

  return object->surface;
}

void
w42_object_table_set_wrap (W42ObjectTable *table, W42ObjectIdx idx, W42Wrap wrap)
{
  g_return_if_fail (table != NULL);

  if (idx < table->objects->len)
    ((W42Object *) g_ptr_array_index (table->objects, idx))->wrap = wrap;
}
