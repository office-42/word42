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
  g_clear_pointer (&object->original, g_bytes_unref);
  g_clear_pointer (&object->surface, cairo_surface_destroy);
  g_clear_pointer (&object->math, w42_math_box_free);
  g_free (object->mathml);
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
  object->width   = CLAMP (width, 15, W42_OBJECT_MAX_TWIPS);
  object->height  = CLAMP (height, 15, W42_OBJECT_MAX_TWIPS);

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

/* A picture another object has already decoded, so the same bytes are
 * decoded once however many times a file sets them: the same GBytes,
 * which is what a reader hands every frame of one drawing, or an equal
 * one of the same size, which is what a .docx reading the same part
 * twice makes.  The surface is cairo's, reference counted, so each
 * object that shares it holds one reference and object_free lets go of
 * one; the byte compare is only reached when the sizes already match and
 * the pointers differ, so it is rare and never on the common path. */
static cairo_surface_t *
shared_surface (W42ObjectTable *table, const W42Object *object)
{
  for (guint i = 0; i < table->objects->len; i++)
    {
      const W42Object *other = g_ptr_array_index (table->objects, i);

      if (other == object || other->surface == NULL || other->data == NULL)
        continue;
      if (other->data == object->data ||
          (other->pixel_w == object->pixel_w && other->pixel_h == object->pixel_h &&
           object->data != NULL && g_bytes_equal (other->data, object->data)))
        return cairo_surface_reference (other->surface);
    }
  return NULL;
}

cairo_surface_t *
w42_object_surface (W42ObjectTable *table, W42ObjectIdx idx)
{
  W42Object *object;

  g_return_val_if_fail (table != NULL, NULL);

  if (idx >= table->objects->len)
    return NULL;

  object = g_ptr_array_index (table->objects, idx);

  if (object->surface == NULL && object->data != NULL)
    {
      /* A picture bomb -- a small file whose forty frames all name one
       * large picture -- would otherwise decode and cache a full-size
       * surface for each; they share the one decode instead. */
      object->surface = shared_surface (table, object);
      if (object->surface == NULL)
        object->surface = w42_image_surface (object->data);
    }

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

void
w42_object_table_set_original (W42ObjectTable *table, W42ObjectIdx idx,
                               GBytes *bytes, const char *format)
{
  W42Object *object;

  g_return_if_fail (table != NULL);
  if (idx >= table->objects->len)
    return;
  object = g_ptr_array_index (table->objects, idx);
  g_clear_pointer (&object->original, g_bytes_unref);
  object->original = bytes != NULL ? g_bytes_ref (bytes) : NULL;
  object->original_format = format != NULL ? g_intern_string (format) : NULL;
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
  copy->original = object->original != NULL ? g_bytes_ref (object->original) : NULL;
  copy->original_format = object->original_format;
  copy->mathml = g_strdup (object->mathml);
  /* The depth goes with the height, as the equation is drawn scaled. */
  copy->descent = object->height > 0
                    ? (int) ((double) object->descent * copy->height / object->height + 0.5) : 0;
  return fresh;
}

/* The size equations are set at before they are scaled to their box:
 * everything in one is in proportion to it, so one size does for all. */
#define MATH_SET_SIZE 10.0

W42ObjectIdx
w42_object_table_add_math (W42ObjectTable *table, const char *mathml, double size)
{
  char *canonical;
  int width, height, descent, pw = 0, ph = 0;
  GBytes *png;
  W42ObjectIdx idx;
  W42Object *object;

  g_return_val_if_fail (table != NULL, W42_OBJECT_NONE);
  canonical = mathml != NULL ? w42_math_canonical (mathml, -1) : NULL;
  if (canonical == NULL)
    return W42_OBJECT_NONE;
  if (size <= 0.0)
    size = 10.0;
  if (!w42_math_measure (canonical, size, &width, &height, &descent) ||
      (png = w42_math_render_png (canonical, size, 192, &pw, &ph)) == NULL)
    {
      g_free (canonical);
      return W42_OBJECT_NONE;
    }
  idx = w42_object_table_add (table, png, "png", pw, ph, width, height);
  g_bytes_unref (png);
  object = g_ptr_array_index (table->objects, idx);
  object->mathml = canonical;
  object->descent = CLAMP (descent, 0, object->height);
  return idx;
}

void
w42_object_table_set_math (W42ObjectTable *table, W42ObjectIdx idx, const char *mathml,
                           int descent)
{
  W42Object *object;

  g_return_if_fail (table != NULL);
  if (idx >= table->objects->len)
    return;
  object = g_ptr_array_index (table->objects, idx);
  g_clear_pointer (&object->math, w42_math_box_free);
  g_free (object->mathml);
  object->mathml = g_strdup (mathml);
  object->descent = CLAMP (descent, 0, object->height);
}

const W42MathBox *
w42_object_math (W42ObjectTable *table, W42ObjectIdx idx)
{
  W42Object *object;

  g_return_val_if_fail (table != NULL, NULL);
  if (idx >= table->objects->len)
    return NULL;
  object = g_ptr_array_index (table->objects, idx);
  if (object->mathml == NULL)
    return NULL;
  if (object->math == NULL)
    {
      W42MathNode *root = w42_math_parse (object->mathml, -1, NULL);

      if (root == NULL)
        return NULL;
      object->math = w42_math_box_new (root, MATH_SET_SIZE);
      w42_math_node_free (root);
    }
  return object->math;
}
