/* w42-layout.c - see w42-layout.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-layout.h"

#include <math.h>
#include <string.h>

struct _W42Layout {
  PangoContext *ctx;
  PangoContext *ctx_rtl;   /* the same, for right-to-left paragraphs */
  GPtrArray    *blocks;    /* W42Block*, owned */
  GPtrArray    *layouts;   /* PangoLayout*, one per block, owned */
  GArray       *lines;     /* W42LineBox */
  int           n_pages;

  double        page_w;
  double        page_h;
  double        mar_l;
  double        mar_t;
  double        mar_r;
  double        mar_b;
  gboolean      galley;
  gboolean      show_marks;

  W42ObjectTable *objects;   /* the document's pictures; not owned */
  GArray         *floats;    /* W42FloatBox: the wrapped pictures placed */
  GArray         *caps;      /* CapBox: dropped letters placed */
  gboolean        gridlines; /* show the cells of unruled tables */
  GPtrArray      *cap_layouts;   /* their PangoLayouts */
  W42StyleSheet  *styles;    /* not owned */
  W42ApTable     *aps;       /* not owned; for the backdrop painter */
  double        text_w;      /* the column pictures must fit inside */
  GPtrArray    *prefixes;    /* PangoLayout*, the section numbers */
  W42Spell     *spell;       /* not owned; NULL for no underlining */
  gsize         spell_caret;
  GArray       *furniture;   /* W42Furniture: headers and footers, per page */
  GPtrArray    *furniture_layouts;
  GArray       *cell_rects;  /* W42CellRect: table cell borders */
  GPtrArray    *note_marks;  /* PangoLayout*, the footnote numbers in the text */
  GArray       *note_rules;  /* W42NoteRule: the separator above the notes */
};

typedef struct {
  int    page;
  double x;      /* the column's left edge */
  double y;
} W42NoteRule;

/* A dropped letter: its layout drawn with its ink's top-left at (x, y). */
typedef struct {
  int          page;
  double       x, y;
  PangoLayout *layout;     /* owned by cap_layouts */
  double       ink_x, ink_y;
} CapBox;

/* One line of a paragraph laid out in one or two layouts: the part set
 * beside a picture, frame or dropped letter is narrow, the rest full. */
typedef struct {
  PangoLayout     *layout;
  PangoLayoutLine *line;
  PangoRectangle   logical;
  double           baseline;      /* px from the line's top */
  guint            start_index, length;
  gboolean         narrow;
} BlockLine;

static guint8 cell_sides (W42ApTable *aps, const W42Block *blk, gboolean table_borders);

#define NOTE_SEP 12.0   /* px between the text and the notes, for the rule */

#define CELL_PAD 4.0

/* Pictures are drawn by Pango's shape renderer.  A picture is one U+FFFC in
 * the block's text carrying a shape attribute with the picture's size, so
 * Pango breaks lines around it and measures it like any other glyph; when
 * the line is painted Pango calls back here for the shape, on whatever cairo
 * context is being painted to -- the screen, the printer or a PDF. */
static void
shape_renderer (cairo_t        *cr,
                PangoAttrShape *attr,
                gboolean        do_path,
                gpointer        data)
{
  W42Layout *self = data;
  guint tag = GPOINTER_TO_UINT (attr->data);
  W42ObjectIdx idx = (tag & 0x7fffffffu) - 1;
  cairo_surface_t *surface;
  const W42Object *object;
  double x, y, w, h, sx, sy;

  if (do_path || attr->data == NULL)
    return;                       /* NULL data: a glyph of no size, hidden */

  /* A footnote's number, raised above the baseline. */
  if (tag & 0x80000000u)
    {
      if (idx < self->note_marks->len)
        {
          PangoLayout *mark = g_ptr_array_index (self->note_marks, idx);

          cairo_get_current_point (cr, &x, &y);
          cairo_save (cr);
          cairo_move_to (cr, x, y + (double) attr->logical_rect.y / PANGO_SCALE);
          pango_cairo_show_layout (cr, mark);
          cairo_restore (cr);
        }
      return;
    }

  if (self->objects == NULL)
    return;

  object = w42_object_table_get (self->objects, idx);
  if (object == NULL || object->wrap != W42_WRAP_INLINE)
    return;
  surface = w42_object_surface (self->objects, idx);
  if (surface == NULL)
    return;

  /* The current point is the glyph origin: the baseline, at the left. */
  cairo_get_current_point (cr, &x, &y);

  w = (double) attr->logical_rect.width / PANGO_SCALE;
  h = (double) attr->logical_rect.height / PANGO_SCALE;
  y += (double) attr->logical_rect.y / PANGO_SCALE;

  sx = w / cairo_image_surface_get_width (surface);
  sy = h / cairo_image_surface_get_height (surface);

  cairo_save (cr);
  cairo_translate (cr, x, y);
  cairo_scale (cr, sx, sy);
  cairo_set_source_surface (cr, surface, 0, 0);
  cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
  cairo_paint (cr);
  cairo_restore (cr);
}

/* ---------------------------------------------------------------------- */
/* Position mapping                                                        */
/* ---------------------------------------------------------------------- */

/* A block's characters occupy consecutive document positions right after the
 * paragraph mark, so the mapping is just a character count. */
gsize
w42_block_byte_to_pos (const W42Block *block, gsize byte)
{
  glong chars;

  g_return_val_if_fail (block != NULL, 0);

  if (byte > block->text->len)
    byte = block->text->len;

  chars = g_utf8_pointer_to_offset (block->text->str,
                                    block->text->str + byte);

  return block->start_pos + 1 + (gsize) chars;
}

gsize
w42_block_pos_to_byte (const W42Block *block, gsize pos)
{
  const char *p;
  gsize chars;

  g_return_val_if_fail (block != NULL, 0);

  if (pos <= block->start_pos)
    return 0;

  chars = pos - block->start_pos - 1;
  p = g_utf8_offset_to_pointer (block->text->str, (glong) chars);

  if (p < block->text->str)
    return 0;
  if ((gsize) (p - block->text->str) > block->text->len)
    return block->text->len;

  return (gsize) (p - block->text->str);
}

int
w42_layout_block_at_pos (W42Layout *self, gsize pos)
{
  int found = 0;

  g_return_val_if_fail (self != NULL, 0);

  if (self->blocks == NULL || self->blocks->len == 0)
    return -1;

  /* The last block whose paragraph mark comes before `pos`. */
  for (guint i = 0; i < self->blocks->len; i++)
    {
      const W42Block *block = g_ptr_array_index (self->blocks, i);

      if (block->start_pos < pos)
        found = (int) i;
      else
        break;
    }

  return found;
}

/* ---------------------------------------------------------------------- */
/* Footnotes                                                               */
/* ---------------------------------------------------------------------- */

static PangoLayout *build_block_layout (W42Layout *self, const W42Block *block,
                                        W42ApTable *aps, double text_width,
                                        double extra_indent, guint cap_bytes,
                                        guint hide_from, guint hide_to);

/* Lays out note `id`'s paragraphs at y = 0 into `out`, returning their
 * height.  The blocks' lines are placed when the page is finished. */
static double
layout_note (W42Layout *self, W42ApTable *aps, int id, double text_w,
             GArray *out, double y_base)
{
  double y = y_base;

  for (guint b = 0; b < self->blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (self->blocks, b);
      const W42ParaFmt *pa;
      PangoLayout *layout;
      PangoLayoutIter *iter;
      double indent_x;

      if (block->note != id)
        continue;

      pa = &w42_ap_table_get (aps, block->ap)->pa;
      indent_x = w42_twips_to_px (pa->indent_left);
      layout = build_block_layout (self, block, aps, text_w, 0.0, 0, 0, 0);
      g_ptr_array_add (self->layouts, layout);

      iter = pango_layout_get_iter (layout);
      do
        {
          PangoLayoutLine *line = pango_layout_iter_get_line_readonly (iter);
          PangoRectangle logical;
          W42LineBox box;

          pango_layout_iter_get_line_extents (iter, NULL, &logical);

          box.page        = -1;
          box.origin_x    = self->mar_l + indent_x;
          box.x           = box.origin_x + (double) logical.x / PANGO_SCALE;
          box.y           = y;
          box.width       = (double) logical.width / PANGO_SCALE;
          box.height      = (double) logical.height / PANGO_SCALE;
          box.baseline    = (double) (pango_layout_iter_get_baseline (iter) -
                                      logical.y) / PANGO_SCALE;
          box.block       = (int) b;
          box.start_index = line->start_index;
          box.length      = line->length;
          box.line        = line;
          box.prefix      = NULL;
          box.prefix_x    = box.origin_x;

          g_array_append_val (out, box);
          y += box.height;
        }
      while (pango_layout_iter_next_line (iter));
      pango_layout_iter_free (iter);
    }

  return y - y_base;
}

/* The notes gathered for a page go to its foot -- or, in the galley, after
 * the text -- and the page's note rule is recorded. */
/* A section's newspaper columns: its first "column page", how many
 * columns a real page holds, and their width and gap.  Column pages are
 * folded on to real pages a section at a time. */
typedef struct {
  int    first_cp;
  int    n;
  double w;
  double gap;
} SectionCols;

/* The last page of a section with columns is balanced: its text is
 * shared out so the columns end level, as Word did.  Not when a table or
 * footnotes sit there, whose shapes are not lines.  `first_cp` is the
 * page's first column page, `end_cp` one past the section's last. */
static void
balance_last_page (W42Layout *self, int first_cp, int end_cp, int n_columns)
{
  gboolean plain = TRUE;
  GArray *idx = g_array_new (FALSE, FALSE, sizeof (guint));
  double total = 0.0;

  for (guint i = 0; i < self->cell_rects->len && plain; i++)
    {
      int p = g_array_index (self->cell_rects, W42CellRect, i).page;
      if (p >= first_cp && p < end_cp)
        plain = FALSE;
    }
  for (guint i = 0; i < self->note_rules->len && plain; i++)
    {
      int p = g_array_index (self->note_rules, W42NoteRule, i).page;
      if (p >= first_cp && p < end_cp)
        plain = FALSE;
    }

  for (guint i = 0; i < self->lines->len; i++)
    {
      const W42LineBox *box = &g_array_index (self->lines, W42LineBox, i);

      if (box->page >= first_cp && box->page < end_cp)
        g_array_append_val (idx, i);
    }

  /* Lines come in flow order: column page ascending, y ascending.  Each
   * line's share of height is the gap to the next line in its column, or
   * its own height for the last. */
  if (plain && idx->len > 1 && (int) idx->len > n_columns)
    {
      double *share = g_new (double, idx->len);
      double cum = 0.0, target;
      int col = 0;
      double col_y = 0.0;

      for (guint k = 0; k < idx->len; k++)
        {
          const W42LineBox *box = &g_array_index (self->lines, W42LineBox, g_array_index (idx, guint, k));
          const W42LineBox *next = k + 1 < idx->len
            ? &g_array_index (self->lines, W42LineBox, g_array_index (idx, guint, k + 1)) : NULL;

          share[k] = (next != NULL && next->page == box->page && next->y > box->y)
                       ? next->y - box->y : box->height;
          total += share[k];
        }
      target = total / n_columns;

      for (guint k = 0; k < idx->len; k++)
        {
          W42LineBox *box = &g_array_index (self->lines, W42LineBox, g_array_index (idx, guint, k));

          if (col + 1 < n_columns && cum + share[k] / 2.0 > target * (col + 1))
            {
              col++;
              col_y = 0.0;
            }
          box->page = first_cp + col;
          box->y = self->mar_t + col_y;
          col_y += share[k];
          cum += share[k];
        }
      g_free (share);
    }
  g_array_free (idx, TRUE);
}

static void
flush_notes (W42Layout *self, GArray *page_notes, double *notes_h,
             int page, double text_h, double *y_io)
{
  double y0;

  if (page_notes->len == 0)
    {
      *notes_h = 0.0;
      return;
    }

  if (self->galley)
    y0 = self->mar_t + *y_io + NOTE_SEP;
  else
    y0 = self->mar_t + text_h - *notes_h + NOTE_SEP;

  {
    W42NoteRule rule = { page, self->mar_l, y0 - NOTE_SEP / 2 };
    g_array_append_val (self->note_rules, rule);
  }

  for (guint i = 0; i < page_notes->len; i++)
    {
      W42LineBox box = g_array_index (page_notes, W42LineBox, i);

      box.page = page;
      box.y += y0;
      g_array_append_val (self->lines, box);
    }

  if (self->galley)
    *y_io += *notes_h;

  g_array_set_size (page_notes, 0);
  *notes_h = 0.0;
}

/* ---------------------------------------------------------------------- */
/* Building                                                                */
/* ---------------------------------------------------------------------- */

W42Layout *
w42_layout_new (void)
{
  W42Layout *self = g_new0 (W42Layout, 1);

  self->ctx = pango_font_map_create_context (pango_cairo_font_map_get_default ());

  /* Fix the resolution so that a 12pt font is 16px tall no matter what the
   * screen or the desktop's text scaling says.  Zoom happens later. */
  pango_cairo_context_set_resolution (self->ctx, W42_LAYOUT_DPI);
  pango_cairo_context_set_shape_renderer (self->ctx, shape_renderer, self, NULL);
  self->ctx_rtl = pango_font_map_create_context (pango_cairo_font_map_get_default ());
  pango_cairo_context_set_resolution (self->ctx_rtl, W42_LAYOUT_DPI);
  {
    /* Text is drawn smoothed in grey, lightly hinted, with unhinted
     * metrics so that letters keep their spacing at every zoom; the
     * default on some systems is aliased or heavily hinted text. */
    cairo_font_options_t *opts = cairo_font_options_create ();

    cairo_font_options_set_antialias (opts, CAIRO_ANTIALIAS_GRAY);
    cairo_font_options_set_hint_style (opts, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_hint_metrics (opts, CAIRO_HINT_METRICS_OFF);
    pango_cairo_context_set_font_options (self->ctx, opts);
    pango_cairo_context_set_font_options (self->ctx_rtl, opts);
    pango_context_set_round_glyph_positions (self->ctx, FALSE);
    pango_context_set_round_glyph_positions (self->ctx_rtl, FALSE);
    cairo_font_options_destroy (opts);
  }
  pango_cairo_context_set_shape_renderer (self->ctx_rtl, shape_renderer, self, NULL);
  pango_context_set_base_dir (self->ctx_rtl, PANGO_DIRECTION_RTL);

  self->layouts = g_ptr_array_new_with_free_func (g_object_unref);
  self->prefixes = g_ptr_array_new_with_free_func (g_object_unref);
  self->spell_caret = (gsize) -1;
  self->furniture = g_array_new (FALSE, FALSE, sizeof (W42Furniture));
  self->cell_rects = g_array_new (FALSE, FALSE, sizeof (W42CellRect));
  self->note_marks = g_ptr_array_new_with_free_func (g_object_unref);
  self->note_rules = g_array_new (FALSE, FALSE, sizeof (W42NoteRule));
  self->floats = g_array_new (FALSE, FALSE, sizeof (W42FloatBox));
  self->caps = g_array_new (FALSE, FALSE, sizeof (CapBox));
  self->cap_layouts = g_ptr_array_new_with_free_func (g_object_unref);
  self->furniture_layouts = g_ptr_array_new_with_free_func (g_object_unref);
  self->lines   = g_array_new (FALSE, FALSE, sizeof (W42LineBox));
  self->n_pages = 1;

  return self;
}

void
w42_layout_free (W42Layout *self)
{
  if (self == NULL)
    return;

  g_clear_pointer (&self->blocks, g_ptr_array_unref);
  g_ptr_array_free (self->layouts, TRUE);
  g_ptr_array_free (self->prefixes, TRUE);
  g_ptr_array_free (self->furniture_layouts, TRUE);
  g_array_free (self->furniture, TRUE);
  g_array_free (self->cell_rects, TRUE);
  g_ptr_array_free (self->note_marks, TRUE);
  g_array_free (self->note_rules, TRUE);
  g_array_free (self->floats, TRUE);
  g_array_free (self->caps, TRUE);
  g_ptr_array_free (self->cap_layouts, TRUE);
  g_array_free (self->lines, TRUE);
  g_object_unref (self->ctx);
  g_object_unref (self->ctx_rtl);
  g_free (self);
}

void
w42_layout_set_galley (W42Layout *self, gboolean galley)
{
  g_return_if_fail (self != NULL);
  self->galley = galley;
}

void
w42_layout_set_show_marks (W42Layout *self, gboolean show)
{
  g_return_if_fail (self != NULL);
  self->show_marks = show;
}

gboolean
w42_layout_get_galley (W42Layout *self)
{
  g_return_val_if_fail (self != NULL, FALSE);
  return self->galley;
}

static void
add_attr (PangoAttrList  *list,
          PangoAttribute *attr,
          guint           start,
          guint           end)
{
  attr->start_index = start;
  attr->end_index   = end;
  pango_attr_list_insert (list, attr);
}

/* A family list for Pango: the font the document asks for, then faces
 * that cover the scripts a Latin font has no glyphs for.  Pango tries
 * them in turn, character by character, so a paragraph in one font can
 * still show Chinese, Japanese, Korean, Thai and emoji.  Only the
 * display uses this; the document keeps the family it was given. */
static const char *
family_chain (const char *family)
{
  static GHashTable *cache;
  const char *want = family != NULL && *family != '\0' ? family : "Serif";
  const char *found;
  char *chain;

  if (cache == NULL)
    cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  found = g_hash_table_lookup (cache, want);
  if (found != NULL)
    return found;

  chain = g_strconcat (want,
                       /* Chinese, Japanese and Korean */
                       ",Noto Sans CJK SC,Noto Sans CJK JP,Noto Sans CJK KR,Source Han Sans"
                       ",Microsoft YaHei,SimSun,MS Gothic,Meiryo,Yu Gothic,Malgun Gothic"
                       ",PingFang SC,Hiragino Sans,Apple SD Gothic Neo,WenQuanYi Zen Hei"
                       /* Thai, Devanagari, Arabic and Hebrew */
                       ",Noto Sans Thai,Leelawadee UI,Tahoma"
                       ",Noto Sans Devanagari,Nirmala UI"
                       ",Noto Naskh Arabic,Segoe UI,Arial Unicode MS"
                       /* and the emoji */
                       ",Noto Color Emoji,Segoe UI Emoji,Apple Color Emoji,Symbola",
                       NULL);
  found = g_intern_string (chain);
  g_hash_table_insert (cache, g_strdup (want), (gpointer) found);
  g_free (chain);
  return found;
}

static void
apply_font_description (PangoFontDescription *desc, const W42CharFmt *ch)
{
  pango_font_description_set_family (desc, family_chain (ch->family));
  pango_font_description_set_size (desc,
    (int) (w42_halfpt_to_pt (ch->size > 0 ? ch->size : 20) * PANGO_SCALE));
  pango_font_description_set_weight (desc,
    ch->bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_style (desc,
    ch->italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
}

static PangoAttrList *
build_attributes (W42Layout *self, const W42Block *block, W42ApTable *aps)
{
  PangoAttrList *list = pango_attr_list_new ();

  for (guint i = 0; i < block->runs->len; i++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, i);
      const W42Fmt *fmt = w42_ap_table_get (aps, run->ap);
      const W42CharFmt *ch = &fmt->ch;
      guint start = (guint) run->byte_offset;
      guint end   = (guint) (run->byte_offset + run->n_bytes);
      int size_pu = (int) (w42_halfpt_to_pt (ch->size > 0 ? ch->size : 20) *
                           PANGO_SCALE);

      if (start == end)
        continue;

      if (run->footnote > 0)
        {
          /* The note's number in the run's font at two thirds size, sat
           * above the baseline the way a superscript is. */
          PangoLayout *mark = pango_layout_new (self->ctx);
          PangoFontDescription *desc = pango_font_description_new ();
          PangoRectangle rect;
          char number[16];
          int mw = 0, mh = 0;

          apply_font_description (desc, ch);
          pango_font_description_set_size (desc, (int) (size_pu * 0.65));
          pango_layout_set_font_description (mark, desc);
          pango_font_description_free (desc);
          if (run->endnote)
            w42_roman_lower (run->footnote, number, sizeof number);
          else
            g_snprintf (number, sizeof number, "%d", run->footnote);
          pango_layout_set_text (mark, number, -1);
          pango_layout_get_pixel_size (mark, &mw, &mh);

          rect.x = 0;
          rect.y = -(int) (mh * 1.15 * PANGO_SCALE);
          rect.width = (mw + 1) * PANGO_SCALE;
          rect.height = (int) (mh * 1.15 * PANGO_SCALE);

          g_ptr_array_add (self->note_marks, mark);
          add_attr (list,
                    pango_attr_shape_new_with_data (&rect, &rect,
                      GUINT_TO_POINTER (0x80000000u | self->note_marks->len), NULL, NULL),
                    start, end);
          continue;
        }

      if (run->object != W42_OBJECT_NONE && self->objects != NULL)
        {
          const W42Object *object = w42_object_table_get (self->objects,
                                                          run->object);
          PangoRectangle rect;
          double w, h;

          if (object == NULL)
            continue;

          if (object->wrap != W42_WRAP_INLINE)
            {
              /* The picture floats beside the paragraph; its anchor is a
               * character of no size, so the caret can still sit at it. */
              rect.x = rect.y = rect.width = rect.height = 0;
              add_attr (list,
                        pango_attr_shape_new_with_data (&rect, &rect,
                          GUINT_TO_POINTER (run->object + 1), NULL, NULL),
                        start, end);
              continue;
            }

          w = w42_twips_to_px (object->width);
          h = w42_twips_to_px (object->height);

          /* A picture wider than the column is shown scaled to fit it.  The
           * document keeps the size that was asked for; only the display
           * yields, and only as far as it must. */
          if (w > self->text_w && self->text_w > 0)
            {
              h = h * (self->text_w / w);
              w = self->text_w;
            }

          rect.x = 0;
          rect.y = -(int) (h * PANGO_SCALE);
          rect.width = (int) (w * PANGO_SCALE);
          rect.height = (int) (h * PANGO_SCALE);

          add_attr (list,
                    pango_attr_shape_new_with_data (&rect, &rect,
                      GUINT_TO_POINTER (run->object + 1), NULL, NULL),
                    start, end);
          continue;
        }

      add_attr (list, pango_attr_family_new (family_chain (ch->family)),
                start, end);
      add_attr (list, pango_attr_weight_new (ch->bold ? PANGO_WEIGHT_BOLD
                                                      : PANGO_WEIGHT_NORMAL),
                start, end);
      add_attr (list, pango_attr_style_new (ch->italic ? PANGO_STYLE_ITALIC
                                                       : PANGO_STYLE_NORMAL),
                start, end);
      add_attr (list, pango_attr_underline_new (ch->underline
                                                  ? PANGO_UNDERLINE_SINGLE
                                                  : PANGO_UNDERLINE_NONE),
                start, end);
      add_attr (list, pango_attr_strikethrough_new (ch->strikeout != 0),
                start, end);
      if (ch->overline)
        add_attr (list, pango_attr_overline_new (PANGO_OVERLINE_SINGLE), start, end);
      if (ch->smallcaps)
        add_attr (list, pango_attr_variant_new (PANGO_VARIANT_SMALL_CAPS), start, end);
      if (ch->allcaps)
        add_attr (list, pango_attr_text_transform_new (PANGO_TEXT_TRANSFORM_UPPERCASE),
                  start, end);
      if (ch->spacing != 0)
        add_attr (list, pango_attr_letter_spacing_new (
                    (int) (w42_twips_to_px (ch->spacing) * PANGO_SCALE)), start, end);
      if (ch->comment != NULL && ch->highlight == 0)
        add_attr (list, pango_attr_background_new (0xffff, 0xf5f5, 0xb0b0), start, end);
      else if (ch->field != NULL && ch->highlight == 0)
        add_attr (list, pango_attr_background_new (0xe6e6, 0xe6e6, 0xe6e6), start, end);
      if (ch->highlight != 0)
        {
          guint32 rgb = w42_highlight_rgb (ch->highlight);

          add_attr (list,
                    pango_attr_background_new ((guint16) (((rgb >> 16) & 0xff) * 257),
                                               (guint16) (((rgb >> 8) & 0xff) * 257),
                                               (guint16) ((rgb & 0xff) * 257)),
                    start, end);
        }
      add_attr (list,
                pango_attr_foreground_new ((guint16) (((ch->color >> 16) & 0xff) * 257),
                                           (guint16) (((ch->color >> 8) & 0xff) * 257),
                                           (guint16) ((ch->color & 0xff) * 257)),
                start, end);

      /* A hyperlink is blue and underlined, as every program since Mosaic
       * has drawn them, whatever else the run says. */
      if (ch->link != NULL)
        {
          add_attr (list, pango_attr_foreground_new (0, 0, 0x8080), start, end);
          add_attr (list, pango_attr_underline_new (PANGO_UNDERLINE_SINGLE), start, end);
        }

      /* Revision marks the way Word 6 showed them: inserted text
       * underlined, deleted text struck through, both in a colour of
       * their own. */
      if (ch->revision != 0)
        {
          add_attr (list, pango_attr_foreground_new (0xb0b0, 0, 0), start, end);
          if (ch->revision == 1)
            add_attr (list, pango_attr_underline_new (PANGO_UNDERLINE_SINGLE), start, end);
          else
            add_attr (list, pango_attr_strikethrough_new (TRUE), start, end);
        }

      if (ch->script != 0)
        {
          /* Two thirds the size, raised or dropped by a third of the em, the
           * proportions Word has always used. */
          add_attr (list, pango_attr_size_new ((size_pu * 2) / 3), start, end);
          add_attr (list, pango_attr_rise_new (ch->script > 0 ? size_pu / 3
                                                              : -size_pu / 4),
                    start, end);
        }
      else
        {
          add_attr (list, pango_attr_size_new (size_pu), start, end);
        }
    }

  return list;
}

/* Red squiggles under the words the dictionary does not know, except the
 * one the caret is in. */
static void
add_spelling (W42Layout *self, PangoAttrList *attrs, const W42Block *block)
{
  const char *text = block->text->str;
  gsize len = block->text->len;
  gsize start = 0, end = 0;
  gsize caret_byte = (gsize) -1;

  for (guint i = 0; i < block->runs->len; i++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, i);

      if (self->spell_caret >= run->doc_pos &&
          self->spell_caret <= run->doc_pos + run->n_chars)
        {
          const char *p = g_utf8_offset_to_pointer (text + run->byte_offset,
                            (glong) (self->spell_caret - run->doc_pos));
          caret_byte = (gsize) (p - text);
          break;
        }
    }

  while (w42_spell_next_word (text, len, &start, &end))
    {
      if (caret_byte >= start && caret_byte <= end)
        continue;
      if (w42_spell_check (self->spell, text + start, (gssize) (end - start)))
        continue;

      add_attr (attrs, pango_attr_underline_new (PANGO_UNDERLINE_ERROR),
                (guint) start, (guint) end);
      add_attr (attrs, pango_attr_underline_color_new (0xffff, 0, 0),
                (guint) start, (guint) end);
    }
}

void
w42_layout_set_spell (W42Layout *self, W42Spell *spell)
{
  g_return_if_fail (self != NULL);
  self->spell = spell;
}

void
w42_layout_set_spell_caret (W42Layout *self, gsize pos)
{
  g_return_if_fail (self != NULL);
  self->spell_caret = pos;
}

static PangoLayout *
build_block_layout (W42Layout      *self,
                    const W42Block *block,
                    W42ApTable     *aps,
                    double          text_width,
                    double          extra_indent,
                    guint           cap_bytes,
                    guint           hide_from,
                    guint           hide_to)
{
  const W42ParaFmt *dir_pa = &w42_ap_table_get (aps, block->ap)->pa;
  PangoLayout *layout = pango_layout_new (dir_pa->rtl ? self->ctx_rtl : self->ctx);
  const W42Fmt *fmt = w42_ap_table_get (aps, block->ap);
  const W42ParaFmt *pa = &fmt->pa;
  PangoFontDescription *desc = pango_font_description_new ();
  PangoAttrList *attrs;
  double width;

  /* The paragraph mark carries the formatting an empty paragraph shows, so
   * its character half decides the height of a blank line. */
  apply_font_description (desc, &fmt->ch);
  pango_layout_set_font_description (layout, desc);
  pango_font_description_free (desc);

  pango_layout_set_text (layout, block->text->str, (int) block->text->len);

  attrs = build_attributes (self, block, aps);
  if (self->spell != NULL)
    add_spelling (self, attrs, block);
  /* Text set elsewhere -- a dropped letter, or the part of the paragraph
   * in another layout -- is still here, as glyphs of no size, so that byte
   * offsets mean the same in every layout of the paragraph. */
  if (cap_bytes > 0 || hide_to > hide_from)
    {
      PangoRectangle none = { 0, 0, 0, 0 };

      if (cap_bytes > 0)
        add_attr (attrs, pango_attr_shape_new (&none, &none), 0, cap_bytes);
      if (hide_to > hide_from)
        add_attr (attrs, pango_attr_shape_new (&none, &none), hide_from, hide_to);
      /* A picture or note mark in the hidden part has a shape of its own
       * that starts later and would win: hide it by name. */
      for (guint i = 0; i < block->runs->len; i++)
        {
          const W42Run *run = &g_array_index (block->runs, W42Run, i);
          guint rs = (guint) run->byte_offset, re = rs + (guint) run->n_bytes;

          if ((run->object == W42_OBJECT_NONE && run->footnote == 0) || re <= rs)
            continue;
          if (re <= cap_bytes || (rs >= hide_from && re <= hide_to))
            add_attr (attrs, pango_attr_shape_new (&none, &none), rs, re);
        }
    }
  pango_layout_set_attributes (layout, attrs);
  pango_attr_list_unref (attrs);

  width = text_width - w42_twips_to_px (pa->indent_left)
                     - w42_twips_to_px (pa->indent_right);
  if (width < 1.0)
    width = 1.0;

  pango_layout_set_width (layout, (int) (width * PANGO_SCALE));
  pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);

  /* Tab stops: the paragraph's own, then Word's default every half inch
   * beyond the last of them.  Word measures from the margin and Pango from
   * the layout's left edge, which is the left indent, so they shift. */
  {
    PangoTabArray *tabs = pango_tab_array_new (0, TRUE);
    int n = 0, last = 0;
    int width_twips = (int) (text_width * W42_TWIPS_PER_PX);

    for (int i = 0; i < pa->n_tabs; i++)
      {
        int rel = pa->tab_pos[i] - pa->indent_left;
        PangoTabAlign align = PANGO_TAB_LEFT;

        last = pa->tab_pos[i];
        if (rel <= 0)
          continue;

        switch (W42_TAB_KIND (pa->tab_kind[i]))
          {
          case W42_TAB_CENTER:  align = PANGO_TAB_CENTER;  break;
          case W42_TAB_RIGHT:   align = PANGO_TAB_RIGHT;   break;
          case W42_TAB_DECIMAL: align = PANGO_TAB_DECIMAL; break;
          default: break;
          }

        pango_tab_array_resize (tabs, n + 1);
        pango_tab_array_set_tab (tabs, n, align, (int) w42_twips_to_px (rel));
        if (align == PANGO_TAB_DECIMAL)
          pango_tab_array_set_decimal_point (tabs, n, '.');
        n++;
      }

    for (int t = 720; t < width_twips; t += 720)
      {
        int rel = t - pa->indent_left;

        if (t <= last || rel <= 0)
          continue;
        pango_tab_array_resize (tabs, n + 1);
        pango_tab_array_set_tab (tabs, n, PANGO_TAB_LEFT, (int) w42_twips_to_px (rel));
        n++;
      }

    pango_layout_set_tabs (layout, n > 0 ? tabs : NULL);
    pango_tab_array_free (tabs);
  }
  pango_layout_set_indent (layout,
    (int) ((w42_twips_to_px (pa->indent_first) + extra_indent) * PANGO_SCALE));

  /* Alignment is what it says: Left is the left edge and Right the right
   * edge, whichever way the paragraph runs.  Format > Paragraph offers
   * Alignment and Direction as two settings, so they mean two things --
   * the direction shapes and orders the text, the alignment says which
   * margin the lines sit against.  Word's file formats store alignment
   * the other way, relative to the direction; the readers and writers
   * turn it round, not the page. */
  if (pa->rtl)
    pango_layout_set_auto_dir (layout, FALSE);
  switch (pa->align)
    {
    case W42_ALIGN_CENTER:
      pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
      break;
    case W42_ALIGN_RIGHT:
      pango_layout_set_alignment (layout, PANGO_ALIGN_RIGHT);
      break;
    case W42_ALIGN_JUSTIFY:
      pango_layout_set_alignment (layout, pa->rtl ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
      pango_layout_set_justify (layout, TRUE);
      break;
    case W42_ALIGN_LEFT:
    default:
      pango_layout_set_alignment (layout, PANGO_ALIGN_LEFT);
      break;
    }

  return layout;
}

static void build_furniture (W42Layout *self, W42PieceTable *pt);
static void layout_table (W42Layout *self, W42PieceTable *pt, W42ApTable *aps,
                          guint first, guint last, double text_w, double text_h,
                          double *y_io, int *page_io);

/* Lays the cell's paragraphs out one under another in a column `width`
 * wide, at x, starting at y on `page`, and returns the height used.  Lines
 * are appended as boxes; a row is placed whole, so nothing here breaks a
 * page. */
static double
layout_cell (W42Layout      *self,
             W42ApTable     *aps,
             guint           first,
             guint           last,
             double          x,
             double          y,
             double          width,
             int             page)
{
  double top = y;

  for (guint b = first; b <= last; b++)
    {
      const W42Block *block = g_ptr_array_index (self->blocks, b);
      const W42Fmt *fmt = w42_ap_table_get (aps, block->ap);
      const W42ParaFmt *pa = &fmt->pa;
      PangoLayout *layout = build_block_layout (self, block, aps, width, 0.0, 0, 0, 0);
      PangoLayoutIter *iter;
      double indent_x = w42_twips_to_px (pa->indent_left);

      g_ptr_array_add (self->layouts, layout);
      y += w42_twips_to_px (pa->space_before);

      iter = pango_layout_get_iter (layout);
      do
        {
          PangoLayoutLine *line = pango_layout_iter_get_line_readonly (iter);
          PangoRectangle logical;
          W42LineBox box;
          double line_h, advance;

          pango_layout_iter_get_line_extents (iter, NULL, &logical);
          line_h = (double) logical.height / PANGO_SCALE;
          advance = (pa->line_spacing_pct > 0) ? line_h * (pa->line_spacing_pct / 100.0)
                  : (pa->line_spacing > 0)     ? w42_twips_to_px (pa->line_spacing)
                  : line_h;

          box.page        = page;
          box.origin_x    = x + indent_x;
          box.x           = box.origin_x + (double) logical.x / PANGO_SCALE;
          box.y           = y;
          box.width       = (double) logical.width / PANGO_SCALE;
          box.height      = line_h;
          box.baseline    = (double) (pango_layout_iter_get_baseline (iter) - logical.y) / PANGO_SCALE;
          box.block       = (int) b;
          box.start_index = line->start_index;
          box.length      = line->length;
          box.line        = line;
          box.prefix      = NULL;
          box.prefix_x    = box.origin_x;

          g_array_append_val (self->lines, box);
          y += advance;
        }
      while (pango_layout_iter_next_line (iter));
      pango_layout_iter_free (iter);

      y += w42_twips_to_px (pa->space_after);
    }

  return y - top;
}

/* A row is laid out once and then placed, in one piece when it fits and
 * in several when it is taller than a page. */
typedef struct {
  int    page;
  double top;        /* page y of the piece's top, without the margin */
  double row_top;    /* how far into the row the piece starts */
  double height;
} RowSeg;

#define MAX_ROW_PIECES 256   /* a row cannot go on for ever */

/* The table's header rows set again at the top of `page`: every cell of
 * rows 0 to n_header - 1, with their borders, as the first rows of a page
 * a table runs on to.  Returns the height they take. */
static double
layout_header_rows (W42Layout *self, W42ApTable *aps, guint first, guint last,
                    int n_header, const double *col_x, int n_cols, int page,
                    double text_h, gboolean borders)
{
  const W42Block *head = g_ptr_array_index (self->blocks, first);
  double y = 0.0;
  guint b = first;

  while (b <= last)
    {
      const W42Block *blk = g_ptr_array_index (self->blocks, b);
      int row = blk->row;
      double row_h = 0.0;
      guint c = b;

      if (row >= n_header)
        break;

      while (c <= last)
        {
          const W42Block *cb = g_ptr_array_index (self->blocks, c);
          guint cell_last = c;
          int col, span;

          if (cb->row != row)
            break;
          col = CLAMP (cb->col, 0, n_cols - 1);
          span = CLAMP (cb->span, 1, n_cols - col);
          while (cell_last + 1 <= last)
            {
              const W42Block *next = g_ptr_array_index (self->blocks, cell_last + 1);

              if (next->row != row || next->col != cb->col)
                break;
              cell_last++;
            }
          row_h = MAX (row_h, layout_cell (self, aps, c, cell_last,
                                           col_x[col] + CELL_PAD, y + CELL_PAD,
                                           MAX (col_x[col + span] - col_x[col] - 2 * CELL_PAD, 8.0),
                                           page));
          c = cell_last + 1;
        }
      row_h += 2 * CELL_PAD;

      /* The rectangles, one per cell; merged cells are not repeated in
       * detail, each column gets its own. */
      for (guint k = b; k < c; k++)
        {
          const W42Block *cb = g_ptr_array_index (self->blocks, k);
          W42CellRect rect;
          int col, span;

          if (k > b && cb->col == ((const W42Block *) g_ptr_array_index (self->blocks, k - 1))->col)
            continue;
          col = CLAMP (cb->col, 0, n_cols - 1);
          span = CLAMP (cb->span, 1, n_cols - col);
          rect.page = page;
          rect.x = col_x[col];
          rect.y = self->mar_t + y;
          rect.w = col_x[col + span] - col_x[col];
          rect.h = row_h;
          rect.table = head->table;
          rect.col = col + span - 1;
          rect.sides = cell_sides (aps, cb, borders);
          rect.borders = rect.sides != 0;
          g_array_append_val (self->cell_rects, rect);
        }

      y += row_h;
      b = c;
      if (y > text_h / 2.0)
        break;                        /* a header that ate the page: enough */
    }

  return y;
}

/* Which sides of a cell are ruled: the cell's own, if it has them, else
 * all or none by the table's setting. */
static guint8
cell_sides (W42ApTable *aps, const W42Block *blk, gboolean table_borders)
{
  const W42ParaFmt *cpa = &w42_ap_table_get (aps, blk->cell_ap)->pa;

  if (cpa->border & W42_BORDER_CELL_SET)
    return cpa->border & W42_BORDER_BOX;
  return table_borders ? W42_BORDER_BOX : 0;
}

/* The lines of `layout` that show any of [from, to), clipped to it. */
static void
collect_lines (PangoLayout *layout, GArray *out, guint from, guint to, gboolean narrow, gboolean last)
{
  PangoLayoutIter *iter = pango_layout_get_iter (layout);

  do
    {
      PangoLayoutLine *line = pango_layout_iter_get_line_readonly (iter);
      guint ls = (guint) line->start_index, le = ls + (guint) line->length;
      guint cs = MAX (ls, from), ce = MIN (le, to);
      BlockLine bl;

      /* An empty line belongs to the part it starts in; a trailing one to
       * the last part. */
      if (ce <= cs && !(line->length == 0 && ((ls >= from && ls < to) || (last && ls == to))))
        continue;
      bl.layout = layout;
      bl.line = line;
      pango_layout_iter_get_line_extents (iter, NULL, &bl.logical);
      bl.baseline = (double) (pango_layout_iter_get_baseline (iter) - bl.logical.y) / PANGO_SCALE;
      bl.start_index = cs;
      bl.length = ce > cs ? ce - cs : 0;
      bl.narrow = narrow;
      g_array_append_val (out, bl);
    }
  while (pango_layout_iter_next_line (iter));
  pango_layout_iter_free (iter);
}

static double
line_advance (const W42ParaFmt *pa, double line_h)
{
  if (pa->line_spacing_pct > 0)
    return line_h * (pa->line_spacing_pct / 100.0);
  if (pa->line_spacing > 0)
    return w42_twips_to_px (pa->line_spacing);
  return line_h;
}

/* Lays a paragraph out into `lines`.  With `wrap_w` taken off the column
 * for `beside_h` pixels (or for the whole paragraph when beside_h < 0),
 * the lines that fit beside the obstacle are set narrow and the rest at
 * the full width, in a second layout; each layout hides the other's
 * part.  The layouts go into self->layouts. */
static void
layout_block_lines (W42Layout *self, const W42Block *block, W42ApTable *aps,
                    const W42ParaFmt *pa, double text_w, double prefix_w,
                    double wrap_w, double beside_h, guint cap_bytes, GArray *lines)
{
  guint len = (guint) block->text->len;
  PangoLayout *a = build_block_layout (self, block, aps, text_w - wrap_w, prefix_w, cap_bytes, 0, 0);

  g_ptr_array_add (self->layouts, a);
  if (wrap_w > 0.0 && beside_h >= 0.0)
    {
      PangoLayoutIter *it = pango_layout_get_iter (a);
      double cum = 0.0;
      int k = 0, n = pango_layout_get_line_count (a);
      guint split = len;

      do
        {
          PangoRectangle lg;

          if (cum >= beside_h - 0.5)
            {
              split = (guint) pango_layout_iter_get_line_readonly (it)->start_index;
              break;
            }
          pango_layout_iter_get_line_extents (it, NULL, &lg);
          cum += line_advance (pa, (double) lg.height / PANGO_SCALE);
          k++;
        }
      while (pango_layout_iter_next_line (it));
      pango_layout_iter_free (it);

      if (k == 0)
        {
          /* No room beside it at all: the whole paragraph at full width. */
          PangoLayout *wide = build_block_layout (self, block, aps, text_w, prefix_w, cap_bytes, 0, 0);

          g_object_unref (g_ptr_array_index (self->layouts, self->layouts->len - 1));
          g_ptr_array_index (self->layouts, self->layouts->len - 1) = wide;
          collect_lines (wide, lines, 0, len, FALSE, TRUE);
          return;
        }
      if (k < n && split < len)
        {
          PangoLayout *narrow = build_block_layout (self, block, aps, text_w - wrap_w, prefix_w, cap_bytes, split, len);
          PangoLayout *rest = build_block_layout (self, block, aps, text_w,
                                                  -w42_twips_to_px (pa->indent_first), 0, 0, split);

          g_object_unref (g_ptr_array_index (self->layouts, self->layouts->len - 1));
          g_ptr_array_index (self->layouts, self->layouts->len - 1) = narrow;
          g_ptr_array_add (self->layouts, rest);
          collect_lines (narrow, lines, 0, split, TRUE, FALSE);
          collect_lines (rest, lines, split, len, FALSE, TRUE);
          return;
        }
    }
  collect_lines (a, lines, 0, len, wrap_w > 0.0, TRUE);
}

/* A table: blocks `first` to `last`, all cells of one table.  Column widths
 * come from the table's properties, or share the text column equally.  Each
 * row is as tall as its tallest cell and is placed whole; a row that will
 * not fit starts the next page.  The line boxes are laid out once per row
 * at a provisional y and moved if the row has to move, which is cheaper than
 * laying them out twice. */
static void
layout_table (W42Layout      *self,
              W42PieceTable  *pt,
              W42ApTable     *aps,
              guint           first,
              guint           last,
              double          text_w,
              double          text_h,
              double         *y_io,
              int            *page_io)
{
  const W42Block *head = g_ptr_array_index (self->blocks, first);
  const W42TableProps *props = w42_pt_table_props (pt, head->table);
  int n_cols = props != NULL ? props->n_cols : 1;
  double *col_x = g_new0 (double, n_cols + 1);
  double y = *y_io;
  int page = *page_io;
  guint b = first;
  double total = 0.0;

  /* Column edges. */
  for (int c = 0; c < n_cols; c++)
    {
      int w = (props != NULL && c < (int) props->widths->len)
                ? g_array_index (props->widths, int, c) : 0;
      total += (w > 0) ? w42_twips_to_px (w) : text_w / n_cols;
    }
  {
    double scale = (total > text_w && total > 0) ? text_w / total : 1.0;
    col_x[0] = self->mar_l;
    for (int c = 0; c < n_cols; c++)
      {
        int w = (props != NULL && c < (int) props->widths->len)
                  ? g_array_index (props->widths, int, c) : 0;
        double px = ((w > 0) ? w42_twips_to_px (w) : text_w / n_cols) * scale;
        col_x[c + 1] = col_x[c] + px;
      }
  }

  while (b <= last)
    {
      int row = ((const W42Block *) g_ptr_array_index (self->blocks, b))->row;
      guint row_first_line = self->lines->len;
      guint row_line_end;
      double y0 = y;            /* where the row's lines were laid out */
      int page0 = page;
      GArray *segs;             /* the pieces of the row, one per page */
      double row_h = 0.0;
      guint c = b;
      int *owner = g_new (int, n_cols);
      int *spans = g_new0 (int, n_cols);
      guint8 *sides = g_new0 (guint8, n_cols);

      /* Which cell owns each column of the row: a merged cell owns the
       * ones it spans, and those get no border of their own. */
      for (int k = 0; k < n_cols; k++)
        owner[k] = -1;
      for (guint k = b; k <= last; k++)
        {
          const W42Block *blk = g_ptr_array_index (self->blocks, k);
          int col0, span;

          if (blk->row != row)
            break;
          col0 = CLAMP (blk->col, 0, n_cols - 1);
          span = CLAMP (blk->span, 1, n_cols - col0);
          spans[col0] = span;
          sides[col0] = cell_sides (aps, blk, props == NULL || props->borders);
          for (int k2 = col0; k2 < col0 + span; k2++)
            owner[k2] = col0;
        }

      /* Lay each cell of the row out at y; the row's height is the tallest. */
      while (c <= last)
        {
          const W42Block *blk = g_ptr_array_index (self->blocks, c);
          guint cell_last = c;
          double h;
          int col;

          if (blk->row != row)
            break;

          col = CLAMP (blk->col, 0, n_cols - 1);
          while (cell_last + 1 <= last)
            {
              const W42Block *next = g_ptr_array_index (self->blocks, cell_last + 1);
              if (next->row != row || next->col != blk->col)
                break;
              cell_last++;
            }

          {
            int span = CLAMP (blk->span, 1, n_cols - col);

            h = layout_cell (self, aps, c, cell_last,
                             col_x[col] + CELL_PAD, y + CELL_PAD,
                             MAX (col_x[col + span] - col_x[col] - 2 * CELL_PAD, 8.0),
                             page);
          }
          row_h = MAX (row_h, h);
          c = cell_last + 1;
        }

      row_h += 2 * CELL_PAD;

      /* Table Properties can ask for a row at least so tall. */
      if (props != NULL && (guint) row < props->row_heights->len)
        row_h = MAX (row_h, w42_twips_to_px (g_array_index (props->row_heights, int, row)));

      /* Where the row goes.  A row that fits what is left of the page
       * stays where it is; a row that fits a page but not the rest of
       * this one moves whole to the next; and a row taller than a page
       * is broken over as many as it takes, between its lines, rather
       * than running off the bottom of the sheet.  The header rows are
       * set again at the top of every page the table runs on to. */
      row_line_end = self->lines->len;
      segs = g_array_new (FALSE, FALSE, sizeof (RowSeg));
      {
        RowSeg seg;

        if (self->galley)
          {
            seg.page = page; seg.top = y; seg.row_top = 0.0; seg.height = row_h;
            g_array_append_val (segs, seg);
          }
        else
          {
            double top = y;
            int pg = page;
            double consumed = 0.0;

            if (top > 0.0 && top + row_h > text_h && row_h <= text_h)
              {
                pg++;
                top = 0.0;
                if (props != NULL && props->header_rows > 0 && row >= props->header_rows)
                  top = layout_header_rows (self, aps, first, last, props->header_rows,
                                            col_x, n_cols, pg, text_h, props->borders);
              }

            while (segs->len < MAX_ROW_PIECES)
              {
                double room = text_h - top;
                double cut, best;

                if (row_h - consumed <= room || room <= 0.0)
                  {
                    seg.page = pg; seg.top = top; seg.row_top = consumed;
                    seg.height = row_h - consumed;
                    g_array_append_val (segs, seg);
                    break;
                  }

                /* Break after the last line that ends inside the room
                 * left; a line taller than the page goes over the edge,
                 * having nowhere else to be. */
                cut = consumed + room;
                best = consumed;
                for (guint i = row_first_line; i < row_line_end; i++)
                  {
                    const W42LineBox *box = &g_array_index (self->lines, W42LineBox, i);
                    double bottom = (box->y - y0) + box->height;

                    if (bottom <= cut && bottom > best)
                      best = bottom;
                  }
                if (best <= consumed)
                  best = cut;

                seg.page = pg; seg.top = top; seg.row_top = consumed;
                seg.height = best - consumed;
                g_array_append_val (segs, seg);
                consumed = best;

                pg++;
                top = 0.0;
                if (props != NULL && props->header_rows > 0 && row >= props->header_rows)
                  top = layout_header_rows (self, aps, first, last, props->header_rows,
                                            col_x, n_cols, pg, text_h, props->borders);
              }
          }
      }

      /* The row's own lines follow their piece; the header rows' lines,
       * appended above, are already where they belong. */
      for (guint i = row_first_line; i < row_line_end; i++)
        {
          W42LineBox *box = &g_array_index (self->lines, W42LineBox, i);
          double top_rel = box->y - y0;
          const RowSeg *seg = &g_array_index (segs, RowSeg, 0);

          for (guint j = 1; j < segs->len; j++)
            if (g_array_index (segs, RowSeg, j).row_top <= top_rel + 0.01)
              seg = &g_array_index (segs, RowSeg, j);

          box->page = seg->page;
          box->y = seg->top + (top_rel - seg->row_top);
        }
      (void) page0;

      /* Borders: one rectangle per cell of the row, empty cells included,
       * a merged cell's covering all the columns it spans.  A row broken
       * over pages is ruled as the pieces it became: no line where it was
       * cut, so the cell reads as one cell continued. */
      for (guint sg = 0; sg < segs->len; sg++)
        {
          const RowSeg *seg = &g_array_index (segs, RowSeg, sg);
          guint8 cut_sides = 0;

          if (sg > 0)
            cut_sides |= W42_BORDER_TOP;
          if (sg + 1 < segs->len)
            cut_sides |= W42_BORDER_BOTTOM;

          for (int col = 0; col < n_cols; col++)
            {
              W42CellRect rect;
              int span = 1;

              if (owner[col] >= 0 && owner[col] != col)
                continue;
              if (owner[col] == col)
                span = CLAMP (spans[col], 1, n_cols - col);

              rect.page = seg->page;
              rect.x = col_x[col];
              rect.y = self->mar_t + seg->top;
              rect.w = col_x[col + span] - col_x[col];
              rect.h = seg->height;
              rect.table = head->table;
              rect.col = col + span - 1;
              rect.sides = owner[col] == col ? sides[col]
                           : (props == NULL || props->borders) ? W42_BORDER_BOX : 0;
              rect.sides &= (guint8) ~cut_sides;
              rect.borders = rect.sides != 0;
              g_array_append_val (self->cell_rects, rect);
            }
        }

      {
        const RowSeg *last_seg = &g_array_index (segs, RowSeg, segs->len - 1);

        y = last_seg->top;
        page = last_seg->page;
        row_h = last_seg->height;      /* what is left of the row on this page */
      }
      g_array_free (segs, TRUE);

      g_free (owner);
      g_free (spans);
      g_free (sides);

      /* The lines were laid out relative to y=0 at the top margin;
       * layout_cell wrote box.y as a page-relative value from `y` without
       * the margin, so add it now. */
      for (guint i = row_first_line; i < self->lines->len; i++)
        g_array_index (self->lines, W42LineBox, i).y += self->mar_t;

      y += row_h;
      b = c;
    }

  g_free (col_x);
  *y_io = y;
  *page_io = page;
}

void
w42_layout_build (W42Layout *self, W42Document *doc)
{
  g_return_if_fail (self != NULL);
  g_return_if_fail (W42_IS_DOCUMENT (doc));

  w42_layout_build_pt (self, w42_document_pt (doc),
                       w42_document_page_setup (doc));
}

void
w42_layout_build_pt (W42Layout          *self,
                     W42PieceTable      *pt,
                     const W42PageSetup *page)
{
  W42ApTable *aps;
  double text_w, text_h, y;
  int current_page = 0;
  GArray *sections = g_array_new (FALSE, FALSE, sizeof (SectionCols));

  g_return_if_fail (self != NULL);
  g_return_if_fail (pt != NULL);
  g_return_if_fail (page != NULL);

  aps = w42_pt_ap_table (pt);
  self->objects = w42_pt_object_table (pt);
  self->styles = w42_pt_stylesheet (pt);
  self->aps = w42_pt_ap_table (pt);
  g_ptr_array_set_size (self->prefixes, 0);
  g_ptr_array_set_size (self->furniture_layouts, 0);
  g_array_set_size (self->furniture, 0);
  g_array_set_size (self->cell_rects, 0);
  g_ptr_array_set_size (self->note_marks, 0);
  g_array_set_size (self->note_rules, 0);

  self->page_w = w42_twips_to_px (page->width);
  self->page_h = w42_twips_to_px (page->height);
  self->mar_l  = w42_twips_to_px (page->margin_left);
  self->mar_r  = w42_twips_to_px (page->margin_right);
  self->mar_t  = w42_twips_to_px (page->margin_top);
  self->mar_b  = w42_twips_to_px (page->margin_bottom);

  text_w = self->page_w - self->mar_l - self->mar_r;
  text_h = self->page_h - self->mar_t - self->mar_b;

  /* Newspaper columns: the text flows down one column and on to the next,
   * so the flow below is laid out into columns as if each were a page, and
   * those "column pages" are folded on to real pages at the end.  Normal
   * view shows one column, as Word 6's did. */
  int n_columns = self->galley ? 1 : w42_page_columns (page);
  double column_gap = w42_twips_to_px (w42_page_column_gap (page));
  double column_w = (text_w - (n_columns - 1) * column_gap) / n_columns;

  if (n_columns > 1)
    text_w = column_w;
  {
    SectionCols first = { 0, n_columns, column_w, column_gap };
    g_array_append_val (sections, first);
  }

  /* Normal view keeps the text column the width the page gives it, but does
   * not show the page's margins: Word 6 sat the galley just inside the window
   * with a narrow selection bar to its left, and nothing above it. */
  if (self->galley)
    {
      self->mar_l = w42_twips_to_px (360);   /* a quarter inch */
      self->mar_t = w42_twips_to_px (180);
      self->mar_r = self->mar_l;
      self->mar_b = self->mar_t;
      self->page_w = self->mar_l + text_w + self->mar_r;
      text_h = G_MAXDOUBLE / 4.0;
    }

  g_clear_pointer (&self->blocks, g_ptr_array_unref);
  g_ptr_array_set_size (self->layouts, 0);
  g_array_set_size (self->lines, 0);

  self->text_w = text_w;
  self->blocks = w42_pt_snapshot_blocks (pt);
  g_array_set_size (self->floats, 0);

  /* A wrapped picture: the paragraph it is anchored to and those after it
   * on the same page, down to its foot, are set in the rest of the column.
   * Word 6 framed pictures the same way, paragraph by paragraph. */
  /* An obstacle at either side of the column -- a picture, a frame or a
   * dropped letter -- with the page it is on, its foot and what it takes
   * off the column.  One at each side can stand at once. */
  int float_page[2] = { -1, -1 };          /* 0: left, 1: right */
  double float_bottom[2] = { 0.0, 0.0 }, float_w[2] = { 0.0, 0.0 };
  W42Wrap float_side = W42_WRAP_INLINE;    /* the side of this paragraph's own picture */
  const double float_gap = w42_twips_to_px (180);
  /* A text frame being built up from framed paragraphs. */
  gboolean frame_open = FALSE;
  int frame_side = W42_FRAME_NONE, frame_page = -1;
  double frame_next_y = 0.0;
  g_array_set_size (self->caps, 0);
  g_ptr_array_set_size (self->cap_layouts, 0);

  y = 0.0;

  /* Footnotes: the notes referenced on a page are laid out as the page
   * fills and stacked at its foot when it is done, so a line and its note
   * move to the next page together when the foot is full. */
  GArray *page_notes = g_array_new (FALSE, FALSE, sizeof (W42LineBox));
  double notes_h = 0.0;
  GArray *placed = g_array_new (FALSE, TRUE, sizeof (gboolean));

  /* Section numbers: one counter per outline level, the deeper ones reset
   * whenever a shallower heading comes along.  Word 6's Heading Numbering
   * did exactly this and nothing more. */
  int counters[10] = { 0 };
  gboolean numbering = w42_stylesheet_get_number_headings (self->styles);

  /* List numbers: a run of numbered paragraphs counts from 1, and anything
   * that is not one -- plain text, a bullet, a table -- starts the next run
   * over. */
  int list_n = 0;
  int level_n[9] = { 0 };            /* the count at each level */
  W42ListKind level_kind[9] = { W42_LIST_NONE };

  for (guint b = 0; b < self->blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (self->blocks, b);
      const W42Fmt *fmt = w42_ap_table_get (aps, block->ap);
      const W42ParaFmt *pa = &fmt->pa;
      double indent_x = w42_twips_to_px (pa->indent_left);
      PangoLayout *prefix = NULL;
      double prefix_w = 0.0;
      gboolean first_line = TRUE;
      int level = w42_stylesheet_outline (self->styles, pa->style);
      const W42Object *fobj = NULL;   /* a wrapped picture anchored here */
      W42ObjectIdx fidx = W42_OBJECT_NONE;
      gsize fpos = 0;
      double fw = 0.0, fh = 0.0;
      double wrap_w = 0.0;            /* what the obstacles take off the column */
      double own_w = 0.0;             /* this paragraph's own picture's share */
      gboolean narrowed = FALSE;
      gboolean left_on = FALSE, right_on = FALSE;   /* obstacles from before, still beside */

      if (block->note >= 0)
        continue;           /* laid out with the page its reference is on */

      if (block->table >= 0)
        {
          /* The whole table at once: every block up to the next one that
           * is not a cell of this table. */
          guint last = b;

          while (last + 1 < self->blocks->len &&
                 ((const W42Block *) g_ptr_array_index (self->blocks, last + 1))->table == block->table)
            last++;

          layout_table (self, pt, aps, b, last, text_w, text_h, &y, &current_page);
          b = last;
          list_n = 0;
          frame_open = FALSE;         /* a frame does not run on past a table */
          continue;
        }

      /* A framed paragraph: set in a box at the side of the column, level
       * with where the text is, which then runs down the other side of it.
       * Framed paragraphs one after another share the frame. */
      if (pa->frame_side != W42_FRAME_NONE && block->table < 0)
        {
          double fw_px = pa->frame_width > 0 ? w42_twips_to_px (pa->frame_width) : text_w / 3.0;
          gboolean cont = frame_open && frame_side == pa->frame_side && frame_page == current_page;
          double fy, x0, total = 0.0;
          PangoLayout *fl;
          PangoLayoutIter *fi;

          fw_px = CLAMP (fw_px, 24.0, text_w * 0.8);
          fl = build_block_layout (self, block, aps, fw_px, 0.0, 0, 0, 0);
          g_ptr_array_add (self->layouts, fl);
          fi = pango_layout_get_iter (fl);
          do
            {
              PangoRectangle lg;

              pango_layout_iter_get_line_extents (fi, NULL, &lg);
              total += line_advance (pa, (double) lg.height / PANGO_SCALE);
            }
          while (pango_layout_iter_next_line (fi));
          pango_layout_iter_free (fi);

          fy = cont ? frame_next_y : y + w42_twips_to_px (pa->space_before);
          if (!self->galley && y > 0.0 && fy + total > text_h - notes_h)
            {
              flush_notes (self, page_notes, &notes_h, current_page, text_h, &y);
              current_page++;
              y = 0.0;
              fy = w42_twips_to_px (pa->space_before);
              cont = FALSE;
            }
          x0 = pa->frame_side == W42_FRAME_LEFT ? self->mar_l : self->mar_l + text_w - fw_px;

          fi = pango_layout_get_iter (fl);
          do
            {
              PangoLayoutLine *line = pango_layout_iter_get_line_readonly (fi);
              PangoRectangle logical;
              W42LineBox box;
              double line_h;

              pango_layout_iter_get_line_extents (fi, NULL, &logical);
              line_h = (double) logical.height / PANGO_SCALE;
              box.page        = current_page;
              box.origin_x    = x0;
              box.x           = x0 + (double) logical.x / PANGO_SCALE;
              box.y           = self->mar_t + fy;
              box.width       = (double) logical.width / PANGO_SCALE;
              box.height      = line_h;
              box.baseline    = (double) (pango_layout_iter_get_baseline (fi) - logical.y) / PANGO_SCALE;
              box.block       = (int) b;
              box.start_index = line->start_index;
              box.length      = line->length;
              box.line        = line;
              box.prefix      = NULL;
              box.prefix_x    = x0;
              g_array_append_val (self->lines, box);
              fy += line_advance (pa, line_h);
            }
          while (pango_layout_iter_next_line (fi));
          pango_layout_iter_free (fi);

          fy += w42_twips_to_px (pa->space_after);
          frame_open = TRUE;
          frame_side = pa->frame_side;
          frame_page = current_page;
          frame_next_y = fy;
          /* The text that follows wraps beside it, as beside a picture. */
          {
            int fs = pa->frame_side == W42_FRAME_LEFT ? 0 : 1;

            float_page[fs] = current_page;
            float_w[fs] = fw_px + float_gap;
            float_bottom[fs] = fy + float_gap;
          }
          continue;
        }
      frame_open = FALSE;

      if (self->objects != NULL)
        for (guint r = 0; r < block->runs->len; r++)
          {
            const W42Run *run = &g_array_index (block->runs, W42Run, r);
            const W42Object *obj = run->object != W42_OBJECT_NONE
                                   ? w42_object_table_get (self->objects, run->object) : NULL;

            if (obj == NULL || obj->wrap == W42_WRAP_INLINE)
              continue;
            fobj = obj;
            fidx = run->object;
            fpos = run->doc_pos;
            fw = w42_twips_to_px (obj->width);
            fh = w42_twips_to_px (obj->height);
            /* Room must be left for the text: at most six tenths of the column. */
            if (fw > text_w * 0.6 && fw > 0.0)
              {
                fh *= text_w * 0.6 / fw;
                fw = text_w * 0.6;
              }
            break;
          }
      if (fobj != NULL)
        {
          float_side = fobj->wrap;
          own_w = fw + float_gap;
          narrowed = TRUE;
        }
      left_on = float_page[0] == current_page && y < float_bottom[0];
      right_on = float_page[1] == current_page && y < float_bottom[1];
      if (left_on || right_on)
        narrowed = TRUE;
      wrap_w = own_w + (left_on ? float_w[0] : 0.0) + (right_on ? float_w[1] : 0.0);

      /* A run of one numbered kind counts on; a different kind, a plain
       * paragraph or a restart begins again. */
      if (w42_list_is_numbered (pa->list))
        {
          int lv = MIN (pa->list_level, 8);

          if (pa->list_start > 0)
            level_n[lv] = pa->list_start;
          else if (pa->list != level_kind[lv])
            level_n[lv] = 1;
          else
            level_n[lv]++;
          level_kind[lv] = pa->list;
          /* An item at this level starts the deeper ones over. */
          for (int deeper = lv + 1; deeper < 9; deeper++)
            {
              level_n[deeper] = 0;
              level_kind[deeper] = W42_LIST_NONE;
            }
          list_n = level_n[lv];
        }
      else if (pa->list == W42_LIST_NONE)
        {
          list_n = 0;
          for (int lv = 0; lv < 9; lv++)
            {
              level_n[lv] = 0;
              level_kind[lv] = W42_LIST_NONE;
            }
        }
      else
        list_n = 0;                   /* a bullet: nothing to count */

      if (pa->list != W42_LIST_NONE)
        {
          /* The marker sits in the hanging indent, if the paragraph has
           * one, and the text starts where the wrapped lines will; without
           * a hanging indent the marker pushes the first line along by its
           * own width and a gap, as a section number does. */
          char marker[16];
          PangoFontDescription *desc = pango_font_description_new ();
          const W42CharFmt *ch = &fmt->ch;
          int pw = 0, ph = 0;

          w42_list_marker (pa->list, list_n, marker, sizeof marker);

          if (block->runs->len > 0)
            ch = &w42_ap_table_get (aps, g_array_index (block->runs, W42Run, 0).ap)->ch;

          prefix = pango_layout_new (self->ctx);
          apply_font_description (desc, ch);
          pango_layout_set_font_description (prefix, desc);
          pango_font_description_free (desc);
          pango_layout_set_text (prefix, marker, -1);
          pango_layout_get_pixel_size (prefix, &pw, &ph);

          if (pa->indent_first < 0)
            prefix_w = MAX (w42_twips_to_px (-pa->indent_first), pw + 6.0);
          else
            prefix_w = MAX (pw + 12.0, 36.0);

          g_ptr_array_add (self->prefixes, prefix);
        }
      else if (level > 0 && level < 10)
        {
          counters[level]++;
          for (int l = level + 1; l < 10; l++)
            counters[l] = 0;

          if (numbering)
            {
              GString *number = g_string_new (NULL);
              PangoFontDescription *desc = pango_font_description_new ();
              int pw = 0, ph = 0;

              for (int l = 1; l <= level; l++)
                g_string_append_printf (number, l > 1 ? ".%d" : "%d",
                                        counters[l]);

              /* The number is set in the heading's own font: the first
               * run's, since the paragraph mark keeps whatever character
               * formatting it had before the style was applied. */
              const W42CharFmt *ch = &fmt->ch;
              if (block->runs->len > 0)
                ch = &w42_ap_table_get (aps, g_array_index (block->runs, W42Run, 0).ap)->ch;

              prefix = pango_layout_new (self->ctx);
              apply_font_description (desc, ch);
              pango_layout_set_font_description (prefix, desc);
              pango_font_description_free (desc);
              pango_layout_set_text (prefix, number->str, -1);
              pango_layout_get_pixel_size (prefix, &pw, &ph);

              /* The number, then a gap of a quarter inch or the number's own
               * width, whichever is larger, so short numbers and long ones
               * both leave the heading text at a tab stop. */
              prefix_w = MAX (pw + 12.0, 36.0);

              g_ptr_array_add (self->prefixes, prefix);
              g_string_free (number, TRUE);
            }
        }

      /* How far down the paragraph the obstacle beside it reaches: the
       * picture or frame's foot, or the whole paragraph. */
      double beside_h = -1.0;
      guint cap_bytes = 0;
      double cap_shift = 0.0, cap_h = 0.0;
      PangoLayout *cap = NULL;
      PangoRectangle cap_ink = { 0, 0, 0, 0 };
      double narrow_shift = (left_on ? float_w[0] : 0.0) +
                            ((fobj != NULL && float_side == W42_WRAP_LEFT) ? own_w : 0.0);
      GArray *blines = g_array_new (FALSE, FALSE, sizeof (BlockLine));
      double cap_top = -1.0;          /* where the dropped letter's lines began */
      int cap_page = -1;

      if (fobj != NULL)
        beside_h = fh + float_gap;
      else if (narrowed)
        {
          /* Down to the nearer foot; below it the column is used in full. */
          double foot = G_MAXDOUBLE;

          if (left_on)  foot = MIN (foot, float_bottom[0]);
          if (right_on) foot = MIN (foot, float_bottom[1]);
          beside_h = MAX (foot - y - w42_twips_to_px (pa->space_before), 0.0);
        }

      /* A drop cap: the first letter set large at the left, the first
       * lines beside it. */
      if (pa->drop_cap > 0 && block->text->len > 0 && block->table < 0 &&
          block->runs->len > 0 && g_array_index (block->runs, W42Run, 0).object == W42_OBJECT_NONE &&
          g_array_index (block->runs, W42Run, 0).footnote == 0)
        {
          const char *text = block->text->str;
          gunichar first = g_utf8_get_char (text);

          if (g_unichar_isgraph (first))
            {
              const W42CharFmt *ch = &w42_ap_table_get (aps, g_array_index (block->runs, W42Run, 0).ap)->ch;
              const W42CharFmt *body = ch;
              PangoFontDescription *desc = pango_font_description_new ();
              int n = CLAMP (pa->drop_cap, 1, 10);
              double body_pt;

              /* The lines beside the letter are the size of the text after
               * it; a file may have left the letter itself set large. */
              for (guint r = 0; r < block->runs->len; r++)
                {
                  const W42Run *run = &g_array_index (block->runs, W42Run, r);

                  if (run->byte_offset + run->n_bytes > (gsize) (g_utf8_next_char (text) - text) && run->n_bytes > 0)
                    {
                      body = &w42_ap_table_get (aps, run->ap)->ch;
                      break;
                    }
                }
              body_pt = w42_halfpt_to_pt (body->size > 0 ? body->size : 20);
              double line_px = body_pt * 1.15 * W42_LAYOUT_DPI / 72.0;
              PangoRectangle logical;

              cap_bytes = (guint) (g_utf8_next_char (text) - text);
              cap_h = n * line_px;
              cap = pango_layout_new (self->ctx);
              apply_font_description (desc, ch);
              /* Capitals stand about seven tenths of an em tall. */
              pango_font_description_set_size (desc, (int) (cap_h * 72.0 / W42_LAYOUT_DPI / 0.72 * PANGO_SCALE));
              pango_layout_set_font_description (cap, desc);
              pango_font_description_free (desc);
              pango_layout_set_text (cap, text, (int) cap_bytes);
              pango_layout_get_pixel_extents (cap, &cap_ink, &logical);
              g_ptr_array_add (self->cap_layouts, cap);
              cap_shift = cap_ink.width + 6.0;
              wrap_w += cap_shift;
              narrow_shift += cap_shift;
              if (beside_h < 0.0 && !narrowed)
                beside_h = cap_h;
              else if (beside_h >= 0.0)
                beside_h = MAX (beside_h, cap_h);
            }
        }

      layout_block_lines (self, block, aps, pa, text_w, prefix_w, wrap_w, beside_h, cap_bytes, blines);

      /* Text flow: keep with next, keep lines together, widows and orphans.
       * Decided from the lines' heights before any is placed: whether the
       * whole paragraph moves to the next page, or where it breaks. */
      gboolean flow_break_before = FALSE;
      int flow_break_at = -1;
      int flow_line = -1;

      if (!self->galley && y > 0.0)
        {
          GArray *adv = g_array_new (FALSE, FALSE, sizeof (double));
          double total = 0.0, cum = 0.0, avail;
          int n_lines, fits = 0;

          for (guint li = 0; li < blines->len; li++)
            {
              const BlockLine *bl = &g_array_index (blines, BlockLine, li);
              double a = line_advance (pa, (double) bl->logical.height / PANGO_SCALE);

              g_array_append_val (adv, a);
              total += a;
            }

          n_lines = (int) adv->len;
          avail = text_h - notes_h - y - w42_twips_to_px (pa->space_before);
          for (int k = 0; k < n_lines; k++)
            {
              cum += g_array_index (adv, double, k);
              if (cum <= avail)
                fits = k + 1;
              else
                break;
            }

          if (fits < n_lines)
            {
              if (pa->keep_together)
                flow_break_before = TRUE;
              else if (pa->widow_control && fits > 0)
                {
                  /* One line alone at the foot is an orphan; one alone at
                   * the top a widow.  Either way one more line moves. */
                  if (fits == 1 || (n_lines - fits == 1 && fits - 1 < 2))
                    flow_break_before = TRUE;
                  else if (n_lines - fits == 1)
                    flow_break_at = fits - 1;
                }
            }
          else if (pa->keep_next && b + 1 < self->blocks->len)
            {
              const W42Block *next = g_ptr_array_index (self->blocks, b + 1);

              if (next->table < 0 && next->note < 0)
                {
                  const W42ParaFmt *npa = &w42_ap_table_get (aps, next->ap)->pa;
                  PangoLayout *nl = build_block_layout (self, next, aps, text_w, 0.0, 0, 0, 0);
                  PangoLayoutIter *ni = pango_layout_get_iter (nl);
                  PangoRectangle lg;
                  double need;

                  pango_layout_iter_get_line_extents (ni, NULL, &lg);
                  need = total + w42_twips_to_px (pa->space_after) +
                         w42_twips_to_px (npa->space_before) +
                         (double) lg.height / PANGO_SCALE;
                  pango_layout_iter_free (ni);
                  g_object_unref (nl);

                  if (need > avail)
                    flow_break_before = TRUE;
                }
            }

          g_array_free (adv, TRUE);
        }

      /* A section break: the paragraph starts a fresh page, and from here
       * the text flows in the section's own columns. */
      if (pa->section_break && b > 0 && !self->galley)
        {
          SectionCols *prev = &g_array_index (sections, SectionCols, sections->len - 1);
          double full_w = self->page_w - self->mar_l - self->mar_r;
          SectionCols sc;
          int used, rem;

          if (y > 0.0)
            {
              flush_notes (self, page_notes, &notes_h, current_page, text_h, &y);
              current_page++;
              y = 0.0;
            }
          /* The section's column pages start on a real page of their
           * own, so the last section's are padded out to a whole page. */
          used = current_page - prev->first_cp;
          rem = used % prev->n;
          if (rem != 0)
            current_page += prev->n - rem;

          sc.first_cp = current_page;
          sc.n = MAX (pa->columns, 1);
          sc.gap = w42_twips_to_px (pa->column_gap > 0 ? pa->column_gap : 720);
          sc.w = (full_w - (sc.n - 1) * sc.gap) / sc.n;
          g_array_append_val (sections, sc);

          n_columns = sc.n;
          column_w = sc.w;
          column_gap = sc.gap;
          text_w = sc.n > 1 ? sc.w : full_w;
          self->text_w = text_w;
          /* Laid out already at the old width: again at the new one. */
          g_array_set_size (blines, 0);
          layout_block_lines (self, block, aps, pa, text_w, prefix_w, wrap_w, beside_h, cap_bytes, blines);
        }

      /* A wrapped picture goes over with its paragraph when it will not
       * fit under what is on the page. */
      if (fobj != NULL && !self->galley && y > 0.0 &&
          y + w42_twips_to_px (pa->space_before) + fh > text_h - notes_h)
        flow_break_before = TRUE;

      /* Page break before: the paragraph starts a fresh page, unless it is
       * already at the top of one. */
      if ((pa->page_break_before || flow_break_before) && !self->galley && y > 0.0)
        {
          flush_notes (self, page_notes, &notes_h, current_page, text_h, &y);
          current_page++;
          y = 0.0;
        }

      /* Beside the picture no longer, after a page break: the full column,
       * apart from a dropped letter of its own. */
      if (narrowed && fobj == NULL && float_page[0] != current_page && float_page[1] != current_page)
        {
          wrap_w = cap_shift;
          narrow_shift = cap_shift;
          beside_h = cap != NULL ? cap_h : -1.0;
          g_array_set_size (blines, 0);
          layout_block_lines (self, block, aps, pa, text_w, prefix_w, wrap_w, beside_h, cap_bytes, blines);
          narrowed = FALSE;
        }

      y += w42_twips_to_px (pa->space_before);

      for (guint li = 0; li < blines->len; li++)
        {
          const BlockLine *bl = &g_array_index (blines, BlockLine, li);
          PangoLayoutLine *line = bl->line;
          PangoRectangle logical = bl->logical;
          W42LineBox box;
          double line_h, advance;

          line_h  = (double) logical.height / PANGO_SCALE;
          /* A multiple scales the line's own height, so a 24pt line double
           * spaced is twice as tall as a 24pt line, not twice as tall as a
           * 10pt one.  An exact leading overrides the type size entirely,
           * which is what "Exactly" means in the Paragraph box. */
          advance = line_advance (pa, line_h);

          flow_line++;
          if (flow_line == flow_break_at && !self->galley && y > 0.0)
            {
              /* The widow rule: this line and the last go over together. */
              flush_notes (self, page_notes, &notes_h, current_page, text_h, &y);
              current_page++;
              y = 0.0;
            }

          /* The footnotes this line refers to are laid out now, so that
           * the line and its notes can be judged together. */
          GArray *line_notes = g_array_new (FALSE, FALSE, sizeof (W42LineBox));
          double line_notes_h = 0.0;

          for (guint r = 0; r < block->runs->len; r++)
            {
              const W42Run *run = &g_array_index (block->runs, W42Run, r);

              if (run->footnote <= 0 || run->endnote)
                continue;
              if (run->byte_offset < (gsize) bl->start_index ||
                  run->byte_offset >= (gsize) (bl->start_index + bl->length))
                continue;
              if ((guint) run->footnote_id < placed->len &&
                  g_array_index (placed, gboolean, run->footnote_id))
                continue;

              line_notes_h += layout_note (self, aps, run->footnote_id, text_w,
                                           line_notes, line_notes_h);
              if ((guint) run->footnote_id >= placed->len)
                g_array_set_size (placed, run->footnote_id + 1);
              g_array_index (placed, gboolean, run->footnote_id) = TRUE;
            }

          /* A line that will not fit starts the next page.  `y > 0` keeps a
           * single over-tall line from looping forever on an empty page.
           * Normal view never breaks: the galley runs on. */
          if (!self->galley && y > 0.0 &&
              y + line_h + notes_h + line_notes_h +
                ((notes_h > 0.0 || line_notes_h > 0.0) ? NOTE_SEP : 0.0) > text_h)
            {
              flush_notes (self, page_notes, &notes_h, current_page, text_h, &y);
              current_page++;
              y = 0.0;
            }

          for (guint k = 0; k < line_notes->len; k++)
            {
              W42LineBox nb = g_array_index (line_notes, W42LineBox, k);
              nb.y += notes_h;
              g_array_append_val (page_notes, nb);
            }
          notes_h += line_notes_h;
          g_array_free (line_notes, TRUE);

          if (first_line && fobj != NULL)
            {
              /* The picture's top is level with its paragraph's first line. */
              W42FloatBox fb;

              fb.page = current_page;
              fb.x = float_side == W42_WRAP_LEFT ? self->mar_l
                                                 : self->mar_l + text_w - fw;
              fb.y = self->mar_t + y;
              fb.w = fw;
              fb.h = fh;
              fb.object = fidx;
              fb.pos = fpos;
              g_array_append_val (self->floats, fb);
              {
                int fs = float_side == W42_WRAP_LEFT ? 0 : 1;

                float_page[fs] = current_page;
                float_bottom[fs] = y + fh + float_gap;
                float_w[fs] = own_w;
              }
            }

          if (first_line && cap != NULL)
            {
              /* The letter's foot sits on the baseline of the last line it
               * spans; its top is level with the first line's. */
              CapBox cb;

              cb.page = current_page;
              cb.x = self->mar_l + indent_x + (left_on ? float_w[0] : 0.0) +
                     ((fobj != NULL && float_side == W42_WRAP_LEFT) ? own_w : 0.0);
              cb.y = self->mar_t + y + (CLAMP (pa->drop_cap, 1, 10) - 1) * advance + bl->baseline - cap_ink.height;
              cb.layout = cap;
              cb.ink_x = cap_ink.x;
              cb.ink_y = cap_ink.y;
              g_array_append_val (self->caps, cb);
              cap_top = y;
              cap_page = current_page;
            }

          box.page        = current_page;
          box.origin_x    = self->mar_l + indent_x + (bl->narrow ? narrow_shift : 0.0);
          box.x           = box.origin_x + (double) logical.x / PANGO_SCALE;
          box.y           = self->mar_t + y;
          box.width       = (double) logical.width / PANGO_SCALE;
          box.height      = line_h;
          box.baseline    = bl->baseline;
          box.block       = (int) b;
          box.start_index = bl->start_index;
          box.length      = bl->length;
          box.line        = line;
          box.prefix      = first_line ? prefix : NULL;
          box.prefix_x    = box.origin_x + w42_twips_to_px (pa->indent_first);
          first_line      = FALSE;

          g_array_append_val (self->lines, box);

          y += advance;
        }
      g_array_free (blines, TRUE);

      /* A dropped letter taller than its paragraph: what follows wraps
       * beside the rest of it, as beside a picture. */
      if (cap != NULL && cap_top >= 0.0 && cap_page == current_page && y < cap_top + cap_h &&
          !(float_page[0] == current_page && float_bottom[0] > cap_top + cap_h))
        {
          float_page[0] = current_page;
          float_w[0] = cap_shift;
          float_bottom[0] = cap_top + cap_h + 2.0;
        }

      y += w42_twips_to_px (pa->space_after);
    }


  /* Endnotes follow the text, under a rule, breaking pages as text does. */
  {
    gboolean any = FALSE;

    for (guint b = 0; b < self->blocks->len; b++)
      {
        const W42Block *block = g_ptr_array_index (self->blocks, b);
        GArray *note_lines;
        double h;

        if (block->note < 0 || !block->note_end)
          continue;
        if ((guint) block->note < placed->len &&
            g_array_index (placed, gboolean, block->note))
          continue;

        note_lines = g_array_new (FALSE, FALSE, sizeof (W42LineBox));
        h = layout_note (self, aps, block->note, text_w, note_lines, 0.0);
        (void) h;

        if (!any)
          {
            W42NoteRule rule;

            if (!self->galley && y > 0.0 && y + NOTE_SEP + 20.0 > text_h - notes_h)
              {
                flush_notes (self, page_notes, &notes_h, current_page, text_h, &y);
                current_page++;
                y = 0.0;
              }
            y += NOTE_SEP;
            rule.page = current_page;
            rule.x = self->mar_l;
            rule.y = self->mar_t + y - NOTE_SEP / 2;
            g_array_append_val (self->note_rules, rule);
            any = TRUE;
          }

        for (guint k = 0; k < note_lines->len; k++)
          {
            W42LineBox nb = g_array_index (note_lines, W42LineBox, k);

            if (!self->galley && y > 0.0 && y + nb.height > text_h - notes_h)
              {
                flush_notes (self, page_notes, &notes_h, current_page, text_h, &y);
                current_page++;
                y = 0.0;
              }
            nb.page = current_page;
            nb.y = self->mar_t + y;
            g_array_append_val (self->lines, nb);
            y += nb.height;
          }
        g_array_free (note_lines, TRUE);

        if ((guint) block->note >= placed->len)
          g_array_set_size (placed, block->note + 1);
        g_array_index (placed, gboolean, block->note) = TRUE;
      }
  }

  /* Notes whose reference sits somewhere the flow did not visit -- a
   * table cell, say -- still get shown, on the last page. */
  for (guint b = 0; b < self->blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (self->blocks, b);

      if (block->note < 0)
        continue;
      if ((guint) block->note < placed->len &&
          g_array_index (placed, gboolean, block->note))
        continue;

      {
        GArray *extra = g_array_new (FALSE, FALSE, sizeof (W42LineBox));
        double h = layout_note (self, aps, block->note, text_w, extra, notes_h);

        for (guint k2 = 0; k2 < extra->len; k2++)
          g_array_append_val (page_notes, g_array_index (extra, W42LineBox, k2));
        notes_h += h;
        g_array_free (extra, TRUE);
      }
      if ((guint) block->note >= placed->len)
        g_array_set_size (placed, block->note + 1);
      g_array_index (placed, gboolean, block->note) = TRUE;
    }

  flush_notes (self, page_notes, &notes_h, current_page, text_h, &y);
  g_array_free (page_notes, TRUE);
  g_array_free (placed, TRUE);
  self->n_pages = current_page + 1;

  /* Column pages fold on to real pages a section at a time: column page
   * c of a section is column (c - first) % n of real page base +
   * (c - first) / n, shifted right by the columns before it.  With one
   * column this is the identity. */
  {
    int real_base = 0;

    for (guint si = 0; si < sections->len; si++)
      {
        const SectionCols *sc = &g_array_index (sections, SectionCols, si);
        int end_cp = si + 1 < sections->len
                       ? g_array_index (sections, SectionCols, si + 1).first_cp
                       : current_page + 1;
        int n = sc->n;
        double step = sc->w + sc->gap;
        int real_pages = (end_cp - sc->first_cp + n - 1) / n;

        if (n > 1)
          {
            int last_first = sc->first_cp + ((end_cp - 1 - sc->first_cp) / n) * n;

            balance_last_page (self, last_first, end_cp, n);
            /* Balancing may have moved lines into columns past the last
             * column page used; they belong to this section still. */
            end_cp = MAX (end_cp, last_first + n);
          }

        for (guint i = 0; i < self->lines->len; i++)
          {
            W42LineBox *box = &g_array_index (self->lines, W42LineBox, i);
            int local = box->page - sc->first_cp;

            if (box->page < sc->first_cp || box->page >= end_cp)
              continue;
            box->page = real_base + local / n;
            box->x += (local % n) * step;
            box->origin_x += (local % n) * step;
            box->prefix_x += (local % n) * step;
          }
        for (guint i = 0; i < self->cell_rects->len; i++)
          {
            W42CellRect *r = &g_array_index (self->cell_rects, W42CellRect, i);
            int local = r->page - sc->first_cp;

            if (r->page < sc->first_cp || r->page >= end_cp)
              continue;
            r->page = real_base + local / n;
            r->x += (local % n) * step;
          }
        for (guint i = 0; i < self->note_rules->len; i++)
          {
            W42NoteRule *rule = &g_array_index (self->note_rules, W42NoteRule, i);
            int local = rule->page - sc->first_cp;

            if (rule->page < sc->first_cp || rule->page >= end_cp)
              continue;
            rule->page = real_base + local / n;
            rule->x += (local % n) * step;
          }
        for (guint i = 0; i < self->floats->len; i++)
          {
            W42FloatBox *f = &g_array_index (self->floats, W42FloatBox, i);
            int local = f->page - sc->first_cp;

            if (f->page < sc->first_cp || f->page >= end_cp)
              continue;
            f->page = real_base + local / n;
            f->x += (local % n) * step;
          }
        for (guint i = 0; i < self->caps->len; i++)
          {
            CapBox *c = &g_array_index (self->caps, CapBox, i);
            int local = c->page - sc->first_cp;

            if (c->page < sc->first_cp || c->page >= end_cp)
              continue;
            c->page = real_base + local / n;
            c->x += (local % n) * step;
          }
        real_base += MAX (real_pages, 1);
      }
    self->n_pages = MAX (real_base, 1);
  }
  g_array_free (sections, TRUE);

  if (!self->galley)
    build_furniture (self, pt);

  if (self->galley)
    {
      /* One page, as tall as what went on it, so the view scrolls the text
       * rather than a sheet of a fixed size. */
      self->n_pages = 1;
      self->page_h = self->mar_t + y + self->mar_b;
    }
}

/* Expands {PAGE}, {NUMPAGES} and {DATE} in a header or footer. */
static char *
expand_fields (const char *text, int page, int n_pages)
{
  GString *out = g_string_new (NULL);
  const char *p = text;

  while (*p != '\0')
    {
      if (g_str_has_prefix (p, "{PAGE}"))
        {
          g_string_append_printf (out, "%d", page + 1);
          p += 6;
        }
      else if (g_str_has_prefix (p, "{NUMPAGES}"))
        {
          g_string_append_printf (out, "%d", n_pages);
          p += 10;
        }
      else if (g_str_has_prefix (p, "{DATE}"))
        {
          GDateTime *now = g_date_time_new_now_local ();
          char *date = g_date_time_format (now, "%x");

          g_string_append (out, date);
          g_free (date);
          g_date_time_unref (now);
          p += 6;
        }
      else
        {
          g_string_append_c (out, *p);
          p++;
        }
    }

  return g_string_free (out, FALSE);
}

/* Word put the header half an inch from the top edge and the footer half an
 * inch from the bottom, inside the margins; so does this. */
static void
build_furniture (W42Layout *self, W42PieceTable *pt)
{
  const W42Style *normal = w42_stylesheet_find (self->styles, "Normal");
  double text_w = self->page_w - self->mar_l - self->mar_r;
  double edge = w42_twips_to_px (720);

  for (int which = 0; which < 2; which++)
    {
      for (int page = 0; page < self->n_pages; page++)
        {
          /* A title page and even pages may have their own. */
          const W42PageText *slot = which == 0 ? w42_pt_page_header (pt, page)
                                               : w42_pt_page_footer (pt, page);

          PangoLayout *layout;
          PangoFontDescription *desc;
          char *text;

          if (slot == NULL || slot->text == NULL || *slot->text == '\0')
            continue;
          layout = pango_layout_new (self->ctx);
          desc = pango_font_description_new ();
          text = expand_fields (slot->text, page, self->n_pages);
          W42Furniture f;
          int w, h;

          if (normal != NULL)
            apply_font_description (desc, &normal->ch);
          else
            pango_font_description_set_family (desc, "Serif");
          pango_layout_set_font_description (layout, desc);
          pango_font_description_free (desc);

          pango_layout_set_text (layout, text, -1);
          pango_layout_set_width (layout, (int) (text_w * PANGO_SCALE));
          pango_layout_set_alignment (layout,
            slot->align == W42_ALIGN_CENTER ? PANGO_ALIGN_CENTER :
            slot->align == W42_ALIGN_RIGHT  ? PANGO_ALIGN_RIGHT  : PANGO_ALIGN_LEFT);
          pango_layout_get_pixel_size (layout, &w, &h);
          g_free (text);

          f.page = page;
          f.x = self->mar_l;
          f.y = (which == 0) ? edge : self->page_h - edge - h;
          f.layout = layout;

          g_ptr_array_add (self->furniture_layouts, layout);
          g_array_append_val (self->furniture, f);
        }
    }
}

const GArray *
w42_layout_furniture (W42Layout *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->furniture;
}

const GArray *
w42_layout_cell_rects (W42Layout *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->cell_rects;
}

void
w42_layout_draw_backdrop (W42Layout *self, cairo_t *cr, int page)
{
  g_return_if_fail (self != NULL);

  if (self->blocks == NULL)
    return;

  /* Each paragraph's lines on this page, taken together, give the box the
   * shading fills and the borders run round.  A paragraph that runs on to
   * the next page gets its top border on this one and its bottom on the
   * next, which is what Word did too. */
  for (guint i = 0; i < self->lines->len; )
    {
      const W42LineBox *first = &g_array_index (self->lines, W42LineBox, i);
      const W42Block *block = g_ptr_array_index (self->blocks, first->block);
      const W42ParaFmt *pa = &w42_ap_table_get (self->aps, block->ap)->pa;
      guint j = i;
      double top = first->y, bottom = first->y + first->height;
      double left, right;

      while (j < self->lines->len)
        {
          const W42LineBox *box = &g_array_index (self->lines, W42LineBox, j);

          if (box->block != first->block || box->page != first->page)
            break;
          top = MIN (top, box->y);
          bottom = MAX (bottom, box->y + box->height);
          j++;
        }

      if (first->page == page && (pa->shading > 0 || pa->border != 0) &&
          block->table < 0)
        {
          double width = (double) pa->border_width / W42_TWIPS_PER_PX;

          /* From the line's own origin, so a paragraph in the second column
           * is shaded in the second column. */
          left  = first->origin_x;
          right = first->origin_x - w42_twips_to_px (pa->indent_left) +
                  self->text_w - w42_twips_to_px (pa->indent_right);
          if (pa->indent_first < 0)
            left += w42_twips_to_px (pa->indent_first);
          top -= 2.0;
          bottom += 2.0;

          cairo_save (cr);
          if (pa->shading > 0)
            {
              double g = 1.0 - CLAMP (pa->shading, 0, 100) / 100.0;

              cairo_set_source_rgb (cr, g, g, g);
              cairo_rectangle (cr, left, top, right - left, bottom - top);
              cairo_fill (cr);
            }
          if (pa->border != 0)
            {
              if (width < 0.75)
                width = 0.75;
              cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
              cairo_set_line_width (cr, width);
              if (pa->border & W42_BORDER_TOP)
                {
                  cairo_move_to (cr, left, top);
                  cairo_line_to (cr, right, top);
                }
              if (pa->border & W42_BORDER_BOTTOM)
                {
                  cairo_move_to (cr, left, bottom);
                  cairo_line_to (cr, right, bottom);
                }
              if (pa->border & W42_BORDER_LEFT)
                {
                  cairo_move_to (cr, left, top);
                  cairo_line_to (cr, left, bottom);
                }
              if (pa->border & W42_BORDER_RIGHT)
                {
                  cairo_move_to (cr, right, top);
                  cairo_line_to (cr, right, bottom);
                }
              cairo_stroke (cr);
            }
          cairo_restore (cr);
        }

      i = j;
    }
}

void
w42_layout_draw_furniture (W42Layout *self, cairo_t *cr, int page)
{
  g_return_if_fail (self != NULL);

  /* Dropped letters. */
  for (guint i = 0; i < self->caps->len; i++)
    {
      const CapBox *c = &g_array_index (self->caps, CapBox, i);

      if (c->page != page)
        continue;
      cairo_save (cr);
      cairo_set_source_rgb (cr, 0, 0, 0);
      cairo_move_to (cr, c->x - c->ink_x, c->y - c->ink_y);
      pango_cairo_show_layout (cr, c->layout);
      cairo_restore (cr);
    }

  /* Wrapped pictures, under the text that runs beside them. */
  for (guint i = 0; self->objects != NULL && i < self->floats->len; i++)
    {
      const W42FloatBox *f = &g_array_index (self->floats, W42FloatBox, i);
      cairo_surface_t *surface;

      if (f->page != page)
        continue;
      surface = w42_object_surface (self->objects, f->object);
      if (surface == NULL || f->w <= 0.0 || f->h <= 0.0)
        continue;
      cairo_save (cr);
      cairo_translate (cr, f->x, f->y);
      cairo_scale (cr, f->w / cairo_image_surface_get_width (surface),
                       f->h / cairo_image_surface_get_height (surface));
      cairo_set_source_surface (cr, surface, 0, 0);
      cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
      cairo_paint (cr);
      cairo_restore (cr);
    }

  /* The short rule between the text and its footnotes. */
  for (guint i = 0; i < self->note_rules->len; i++)
    {
      const W42NoteRule *rule = &g_array_index (self->note_rules, W42NoteRule, i);

      if (rule->page != page)
        continue;
      cairo_save (cr);
      cairo_set_source_rgb (cr, 0, 0, 0);
      cairo_set_line_width (cr, 1.0);
      cairo_move_to (cr, rule->x, floor (rule->y) + 0.5);
      cairo_line_to (cr, rule->x + self->text_w / 3.0, floor (rule->y) + 0.5);
      cairo_stroke (cr);
      cairo_restore (cr);
    }

  /* The cells of an unruled table, faintly, when gridlines are on. */
  if (self->gridlines && self->cell_rects->len > 0)
    {
      static const double dashes[2] = { 1.0, 2.0 };

      cairo_save (cr);
      cairo_set_line_width (cr, 1.0);
      cairo_set_source_rgb (cr, 0.65, 0.65, 0.72);
      cairo_set_dash (cr, dashes, 2, 0.0);
      for (guint i = 0; i < self->cell_rects->len; i++)
        {
          const W42CellRect *r = &g_array_index (self->cell_rects, W42CellRect, i);

          if (r->page != page || r->sides == W42_BORDER_BOX)
            continue;
          cairo_rectangle (cr, floor (r->x) + 0.5, floor (r->y) + 0.5,
                           floor (r->w), floor (r->h));
        }
      cairo_stroke (cr);
      cairo_restore (cr);
    }

  /* Cell borders: Word's default grid, a hairline round every cell. */
  if (self->cell_rects->len > 0)
    {
      cairo_save (cr);
      cairo_set_line_width (cr, 1.0);
      cairo_set_source_rgb (cr, 0, 0, 0);
      for (guint i = 0; i < self->cell_rects->len; i++)
        {
          const W42CellRect *r = &g_array_index (self->cell_rects, W42CellRect, i);
          if (!r->borders)
            continue;

          if (r->page != page)
            continue;

          {
            double x0 = floor (r->x) + 0.5, y0 = floor (r->y) + 0.5;
            double x1 = x0 + floor (r->w), y1 = y0 + floor (r->h);

            if (r->sides == W42_BORDER_BOX)
              cairo_rectangle (cr, x0, y0, floor (r->w), floor (r->h));
            else
              {
                if (r->sides & W42_BORDER_TOP)    { cairo_move_to (cr, x0, y0); cairo_line_to (cr, x1, y0); }
                if (r->sides & W42_BORDER_BOTTOM) { cairo_move_to (cr, x0, y1); cairo_line_to (cr, x1, y1); }
                if (r->sides & W42_BORDER_LEFT)   { cairo_move_to (cr, x0, y0); cairo_line_to (cr, x0, y1); }
                if (r->sides & W42_BORDER_RIGHT)  { cairo_move_to (cr, x1, y0); cairo_line_to (cr, x1, y1); }
              }
          }
        }
      cairo_stroke (cr);
      cairo_restore (cr);
    }

  for (guint i = 0; i < self->furniture->len; i++)
    {
      const W42Furniture *f = &g_array_index (self->furniture, W42Furniture, i);

      if (f->page != page)
        continue;

      cairo_move_to (cr, f->x, f->y);
      pango_cairo_show_layout (cr, f->layout);
    }
}

/* The marks Word showed with its pilcrow button: a dot for a space, an
 * arrow for a tab, a bent arrow for a line break, a pilcrow where the
 * paragraph ends.  Drawn in blue over the line, in the line's own size. */
static void
draw_marks (W42Layout *self, cairo_t *cr, const W42LineBox *box)
{
  const W42Block *block = g_ptr_array_index (self->blocks, box->block);
  const char *text = block->text->str;
  gsize text_len = block->text->len;
  PangoLayout *marks = pango_layout_new (self->ctx);
  PangoFontDescription *desc = pango_font_description_new ();
  double size_pt = 10.0;
  double mark_baseline;

  /* The size of the line's first run, so the marks match the text. */
  if (block->runs->len > 0)
    {
      const W42CharFmt *ch = &w42_ap_table_get (self->aps,
                                                g_array_index (block->runs, W42Run, 0).ap)->ch;
      size_pt = MAX (ch->size / 2.0, 6.0);
    }
  pango_font_description_set_family (desc, "Sans");
  pango_font_description_set_absolute_size (desc, size_pt * W42_LAYOUT_DPI / 72.0 * PANGO_SCALE);
  pango_layout_set_font_description (marks, desc);
  pango_font_description_free (desc);

  cairo_save (cr);
  cairo_set_source_rgba (cr, 0.21, 0.52, 0.89, 0.85);

  for (guint i = box->start_index; i < box->start_index + box->length && i < text_len; i++)
    {
      const char *mark = NULL;
      int x_pango = 0;

      if (text[i] == ' ')
        {
          /* A dot in the middle of the space, drawn rather than set:
           * a font's middle dot is too faint to see at text sizes. */
          int x_after = 0;
          double px, r = MAX (size_pt / 10.0, 1.0);

          pango_layout_line_index_to_x (box->line, (int) i, FALSE, &x_pango);
          pango_layout_line_index_to_x (box->line, (int) i, TRUE, &x_after);
          px = box->x + (x_pango + x_after) / 2.0 / PANGO_SCALE;
          cairo_arc (cr, px, box->y + box->baseline - size_pt * W42_LAYOUT_DPI / 72.0 * 0.28, r, 0, 2 * G_PI);
          cairo_fill (cr);
          continue;
        }
      else if (text[i] == '\t')
        mark = "\342\206\222";                    /* rightwards arrow */
      else if ((guchar) text[i] == 0xE2 && i + 2 < text_len &&
               (guchar) text[i + 1] == 0x80 && (guchar) text[i + 2] == 0xA8)
        mark = "\342\206\265";                    /* downwards arrow with corner leftwards */
      if (mark == NULL)
        continue;
      pango_layout_line_index_to_x (box->line, (int) i, FALSE, &x_pango);
      pango_layout_set_text (marks, mark, -1);
      mark_baseline = pango_layout_get_baseline (marks) / (double) PANGO_SCALE;
      cairo_move_to (cr, box->x + x_pango / (double) PANGO_SCALE + 1.0,
                     box->y + box->baseline - mark_baseline);
      pango_cairo_show_layout (cr, marks);
    }

  /* The paragraph mark after the last line. */
  if (box->start_index + box->length >= text_len)
    {
      pango_layout_set_text (marks, "\302\266", -1);
      mark_baseline = pango_layout_get_baseline (marks) / (double) PANGO_SCALE;
      cairo_move_to (cr, box->x + box->width + 2.0, box->y + box->baseline - mark_baseline);
      pango_cairo_show_layout (cr, marks);
    }

  cairo_restore (cr);
  g_object_unref (marks);
}

/* Dots, dashes or a rule in the gap a tab leaves, for the stops that ask
 * for one.  Pango sets the gap and says nothing about it, so the leader
 * is drawn afterwards over the space the tab took: from where the tab
 * character starts to where the text after it begins. */
static void
draw_leaders (W42Layout *self, cairo_t *cr, const W42LineBox *box)
{
  const W42Block *block = g_ptr_array_index (self->blocks, box->block);
  const W42ParaFmt *pa = &w42_ap_table_get (self->aps, block->ap)->pa;
  const char *text = block->text->str;
  gsize text_len = block->text->len;

  if (pa->n_tabs == 0)
    return;

  for (gsize i = box->start_index; i < box->start_index + box->length && i < text_len; i++)
    {
      W42TabLeader leader;
      int x_before = 0, x_after = 0;
      double from, to, y, size_px;

      if (text[i] != '\t')
        continue;

      pango_layout_line_index_to_x (box->line, (int) i, FALSE, &x_before);
      pango_layout_line_index_to_x (box->line, (int) i, TRUE, &x_after);
      from = box->x + MIN (x_before, x_after) / (double) PANGO_SCALE;
      to   = box->x + MAX (x_before, x_after) / (double) PANGO_SCALE;
      if (to - from < 2.0)
        continue;

      /* Which stop the tab landed on: the first one past where the tab
       * began, which is what the tab advanced to.  Where the text starts
       * again is no guide -- at a right or centre stop it starts before
       * the stop, by the width of the text itself -- and counting tabs
       * from the start of the line goes wrong as soon as a line wraps.
       * Beyond the last stop the layout falls back on one every half
       * inch, and those carry no leader. */
      leader = W42_TAB_LEAD_NONE;
      {
        double best = 0.0;
        gboolean found = FALSE;

        for (int t = 0; t < pa->n_tabs; t++)
          {
            double stop_x = box->origin_x +
                            w42_twips_to_px (pa->tab_pos[t] - pa->indent_left);

            if (stop_x <= from + 0.5)
              continue;
            if (!found || stop_x < best)
              {
                best = stop_x;
                found = TRUE;
                leader = W42_TAB_LEADER (pa->tab_kind[t]);
              }
          }
        /* The text after the tab ends at the stop, or begins before it;
         * either way it cannot reach past it. */
        if (!found || to > best + 1.0)
          leader = W42_TAB_LEAD_NONE;
      }
      if (leader == W42_TAB_LEAD_NONE)
        continue;

      size_px = MAX (box->height, 8.0);
      y = box->y + box->baseline;

      cairo_save (cr);
      switch (leader)
        {
        case W42_TAB_LEAD_LINE:
          cairo_set_line_width (cr, MAX (size_px / 14.0, 1.0));
          cairo_move_to (cr, from + 1.0, floor (y + size_px / 8.0) + 0.5);
          cairo_line_to (cr, to - 1.0,   floor (y + size_px / 8.0) + 0.5);
          cairo_stroke (cr);
          break;

        case W42_TAB_LEAD_DASH:
        case W42_TAB_LEAD_DOT:
        default:
          {
            double step = leader == W42_TAB_LEAD_DASH ? size_px / 2.2 : size_px / 3.4;
            double r = MAX (size_px / 18.0, 0.6);
            double dash = MAX (size_px / 4.5, 2.0);

            cairo_set_line_width (cr, MAX (size_px / 16.0, 1.0));
            /* A little air at each end, so the leader does not touch
             * the text it runs between. */
            for (double x = from + step; x < to - step; x += step)
              {
                if (leader == W42_TAB_LEAD_DASH)
                  {
                    cairo_move_to (cr, x, floor (y - size_px / 6.0) + 0.5);
                    cairo_line_to (cr, MIN (x + dash, to), floor (y - size_px / 6.0) + 0.5);
                    cairo_stroke (cr);
                  }
                else
                  {
                    cairo_arc (cr, x, y - size_px / 6.0, r, 0, 2 * G_PI);
                    cairo_fill (cr);
                  }
              }
          }
          break;
        }
      cairo_restore (cr);
    }
}

void
w42_layout_draw_line (W42Layout *self, cairo_t *cr, const W42LineBox *box)
{
  if (box->prefix != NULL)
    {
      double baseline = pango_layout_get_baseline (box->prefix) /
                        (double) PANGO_SCALE;

      cairo_move_to (cr, box->prefix_x, box->y + box->baseline - baseline);
      pango_cairo_show_layout (cr, box->prefix);
    }

  cairo_move_to (cr, box->x, box->y + box->baseline);
  pango_cairo_show_layout_line (cr, box->line);

  if (self->blocks != NULL && box->block >= 0 &&
      (guint) box->block < self->blocks->len)
    draw_leaders (self, cr, box);

  if (self->show_marks && self->blocks != NULL && box->block >= 0 &&
      (guint) box->block < self->blocks->len)
    draw_marks (self, cr, box);
}

/* ---------------------------------------------------------------------- */
/* Accessors                                                               */
/* ---------------------------------------------------------------------- */

int
w42_layout_n_pages (W42Layout *self)
{
  g_return_val_if_fail (self != NULL, 1);
  return self->n_pages;
}

double
w42_layout_page_width (W42Layout *self)
{
  g_return_val_if_fail (self != NULL, 0.0);
  return self->page_w;
}

double
w42_layout_page_height (W42Layout *self)
{
  g_return_val_if_fail (self != NULL, 0.0);
  return self->page_h;
}

const GArray *
w42_layout_lines (W42Layout *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->lines;
}

GPtrArray *
w42_layout_blocks (W42Layout *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->blocks;
}

/* ---------------------------------------------------------------------- */
/* Caret geometry                                                          */
/* ---------------------------------------------------------------------- */

/* The last line of `block` that starts at or before `byte`.  Taking the last
 * one puts the caret at the start of the following line when a position falls
 * exactly on a wrap, which is where Word puts it. */
static int
line_for_byte (W42Layout *self, int block, gsize byte)
{
  int found = -1;

  for (guint i = 0; i < self->lines->len; i++)
    {
      const W42LineBox *box = &g_array_index (self->lines, W42LineBox, i);

      if (box->block != block)
        {
          if (found >= 0)
            break;
          continue;
        }

      if (box->start_index <= byte)
        found = (int) i;
      else
        break;
    }

  return found;
}

gboolean
w42_layout_pos_to_caret (W42Layout *self,
                         gsize      pos,
                         int       *page,
                         double    *x,
                         double    *y,
                         double    *height)
{
  int block, line_index;
  const W42LineBox *box;
  const W42Block *blk;
  gsize byte;
  int px = 0;

  g_return_val_if_fail (self != NULL, FALSE);

  if (self->lines->len == 0 || self->blocks == NULL)
    return FALSE;

  block = w42_layout_block_at_pos (self, pos);
  if (block < 0)
    return FALSE;

  blk  = g_ptr_array_index (self->blocks, block);
  byte = w42_block_pos_to_byte (blk, pos);

  line_index = line_for_byte (self, block, byte);
  if (line_index < 0)
    return FALSE;

  box = &g_array_index (self->lines, W42LineBox, line_index);

  pango_layout_line_index_to_x (box->line, (int) byte, FALSE, &px);

  if (page)   *page   = box->page;
  if (x)      *x      = box->x + (double) px / PANGO_SCALE;
  if (y)      *y      = box->y;
  if (height) *height = box->height;

  return TRUE;
}

gboolean
w42_layout_object_rect (W42Layout *self, gsize pos, int *page,
                        double *x, double *y, double *width, double *height)
{
  int block, line_index;
  const W42LineBox *box;
  const W42Block *blk;
  const W42Object *object = NULL;
  gsize byte;
  int px0 = 0, px1 = 0;
  double w, h;

  g_return_val_if_fail (self != NULL, FALSE);

  if (self->lines->len == 0 || self->blocks == NULL || self->objects == NULL)
    return FALSE;

  block = w42_layout_block_at_pos (self, pos);
  if (block < 0)
    return FALSE;

  blk = g_ptr_array_index (self->blocks, block);
  for (guint i = 0; i < blk->runs->len; i++)
    {
      const W42Run *run = &g_array_index (blk->runs, W42Run, i);

      if (run->object != W42_OBJECT_NONE && run->doc_pos == pos)
        object = w42_object_table_get (self->objects, run->object);
    }
  if (object == NULL)
    return FALSE;

  if (object->wrap != W42_WRAP_INLINE)
    {
      for (guint i = 0; i < self->floats->len; i++)
        {
          const W42FloatBox *f = &g_array_index (self->floats, W42FloatBox, i);

          if (f->pos != pos)
            continue;
          if (page)   *page   = f->page;
          if (x)      *x      = f->x;
          if (y)      *y      = f->y;
          if (width)  *width  = f->w;
          if (height) *height = f->h;
          return TRUE;
        }
      return FALSE;
    }

  byte = w42_block_pos_to_byte (blk, pos);
  line_index = line_for_byte (self, block, byte);
  if (line_index < 0)
    return FALSE;

  box = &g_array_index (self->lines, W42LineBox, line_index);

  /* The same fit-to-column that build_attributes applies. */
  w = w42_twips_to_px (object->width);
  h = w42_twips_to_px (object->height);
  if (w > self->text_w && self->text_w > 0)
    {
      h = h * (self->text_w / w);
      w = self->text_w;
    }

  pango_layout_line_index_to_x (box->line, (int) byte, FALSE, &px0);
  pango_layout_line_index_to_x (box->line, (int) byte, TRUE, &px1);

  if (page)   *page   = box->page;
  if (x)      *x      = box->x + (double) MIN (px0, px1) / PANGO_SCALE;
  if (y)      *y      = box->y + box->baseline - h;
  if (width)  *width  = w;
  if (height) *height = h;

  return TRUE;
}

gsize
w42_layout_point_to_pos (W42Layout *self, int page, double x, double y)
{
  const W42LineBox *best = NULL;
  double best_score = 0.0;
  int index = 0, trailing = 0;
  const W42Block *blk;

  g_return_val_if_fail (self != NULL, 0);

  if (self->lines->len == 0 || self->blocks == NULL)
    return 0;

  /* The line nearest the point, vertically first and then horizontally --
   * horizontally too, because table cells put lines side by side and the
   * nearest line by height alone is the first cell in the row. */
  for (guint i = 0; i < self->lines->len; i++)
    {
      const W42LineBox *box = &g_array_index (self->lines, W42LineBox, i);
      double dy, dx, score;

      if (box->page != page)
        continue;

      if (y >= box->y && y < box->y + box->height)
        dy = 0.0;
      else
        dy = (y < box->y) ? box->y - y : y - (box->y + box->height);

      if (x >= box->origin_x && x < box->x + MAX (box->width, 1.0))
        dx = 0.0;
      else
        dx = (x < box->origin_x) ? box->origin_x - x : x - (box->x + box->width);

      score = dy * 1000.0 + dx;
      if (best == NULL || score < best_score)
        {
          best = box;
          best_score = score;
        }
    }

  /* Clicking on a page with nothing on it lands on the nearest line anywhere. */
  if (best == NULL)
    {
      for (guint i = 0; i < self->lines->len; i++)
        {
          const W42LineBox *box = &g_array_index (self->lines, W42LineBox, i);

          if (best == NULL || ABS (box->page - page) < ABS (best->page - page))
            best = box;
        }
    }

  if (best == NULL)
    return 0;

  pango_layout_line_x_to_index (best->line,
                                (int) ((x - best->x) * PANGO_SCALE),
                                &index, &trailing);

  /* A line of a split paragraph holds hidden text beyond its own part;
   * the caret stays within the part that shows. */
  if (index < (int) best->start_index)
    {
      index = (int) best->start_index;
      trailing = 0;
    }
  else if (index >= (int) (best->start_index + best->length))
    {
      index = (int) (best->start_index + best->length);
      trailing = 0;
    }

  blk = g_ptr_array_index (self->blocks, best->block);

  if (trailing > 0)
    {
      const char *p = blk->text->str + index;
      for (int i = 0; i < trailing && *p != '\0'; i++)
        p = g_utf8_next_char (p);
      index = (int) (p - blk->text->str);
    }

  return w42_block_byte_to_pos (blk, (gsize) index);
}

static int
line_index_for_pos (W42Layout *self, gsize pos)
{
  int block = w42_layout_block_at_pos (self, pos);
  const W42Block *blk;

  if (block < 0)
    return -1;

  blk = g_ptr_array_index (self->blocks, block);
  return line_for_byte (self, block, w42_block_pos_to_byte (blk, pos));
}

gsize
w42_layout_move_line (W42Layout *self, gsize pos, int dir, double *want_x)
{
  int current;
  const W42LineBox *from, *best = NULL;
  double x, best_score = 0.0;

  g_return_val_if_fail (self != NULL, pos);

  current = line_index_for_pos (self, pos);
  if (current < 0)
    return pos;

  from = &g_array_index (self->lines, W42LineBox, current);

  if (want_x != NULL && *want_x >= 0.0)
    {
      x = *want_x;
    }
  else
    {
      double cx = 0.0;
      if (!w42_layout_pos_to_caret (self, pos, NULL, &cx, NULL, NULL))
        return pos;
      x = cx;
      if (want_x != NULL)
        *want_x = cx;
    }

  /* The nearest line in the direction of travel, geometrically: the next
   * line in the array is the wrong one when cells sit side by side. */
  for (guint i = 0; i < self->lines->len; i++)
    {
      const W42LineBox *box = &g_array_index (self->lines, W42LineBox, i);
      double dy, dx, score;
      gboolean beyond;

      if (box == from)
        continue;

      if (box->page != from->page)
        beyond = (dir > 0) ? box->page > from->page : box->page < from->page;
      else
        beyond = (dir > 0) ? box->y >= from->y + from->height - 0.5
                           : box->y + box->height <= from->y + 0.5;

      if (!beyond)
        continue;

      dy = (box->page == from->page)
             ? fabs (box->y - from->y)
             : 100000.0 * ABS (box->page - from->page) + box->y;
      if (x >= box->origin_x && x <= box->x + box->width)
        dx = 0.0;
      else
        dx = (x < box->origin_x) ? box->origin_x - x : x - (box->x + box->width);

      score = dy * 1000.0 + dx;
      if (best == NULL || score < best_score)
        {
          best = box;
          best_score = score;
        }
    }

  if (best == NULL)
    return pos;

  return w42_layout_point_to_pos (self, best->page, x, best->y + best->height / 2.0);
}

gsize
w42_layout_line_start (W42Layout *self, gsize pos)
{
  int index;
  const W42LineBox *box;
  const W42Block *blk;

  g_return_val_if_fail (self != NULL, pos);

  index = line_index_for_pos (self, pos);
  if (index < 0)
    return pos;

  box = &g_array_index (self->lines, W42LineBox, index);
  blk = g_ptr_array_index (self->blocks, box->block);

  return w42_block_byte_to_pos (blk, box->start_index);
}

gsize
w42_layout_line_end (W42Layout *self, gsize pos)
{
  int index;
  const W42LineBox *box;
  const W42Block *blk;
  gsize byte;

  g_return_val_if_fail (self != NULL, pos);

  index = line_index_for_pos (self, pos);
  if (index < 0)
    return pos;

  box = &g_array_index (self->lines, W42LineBox, index);
  blk = g_ptr_array_index (self->blocks, box->block);
  byte = box->start_index + box->length;

  /* Do not step past a trailing space that only exists because the line
   * wrapped there. */
  if (byte < blk->text->len)
    {
      const char *p = blk->text->str + byte;
      const char *prev = g_utf8_find_prev_char (blk->text->str, p);

      if (prev != NULL && *prev == ' ')
        byte = (gsize) (prev - blk->text->str);
    }

  return w42_block_byte_to_pos (blk, byte);
}

void
w42_layout_describe_pos (W42Layout *self,
                         gsize      pos,
                         int       *page,
                         int       *line,
                         int       *column)
{
  int index;
  const W42LineBox *box;
  const W42Block *blk;
  int line_on_page = 1;

  if (page)   *page   = 1;
  if (line)   *line   = 1;
  if (column) *column = 1;

  g_return_if_fail (self != NULL);

  index = line_index_for_pos (self, pos);
  if (index < 0)
    return;

  box = &g_array_index (self->lines, W42LineBox, index);
  blk = g_ptr_array_index (self->blocks, box->block);

  for (int i = index - 1; i >= 0; i--)
    {
      if (g_array_index (self->lines, W42LineBox, i).page != box->page)
        break;
      line_on_page++;
    }

  if (page) *page = box->page + 1;
  if (line) *line = line_on_page;

  if (column)
    {
      gsize byte = w42_block_pos_to_byte (blk, pos);
      const char *start = blk->text->str + box->start_index;
      const char *here  = blk->text->str + MIN (byte, blk->text->len);

      if (here < start)
        here = start;

      *column = (int) g_utf8_pointer_to_offset (start, here) + 1;
    }
}

const GArray *
w42_layout_floats (W42Layout *self)
{
  g_return_val_if_fail (self != NULL, NULL);
  return self->floats;
}

void
w42_layout_set_gridlines (W42Layout *self, gboolean show)
{
  g_return_if_fail (self != NULL);
  self->gridlines = show;
}
