/* w42-rtf.c - see w42-rtf.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-rtf.h"

#include "w42-image.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================== */
/* Writing                                                                 */
/* ====================================================================== */

/* RTF names fonts and colours by index into tables at the top of the file,
 * so both have to be collected before a single character can be written. */
typedef struct {
  GPtrArray  *fonts;    /* interned family names, in table order */
  GArray     *colours;  /* guint32, in table order; index 0 is \cf1 */
} RtfTables;

/* A name in a table: the characters RTF would read as syntax are left
 * out, since a font or style name cannot be escaped there. */
static void
rtf_plain (GString *out, const char *name)
{
  for (const char *p = name; p != NULL && *p != '\0'; p++)
    if (*p != '{' && *p != '}' && *p != '\\' && *p != ';' && (guchar) *p >= 0x20)
      g_string_append_c (out, *p);
}

static guint
table_intern_font (RtfTables *tables, const char *family)
{
  if (family == NULL)
    family = "Times New Roman";

  for (guint i = 0; i < tables->fonts->len; i++)
    if (g_strcmp0 (g_ptr_array_index (tables->fonts, i), family) == 0)
      return i;

  g_ptr_array_add (tables->fonts, (gpointer) g_intern_string (family));
  return tables->fonts->len - 1;
}

static guint
table_intern_colour (RtfTables *tables, guint32 colour)
{
  for (guint i = 0; i < tables->colours->len; i++)
    if (g_array_index (tables->colours, guint32, i) == colour)
      return i;

  g_array_append_val (tables->colours, colour);
  return tables->colours->len - 1;
}

static void
collect_tables (GPtrArray *blocks, W42ApTable *aps, RtfTables *tables,
                W42StyleSheet *styles)
{
  for (guint i = 0; i < w42_stylesheet_size (styles); i++)
    {
      const W42Style *style = w42_stylesheet_get (styles, i);

      table_intern_font (tables, style->ch.family);
      table_intern_colour (tables, style->ch.color);
    }

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42Fmt *para = w42_ap_table_get (aps, block->ap);

      table_intern_font (tables, para->ch.family);
      table_intern_colour (tables, para->ch.color);

      for (guint r = 0; r < block->runs->len; r++)
        {
          const W42Run *run = &g_array_index (block->runs, W42Run, r);
          const W42Fmt *fmt = w42_ap_table_get (aps, run->ap);

          table_intern_font (tables, fmt->ch.family);
          table_intern_colour (tables, fmt->ch.color);
        }
    }
}

/* RTF is ASCII.  Anything above it goes out as \uN, with a question mark
 * behind it for readers too old to understand \u -- which is what the
 * specification asks for and what Word itself emits. */
static void
write_text (GString *out, const char *utf8, gsize len)
{
  const char *p = utf8;
  const char *end = utf8 + len;

  while (p < end)
    {
      gunichar c = g_utf8_get_char (p);

      switch (c)
        {
        case '\\': g_string_append (out, "\\\\"); break;
        case '{':  g_string_append (out, "\\{");  break;
        case '}':  g_string_append (out, "\\}");  break;
        case '\t': g_string_append (out, "\\tab "); break;
        case 0x00AD: g_string_append (out, "\\-"); break;   /* optional hyphen */
        case 0x2028: g_string_append (out, "\\line "); break; /* a line break */
        default:
          if (c < 0x80)
            {
              g_string_append_c (out, (char) c);
            }
          else if (c <= 0xFFFF)
            {
              /* \u takes a signed 16-bit value. */
              g_string_append_printf (out, "\\u%d?",
                                      c > 32767 ? (int) c - 65536 : (int) c);
            }
          else
            {
              /* Beyond the basic plane RTF wants a surrogate pair. */
              gunichar v = c - 0x10000;
              int hi = 0xD800 + (v >> 10);
              int lo = 0xDC00 + (v & 0x3FF);

              g_string_append_printf (out, "\\u%d?\\u%d?",
                                      hi > 32767 ? hi - 65536 : hi,
                                      lo > 32767 ? lo - 65536 : lo);
            }
          break;
        }

      p = g_utf8_next_char (p);
    }
}

static void
write_char_props (GString *out, const W42CharFmt *ch, RtfTables *tables)
{
  g_string_append_printf (out, "\\f%u", table_intern_font (tables, ch->family));
  g_string_append_printf (out, "\\fs%d", ch->size > 0 ? ch->size : 20);
  g_string_append_printf (out, "\\cf%u",
                          table_intern_colour (tables, ch->color) + 1);

  if (ch->bold)      g_string_append (out, "\\b");
  if (ch->italic)    g_string_append (out, "\\i");
  if (ch->underline) g_string_append (out, "\\ul");
  if (ch->strikeout) g_string_append (out, "\\strike");
  if (ch->overline)  g_string_append (out, "\\ol");
  if (ch->script > 0) g_string_append (out, "\\super");
  if (ch->script < 0) g_string_append (out, "\\sub");
  if (ch->smallcaps) g_string_append (out, "\\scaps");
  if (ch->allcaps)   g_string_append (out, "\\caps");
  if (ch->highlight) g_string_append_printf (out, "\\highlight%d", ch->highlight);
  if (ch->spacing)   g_string_append_printf (out, "\\expndtw%d", ch->spacing);
  if (ch->revision == 1) g_string_append (out, "\\revised\\revauth1");
  if (ch->revision == 2) g_string_append (out, "\\deleted\\revauth1");

  g_string_append_c (out, ' ');
}

/* The style's index in the stylesheet, which is what \s names. */
static int
style_index (W42StyleSheet *styles, const char *name)
{
  for (guint i = 0; i < w42_stylesheet_size (styles); i++)
    if (g_ascii_strcasecmp (w42_stylesheet_get (styles, i)->name,
                            name != NULL ? name : "Normal") == 0)
      return (int) i;

  return 0;
}

static void write_para_body (GString *out, const W42ParaFmt *pa);

static void
write_para_props (GString *out, const W42ParaFmt *pa, W42StyleSheet *styles)
{
  g_string_append (out, "\\pard\\plain");
  g_string_append_printf (out, "\\s%d", style_index (styles, pa->style));
  write_para_body (out, pa);
}

/* Alignment, indents and spacing: the part of a paragraph's formatting that
 * a stylesheet entry and a paragraph both carry. */
static void
write_para_body (GString *out, const W42ParaFmt *pa)
{
  switch (pa->align)
    {
    case W42_ALIGN_CENTER:  g_string_append (out, "\\qc"); break;
    case W42_ALIGN_RIGHT:   g_string_append (out, "\\qr"); break;
    case W42_ALIGN_JUSTIFY: g_string_append (out, "\\qj"); break;
    default:                g_string_append (out, "\\ql"); break;
    }
  if (pa->rtl) g_string_append (out, "\\rtlpar");

  if (pa->indent_left)  g_string_append_printf (out, "\\li%d", pa->indent_left);
  if (pa->indent_right) g_string_append_printf (out, "\\ri%d", pa->indent_right);
  if (pa->indent_first) g_string_append_printf (out, "\\fi%d", pa->indent_first);
  if (pa->page_break_before) g_string_append (out, "\\pagebb");
  if (pa->keep_next)     g_string_append (out, "\\keepn");
  if (pa->keep_together) g_string_append (out, "\\keep");
  g_string_append (out, pa->widow_control ? "\\widctlpar" : "\\nowidctlpar");

  /* Borders as Word wrote them: a side, its style and width; shading in
   * hundredths of a percent. */
  if (pa->border != 0)
    {
      int w = pa->border_width > 0 ? pa->border_width : 15;

      if (pa->border == W42_BORDER_BOX)
        g_string_append_printf (out, "\\box\\brdrs\\brdrw%d\\brsp20", w);
      else
        {
          if (pa->border & W42_BORDER_TOP)
            g_string_append_printf (out, "\\brdrt\\brdrs\\brdrw%d\\brsp20", w);
          if (pa->border & W42_BORDER_BOTTOM)
            g_string_append_printf (out, "\\brdrb\\brdrs\\brdrw%d\\brsp20", w);
          if (pa->border & W42_BORDER_LEFT)
            g_string_append_printf (out, "\\brdrl\\brdrs\\brdrw%d\\brsp20", w);
          if (pa->border & W42_BORDER_RIGHT)
            g_string_append_printf (out, "\\brdrr\\brdrs\\brdrw%d\\brsp20", w);
        }
    }
  if (pa->shading > 0)
    g_string_append_printf (out, "\\shading%d", pa->shading * 100);

  for (int i = 0; i < pa->n_tabs; i++)
    {
      switch (W42_TAB_KIND (pa->tab_kind[i]))
        {
        case W42_TAB_CENTER:  g_string_append (out, "\\tqc");   break;
        case W42_TAB_RIGHT:   g_string_append (out, "\\tqr");   break;
        case W42_TAB_DECIMAL: g_string_append (out, "\\tqdec"); break;
        default: break;
        }
      switch (W42_TAB_LEADER (pa->tab_kind[i]))
        {
        case W42_TAB_LEAD_DOT:  g_string_append (out, "\\tldot");  break;
        case W42_TAB_LEAD_DASH: g_string_append (out, "\\tlhyph"); break;
        case W42_TAB_LEAD_LINE: g_string_append (out, "\\tlul");   break;
        default: break;
        }
      g_string_append_printf (out, "\\tx%d", pa->tab_pos[i]);
    }
  if (pa->space_before) g_string_append_printf (out, "\\sb%d", pa->space_before);
  if (pa->space_after)  g_string_append_printf (out, "\\sa%d", pa->space_after);

  /* RTF expresses a multiple as a negative-going \slmult1 with \sl in twips
   * of a nominal 240-twip line, which is how Word writes 1.5 and double. */
  if (pa->line_spacing_pct > 100)
    g_string_append_printf (out, "\\sl%d\\slmult1",
                            (240 * pa->line_spacing_pct) / 100);
  else if (pa->line_spacing > 0)
    g_string_append_printf (out, "\\sl%d\\slmult0", pa->line_spacing);
}

/* A picture is a \pict group: the format, the size, and the bytes as hex.
 * PNG and JPEG go in as they are, which is what \pngblip and \jpegblip mean;
 * anything else is re-encoded as PNG, because those two are the only bitmap
 * formats every RTF reader agrees on. */
static void
write_pict (GString *out, W42ObjectTable *objects, W42ObjectIdx idx)
{
  const W42Object *object = w42_object_table_get (objects, idx);
  GBytes *data;
  const guint8 *bytes;
  gsize len;
  const char *blip;

  if (object == NULL)
    return;

  if (g_strcmp0 (object->format, "png") == 0)
    {
      data = g_bytes_ref (object->data);
      blip = "\\pngblip";
    }
  else if (g_strcmp0 (object->format, "jpeg") == 0)
    {
      data = g_bytes_ref (object->data);
      blip = "\\jpegblip";
    }
  else
    {
      data = w42_image_to_png (object->data);
      blip = "\\pngblip";
      if (data == NULL)
        return;
    }

  bytes = g_bytes_get_data (data, &len);

  g_string_append_printf (out,
    "{\\pict%s\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d\n",
    blip, object->pixel_w, object->pixel_h, object->width, object->height);

  for (gsize i = 0; i < len; i++)
    {
      static const char hex[] = "0123456789abcdef";

      g_string_append_c (out, hex[bytes[i] >> 4]);
      g_string_append_c (out, hex[bytes[i] & 15]);

      /* Word wraps the hex; readers do not care, but a file a person can
       * look at is worth a few newlines. */
      if ((i & 63) == 63)
        g_string_append_c (out, '\n');
    }

  g_string_append (out, "}\n");
  g_bytes_unref (data);
}

/* A header or footer: one paragraph in the Normal style, with the fields
 * written as Word writes them -- \chpgn for the page number, and \field
 * groups carrying the instruction and a cached result for the others. */
static void
write_page_text (GString *out, const char *dest, const W42PageText *slot,
                 W42StyleSheet *styles, RtfTables *tables)
{
  const W42Style *normal = w42_stylesheet_find (styles, "Normal");
  const char *p;

  if (slot == NULL || slot->text == NULL || *slot->text == '\0')
    return;

  g_string_append_printf (out, "{\\%s\\pard\\plain", dest);
  switch (slot->align)
    {
    case W42_ALIGN_CENTER: g_string_append (out, "\\qc"); break;
    case W42_ALIGN_RIGHT:  g_string_append (out, "\\qr"); break;
    default:               g_string_append (out, "\\ql"); break;
    }
  g_string_append_c (out, ' ');
  if (normal != NULL)
    write_char_props (out, &normal->ch, tables);

  for (p = slot->text; *p != '\0'; )
    {
      if (g_str_has_prefix (p, "{PAGE}"))
        {
          g_string_append (out, "\\chpgn ");
          p += 6;
        }
      else if (g_str_has_prefix (p, "{NUMPAGES}"))
        {
          g_string_append (out, "{\\field{\\*\\fldinst NUMPAGES }{\\fldrslt 1}}");
          p += 10;
        }
      else if (g_str_has_prefix (p, "{DATE}"))
        {
          g_string_append (out, "{\\field{\\*\\fldinst DATE }{\\fldrslt }}");
          p += 6;
        }
      else
        {
          const char *next = g_utf8_next_char (p);
          write_text (out, p, (gsize) (next - p));
          p = next;
        }
    }

  g_string_append (out, "\\par}\n");
}

gboolean
w42_rtf_save (W42PieceTable      *pt,
              const W42PageSetup *page,
              GFile              *file,
              GError            **error)
{
  GString *out;
  GPtrArray *blocks;
  W42ApTable *aps;
  W42StyleSheet *styles;
  RtfTables tables;
  gboolean ok;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  aps = w42_pt_ap_table (pt);
  styles = w42_pt_stylesheet (pt);
  blocks = w42_pt_snapshot_blocks (pt);

  tables.fonts = g_ptr_array_new ();
  tables.colours = g_array_new (FALSE, FALSE, sizeof (guint32));
  collect_tables (blocks, aps, &tables, styles);

  out = g_string_new (NULL);
  g_string_append (out, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\deflang1033\n");
  {
    /* What the document says about itself. */
    const W42DocInfo *info = w42_pt_get_info (pt);
    static const struct { const char *word; gsize offset; } fields[] = {
      { "title",    G_STRUCT_OFFSET (W42DocInfo, title) },
      { "subject",  G_STRUCT_OFFSET (W42DocInfo, subject) },
      { "author",   G_STRUCT_OFFSET (W42DocInfo, author) },
      { "keywords", G_STRUCT_OFFSET (W42DocInfo, keywords) },
      { "doccomm",  G_STRUCT_OFFSET (W42DocInfo, comments) },
    };
    gboolean any = FALSE;

    for (guint i = 0; i < G_N_ELEMENTS (fields); i++)
      {
        const char *value = G_STRUCT_MEMBER (const char *, info, fields[i].offset);

        if (value == NULL)
          continue;
        if (!any)
          g_string_append (out, "{\\info");
        any = TRUE;
        g_string_append_printf (out, "{\\%s ", fields[i].word);
        write_text (out, value, strlen (value));
        g_string_append_c (out, '}');
      }
    if (any)
      g_string_append (out, "}\n");
  }

  g_string_append (out, "{\\fonttbl");
  for (guint i = 0; i < tables.fonts->len; i++)
    {
      g_string_append_printf (out, "{\\f%u\\fnil\\fcharset0 ", i);
      rtf_plain (out, g_ptr_array_index (tables.fonts, i));
      g_string_append (out, ";}");
    }
  g_string_append (out, "}\n");

  g_string_append (out, "{\\colortbl ;");
  for (guint i = 0; i < tables.colours->len; i++)
    {
      guint32 c = g_array_index (tables.colours, guint32, i);
      g_string_append_printf (out, "\\red%u\\green%u\\blue%u;",
                              (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
    }
  g_string_append (out, "}\n");

  /* The stylesheet.  Word wants Normal at \s0 and reads the names to pick
   * the headings out, so both are as Word would have written them. */
  g_string_append (out, "{\\stylesheet");
  for (guint i = 0; i < w42_stylesheet_size (styles); i++)
    {
      const W42Style *style = w42_stylesheet_get (styles, i);

      g_string_append_printf (out, "{\\s%u", i);
      write_para_body (out, &style->pa);
      if (style->outline > 0)
        g_string_append_printf (out, "\\outlinelevel%d", style->outline - 1);
      g_string_append_c (out, ' ');
      write_char_props (out, &style->ch, &tables);
      g_string_append (out, "\\sbasedon0\\snext0 ");
      rtf_plain (out, style->name);
      g_string_append (out, ";}");
    }
  g_string_append (out, "}\n");

  g_string_append (out, "{\\*\\generator Word42;}\n");

  if (w42_stylesheet_get_number_headings (styles))
    g_string_append (out, "{\\*\\wfnumhead1}\n");

  if (page != NULL)
    g_string_append_printf (out,
      "\\paperw%d\\paperh%d\\margl%d\\margr%d\\margt%d\\margb%d\n",
      page->width, page->height, page->margin_left, page->margin_right,
      page->margin_top, page->margin_bottom);
  if (w42_page_columns (page) > 1)
    g_string_append_printf (out, "\\sectd\\cols%d\\colsx%d\n",
                            w42_page_columns (page), w42_page_column_gap (page));

  /* \titlepg and \facingp say that a title page and the left-hand pages
   * have their own, which \headerf and \headerl carry. */
  if (w42_pt_get_title_page (pt))
    g_string_append (out, "\\titlepg");
  if (w42_pt_get_facing_pages (pt))
    g_string_append (out, "\\facingp");
  write_page_text (out, "header", w42_pt_get_header (pt), styles, &tables);
  write_page_text (out, "footer", w42_pt_get_footer (pt), styles, &tables);
  if (w42_pt_get_title_page (pt))
    {
      write_page_text (out, "headerf", w42_pt_get_header_kind (pt, W42_PAGE_TEXT_FIRST), styles, &tables);
      write_page_text (out, "footerf", w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_FIRST), styles, &tables);
    }
  if (w42_pt_get_facing_pages (pt))
    {
      write_page_text (out, "headerl", w42_pt_get_header_kind (pt, W42_PAGE_TEXT_EVEN), styles, &tables);
      write_page_text (out, "footerl", w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_EVEN), styles, &tables);
    }

  int list_n = 0;
  int level_n[9] = { 0 };
  W42ListKind level_kind[9] = { W42_LIST_NONE };
  guint annotation_n = 0;

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42Fmt *para = w42_ap_table_get (aps, block->ap);

      if (block->note >= 0)
        continue;         /* written inside {\\footnote} at its reference */
      const W42Block *prev = (b > 0) ? g_ptr_array_index (blocks, b - 1) : NULL;
      const W42Block *next = (b + 1 < blocks->len) ? g_ptr_array_index (blocks, b + 1) : NULL;
      gboolean in_table = block->table >= 0;
      gboolean row_start = in_table && (prev == NULL || prev->table != block->table ||
                                        prev->row != block->row);
      gboolean cell_end = in_table && (next == NULL || next->table != block->table ||
                                       next->row != block->row || next->col != block->col);
      gboolean row_end = in_table && (next == NULL || next->table != block->table ||
                                      next->row != block->row);

      /* A row opens with its definition: the right edge of every cell in
       * twips from the left margin, which is how RTF says how wide the
       * columns are. */
      if (row_start)
        {
          const W42TableProps *props = w42_pt_table_props (pt, block->table);
          int n_cols = props != NULL ? props->n_cols : 1;
          int text_w = page != NULL ? page->width - page->margin_left - page->margin_right : 9360;
          int edge = 0;

          int *owner = g_new (int, n_cols);

          /* Merged cells: \clmgf on the first column, \clmrg on the ones
           * it covers, each still with its \cellx. */
          for (int c = 0; c < n_cols; c++)
            owner[c] = -1;
          for (guint k = b; k < blocks->len; k++)
            {
              const W42Block *rb = g_ptr_array_index (blocks, k);
              int col0, span;

              if (rb->table != block->table || rb->row != block->row)
                break;
              col0 = CLAMP (rb->col, 0, n_cols - 1);
              span = CLAMP (rb->span, 1, n_cols - col0);
              for (int c = col0; c < col0 + span; c++)
                owner[c] = col0;
              if (span > 1)
                owner[col0] = -2 - col0;   /* marks the first of a merge */
            }

          g_string_append (out, "\\trowd\\trgaph80");
          {
            int least = w42_pt_table_get_row_height (pt, block->table, block->row);

            if (least > 0)
              g_string_append_printf (out, "\\trrh%d", least);
            if (props != NULL && block->row < props->header_rows)
              g_string_append (out, "\\trhdr");
          }
          for (int c = 0; c < n_cols; c++)
            {
              int w = (props != NULL && c < (int) props->widths->len)
                        ? g_array_index (props->widths, int, c) : 0;
              edge += (w > 0) ? w : text_w / n_cols;
              {
                /* The owning cell's own sides, or the table's setting. */
                int sides = (props != NULL && !props->borders) ? 0 : W42_BORDER_BOX;
                static const char letters[4] = { 't', 'l', 'b', 'r' };
                static const int bits[4] = { W42_BORDER_TOP, W42_BORDER_LEFT, W42_BORDER_BOTTOM, W42_BORDER_RIGHT };
                int oc = owner[c] <= -2 ? -2 - owner[c] : owner[c] >= 0 ? owner[c] : c;

                for (guint k = b; k < blocks->len; k++)
                  {
                    const W42Block *rb = g_ptr_array_index (blocks, k);
                    const W42ParaFmt *cpa;

                    if (rb->table != block->table || rb->row != block->row)
                      break;
                    if (rb->col != oc)
                      continue;
                    cpa = &w42_ap_table_get (aps, rb->cell_ap)->pa;
                    if (cpa->border & W42_BORDER_CELL_SET)
                      sides = cpa->border & W42_BORDER_BOX;
                    break;
                  }
                for (int k = 0; k < 4; k++)
                  g_string_append_printf (out, "\\clbrdr%c%s", letters[k],
                                          (sides & bits[k]) ? "\\brdrs\\brdrw10" : "\\brdrnone");
              }
              if (owner[c] <= -2)
                g_string_append (out, "\\clmgf");
              else if (owner[c] >= 0 && owner[c] != c)
                g_string_append (out, "\\clmrg");
              g_string_append_printf (out, "\\cellx%d", edge);
            }
          g_string_append_c (out, '\n');
          g_free (owner);
        }

      /* A section break: Word's \sect ends the section, \sectd starts
       * the next with its own columns. */
      if (para->pa.section_break)
        g_string_append_printf (out, "\\sect\\sectd\\sbkpage\\cols%d\\colsx%d\n",
                                MAX (para->pa.columns, 1),
                                para->pa.column_gap > 0 ? para->pa.column_gap : 720);
      write_para_props (out, &para->pa, styles);
      if (in_table)
        g_string_append (out, "\\intbl");

      /* Lists the way Word 6 wrote them: a \pn group that says what kind of
       * list the paragraph is in, and a \pntext group holding the marker
       * as plain text for readers that do not know \pn. */
      if (w42_list_is_numbered (para->pa.list))
        {
          int lv = MIN (para->pa.list_level, 8);

          if (para->pa.list_start > 0)
            level_n[lv] = para->pa.list_start;
          else if (para->pa.list != level_kind[lv])
            level_n[lv] = 1;
          else
            level_n[lv]++;
          level_kind[lv] = para->pa.list;
          for (int deeper = lv + 1; deeper < 9; deeper++)
            {
              level_n[deeper] = 0;
              level_kind[deeper] = W42_LIST_NONE;
            }
          list_n = level_n[lv];
        }
      else if (para->pa.list == W42_LIST_NONE)
        {
          list_n = 0;
          memset (level_n, 0, sizeof level_n);
          memset (level_kind, 0, sizeof level_kind);
        }

      if (w42_list_is_bullet (para->pa.list))
        {
          char marker[16];

          w42_list_marker (para->pa.list, 1, marker, sizeof marker);
          if (para->pa.list_level > 0)
            g_string_append_printf (out, "{\\*\\pn\\pnlvl%d\\pnindent360{\\pntxtb ", MIN (para->pa.list_level, 8) + 1);
          else
            g_string_append (out, "{\\*\\pn\\pnlvlblt\\pnindent360{\\pntxtb ");
          write_text (out, marker, strlen (marker));
          g_string_append (out, "}}{\\pntext ");
          write_text (out, marker, strlen (marker));
          g_string_append (out, "\\tab}");
        }
      else if (w42_list_is_numbered (para->pa.list))
        {
          static const char *format[W42_LIST_KINDS] = {
            [W42_LIST_NUMBER] = "pndec", [W42_LIST_LOWER_LETTER] = "pnlcltr",
            [W42_LIST_UPPER_LETTER] = "pnucltr", [W42_LIST_LOWER_ROMAN] = "pnlcrm",
            [W42_LIST_UPPER_ROMAN] = "pnucrm"
          };
          char marker[16];

          w42_list_marker (para->pa.list, list_n, marker, sizeof marker);
          if (para->pa.list_level > 0)
            g_string_append_printf (out, "{\\*\\pn\\pnlvl%d", MIN (para->pa.list_level, 8) + 1);
          else
            g_string_append (out, "{\\*\\pn\\pnlvlbody");
          g_string_append_printf (out, "\\%s\\pnstart%d"
                                       "\\pnindent360{\\pntxta.}}"
                                       "{\\pntext %s\\tab}",
                                  format[para->pa.list],
                                  para->pa.list_start > 0 ? para->pa.list_start : 1,
                                  marker);
        }

      if (block->runs->len == 0)
        {
          /* An empty paragraph still carries the mark's own character
           * formatting, which decides how tall the blank line is. */
          write_char_props (out, &para->ch, &tables);
        }

      for (guint r = 0; r < block->runs->len; r++)
        {
          const W42Run *run = &g_array_index (block->runs, W42Run, r);
          const W42Fmt *fmt = w42_ap_table_get (aps, run->ap);

          if (run->object != W42_OBJECT_NONE)
            {
              write_pict (out, w42_pt_object_table (pt), run->object);
              continue;
            }

          if (run->footnote > 0)
            {
              /* The mark, then the note's paragraphs as Word writes them. */
              gboolean first = TRUE;

              g_string_append (out, run->endnote
                               ? "{\\super\\chftn}{\\footnote\\ftnalt "
                               : "{\\super\\chftn}{\\footnote ");
              for (guint nb = 0; nb < blocks->len; nb++)
                {
                  const W42Block *note = g_ptr_array_index (blocks, nb);
                  const W42Fmt *npara;

                  if (note->note != run->footnote_id)
                    continue;
                  npara = w42_ap_table_get (aps, note->ap);
                  if (!first)
                    g_string_append (out, "\\par\n");
                  write_para_props (out, &npara->pa, styles);
                  if (first)
                    g_string_append (out, "{\\super\\chftn }");
                  first = FALSE;
                  if (note->runs->len == 0)
                    write_char_props (out, &npara->ch, &tables);
                  for (guint nr = 0; nr < note->runs->len; nr++)
                    {
                      const W42Run *run2 = &g_array_index (note->runs, W42Run, nr);
                      const W42Fmt *fmt2 = w42_ap_table_get (aps, run2->ap);

                      if (run2->object != W42_OBJECT_NONE || run2->footnote > 0)
                        continue;
                      g_string_append_c (out, '{');
                      write_char_props (out, &fmt2->ch, &tables);
                      write_text (out, note->text->str + run2->byte_offset, run2->n_bytes);
                      g_string_append_c (out, '}');
                    }
                }
              g_string_append (out, "}");
              continue;
            }

          /* A bookmark opens before the first run that carries it and
           * closes after the last. */
          {
            const char *prev_bm = r > 0
              ? w42_ap_table_get (aps, g_array_index (block->runs, W42Run, r - 1).ap)->ch.bookmark
              : NULL;
            if (fmt->ch.bookmark != NULL && fmt->ch.bookmark != prev_bm)
              {
                g_string_append (out, "{\\*\\bkmkstart ");
                write_text (out, fmt->ch.bookmark, strlen (fmt->ch.bookmark));
                g_string_append_c (out, '}');
              }
          }

          /* An annotation opens before the first run that carries it and
           * its text follows the last, as Word wrote them. */
          {
            const char *prev_cm = r > 0
              ? w42_ap_table_get (aps, g_array_index (block->runs, W42Run, r - 1).ap)->ch.comment
              : NULL;
            if (fmt->ch.comment != NULL && fmt->ch.comment != prev_cm)
              g_string_append_printf (out, "{\\*\\atrfstart %u}", ++annotation_n);
          }

          /* A link is a HYPERLINK field with the text as its result; a
           * field is its code with the cached result. */
          if (fmt->ch.link != NULL)
            {
              g_string_append (out, "{\\field{\\*\\fldinst{HYPERLINK \"");
              write_text (out, fmt->ch.link, strlen (fmt->ch.link));
              g_string_append (out, "\"}}{\\fldrslt ");
            }
          else if (fmt->ch.field != NULL)
            g_string_append_printf (out, "{\\field{\\*\\fldinst %s }{\\fldrslt ", fmt->ch.field);

          /* Each run reopens a group, so the properties it sets fall away
           * again at its end and cannot leak into the next one. */
          g_string_append_c (out, '{');
          write_char_props (out, &fmt->ch, &tables);
          write_text (out, block->text->str + run->byte_offset, run->n_bytes);
          g_string_append_c (out, '}');

          if (fmt->ch.link != NULL || fmt->ch.field != NULL)
            g_string_append (out, "}}");

          {
            const char *next_cm = r + 1 < block->runs->len
              ? w42_ap_table_get (aps, g_array_index (block->runs, W42Run, r + 1).ap)->ch.comment
              : NULL;
            if (fmt->ch.comment != NULL && fmt->ch.comment != next_cm)
              {
                g_string_append_printf (out, "{\\*\\atrfend %u}{\\*\\atnid w42}{\\v\\chatn}"
                                        "{\\*\\annotation{\\*\\atnref %u}\\pard\\plain ",
                                        annotation_n, annotation_n);
                write_text (out, fmt->ch.comment, strlen (fmt->ch.comment));
                g_string_append_c (out, '}');
              }
          }

          {
            const char *next_bm = r + 1 < block->runs->len
              ? w42_ap_table_get (aps, g_array_index (block->runs, W42Run, r + 1).ap)->ch.bookmark
              : NULL;
            if (fmt->ch.bookmark != NULL && fmt->ch.bookmark != next_bm)
              {
                g_string_append (out, "{\\*\\bkmkend ");
                write_text (out, fmt->ch.bookmark, strlen (fmt->ch.bookmark));
                g_string_append_c (out, '}');
              }
          }
        }

      if (cell_end)
        {
          g_string_append (out, "\\cell\n");
          /* The columns a merged cell covers still need their empty
           * cells, as Word writes them. */
          for (int extra = 1; extra < block->span; extra++)
            g_string_append (out, "\\pard\\intbl\\cell\n");
        }
      else
        g_string_append (out, "\\par\n");

      if (row_end)
        g_string_append (out, "\\row\n");
    }

  g_string_append (out, "}\n");

  ok = g_file_replace_contents (file, out->str, out->len, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);

  g_string_free (out, TRUE);
  g_ptr_array_free (tables.fonts, TRUE);
  g_array_free (tables.colours, TRUE);
  g_ptr_array_free (blocks, TRUE);

  return ok;
}

/* ====================================================================== */
/* Reading                                                                 */
/* ====================================================================== */

/* The reader understands the subset of RTF that carries formatted prose and
 * skips the rest.  Anything it does not know is discarded rather than
 * guessed at: a destination it cannot read is dropped whole, which is what
 * the specification asks unknown readers to do. */

typedef struct {
  W42CharFmt ch;
  W42ParaFmt pa;
  int        uc;        /* how many characters follow a \u and must be eaten */
  gboolean   skip;      /* inside a destination we do not understand */
  gboolean   pntxtb;    /* inside {\pntxtb ...}: the bullet character */
  gboolean   intbl;     /* the paragraph is a table cell's */
  guint8     tab_kind;  /* \\tqr and friends, for the \\tx that follows */
  guint8     tab_leader; /* \\tldot and friends, likewise */
  guint8     border_side; /* the side the \\brdrw that follows belongs to */
} RtfState;

/* What a list from the file's list table looks like, level by level. */
typedef struct {
  guint8 kind[9];        /* W42ListKind per level */
} ListShape;

typedef struct {
  W42PieceTable *pt;
  W42PageSetup  *page;

  GPtrArray     *fonts;      /* char*, by \f index */
  GArray        *colours;    /* guint32, by \cf index minus one */

  RtfState       state;
  GArray        *stack;      /* RtfState */

  GString       *pending;    /* text accumulated under one set of properties */
  W42ApIdx       pending_ap;
  gboolean       have_pending;
  gboolean       sect_pending;  /* a \\sect was read; the next paragraph starts it */
  gboolean       cell_border;   /* inside a \\clbrdr: the next \\brdr word is its style */
  guint          info_depth;    /* the group depth of an \\info group */
  const char    *info_field;    /* the field being read there, or NULL */
  GString       *info_text;
  char          *info_keep[5];  /* title, subject, author, keywords, comments */
  int            cell_side;     /* which side that \\clbrdr names */
  int            clsides_pending; /* the next \\cellx's ruled sides */
  GArray        *clsides;       /* int, per \\cellx */
  gboolean       no_borders;    /* the row's cells have \\brdrnone */
  int            sect_cols;
  int            sect_gap;

  gsize          pos;        /* where the next thing goes in the document */
  gboolean       first_para; /* the first \par closes the paragraph that the
                              * empty document already has */

  /* Collecting the font and colour tables. */
  int            dest;       /* which destination is being collected */
  int            font_index;
  GString       *font_name;
  int            red, green, blue;

  /* Collecting the stylesheet. */
  guint          style_depth;
  int            style_index;    /* the \sN of the entry being read */
  GString       *style_name;
  GHashTable    *style_names;    /* \s index -> interned name */
  GHashTable    *list_shapes;    /* \ls number -> ListShape, from the tables */
  int            cur_ls;         /* the paragraph's \ls, or -1 */
  int            cur_ilvl;       /* and its \ilvl */
  int            style_outline;  /* \outlinelevel + 1 in a stylesheet entry */

  /* Building a table.  Word writes a row as its definition (\trowd and a
   * \cellx per column), then each cell's paragraphs ending in \cell, then
   * \row.  The table itself is implied: it starts at the first \intbl
   * paragraph and ends at the first paragraph without one. */
  int            table;          /* the open table's id, or -1 */
  int            table_row;
  int            table_col;
  gboolean       in_cell;        /* a CELL mark has been put for this cell */
  gboolean       table_before_block;  /* the table went in ahead of an empty
                                       * paragraph that will follow it */
  GArray        *cellx;          /* int: the \cellx edges of the current row */
  int            trrh;           /* \trrh: the row's least height, twips */
  gboolean       trhdr;          /* \trhdr: the row repeats on every page */
  GArray        *clflags;        /* int, per \cellx: 1 \clmgf, 2 \clmrg */
  int            clpending;      /* the flag for the next \cellx */
  gsize          last_cell_pos;  /* the CELL mark most recently made */
  int            last_cell_span;

  /* Collecting a header or footer, and the fields inside it. */
  guint          hf_depth;
  GString       *hf_text;
  const char    *link;           /* the HYPERLINK whose result is being read */
  const char    *bookmark;       /* the bookmark the text is inside */
  GString       *bkmk_name;      /* a \\bkmkstart or \\bkmkend's name */
  int            bkmk_kind;      /* 1 start, 2 end, 0 neither */
  guint          bkmk_depth;
  GHashTable    *atrf;           /* annotation id -> range start (gsize) */
  GHashTable    *atrf_end;       /* annotation id -> range end */
  GString       *atn_text;       /* the annotation being read */
  GString       *atn_ref;        /* its id, as text */
  int            atn_kind;       /* 1 atrfstart, 2 atrfend, 3 atnref, 4 annotation, 0 none */
  guint          atn_depth;
  guint          atn_group_depth; /* the {\*\annotation group's own depth */
  guint          note_depth;     /* the {\\footnote group's depth */
  gsize          note_return;    /* where the main text goes on afterwards */
  gboolean       in_note;
  W42Align       hf_align;
  W42PageTextKind hf_kind;   /* which of the three is being read */
  gboolean       in_fldinst;     /* inside {\*\fldinst ...}: the field's code */
  guint          fldinst_depth;
  GString       *fldinst;
  gboolean       in_fldrslt;     /* inside {\fldrslt ...}: the cached result */
  const char    *field_code;     /* the field the result belongs to, or NULL */
  gsize          field_start;    /* where that result began */
  guint          fldrslt_depth;

  /* Collecting a picture. */
  guint          pict_depth;     /* stack depth of the \pict group */
  const char    *pict_format;    /* "png", "jpeg", or NULL for unknown */
  int            pict_wgoal;     /* twips, from \picwgoal */
  int            pict_hgoal;
  GString       *pict_hex;
} RtfReader;

enum { DEST_NONE = 0, DEST_FONTTBL, DEST_COLORTBL, DEST_PICT, DEST_STYLESHEET,
       DEST_HEADER, DEST_FOOTER, DEST_INFO };

static W42ApIdx
reader_ap (RtfReader *r)
{
  W42Fmt fmt;

  w42_fmt_init_default (&fmt);
  fmt.ch = r->state.ch;
  fmt.pa = r->state.pa;
  if (r->link != NULL)
    fmt.ch.link = r->link;
  if (r->bookmark != NULL)
    fmt.ch.bookmark = r->bookmark;

  return w42_ap_table_intern (w42_pt_ap_table (r->pt), &fmt);
}

static void
flush_text (RtfReader *r)
{
  if (!r->have_pending || r->pending->len == 0)
    {
      g_string_truncate (r->pending, 0);
      r->have_pending = FALSE;
      return;
    }

  w42_pt_insert_text (r->pt, r->pos, r->pending->str, r->pending_ap);
  r->pos += g_utf8_strlen (r->pending->str, -1);

  g_string_truncate (r->pending, 0);
  r->have_pending = FALSE;
}

/* Ordinary text is about to go into the document: if it belongs to a table
 * cell the cell must be open, and if it does not and a table is open the
 * table must be closed.  Both happen here, so that the paragraph marks come
 * out in the right order without the reader having to look ahead. */
static void
table_sync (RtfReader *r)
{
  if (r->state.intbl && !r->in_cell)
    {
      if (r->table < 0)
        {
          int n_cols = MAX ((int) r->cellx->len, 1);
          int *widths = g_new0 (int, n_cols);
          int prev = 0;

          for (int c = 0; c < (int) r->cellx->len; c++)
            {
              widths[c] = g_array_index (r->cellx, int, c) - prev;
              prev = g_array_index (r->cellx, int, c);
            }

          flush_text (r);

          /* The \par before the table opened an empty paragraph that is now
           * the last thing in the document.  The table goes in ahead of it,
           * and it becomes the paragraph that follows the table; otherwise
           * every table would arrive with a blank line above it. */
          r->table_before_block = FALSE;
          if (r->pos >= 2 && r->pos == w42_pt_length (r->pt))
            {
              /* A paragraph mark is the one thing whose text is a newline. */
              char *tail = w42_pt_get_text (r->pt, r->pos - 1, 1);
              r->table_before_block = (tail != NULL && *tail == '\n');
              g_free (tail);
            }

          if (r->table_before_block)
            r->pos -= 1;

          r->table = w42_pt_insert_table_start (r->pt, r->pos, n_cols, widths);

          w42_pt_table_set_borders (r->pt, r->table, !r->no_borders);
          r->pos += 1;
          r->table_row = 0;
          r->table_col = 0;
          g_free (widths);
        }

      flush_text (r);

      /* \clmrg: this column belongs to the cell before it, which grows
       * to cover it; no cell of its own is made. */
      /* More cells than the row definition has, or rows than the marks
       * can hold: dropped, as the other readers drop them. */
      if (r->table_col >= MAX ((int) r->cellx->len, 1) || r->table_row > 4095)
        {
          r->in_cell = TRUE;
          return;
        }
      if (r->table_col < (int) r->clflags->len &&
          g_array_index (r->clflags, int, r->table_col) == 2 &&
          r->last_cell_pos != (gsize) -1)
        {
          r->last_cell_span = MIN (r->last_cell_span + 1, 1023);
          w42_pt_set_cell_span (r->pt, r->last_cell_pos, r->last_cell_span);
          r->in_cell = TRUE;
          return;
        }

      w42_pt_insert_cell (r->pt, r->pos, r->table, r->table_row, r->table_col,
                          reader_ap (r));
      if (r->table_col < (int) r->clsides->len)
        {
          int sides = g_array_index (r->clsides, int, r->table_col);

          if (sides != (r->no_borders ? 0 : W42_BORDER_BOX))
            w42_pt_cell_set_borders_at (r->pt, r->pos, sides);
        }
      if (r->table_col == 0)
        {
          if (r->trrh > 0)
            w42_pt_table_set_row_height (r->pt, r->table, r->table_row, r->trrh);
          if (r->trhdr)
            w42_pt_table_set_header_rows (r->pt, r->table, r->table_row + 1);
        }
      r->last_cell_pos = r->pos;
      r->last_cell_span = 1;
      r->pos += 2;
      r->in_cell = TRUE;
    }
  else if (!r->state.intbl && r->table >= 0)
    {
      flush_text (r);

      if (r->table_before_block)
        {
          /* The paragraph the table went in ahead of is just past the last
           * cell; only the ENDTABLE mark is needed, and the text that
           * follows goes into that paragraph. */
          w42_pt_insert_table_end_only (r->pt, r->pos);
          r->pos += 2;      /* past ENDTABLE and the waiting paragraph mark */
        }
      else
        {
          w42_pt_insert_table_end (r->pt, r->pos, reader_ap (r));
          r->pos += 2;
        }

      r->table = -1;
      r->in_cell = FALSE;
      r->table_before_block = FALSE;
    }
}

static void
append_char (RtfReader *r, gunichar c)
{
  if (r->dest == DEST_INFO)
    {
      if (r->info_field != NULL)
        g_string_append_unichar (r->info_text, c);
      return;
    }
  char utf8[8];
  int n;

  /* A field's instruction is collected to be read; its cached result is
   * ordinary text in the body, and dropped in a header, where the field is
   * re-evaluated on every page instead. */
  if (r->bkmk_kind != 0)
    {
      g_string_append_unichar (r->bkmk_name, c);
      return;
    }
  if (r->atn_kind == 1 || r->atn_kind == 2 || r->atn_kind == 3)
    {
      g_string_append_unichar (r->atn_ref, c);
      return;
    }
  if (r->atn_kind == 4)
    {
      g_string_append_unichar (r->atn_text, c);
      return;
    }

  if (r->in_fldinst)
    {
      g_string_append_unichar (r->fldinst, c);
      return;
    }

  if (r->dest == DEST_HEADER || r->dest == DEST_FOOTER)
    {
      if (!r->in_fldrslt)
        g_string_append_unichar (r->hf_text, c);
      return;
    }

  /* The bullet character in a \pntxtb group says which bullet it is. */
  if (r->state.pntxtb)
    {
      guint8 kind = c == 'o' ? W42_LIST_BULLET_CIRCLE
                  : (c == 0xA7 || c == 0x25AA || c == 0x25A0) ? W42_LIST_BULLET_SQUARE
                  : (c == '-' || c == 0x2013 || c == 0x2014) ? W42_LIST_BULLET_DASH
                  : W42_LIST_BULLET;

      /* The group is inside the \pn group inside the paragraph: the
       * paragraph's state is two down the stack. */
      for (guint i = 1; i <= 2 && i <= r->stack->len; i++)
        {
          RtfState *outer = &g_array_index (r->stack, RtfState, r->stack->len - i);
          if (outer->pa.list != W42_LIST_NONE)
            outer->pa.list = kind;
        }
      r->state.pntxtb = FALSE;
      return;
    }

  if (r->state.skip || r->dest != DEST_NONE)
    return;

  table_sync (r);

  /* A change of character formatting starts a new run. */
  if (r->have_pending && r->pending_ap != reader_ap (r))
    flush_text (r);

  if (!r->have_pending)
    {
      r->pending_ap = reader_ap (r);
      r->have_pending = TRUE;
    }

  n = g_unichar_to_utf8 (c, utf8);
  g_string_append_len (r->pending, utf8, n);
}

/* Everything a paragraph mark carries, the style name included: without
 * W42_PARA_STYLE a heading read from a file keeps its font and loses its
 * name, and with it its section number. */
#define PARA_MASK W42_PARA_ALL

/* In RTF a paragraph's properties are the ones in force when its \par is
 * reached, and they belong to the paragraph being closed.  In word42 they
 * live on the mark that *starts* a paragraph, so \par has to reach backwards
 * and set them on the block already open before opening the next one.  Doing
 * it the other way round -- the obvious way -- puts every paragraph's
 * formatting on the paragraph after it. */
static void
end_paragraph (RtfReader *r)
{
  W42Fmt fmt;
  W42ApIdx ap;

  table_sync (r);
  flush_text (r);

  w42_pt_apply_para_fmt (r->pt, r->pos, 0, PARA_MASK, &r->state.pa);

  w42_fmt_init_default (&fmt);
  fmt.ch = r->state.ch;
  fmt.pa = r->state.pa;
  ap = w42_ap_table_intern (w42_pt_ap_table (r->pt), &fmt);

  w42_pt_insert_block (r->pt, r->pos, ap);
  r->pos += 1;
  r->first_para = FALSE;
  r->state.pa.list = W42_LIST_NONE;
  r->state.pa.list_start = 0;
}

/* A colour from the file's colour table, or black. */
static guint32
colour_at (RtfReader *r, int index)
{
  if (index >= 0 && (guint) index < r->colours->len)
    return g_array_index (r->colours, guint32, index);
  return 0;
}

/* Which of Word's sixteen highlight colours a colour is nearest to.  Zero
 * means none, so a white or unset background stays unhighlighted. */
static int
nearest_highlight (RtfReader *r, int index)
{
  guint32 want = colour_at (r, index);
  int best = 0;
  long best_away = 0;

  if (index <= 0 || want == 0xFFFFFF)
    return 0;

  for (int i = 1; i <= 16; i++)
    {
      guint32 c = w42_highlight_rgb (i);
      long dr = (long) ((c >> 16) & 0xFF) - (long) ((want >> 16) & 0xFF);
      long dg = (long) ((c >> 8) & 0xFF) - (long) ((want >> 8) & 0xFF);
      long db = (long) (c & 0xFF) - (long) (want & 0xFF);
      long away = dr * dr + dg * dg + db * db;

      if (best == 0 || away < best_away)
        {
          best = i;
          best_away = away;
        }
    }
  return best;
}

/* A stylesheet entry has reached its semicolon: the state holds the entry's
 * formatting and style_name its name.  Word names its headings "heading 1"
 * and so on, which is how the outline level is recovered. */
static void
finish_style (RtfReader *r)
{
  W42Style style;
  char *name = g_strstrip (g_strdup (r->style_name->str));

  if (r->style_index < 0)
    *name = '\0';                      /* a \cs or \ds entry: skipped */
  if (*name != '\0')
    {
      memset (&style, 0, sizeof style);
      style.name = g_intern_string (name);
      style.ch = r->state.ch;
      style.pa = r->state.pa;
      style.pa.style = style.name;

      if (g_ascii_strncasecmp (name, "heading ", 8) == 0)
        style.outline = CLAMP (atoi (name + 8), 0, 9);
      else if (r->style_outline > 0 &&
               w42_stylesheet_find (w42_pt_stylesheet (r->pt), "Heading 1") != NULL)
        {
          /* A heading under another name: give it ours, so that the
           * outline, the table of contents and heading numbering all
           * find it. */
          char *heading = g_strdup_printf ("Heading %d", r->style_outline);

          style.name = g_intern_string (heading);
          style.pa.style = style.name;
          style.outline = r->style_outline;
          g_free (heading);
        }

      w42_stylesheet_set (w42_pt_stylesheet (r->pt), &style);
      g_hash_table_insert (r->style_names, GINT_TO_POINTER (r->style_index),
                           (gpointer) style.name);
    }

  g_free (name);
  g_string_truncate (r->style_name, 0);
  r->style_outline = 0;
}

/* The \pict group has closed: turn the hex back into bytes and put the
 * picture where the text has got to. */
static void
finish_pict (RtfReader *r)
{
  gsize n = r->pict_hex->len / 2;
  guint8 *bytes;
  GBytes *data;
  int pw = 0, ph = 0;
  const char *format = NULL;
  int width, height;
  W42ObjectIdx idx;

  r->dest = DEST_NONE;

  if (n == 0 || r->state.skip)
    return;

  bytes = g_malloc (n);
  for (gsize i = 0; i < n; i++)
    {
      int hi = g_ascii_xdigit_value (r->pict_hex->str[2 * i]);
      int lo = g_ascii_xdigit_value (r->pict_hex->str[2 * i + 1]);
      bytes[i] = (guint8) ((hi << 4) | lo);
    }
  data = g_bytes_new_take (bytes, n);

  /* Metafiles and the other formats gdk-pixbuf has no loader for fail the
   * probe and are dropped, which is the honest thing to do with them. */
  if (!w42_image_probe (data, &pw, &ph, &format))
    {
      g_bytes_unref (data);
      return;
    }

  width  = r->pict_wgoal > 0 ? r->pict_wgoal : pw * 15;
  height = r->pict_hgoal > 0 ? r->pict_hgoal : ph * 15;

  idx = w42_object_table_add (w42_pt_object_table (r->pt), data, format,
                              pw, ph, width, height);
  g_bytes_unref (data);

  flush_text (r);
  w42_pt_insert_object (r->pt, r->pos, idx, reader_ap (r));
  r->pos += 1;
}

static const char *
reader_font_name (RtfReader *r, int index)
{
  if (index < 0 || (guint) index >= r->fonts->len)
    return NULL;

  return g_ptr_array_index (r->fonts, index);
}

static void
apply_control (RtfReader *r, const char *word, gboolean has_param, int param)
{
  RtfState *st = &r->state;

  /* What the document says about itself. */
  if (g_str_equal (word, "info"))
    {
      r->dest = DEST_INFO;
      r->info_depth = r->stack->len;
      r->info_field = NULL;
      g_string_truncate (r->info_text, 0);
      return;
    }
  if (r->dest == DEST_INFO)
    {
      static const char *const fields[] = { "title", "subject", "author", "keywords", "doccomm", NULL };

      for (guint i = 0; fields[i] != NULL; i++)
        if (g_str_equal (word, fields[i]))
          {
            r->info_field = fields[i];
            g_string_truncate (r->info_text, 0);
            return;
          }
      return;                         /* the rest of an \info group is not ours */
    }

  /* Inside the font or colour table, only the table's own words matter. */
  if (r->dest == DEST_FONTTBL)
    {
      if (g_str_equal (word, "f") && has_param)
        r->font_index = CLAMP (param, 0, 4095);
      return;
    }

  if (r->dest == DEST_COLORTBL)
    {
      if (g_str_equal (word, "red"))   r->red = param;
      else if (g_str_equal (word, "green")) r->green = param;
      else if (g_str_equal (word, "blue"))  r->blue = param;
      return;
    }

  /* Tables. */
  if (g_str_equal (word, "clmgf")) { r->clpending = 1; return; }
  if (g_str_equal (word, "clmrg")) { r->clpending = 2; return; }

  if (g_str_equal (word, "trowd"))
    {
      g_array_set_size (r->cellx, 0);
      g_array_set_size (r->clflags, 0);
      g_array_set_size (r->clsides, 0);
      r->clpending = 0;
      r->clsides_pending = W42_BORDER_BOX;
      r->trrh = 0;
      r->trhdr = FALSE;
      return;
    }
  if (g_str_equal (word, "trrh"))
    {
      /* Negative is "exactly", which is read as "at least". */
      r->trrh = has_param ? ABS (param) : 0;
      return;
    }
  if (g_str_equal (word, "trhdr"))
    {
      r->trhdr = TRUE;
      return;
    }
  if (g_str_equal (word, "cellx") && has_param)
    {
      g_array_append_val (r->cellx, param);
      g_array_append_val (r->clflags, r->clpending);
      g_array_append_val (r->clsides, r->clsides_pending);
      /* The table is ruled unless its first cell has no rules at all. */
      if (r->table < 0 && r->clsides->len == 1)
        r->no_borders = (r->clsides_pending == 0);
      r->clpending = 0;
      r->clsides_pending = W42_BORDER_BOX;
      return;
    }
  if (g_str_equal (word, "intbl"))
    {
      st->intbl = TRUE;
      return;
    }
  if (g_str_equal (word, "cell"))
    {
      /* The cell's text has gone in; the next \intbl paragraph opens the
       * next cell.  An empty cell still needs its mark. */
      st->intbl = TRUE;
      table_sync (r);
      flush_text (r);
      r->in_cell = FALSE;
      r->table_col++;
      return;
    }
  if (g_str_equal (word, "row"))
    {
      r->table_row++;
      r->table_col = 0;
      r->in_cell = FALSE;
      return;
    }
  if (g_str_equal (word, "pard"))
    {
      /* \pard resets the paragraph, \intbl included; the table closes when
       * text arrives without it. */
      st->intbl = FALSE;
    }

  /* Fields.  The instruction group is read so that a header can keep its
   * page number as a field; the result group is what the body shows. */
  if (g_str_equal (word, "atrfstart") || g_str_equal (word, "atrfend") ||
      g_str_equal (word, "atnref"))
    {
      flush_text (r);
      r->atn_kind = g_str_equal (word, "atrfstart") ? 1 : g_str_equal (word, "atrfend") ? 2 : 3;
      r->atn_depth = r->stack->len;
      g_string_truncate (r->atn_ref, 0);
      return;
    }
  if (g_str_equal (word, "annotation"))
    {
      flush_text (r);
      r->atn_kind = 4;
      r->atn_depth = r->stack->len;
      r->atn_group_depth = r->stack->len;
      g_string_truncate (r->atn_text, 0);
      g_string_truncate (r->atn_ref, 0);
      return;
    }
  if (g_str_equal (word, "chatn") || g_str_equal (word, "atnid") || g_str_equal (word, "atndate"))
    {
      if (g_str_equal (word, "atnid") || g_str_equal (word, "atndate"))
        r->state.skip = TRUE;
      return;
    }
  if (r->atn_kind == 4 && (g_str_equal (word, "par") || g_str_equal (word, "line")))
    {
      g_string_append_c (r->atn_text, '\n');
      return;
    }

  if (g_str_equal (word, "bkmkstart") || g_str_equal (word, "bkmkend"))
    {
      flush_text (r);
      r->bkmk_kind = g_str_equal (word, "bkmkstart") ? 1 : 2;
      r->bkmk_depth = r->stack->len;
      g_string_truncate (r->bkmk_name, 0);
      return;
    }

  if (g_str_equal (word, "fldinst"))
    {
      r->in_fldinst = TRUE;
      r->fldinst_depth = r->stack->len;
      g_string_truncate (r->fldinst, 0);
      return;
    }
  if (g_str_equal (word, "fldrslt"))
    {
      flush_text (r);
      r->in_fldrslt = TRUE;
      r->fldrslt_depth = r->stack->len;
      r->field_start = r->pos;
      return;
    }
  if (g_str_equal (word, "field"))
    return;

  if (r->dest == DEST_HEADER || r->dest == DEST_FOOTER)
    {
      if (g_str_equal (word, "chpgn"))       g_string_append (r->hf_text, "{PAGE}");
      else if (g_str_equal (word, "qc"))     r->hf_align = W42_ALIGN_CENTER;
      else if (g_str_equal (word, "qr"))     r->hf_align = W42_ALIGN_RIGHT;
      else if (g_str_equal (word, "ql"))     r->hf_align = W42_ALIGN_LEFT;
      else if (g_str_equal (word, "tab"))    g_string_append_c (r->hf_text, '\t');
      else if (g_str_equal (word, "emdash")) g_string_append (r->hf_text, "\342\200\224");
      else if (g_str_equal (word, "endash")) g_string_append (r->hf_text, "\342\200\223");
      /* Formatting words are let through to the state, which the group
       * restores afterwards; \par and the rest are ignored. */
      else if (!g_str_equal (word, "par"))
        goto formatting;
      return;
    }

  if (g_str_equal (word, "footnote") && !r->in_note && r->dest == DEST_NONE)
    {
      /* The note's paragraphs go to the notes section at the end; the
       * main text resumes after the group, one position on for the mark. */
      gsize body;

      flush_text (r);
      body = w42_pt_insert_footnote (r->pt, r->pos, reader_ap (r));
      r->note_return = r->pos + 1;
      r->pos = body;
      r->in_note = TRUE;
      r->note_depth = r->stack->len;
      return;
    }
  if (g_str_equal (word, "chftn"))
    return;                 /* the mark is made by the footnote itself */
  if (g_str_equal (word, "ftnalt") && r->in_note)
    {
      /* An endnote: the mark just made is one. */
      int id = w42_pt_footnote_at (r->pt, r->note_return - 1);

      if (id >= 0)
        w42_pt_set_note_endnote (r->pt, id, TRUE);
      return;
    }

  if (g_str_equal (word, "titlepg"))
    {
      w42_pt_set_title_page (r->pt, TRUE);
      return;
    }
  if (g_str_equal (word, "facingp"))
    {
      w42_pt_set_facing_pages (r->pt, TRUE);
      return;
    }
  if (g_str_equal (word, "headerf") || g_str_equal (word, "footerf") ||
      g_str_equal (word, "headerl") || g_str_equal (word, "footerl") ||
      g_str_equal (word, "headerr") || g_str_equal (word, "footerr"))
    {
      /* A title page's own, or the ones for the left and right pages. */
      flush_text (r);
      r->hf_kind = word[6] == 'f' ? W42_PAGE_TEXT_FIRST
                 : word[6] == 'l' ? W42_PAGE_TEXT_EVEN : W42_PAGE_TEXT_DEFAULT;
      r->dest = word[0] == 'h' ? DEST_HEADER : DEST_FOOTER;
      r->hf_depth = r->stack->len;
      g_string_truncate (r->hf_text, 0);
      r->hf_align = W42_ALIGN_LEFT;
      return;
    }
  if (g_str_equal (word, "header") || g_str_equal (word, "footer"))
    {
      flush_text (r);
      r->dest = g_str_equal (word, "header") ? DEST_HEADER : DEST_FOOTER;
      r->hf_kind = W42_PAGE_TEXT_DEFAULT;
      r->hf_depth = r->stack->len;
      r->hf_align = W42_ALIGN_LEFT;
      g_string_truncate (r->hf_text, 0);
      return;
    }

formatting:
  /* Inside the stylesheet the formatting words apply to the state as they
   * would in a paragraph, and the entry is taken from the state when its
   * name ends at the semicolon.  Only \s itself is special. */
  if (r->dest == DEST_STYLESHEET)
    {
      if (g_str_equal (word, "s") && has_param)
        {
          r->style_index = param;
          return;
        }
      if (g_str_equal (word, "cs") || g_str_equal (word, "ds") || g_str_equal (word, "ts"))
        {
          /* A character, section or table style: not one of the \s
           * paragraph styles, so it must not overwrite the last of those. */
          r->style_index = -1;
          return;
        }
      if (g_str_equal (word, "sbasedon") || g_str_equal (word, "snext"))
        return;
      if (g_str_equal (word, "outlinelevel") && has_param)
        {
          /* Another program's name for a heading -- LibreOffice writes
           * them in the user's language -- is recognised by the outline
           * level the entry carries. */
          r->style_outline = CLAMP (param + 1, 1, 9);
          return;
        }
      /* fall through to the ordinary formatting words */
    }
  else if (g_str_equal (word, "s") && has_param)
    {
      /* A paragraph in a style takes the style's formatting as its base;
       * the properties that follow are direct formatting on top. */
      const char *name = g_hash_table_lookup (r->style_names,
                                              GINT_TO_POINTER (param));
      const W42Style *style = name != NULL
        ? w42_stylesheet_find (w42_pt_stylesheet (r->pt), name) : NULL;

      if (style != NULL)
        {
          /* The section break read before the \pard is not the style's
           * to take away. */
          guint8 section_break = st->pa.section_break;
          guint8 columns = st->pa.columns;
          int column_gap = st->pa.column_gap;

          flush_text (r);
          st->pa = style->pa;
          st->ch = style->ch;
          st->pa.section_break = section_break;
          st->pa.columns = columns;
          st->pa.column_gap = column_gap;
        }
      return;
    }
  else if (g_str_equal (word, "ls") && has_param)
    {
      /* The paragraph belongs to a list in the list table. */
      const ListShape *shape = r->list_shapes != NULL
        ? g_hash_table_lookup (r->list_shapes, GINT_TO_POINTER (param)) : NULL;

      r->cur_ls = param;
      if (shape != NULL)
        {
          st->pa.list = shape->kind[CLAMP (r->cur_ilvl, 0, 8)];
          st->pa.list_level = (guint8) CLAMP (r->cur_ilvl, 0, 8);
        }
      return;
    }
  else if (g_str_equal (word, "ilvl") && has_param)
    {
      const ListShape *shape = r->list_shapes != NULL && r->cur_ls >= 0
        ? g_hash_table_lookup (r->list_shapes, GINT_TO_POINTER (r->cur_ls)) : NULL;

      r->cur_ilvl = CLAMP (param, 0, 8);
      if (shape != NULL)
        {
          st->pa.list = shape->kind[r->cur_ilvl];
          st->pa.list_level = (guint8) r->cur_ilvl;
        }
      return;
    }
  else if (g_str_equal (word, "chcbpat") && has_param)
    {
      /* The character's background, as a colour rather than one of Word's
       * sixteen highlights: the nearest highlight is the one meant. */
      flush_text (r);
      st->ch.highlight = (guint8) nearest_highlight (r, param);
      return;
    }
  else if (g_str_equal (word, "cbpat") && has_param)
    {
      /* The paragraph's background, likewise, as a shade of grey. */
      guint32 rgb = colour_at (r, param);
      int grey = (int) ((((rgb >> 16) & 0xFF) * 30 + ((rgb >> 8) & 0xFF) * 59 +
                         (rgb & 0xFF) * 11) / 100);

      st->pa.shading = (guint8) CLAMP ((255 - grey) * 100 / 255, 0, 100);
      return;
    }
  else if (g_str_equal (word, "stylesheet"))
    {
      r->dest = DEST_STYLESHEET;
      r->style_depth = r->stack->len;
      r->style_index = 0;
      r->style_outline = 0;
      g_string_truncate (r->style_name, 0);
      return;
    }
  else if (g_str_equal (word, "wfnumhead"))
    {
      w42_stylesheet_set_number_headings (w42_pt_stylesheet (r->pt),
                                          !has_param || param != 0);
      return;
    }

  if (r->dest == DEST_PICT)
    {
      if (g_str_equal (word, "pngblip"))       r->pict_format = "png";
      else if (g_str_equal (word, "jpegblip")) r->pict_format = "jpeg";
      else if (g_str_equal (word, "picwgoal") && has_param) r->pict_wgoal = param;
      else if (g_str_equal (word, "pichgoal") && has_param) r->pict_hgoal = param;
      return;
    }

  if (g_str_equal (word, "pict"))
    {
      flush_text (r);
      r->dest = DEST_PICT;
      r->pict_depth = r->stack->len;
      r->pict_format = NULL;
      r->pict_wgoal = r->pict_hgoal = 0;
      g_string_truncate (r->pict_hex, 0);
      return;
    }

  /* Character formatting.  A control word with a parameter of 0 turns its
   * property off, which is how RTF spells \b0. */
  if (g_str_equal (word, "b"))
    { flush_text (r); st->ch.bold = (has_param && param == 0) ? 0 : 1; }
  else if (g_str_equal (word, "i"))
    { flush_text (r); st->ch.italic = (has_param && param == 0) ? 0 : 1; }
  else if (g_str_equal (word, "ul"))
    { flush_text (r); st->ch.underline = (has_param && param == 0) ? 0 : 1; }
  else if (g_str_equal (word, "ulnone"))
    { flush_text (r); st->ch.underline = 0; }
  else if (g_str_equal (word, "ol"))
    { flush_text (r); st->ch.overline = (has_param && param == 0) ? 0 : 1; }
  else if (g_str_equal (word, "strike"))
    { flush_text (r); st->ch.strikeout = (has_param && param == 0) ? 0 : 1; }
  else if (g_str_equal (word, "super"))
    { flush_text (r); st->ch.script = 1; }
  else if (g_str_equal (word, "sub"))
    { flush_text (r); st->ch.script = -1; }
  else if (g_str_equal (word, "nosupersub"))
    { flush_text (r); st->ch.script = 0; }
  else if (g_str_equal (word, "scaps"))
    { flush_text (r); st->ch.smallcaps = (has_param && param == 0) ? 0 : 1; }
  else if (g_str_equal (word, "caps"))
    { flush_text (r); st->ch.allcaps = (has_param && param == 0) ? 0 : 1; }
  else if (g_str_equal (word, "highlight"))
    { flush_text (r); st->ch.highlight = has_param ? (guint8) CLAMP (param, 0, 16) : 0; }
  else if (g_str_equal (word, "revised"))
    { flush_text (r); st->ch.revision = (has_param && param == 0) ? 0 : 1; }
  else if (g_str_equal (word, "deleted"))
    { flush_text (r); st->ch.revision = (has_param && param == 0) ? 0 : 2; }
  else if (g_str_equal (word, "expndtw") && has_param)
    { flush_text (r); st->ch.spacing = (gint16) CLAMP (param, -720, 720); }
  else if (g_str_equal (word, "expnd") && has_param)
    { flush_text (r); st->ch.spacing = (gint16) CLAMP (param * 5, -720, 720); }
  else if (g_str_equal (word, "fs") && has_param)
    { flush_text (r); st->ch.size = param > 0 ? param : 20; }
  else if (g_str_equal (word, "f") && has_param)
    {
      const char *name = reader_font_name (r, param);
      flush_text (r);
      if (name != NULL)
        st->ch.family = g_intern_string (name);
    }
  else if (g_str_equal (word, "cf") && has_param)
    {
      flush_text (r);
      /* The colour table's entries are numbered from zero, and entry zero is
       * the "auto" one that the leading semicolon of \colortbl declares.  So
       * \cf1 is the *second* entry, and the table is indexed by the parameter
       * directly rather than one less than it. */
      if (param >= 0 && (guint) param < r->colours->len)
        st->ch.color = g_array_index (r->colours, guint32, param);
      else
        st->ch.color = 0;
    }
  else if (g_str_equal (word, "plain"))
    {
      W42Fmt def;
      flush_text (r);
      w42_fmt_init_default (&def);
      st->ch = def.ch;
    }

  /* Paragraph formatting. */
  else if (g_str_equal (word, "pard"))
    {
      /* Word 97 and later put the \pn group before the \pard, so the list
       * kind has to outlive the reset; \par forgets it instead. */
      W42Fmt def;
      guint8 list = st->pa.list;
      w42_fmt_init_default (&def);
      st->pa = def.pa;
      st->pa.list = list;
      if (r->sect_pending)
        {
          st->pa.section_break = 1;
          st->pa.columns = (guint8) CLAMP (r->sect_cols, 1, 9);
          st->pa.column_gap = r->sect_gap;
        }
    }
  else if (g_str_has_prefix (word, "clbrdr"))
    {
      r->cell_border = TRUE;
      r->cell_side = word[6] == 't' ? W42_BORDER_TOP : word[6] == 'b' ? W42_BORDER_BOTTOM
                   : word[6] == 'l' ? W42_BORDER_LEFT : word[6] == 'r' ? W42_BORDER_RIGHT : 0;
    }
  else if (g_str_equal (word, "brdrnone") && r->cell_border)
    {
      r->clsides_pending &= ~r->cell_side;
      r->cell_border = FALSE;
    }
  else if (g_str_has_prefix (word, "brdr") && r->cell_border)
    r->cell_border = FALSE;
  else if (g_str_equal (word, "sect"))
    {
      r->sect_pending = TRUE;
      r->sect_cols = 1;
      r->sect_gap = 0;
    }
  else if (g_str_equal (word, "ql")) st->pa.align = W42_ALIGN_LEFT;
  else if (g_str_equal (word, "rtlpar")) st->pa.rtl = 1;
  else if (g_str_equal (word, "ltrpar")) st->pa.rtl = 0;
  else if (g_str_equal (word, "qc")) st->pa.align = W42_ALIGN_CENTER;
  else if (g_str_equal (word, "qr")) st->pa.align = W42_ALIGN_RIGHT;
  else if (g_str_equal (word, "qj")) st->pa.align = W42_ALIGN_JUSTIFY;
  else if (g_str_equal (word, "li") && has_param) st->pa.indent_left = param;
  else if (g_str_equal (word, "ri") && has_param) st->pa.indent_right = param;
  else if (g_str_equal (word, "fi") && has_param) st->pa.indent_first = param;
  else if (g_str_equal (word, "pagebb")) st->pa.page_break_before = 1;
  else if (g_str_equal (word, "keepn"))  st->pa.keep_next = 1;
  else if (g_str_equal (word, "keep"))   st->pa.keep_together = 1;
  else if (g_str_equal (word, "widctlpar"))   st->pa.widow_control = 1;
  else if (g_str_equal (word, "nowidctlpar")) st->pa.widow_control = 0;
  else if (g_str_equal (word, "brdrt")) { st->pa.border |= W42_BORDER_TOP;    st->border_side = W42_BORDER_TOP; }
  else if (g_str_equal (word, "brdrb")) { st->pa.border |= W42_BORDER_BOTTOM; st->border_side = W42_BORDER_BOTTOM; }
  else if (g_str_equal (word, "brdrl")) { st->pa.border |= W42_BORDER_LEFT;   st->border_side = W42_BORDER_LEFT; }
  else if (g_str_equal (word, "brdrr")) { st->pa.border |= W42_BORDER_RIGHT;  st->border_side = W42_BORDER_RIGHT; }
  else if (g_str_equal (word, "box"))   { st->pa.border  = W42_BORDER_BOX;    st->border_side = W42_BORDER_BOX; }
  else if (g_str_equal (word, "brdrw") && has_param && st->border_side != 0)
    st->pa.border_width = (guint8) CLAMP (param, 1, 120);
  else if (g_str_equal (word, "brdrnone") || g_str_equal (word, "brdrnil"))
    {
      if (st->border_side != 0)
        st->pa.border &= (guint8) ~st->border_side;
    }
  else if (g_str_equal (word, "shading") && has_param)
    st->pa.shading = (guint8) CLAMP (param / 100, 0, 100);
  else if (g_str_equal (word, "tqc"))   st->tab_kind = W42_TAB_CENTER;
  else if (g_str_equal (word, "tqr"))   st->tab_kind = W42_TAB_RIGHT;
  else if (g_str_equal (word, "tqdec")) st->tab_kind = W42_TAB_DECIMAL;
  else if (g_str_equal (word, "tldot") || g_str_equal (word, "tlmdot"))
    st->tab_leader = W42_TAB_LEAD_DOT;
  else if (g_str_equal (word, "tlhyph")) st->tab_leader = W42_TAB_LEAD_DASH;
  else if (g_str_equal (word, "tlul") || g_str_equal (word, "tlth") ||
           g_str_equal (word, "tleq"))
    st->tab_leader = W42_TAB_LEAD_LINE;
  else if (g_str_equal (word, "tx") && has_param)
    {
      w42_para_fmt_set_tab_leader (&st->pa, param, (W42TabKind) st->tab_kind,
                                   (W42TabLeader) st->tab_leader);
      st->tab_kind = W42_TAB_LEFT;
      st->tab_leader = W42_TAB_LEAD_NONE;
    }

  /* \pn comes in its own group, so what it says about the list has to
   * reach the paragraph's state underneath, not just the group's. */
  else if (g_str_equal (word, "pnlvlblt") || g_str_equal (word, "pnlvlbody") ||
           (g_str_equal (word, "pnlvl") && has_param))
    {
      guint8 kind = g_str_equal (word, "pnlvlblt") ? W42_LIST_BULLET
                                                   : W42_LIST_NUMBER;
      guint8 level = g_str_equal (word, "pnlvl") ? (guint8) CLAMP (param - 1, 0, 8) : 0;

      st->pa.list = kind;
      st->pa.list_level = level;
      if (r->stack->len > 0)
        {
          g_array_index (r->stack, RtfState, r->stack->len - 1).pa.list = kind;
          g_array_index (r->stack, RtfState, r->stack->len - 1).pa.list_level = level;
        }
    }
  else if (g_str_equal (word, "pndec") || g_str_equal (word, "pnlcltr") ||
           g_str_equal (word, "pnucltr") || g_str_equal (word, "pnlcrm") ||
           g_str_equal (word, "pnucrm"))
    {
      guint8 kind = g_str_equal (word, "pndec")   ? W42_LIST_NUMBER
                  : g_str_equal (word, "pnlcltr") ? W42_LIST_LOWER_LETTER
                  : g_str_equal (word, "pnucltr") ? W42_LIST_UPPER_LETTER
                  : g_str_equal (word, "pnlcrm")  ? W42_LIST_LOWER_ROMAN
                                                  : W42_LIST_UPPER_ROMAN;
      if (w42_list_is_numbered (st->pa.list))
        {
          st->pa.list = kind;
          if (r->stack->len > 0)
            g_array_index (r->stack, RtfState, r->stack->len - 1).pa.list = kind;
        }
    }
  else if (g_str_equal (word, "pnstart") && has_param)
    {
      /* Word writes \pnstart1 on every item; only another value means
       * a restart. */
      guint8 start = param > 1 ? (guint8) CLAMP (param, 1, 255) : 0;

      st->pa.list_start = start;
      if (r->stack->len > 0)
        g_array_index (r->stack, RtfState, r->stack->len - 1).pa.list_start = start;
    }
  else if (g_str_equal (word, "pntxtb"))
    st->pntxtb = TRUE;  /* the bullet character: read it, then skip */
  else if (g_str_equal (word, "pntxta"))
    st->skip = TRUE;    /* the marker text; word42 paints its own */
  else if (g_str_equal (word, "sb") && has_param) st->pa.space_before = param;
  else if (g_str_equal (word, "sa") && has_param) st->pa.space_after = param;
  else if (g_str_equal (word, "sl") && has_param)
    {
      /* Positive is "at least", negative is "exactly"; word42 treats both as
       * an exact leading, and \slmult1 later reinterprets it as a multiple. */
      st->pa.line_spacing = ABS (param);
    }
  else if (g_str_equal (word, "slmult") && has_param)
    {
      if (param == 1 && st->pa.line_spacing > 0)
        {
          st->pa.line_spacing_pct = (st->pa.line_spacing * 100) / 240;
          st->pa.line_spacing = 0;
        }
    }

  /* Structure and whitespace. */
  else if (g_str_equal (word, "par"))
    {
      end_paragraph (r);
      r->sect_pending = FALSE;
    }
  else if (g_str_equal (word, "line"))
    append_char (r, 0x2028);
  else if (g_str_equal (word, "tab"))
    append_char (r, '\t');
  else if (g_str_equal (word, "emdash"))   append_char (r, 0x2014);
  else if (g_str_equal (word, "endash"))   append_char (r, 0x2013);
  else if (g_str_equal (word, "lquote"))   append_char (r, 0x2018);
  else if (g_str_equal (word, "rquote"))   append_char (r, 0x2019);
  else if (g_str_equal (word, "ldblquote")) append_char (r, 0x201C);
  else if (g_str_equal (word, "rdblquote")) append_char (r, 0x201D);
  else if (g_str_equal (word, "bullet"))   append_char (r, 0x2022);
  else if (g_str_equal (word, "uc") && has_param)
    st->uc = param;

  /* Page geometry. */
  else if (r->page != NULL && has_param)
    {
      if      (g_str_equal (word, "paperw")) r->page->width = CLAMP (param, 720, 31680);
      else if (g_str_equal (word, "paperh")) r->page->height = CLAMP (param, 720, 31680);
      else if (g_str_equal (word, "margl"))  r->page->margin_left = param;
      else if (g_str_equal (word, "margr"))  r->page->margin_right = param;
      else if (g_str_equal (word, "margt"))  r->page->margin_top = param;
      else if (g_str_equal (word, "margb"))  r->page->margin_bottom = param;
      else if (g_str_equal (word, "cols"))
        {
          if (r->sect_pending)
            r->sect_cols = st->pa.columns = (guint8) CLAMP (param, 1, 9);
          else
            r->page->columns = param;
        }
      else if (g_str_equal (word, "colsx"))
        {
          if (r->sect_pending)
            r->sect_gap = st->pa.column_gap = param;
          else
            r->page->column_gap = param;
        }
    }
}

/* Destinations whose contents are not document text.  Everything inside one
 * is dropped, table-building aside. */
/* ---------------------------------------------------------------------- */
/* The list tables                                                         */
/* ---------------------------------------------------------------------- */

/* Word 97 and everything since -- LibreOffice and AbiWord included -- put
 * lists in two tables at the top of the file: {\*\listtable} says what
 * each list looks like at each level, and {\*\listoverridetable} ties the
 * \ls number a paragraph carries to one of them.  Word 6's \pn groups are
 * still read below; this is the newer spelling of the same thing.
 *
 * The tables are self-contained, so they are scanned here, before the
 * document is walked, rather than threaded through the reader's state. */

/* \levelnfc: how a level is numbered. */
static guint8
level_kind (int nfc)
{
  switch (nfc)
    {
    case 1:  return W42_LIST_UPPER_ROMAN;
    case 2:  return W42_LIST_LOWER_ROMAN;
    case 3:  return W42_LIST_UPPER_LETTER;
    case 4:  return W42_LIST_LOWER_LETTER;
    case 23: return W42_LIST_BULLET;
    default: return W42_LIST_NUMBER;      /* 0 and the rest: 1. 2. 3. */
    }
}

/* The end of the group that starts at `open` (which must point at its
 * '{'), or the end of the text. */
static gsize
group_end (const char *d, gsize len, gsize open)
{
  int depth = 0;

  for (gsize i = open; i < len; i++)
    {
      if (d[i] == '\\' && i + 1 < len)
        {
          i++;
          continue;
        }
      if (d[i] == '{')
        depth++;
      else if (d[i] == '}' && --depth == 0)
        return i;
    }
  return len;
}

static int
word_param (const char *d, gsize len, gsize from, gsize to, const char *word)
{
  gsize wlen = strlen (word);

  for (gsize i = from; i + wlen + 1 < to && i < len; i++)
    {
      if (d[i] != '\\' || strncmp (d + i + 1, word, wlen) != 0)
        continue;
      /* The control word must end here, not be the start of a longer one. */
      if (g_ascii_isalpha (d[i + 1 + wlen]))
        continue;
      return atoi (d + i + 1 + wlen);
    }
  return -1;
}

/* ls number -> the shape of that list.  Free with g_hash_table_destroy. */
static GHashTable *
scan_list_tables (const char *d, gsize len)
{
  GHashTable *by_id = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
  GHashTable *by_ls = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);

  /* {\*\listtable {\list \listid1 {\listlevel \levelnfc23 ...} ...} ...} */
  for (gsize i = 0; i + 10 < len; i++)
    {
      if (d[i] != '\\' || strncmp (d + i + 1, "listtable", 9) != 0)
        continue;
      {
        gsize open = i;
        gsize end;

        while (open > 0 && d[open] != '{')
          open--;
        end = group_end (d, len, open);

        for (gsize j = open; j < end; j++)
          {
            gsize list_open, list_end;
            int id, level = 0;
            ListShape *shape;

            if (d[j] != '\\' || strncmp (d + j + 1, "list", 4) != 0 ||
                g_ascii_isalpha (d[j + 5]))
              continue;
            list_open = j;
            while (list_open > open && d[list_open] != '{')
              list_open--;
            list_end = group_end (d, len, list_open);
            id = word_param (d, len, list_open, list_end, "listid");
            if (id < 0)
              {
                j = list_end;
                continue;
              }

            shape = g_new0 (ListShape, 1);
            for (guint k = 0; k < G_N_ELEMENTS (shape->kind); k++)
              shape->kind[k] = W42_LIST_NUMBER;

            /* Each {\listlevel ...} in turn is one level of the list. */
            for (gsize k = list_open; k < list_end && level < 9; k++)
              {
                gsize lvl_open, lvl_end;
                int nfc;

                if (d[k] != '\\' || strncmp (d + k + 1, "listlevel", 9) != 0)
                  continue;
                lvl_open = k;
                while (lvl_open > list_open && d[lvl_open] != '{')
                  lvl_open--;
                lvl_end = group_end (d, len, lvl_open);
                nfc = word_param (d, len, lvl_open, lvl_end, "levelnfc");
                shape->kind[level++] = level_kind (nfc);
                k = lvl_end;
              }
            g_hash_table_insert (by_id, GINT_TO_POINTER (id), shape);
            j = list_end;
          }
      }
      break;
    }

  /* {\*\listoverridetable {\listoverride \listid1 \ls1} ...} */
  for (gsize i = 0; i + 18 < len; i++)
    {
      if (d[i] != '\\' || strncmp (d + i + 1, "listoverridetable", 17) != 0)
        continue;
      {
        gsize open = i, end;

        while (open > 0 && d[open] != '{')
          open--;
        end = group_end (d, len, open);

        for (gsize j = open; j < end; j++)
          {
            gsize ov_open, ov_end;
            int id, ls;
            const ListShape *shape;

            if (d[j] != '\\' || strncmp (d + j + 1, "listoverride", 12) != 0 ||
                g_ascii_isalpha (d[j + 13]))
              continue;
            ov_open = j;
            while (ov_open > open && d[ov_open] != '{')
              ov_open--;
            ov_end = group_end (d, len, ov_open);
            id = word_param (d, len, ov_open, ov_end, "listid");
            ls = word_param (d, len, ov_open, ov_end, "ls");
            shape = id >= 0 ? g_hash_table_lookup (by_id, GINT_TO_POINTER (id)) : NULL;
            if (ls >= 0 && shape != NULL)
              g_hash_table_insert (by_ls, GINT_TO_POINTER (ls),
                                   g_memdup2 (shape, sizeof *shape));
            j = ov_end;
          }
      }
      break;
    }

  g_hash_table_destroy (by_id);
  return by_ls;
}

static gboolean
is_ignorable_destination (const char *word)
{
  static const char *names[] = {
    "pntext", "listtext", "listtable", "listoverridetable",
    "object", "nonshppict",   /* the metafile copy of a \shppict, not the picture */
    "themedata", "colorschememapping", "latentstyles", "datastore",
    "generator", "xmlnstbl", "rsidtbl", "mmathPr", "upr",
  };

  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (g_str_equal (word, names[i]))
      return TRUE;

  return FALSE;
}

gboolean
w42_rtf_load (W42PieceTable *pt,
              W42PageSetup  *page,
              GFile         *file,
              GError       **error)
{
  char *contents = NULL;
  gsize length = 0;
  const char *p, *end;
  RtfReader r;
  W42Fmt def;
  gboolean pending_surrogate = FALSE;
  gunichar high_surrogate = 0;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return FALSE;

  if (length < 5 || strncmp (contents, "{\\rtf", 5) != 0)
    {
      g_free (contents);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "That file does not begin like an RTF document.");
      return FALSE;
    }

  w42_pt_load_text (pt, "");

  memset (&r, 0, sizeof r);
  r.pt = pt;
  r.page = page;
  r.fonts = g_ptr_array_new_with_free_func (g_free);
  r.colours = g_array_new (FALSE, FALSE, sizeof (guint32));
  r.stack = g_array_new (FALSE, FALSE, sizeof (RtfState));
  r.pending = g_string_new (NULL);
  r.font_name = g_string_new (NULL);
  r.pict_hex = g_string_new (NULL);
  r.style_name = g_string_new (NULL);
  r.style_names = g_hash_table_new (g_direct_hash, g_direct_equal);
  r.list_shapes = scan_list_tables (contents, length);
  r.cur_ls = -1;
  r.cur_ilvl = 0;
  r.hf_text = g_string_new (NULL);
  r.fldinst = g_string_new (NULL);
  r.bkmk_name = g_string_new (NULL);
  r.atn_text = g_string_new (NULL);
  r.atn_ref = g_string_new (NULL);
  r.atrf = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r.atrf_end = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  r.table = -1;
  r.cellx = g_array_new (FALSE, FALSE, sizeof (int));
  r.clflags = g_array_new (FALSE, FALSE, sizeof (int));
  r.clsides = g_array_new (FALSE, FALSE, sizeof (int));
  r.info_text = g_string_new (NULL);
  r.clsides_pending = W42_BORDER_BOX;
  r.last_cell_pos = (gsize) -1;
  r.pos = w42_pt_first_caret_pos (pt);
  r.first_para = TRUE;
  r.font_index = -1;

  w42_fmt_init_default (&def);
  r.state.ch = def.ch;
  r.state.pa = def.pa;
  r.state.uc = 1;

  p = contents;
  end = contents + length;

  while (p < end)
    {
      if (*p == '{')
        {
          if (r.stack->len > 4096)
            break;                    /* nested past all sense: a hostile file */
          g_array_append_val (r.stack, r.state);
          p++;
          continue;
        }

      if (*p == '}')
        {
          if (r.in_fldinst && r.stack->len == r.fldinst_depth)
            {
              /* The instruction names the field; in a header the field is
               * kept as one, and in the body its cached result was shown. */
              char *code = g_strstrip (g_strdup (r.fldinst->str));

              r.in_fldinst = FALSE;
              if (g_ascii_strncasecmp (code, "HYPERLINK", 9) == 0)
                {
                  /* HYPERLINK "url": the result that follows is the link. */
                  const char *q = strchr (code, '"');
                  if (q != NULL)
                    {
                      const char *e = strchr (q + 1, '"');
                      char *url = e != NULL ? g_strndup (q + 1, (gsize) (e - q - 1))
                                            : g_strdup (q + 1);
                      flush_text (&r);
                      r.link = g_intern_string (url);
                      g_free (url);
                    }
                }
              r.field_code = w42_field_code (code);
              if (r.dest == DEST_HEADER || r.dest == DEST_FOOTER)
                {
                  if (g_ascii_strncasecmp (code, "PAGE", 4) == 0)
                    g_string_append (r.hf_text, "{PAGE}");
                  else if (g_ascii_strncasecmp (code, "NUMPAGES", 8) == 0)
                    g_string_append (r.hf_text, "{NUMPAGES}");
                  else if (g_ascii_strncasecmp (code, "DATE", 4) == 0 ||
                           g_ascii_strncasecmp (code, "TIME", 4) == 0)
                    g_string_append (r.hf_text, "{DATE}");
                }
              g_free (code);
            }
          if (r.in_fldrslt && r.stack->len == r.fldrslt_depth)
            {
              flush_text (&r);
              r.in_fldrslt = FALSE;
              r.link = NULL;
              /* The result is the field's text until it is updated. */
              if (r.field_code != NULL && r.dest == DEST_NONE && r.pos > r.field_start)
                {
                  W42CharFmt want;

                  memset (&want, 0, sizeof want);
                  want.field = r.field_code;
                  w42_pt_apply_char_fmt (r.pt, r.field_start, r.pos - r.field_start, W42_CHAR_FIELD, &want);
                }
              r.field_code = NULL;
            }
          if (r.atn_kind != 0 && r.stack->len == r.atn_depth)
            {
              char *ref = g_strstrip (g_strdup (r.atn_ref->str));
              gsize *slot;

              if (r.atn_kind == 1)
                {
                  slot = g_new (gsize, 1);
                  *slot = r.pos;
                  g_hash_table_insert (r.atrf, g_strdup (ref), slot);
                }
              else if (r.atn_kind == 2)
                {
                  slot = g_new (gsize, 1);
                  *slot = r.pos;
                  g_hash_table_insert (r.atrf_end, g_strdup (ref), slot);
                }
              else if (r.atn_kind == 3)
                {
                  /* the atnref inside the annotation: remember it and go
                   * on reading the annotation's text */
                  r.atn_kind = 4;
                  r.atn_depth = r.atn_group_depth;
                  g_free (ref);
                  goto atn_done;
                }
              else if (r.atn_kind == 4)
                {
                  gsize *a = g_hash_table_lookup (r.atrf, ref);
                  gsize *e = g_hash_table_lookup (r.atrf_end, ref);
                  char *text = g_strstrip (g_strdup (r.atn_text->str));

                  if (a != NULL && e != NULL && *e > *a && *text != '\0')
                    {
                      W42CharFmt want;

                      memset (&want, 0, sizeof want);
                      want.comment = g_intern_string (text);
                      w42_pt_apply_char_fmt (pt, *a, *e - *a, W42_CHAR_COMMENT, &want);
                    }
                  g_free (text);
                }
              r.atn_kind = 0;
              g_free (ref);
            atn_done:
              ;
            }
          if (r.bkmk_kind != 0 && r.stack->len == r.bkmk_depth)
            {
              char *name = g_strstrip (g_strdup (r.bkmk_name->str));

              r.bookmark = (r.bkmk_kind == 1 && *name != '\0')
                             ? g_intern_string (name) : NULL;
              r.bkmk_kind = 0;
              g_free (name);
            }
          if ((r.dest == DEST_HEADER || r.dest == DEST_FOOTER) &&
              r.stack->len == r.hf_depth)
            {
              char *text = g_strstrip (g_strdup (r.hf_text->str));

              if (r.dest == DEST_HEADER)

                w42_pt_set_header_kind (r.pt, r.hf_kind, text, r.hf_align);

              else

                w42_pt_set_footer_kind (r.pt, r.hf_kind, text, r.hf_align);
              g_free (text);
              r.dest = DEST_NONE;
            }
          if (r.in_note && r.stack->len == r.note_depth)
            {
              flush_text (&r);
              r.in_note = FALSE;
              r.pos = r.note_return;
            }
          if (r.dest == DEST_PICT && r.stack->len == r.pict_depth)
            finish_pict (&r);
          if (r.dest == DEST_STYLESHEET && r.stack->len == r.style_depth)
            r.dest = DEST_NONE;
          if (r.dest == DEST_INFO)
            {
              static const char *const fields[] = { "title", "subject", "author", "keywords", "doccomm", NULL };

              /* A field's group has closed: keep what it said. */
              if (r.info_field != NULL && r.info_text->len > 0)
                for (guint i = 0; fields[i] != NULL; i++)
                  if (g_str_equal (r.info_field, fields[i]) && r.info_keep[i] == NULL)
                    r.info_keep[i] = g_strdup (r.info_text->str);
              r.info_field = NULL;
              g_string_truncate (r.info_text, 0);
              if (r.stack->len == r.info_depth)
                r.dest = DEST_NONE;
            }

          /* Leaving the font or colour table commits the entry being built. */
          if (r.dest == DEST_FONTTBL && r.font_name->len > 0)
            {
              while ((guint) r.font_index >= r.fonts->len)
                g_ptr_array_add (r.fonts, g_strdup ("Times New Roman"));
              g_free (g_ptr_array_index (r.fonts, r.font_index));
              g_ptr_array_index (r.fonts, r.font_index) =
                g_strdup (r.font_name->str);
              g_string_truncate (r.font_name, 0);
            }

          flush_text (&r);

          if (r.stack->len > 0)
            {
              r.state = g_array_index (r.stack, RtfState, r.stack->len - 1);
              g_array_set_size (r.stack, r.stack->len - 1);
            }

          /* The table destinations end with the group that opened them. */
          if (r.dest != DEST_NONE && r.stack->len <= 1)
            r.dest = DEST_NONE;

          p++;
          continue;
        }

      if (*p == '\\')
        {
          p++;
          if (p >= end)
            break;

          /* \' introduces a byte in the document's code page. */
          if (*p == '\'' && p + 2 < end)
            {
              char hex[3] = { p[1], p[2], '\0' };
              int byte = (int) strtol (hex, NULL, 16);
              char *utf8;

              /* word42 writes \u for everything above ASCII, so a \' can only
               * come from another program; Windows-1252 is the overwhelmingly
               * likely code page and the one \ansicpg1252 declares. */
              char raw[2] = { (char) byte, '\0' };
              utf8 = g_convert (raw, 1, "UTF-8", "WINDOWS-1252",
                                NULL, NULL, NULL);
              if (utf8 != NULL)
                {
                  append_char (&r, g_utf8_get_char (utf8));
                  g_free (utf8);
                }

              p += 3;
              continue;
            }

          if (!g_ascii_isalpha (*p))
            {
              /* An escaped literal: \\ \{ \} and the like. */
              if (*p == '\n' || *p == '\r')
                end_paragraph (&r);
              else if (*p == '~')
                append_char (&r, 0x00A0);
              else if (*p == '-')
                append_char (&r, 0x00AD);    /* optional hyphen */
              else if (*p == '*')
                {
                  /* \* marks a destination the reader may ignore.  Take the
                   * hint unless it is one we understand. */
                  const char *q = p + 1;
                  if (q < end && *q == '\\')
                    {
                      const char *w = ++q;
                      while (q < end && g_ascii_isalpha (*q))
                        q++;
                      {
                        char *name = g_strndup (w, (gsize) (q - w));
                        if (!g_str_equal (name, "fonttbl") &&
                            !g_str_equal (name, "colortbl") &&
                            !g_str_equal (name, "shppict") &&
                            !g_str_equal (name, "wfnumhead") &&
                            !g_str_equal (name, "pn") &&
                            !g_str_equal (name, "bkmkstart") &&
                            !g_str_equal (name, "bkmkend") &&
                            !g_str_equal (name, "atrfstart") &&
                            !g_str_equal (name, "atrfend") &&
                            !g_str_equal (name, "atnref") &&
                            !g_str_equal (name, "annotation") &&
                            !g_str_equal (name, "fldinst"))
                          r.state.skip = TRUE;
                        g_free (name);
                      }
                      p = w - 1;   /* the word itself is lexed as usual */
                      continue;
                    }
                }
              else
                append_char (&r, (gunichar) (guchar) *p);

              p++;
              continue;
            }

          /* A control word: letters, then an optional signed number, then a
           * single optional space that belongs to the word rather than the
           * text. */
          {
            const char *word_start = p;
            char *word;
            gboolean has_param = FALSE;
            int param = 0;
            int sign = 1;

            while (p < end && g_ascii_isalpha (*p))
              p++;

            word = g_strndup (word_start, (gsize) (p - word_start));

            if (p < end && (*p == '-' || g_ascii_isdigit (*p)))
              {
                has_param = TRUE;
                if (*p == '-') { sign = -1; p++; }
                while (p < end && g_ascii_isdigit (*p))
                  {
                    if (param < 100000000)
                      param = param * 10 + (*p - '0');
                    p++;
                  }
                param *= sign;
              }

            if (p < end && *p == ' ')
              p++;

            if (g_str_equal (word, "fonttbl"))
              {
                r.dest = DEST_FONTTBL;
                r.font_index = 0;
                g_string_truncate (r.font_name, 0);
              }
            else if (g_str_equal (word, "colortbl"))
              {
                r.dest = DEST_COLORTBL;
                r.red = r.green = r.blue = 0;
              }
            else if (g_str_equal (word, "u") && has_param)
              {
                gunichar c = (param < 0) ? (gunichar) (param + 65536)
                                         : (gunichar) param;

                /* A low surrogate is half of a pair: it means something
                 * only when a high one came just before it.  Anything
                 * else out of range is not a character at all. */
                if (c > 0x10FFFF ||
                    (!pending_surrogate && c >= 0xDC00 && c <= 0xDFFF))
                  c = 0xFFFD;

                if (c >= 0xD800 && c <= 0xDBFF)
                  {
                    high_surrogate = c;
                    pending_surrogate = TRUE;
                  }
                else if (pending_surrogate && c >= 0xDC00 && c <= 0xDFFF)
                  {
                    append_char (&r, 0x10000 +
                                 ((high_surrogate - 0xD800) << 10) +
                                 (c - 0xDC00));
                    pending_surrogate = FALSE;
                  }
                else
                  {
                    append_char (&r, c);
                    pending_surrogate = FALSE;
                  }

                /* Skip the fallback characters that follow. */
                for (int i = 0; i < r.state.uc && p < end; i++)
                  {
                    if (*p == '\\' && p + 1 < end && p[1] == '\'')
                      p += 4;
                    else if (*p != '{' && *p != '}' && *p != '\\')
                      p++;
                    else
                      break;
                  }
              }
            else if (is_ignorable_destination (word))
              {
                r.state.skip = TRUE;
              }
            else
              {
                apply_control (&r, word, has_param, param);
              }

            g_free (word);
            continue;
          }
        }

      /* Ordinary text. */
      if (*p == '\r' || *p == '\n')
        {
          p++;                     /* line breaks in the file are not text */
          continue;
        }

      if (r.dest == DEST_PICT)
        {
          /* Only the picture's own hex, and not the text of the groups a
           * writer puts inside \pict: LibreOffice's {\*\picprop} holds
           * words like "wzDescription", whose letters are hex digits as
           * far as this loop can tell. */
          if (!r.state.skip && g_ascii_isxdigit (*p))
            g_string_append_c (r.pict_hex, *p);
          p++;
          continue;
        }

      if (r.dest == DEST_STYLESHEET)
        {
          if (*p == ';')
            finish_style (&r);
          else
            g_string_append_c (r.style_name, *p);
          p++;
          continue;
        }

      if (r.dest == DEST_FONTTBL)
        {
          if (*p == ';')
            {
              while ((guint) r.font_index >= r.fonts->len)
                g_ptr_array_add (r.fonts, g_strdup ("Times New Roman"));
              g_free (g_ptr_array_index (r.fonts, r.font_index));
              g_ptr_array_index (r.fonts, r.font_index) =
                g_strdup (r.font_name->str);
              g_string_truncate (r.font_name, 0);
            }
          else
            {
              g_string_append_c (r.font_name, *p);
            }
          p++;
          continue;
        }

      if (r.dest == DEST_COLORTBL)
        {
          if (*p == ';')
            {
              guint32 c = ((guint32) (r.red & 0xff) << 16) |
                          ((guint32) (r.green & 0xff) << 8) |
                           (guint32) (r.blue & 0xff);
              g_array_append_val (r.colours, c);
              r.red = r.green = r.blue = 0;
            }
          p++;
          continue;
        }

      {
        gunichar c = g_utf8_get_char_validated (p, end - p);

        if (c == (gunichar) -1 || c == (gunichar) -2)
          {
            append_char (&r, (gunichar) (guchar) *p);
            p++;
          }
        else
          {
            append_char (&r, c);
            p = g_utf8_next_char (p);
          }
      }
    }

  flush_text (&r);

  /* A table still open at the end of the file closes here. */
  if (r.table >= 0)
    {
      r.state.intbl = FALSE;
      table_sync (&r);
    }

  /* The closing \par of the last paragraph leaves an empty one behind, just
   * as a trailing newline would in a text file.  Drop it. */
  if (!r.first_para && r.pos >= 1 &&
      (r.pos == w42_pt_length (pt) || r.pos == w42_pt_notes_start (pt)))
    {
      gsize first = w42_pt_first_caret_pos (pt);
      if (r.pos - 1 > first)
        w42_pt_delete (pt, r.pos - 1, 1);
    }

  w42_pt_clear_undo (pt);

  g_string_free (r.pending, TRUE);
  g_string_free (r.font_name, TRUE);
  g_string_free (r.pict_hex, TRUE);
  g_string_free (r.style_name, TRUE);
  g_string_free (r.hf_text, TRUE);
  g_string_free (r.fldinst, TRUE);
  g_string_free (r.bkmk_name, TRUE);
  g_string_free (r.atn_text, TRUE);
  g_string_free (r.atn_ref, TRUE);
  g_hash_table_destroy (r.atrf);
  g_hash_table_destroy (r.atrf_end);
  g_array_free (r.cellx, TRUE);
  g_array_free (r.clflags, TRUE);
  g_array_free (r.clsides, TRUE);
  {
    /* What the \info group said. */
    W42DocInfo info;

    memset (&info, 0, sizeof info);
    info.title = r.info_keep[0];
    info.subject = r.info_keep[1];
    info.author = r.info_keep[2];
    info.keywords = r.info_keep[3];
    info.comments = r.info_keep[4];
    if (info.title || info.subject || info.author || info.keywords || info.comments)
      w42_pt_set_info (pt, &info);
    for (guint i = 0; i < G_N_ELEMENTS (r.info_keep); i++)
      g_free (r.info_keep[i]);
    g_string_free (r.info_text, TRUE);
  }
  g_hash_table_destroy (r.style_names);
  g_array_free (r.stack, TRUE);
  g_array_free (r.colours, TRUE);
  if (r.list_shapes != NULL)
    g_hash_table_destroy (r.list_shapes);
  g_ptr_array_free (r.fonts, TRUE);
  g_free (contents);

  return TRUE;
}
