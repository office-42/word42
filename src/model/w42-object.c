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

void
w42_object_table_set_position (W42ObjectTable *table, W42ObjectIdx idx,
                               gboolean positioned, int x, int y)
{
  W42Object *object;

  g_return_if_fail (table != NULL);
  if (idx >= table->objects->len)
    return;
  object = g_ptr_array_index (table->objects, idx);
  object->positioned = positioned;
  object->pos_x = positioned ? x : 0;
  object->pos_y = positioned ? y : 0;
}

void
w42_object_table_set_shape (W42ObjectTable *table, W42ObjectIdx idx,
                            W42ShapeKind kind, double line_pt, guint32 line_rgb,
                            gboolean filled, guint32 fill_rgb, const char *text)
{
  W42Object *object;

  g_return_if_fail (table != NULL);
  if (idx >= table->objects->len)
    return;
  object = g_ptr_array_index (table->objects, idx);
  object->shape = kind;
  object->line_pt = MAX (line_pt, 0.0);
  object->line_rgb = line_rgb & 0xFFFFFF;
  object->filled = filled;
  object->fill_rgb = fill_rgb & 0xFFFFFF;
  object->text = text != NULL && *text != '\0' ? g_intern_string (text) : NULL;
}

W42ObjectIdx
w42_object_table_clone (W42ObjectTable *table, W42ObjectIdx idx, int width, int height)
{
  const W42Object *object;
  W42ObjectIdx fresh;
  W42Object *copy;

  g_return_val_if_fail (table != NULL, W42_OBJECT_NONE);
  if (idx >= table->objects->len)
    return W42_OBJECT_NONE;
  object = g_ptr_array_index (table->objects, idx);
  fresh = w42_object_table_add (table, object->data, object->format,
                                object->pixel_w, object->pixel_h, width, height);
  if (fresh == W42_OBJECT_NONE)
    return fresh;
  copy = g_ptr_array_index (table->objects, fresh);
  copy->wrap = object->wrap;
  copy->positioned = object->positioned;
  copy->pos_x = object->pos_x;
  copy->pos_y = object->pos_y;
  copy->shape = object->shape;
  copy->line_pt = object->line_pt;
  copy->line_rgb = object->line_rgb;
  copy->filled = object->filled;
  copy->fill_rgb = object->fill_rgb;
  copy->text = object->text;
  return fresh;
}
