/* w42-style.c - see w42-style.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-style.h"

#include <string.h>

struct _W42StyleSheet {
  GPtrArray *styles;          /* W42Style*, in definition order */
  gboolean   number_headings;
};

static W42Style *
style_new (const char *name, const char *family, int size,
           gboolean bold, gboolean italic, int space_before, int space_after,
           int outline)
{
  W42Style *style = g_new0 (W42Style, 1);
  W42Fmt base;

  w42_fmt_init_default (&base);

  style->name = g_intern_string (name);
  style->ch = base.ch;
  style->pa = base.pa;

  style->ch.family = g_intern_string (family);
  style->ch.size   = size;
  style->ch.bold   = bold ? 1 : 0;
  style->ch.italic = italic ? 1 : 0;
  style->pa.style  = style->name;
  style->pa.space_before = space_before;
  style->pa.space_after  = space_after;
  style->outline = outline;
  /* The built-in styles are defined in full. */
  style->pa_own = W42_STYLE_PA_ALL;
  style->ch_own = W42_STYLE_CH_ALL;

  return style;
}

W42StyleSheet *
w42_stylesheet_new (void)
{
  W42StyleSheet *sheet = g_new0 (W42StyleSheet, 1);

  sheet->styles = g_ptr_array_new_with_free_func (g_free);

  /* Word 6's own definitions, near enough: Normal in 10pt Times, headings in
   * Arial with space above, a title centred and large. */
  g_ptr_array_add (sheet->styles,
    style_new ("Normal",    "Times New Roman", 20, FALSE, FALSE,   0,   0, 0));
  g_ptr_array_add (sheet->styles,
    style_new ("Heading 1", "Arial",           28, TRUE,  FALSE, 240, 120, 1));
  g_ptr_array_add (sheet->styles,
    style_new ("Heading 2", "Arial",           24, TRUE,  TRUE,  240,  60, 2));
  g_ptr_array_add (sheet->styles,
    style_new ("Heading 3", "Arial",           24, TRUE,  FALSE, 240,  60, 3));

  {
    W42Style *title = style_new ("Title", "Arial", 32, TRUE, FALSE, 240, 60, 0);
    title->pa.align = W42_ALIGN_CENTER;
    g_ptr_array_add (sheet->styles, title);
  }
  /* Word's Caption: the body face a size smaller, bold, with air above
   * and below so it sits apart from the picture and the text. */
  g_ptr_array_add (sheet->styles,
    style_new ("Caption",   "Times New Roman", 18, TRUE,  FALSE, 120, 120, 0));

  return sheet;
}

void
w42_stylesheet_free (W42StyleSheet *sheet)
{
  if (sheet == NULL)
    return;

  g_ptr_array_free (sheet->styles, TRUE);
  g_free (sheet);
}

guint
w42_stylesheet_size (W42StyleSheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, 0);
  return sheet->styles->len;
}

const W42Style *
w42_stylesheet_get (W42StyleSheet *sheet, guint index)
{
  g_return_val_if_fail (sheet != NULL, NULL);

  if (index >= sheet->styles->len)
    return NULL;

  return g_ptr_array_index (sheet->styles, index);
}

const W42Style *
w42_stylesheet_find (W42StyleSheet *sheet, const char *name)
{
  g_return_val_if_fail (sheet != NULL, NULL);

  if (name == NULL)
    return NULL;

  for (guint i = 0; i < sheet->styles->len; i++)
    {
      const W42Style *style = g_ptr_array_index (sheet->styles, i);

      if (g_ascii_strcasecmp (style->name, name) == 0)
        return style;
    }

  return NULL;
}

void
w42_stylesheet_set (W42StyleSheet *sheet, const W42Style *style)
{
  W42Style *copy;

  g_return_if_fail (sheet != NULL);
  g_return_if_fail (style != NULL && style->name != NULL);

  copy = g_memdup2 (style, sizeof *style);
  copy->name = g_intern_string (style->name);
  copy->pa.style = copy->name;
  if (copy->based_on != NULL)
    copy->based_on = g_intern_string (copy->based_on);

  for (guint i = 0; i < sheet->styles->len; i++)
    {
      const W42Style *existing = g_ptr_array_index (sheet->styles, i);

      if (g_ascii_strcasecmp (existing->name, copy->name) == 0)
        {
          copy->name = existing->name;
          copy->pa.style = existing->name;
          g_free (g_ptr_array_index (sheet->styles, i));
          g_ptr_array_index (sheet->styles, i) = copy;
          return;
        }
    }

  g_ptr_array_add (sheet->styles, copy);
}

int
w42_stylesheet_outline (W42StyleSheet *sheet, const char *name)
{
  const W42Style *style = w42_stylesheet_find (sheet, name);

  return style != NULL ? style->outline : 0;
}

gboolean
w42_stylesheet_get_number_headings (W42StyleSheet *sheet)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  return sheet->number_headings;
}

void
w42_stylesheet_set_number_headings (W42StyleSheet *sheet, gboolean on)
{
  g_return_if_fail (sheet != NULL);
  sheet->number_headings = on ? TRUE : FALSE;
}

gboolean
w42_stylesheet_remove (W42StyleSheet *sheet, const char *name)
{
  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  if (g_ascii_strcasecmp (name, "Normal") == 0)
    return FALSE;
  for (guint i = 0; i < sheet->styles->len; i++)
    {
      const W42Style *existing = g_ptr_array_index (sheet->styles, i);

      if (g_ascii_strcasecmp (existing->name, name) == 0)
        {
          const char *base = existing->based_on;
          const char *gone = existing->name;

          g_ptr_array_remove_index (sheet->styles, i);
          /* What was based on it is based on its base now. */
          for (guint k = 0; k < sheet->styles->len; k++)
            {
              W42Style *s = g_ptr_array_index (sheet->styles, k);

              if (s->based_on == gone)
                s->based_on = base;
            }
          return TRUE;
        }
    }
  return FALSE;
}

/* `dst` takes the base's values, then its own on top. */
static void
style_overlay (W42Style *dst, const W42Style *own, const W42Style *base)
{
  W42ParaFmt pa = base->pa;
  W42CharFmt ch = base->ch;

  if (own->pa_own & W42_STYLE_PA_ALIGN)        pa.align = own->pa.align;
  if (own->pa_own & W42_STYLE_PA_INDENT_LEFT)  pa.indent_left = own->pa.indent_left;
  if (own->pa_own & W42_STYLE_PA_INDENT_RIGHT) pa.indent_right = own->pa.indent_right;
  if (own->pa_own & W42_STYLE_PA_INDENT_FIRST) pa.indent_first = own->pa.indent_first;
  if (own->pa_own & W42_STYLE_PA_SPACE_BEFORE) pa.space_before = own->pa.space_before;
  if (own->pa_own & W42_STYLE_PA_SPACE_AFTER)  pa.space_after = own->pa.space_after;
  if (own->pa_own & W42_STYLE_PA_LINE_SPACING)
    {
      pa.line_spacing = own->pa.line_spacing;
      pa.line_spacing_pct = own->pa.line_spacing_pct;
    }
  if (own->pa_own & W42_STYLE_PA_FLOW)
    {
      pa.keep_next = own->pa.keep_next;
      pa.keep_together = own->pa.keep_together;
      pa.widow_control = own->pa.widow_control;
    }
  /* A style keeps what no bit speaks for.  Tab stops, borders, shading,
   * the list kind, a page break, a dropped capital, a frame and the
   * paragraph's direction have no own-bit, so taking them from the base
   * would empty them out of the style the moment the base was edited. */
  pa.page_break_before = own->pa.page_break_before;
  pa.rtl               = own->pa.rtl;
  pa.list              = own->pa.list;
  pa.list_start        = own->pa.list_start;
  pa.list_level        = own->pa.list_level;
  pa.border            = own->pa.border;
  memcpy (pa.edge, own->pa.edge, sizeof pa.edge);
  pa.shading           = own->pa.shading;
  pa.section_break     = own->pa.section_break;
  pa.columns           = own->pa.columns;
  pa.column_gap        = own->pa.column_gap;
  pa.drop_cap          = own->pa.drop_cap;
  pa.frame_side        = own->pa.frame_side;
  pa.frame_width       = own->pa.frame_width;
  pa.n_tabs            = own->pa.n_tabs;
  memcpy (pa.tab_kind, own->pa.tab_kind, sizeof pa.tab_kind);
  memcpy (pa.tab_pos, own->pa.tab_pos, sizeof pa.tab_pos);

  if (own->ch_own & W42_STYLE_CH_FAMILY)    ch.family = own->ch.family;
  if (own->ch_own & W42_STYLE_CH_SIZE)      ch.size = own->ch.size;
  if (own->ch_own & W42_STYLE_CH_BOLD)      ch.bold = own->ch.bold;
  if (own->ch_own & W42_STYLE_CH_ITALIC)    ch.italic = own->ch.italic;
  if (own->ch_own & W42_STYLE_CH_UNDERLINE) ch.underline = own->ch.underline;
  if (own->ch_own & W42_STYLE_CH_COLOR)     ch.color = own->ch.color;
  /* A character style keeps its paragraph half out of it. */
  if (own->character)
    pa = own->pa;
  dst->pa = pa;
  dst->pa.style = dst->name;
  dst->ch = ch;
}

static void
follow_depth (W42StyleSheet *sheet, const char *name, int depth)
{
  W42Style *style = NULL;

  if (depth > 12)
    return;                           /* a loop of bases: stop */
  for (guint i = 0; i < sheet->styles->len; i++)
    {
      W42Style *s = g_ptr_array_index (sheet->styles, i);

      if (g_ascii_strcasecmp (s->name, name) == 0)
        style = s;
    }
  if (style == NULL)
    return;
  if (style->based_on != NULL && g_ascii_strcasecmp (style->based_on, style->name) != 0)
    {
      const W42Style *base = w42_stylesheet_find (sheet, style->based_on);

      if (base != NULL)
        style_overlay (style, style, base);
    }
  for (guint i = 0; i < sheet->styles->len; i++)
    {
      const W42Style *s = g_ptr_array_index (sheet->styles, i);

      if (s->based_on != NULL && g_ascii_strcasecmp (s->based_on, style->name) == 0 && s != style)
        follow_depth (sheet, s->name, depth + 1);
    }
}

void
w42_stylesheet_follow (W42StyleSheet *sheet, const char *name)
{
  g_return_if_fail (sheet != NULL);
  g_return_if_fail (name != NULL);
  follow_depth (sheet, name, 0);
}

void
w42_style_own_from_base (const W42Style *style, const W42Style *base,
                         guint32 *pa_own, guint32 *ch_own)
{
  guint32 p = 0, c = 0;

  g_return_if_fail (style != NULL && pa_own != NULL && ch_own != NULL);
  if (base == NULL)
    {
      *pa_own = W42_STYLE_PA_ALL;
      *ch_own = W42_STYLE_CH_ALL;
      return;
    }
  if (style->pa.align != base->pa.align)               p |= W42_STYLE_PA_ALIGN;
  if (style->pa.indent_left != base->pa.indent_left)   p |= W42_STYLE_PA_INDENT_LEFT;
  if (style->pa.indent_right != base->pa.indent_right) p |= W42_STYLE_PA_INDENT_RIGHT;
  if (style->pa.indent_first != base->pa.indent_first) p |= W42_STYLE_PA_INDENT_FIRST;
  if (style->pa.space_before != base->pa.space_before) p |= W42_STYLE_PA_SPACE_BEFORE;
  if (style->pa.space_after != base->pa.space_after)   p |= W42_STYLE_PA_SPACE_AFTER;
  if (style->pa.line_spacing != base->pa.line_spacing ||
      style->pa.line_spacing_pct != base->pa.line_spacing_pct) p |= W42_STYLE_PA_LINE_SPACING;
  if (style->pa.keep_next != base->pa.keep_next || style->pa.keep_together != base->pa.keep_together ||
      style->pa.widow_control != base->pa.widow_control) p |= W42_STYLE_PA_FLOW;
  if (style->ch.family != base->ch.family)       c |= W42_STYLE_CH_FAMILY;
  if (style->ch.size != base->ch.size)           c |= W42_STYLE_CH_SIZE;
  if (style->ch.bold != base->ch.bold)           c |= W42_STYLE_CH_BOLD;
  if (style->ch.italic != base->ch.italic)       c |= W42_STYLE_CH_ITALIC;
  if (style->ch.underline != base->ch.underline) c |= W42_STYLE_CH_UNDERLINE;
  if (style->ch.color != base->ch.color)         c |= W42_STYLE_CH_COLOR;
  *pa_own = p;
  *ch_own = c;
}

const char **
w42_stylesheet_descendants (W42StyleSheet *sheet, const char *name)
{
  GPtrArray *out;
  gboolean grew = TRUE;

  g_return_val_if_fail (sheet != NULL && name != NULL, NULL);

  out = g_ptr_array_new ();
  g_ptr_array_add (out, (gpointer) name);
  /* Widen until nothing new is based on anything in the set. */
  while (grew)
    {
      grew = FALSE;
      for (guint i = 0; i < sheet->styles->len; i++)
        {
          const W42Style *s = g_ptr_array_index (sheet->styles, i);
          gboolean in = FALSE, base_in = FALSE;

          for (guint k = 0; k < out->len; k++)
            {
              const char *n = g_ptr_array_index (out, k);

              if (g_ascii_strcasecmp (n, s->name) == 0) in = TRUE;
              if (s->based_on != NULL && g_ascii_strcasecmp (n, s->based_on) == 0) base_in = TRUE;
            }
          if (base_in && !in)
            {
              g_ptr_array_add (out, (gpointer) s->name);
              grew = TRUE;
            }
        }
    }
  g_ptr_array_remove_index (out, 0);   /* the name itself is not a descendant */
  g_ptr_array_add (out, NULL);
  return (const char **) g_ptr_array_free (out, FALSE);
}
