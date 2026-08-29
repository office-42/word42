/* w42-html.c - see w42-html.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-html.h"

#include <string.h>

#include "w42-image.h"

static void
append_escaped (GString *out, const char *text, gsize len)
{
  for (gsize i = 0; i < len; i++)
    {
      switch (text[i])
        {
        case '<':  g_string_append (out, "&lt;"); break;
        case '>':  g_string_append (out, "&gt;"); break;
        case '&':  g_string_append (out, "&amp;"); break;
        case '"':  g_string_append (out, "&quot;"); break;
        case '\t': g_string_append (out, "&emsp;"); break;
        default:
          if ((guchar) text[i] == 0xE2 && i + 2 < len &&
              (guchar) text[i + 1] == 0x80 && (guchar) text[i + 2] == 0xA8)
            {
              g_string_append (out, "<br>");     /* a line break */
              i += 2;
            }
          else if ((guchar) text[i] == 0xC2 && i + 1 < len && (guchar) text[i + 1] == 0xAD)
            {
              g_string_append (out, "&shy;");
              i += 1;
            }
          else
            g_string_append_c (out, text[i]);
        }
    }
}

/* The tag a paragraph's style calls for, and its outline level. */
static const char *
tag_for (W42StyleSheet *styles, const char *style)
{
  int level = w42_stylesheet_outline (styles, style);

  if (level >= 1 && level <= 6)
    {
      static const char *h[] = { "h1", "h2", "h3", "h4", "h5", "h6" };
      return h[level - 1];
    }
  if (style != NULL && g_ascii_strcasecmp (style, "Title") == 0)
    return "h1";
  return "p";
}

/* A CSS length with a full stop for its decimal point whatever the locale. */
static void
css_num (GString *css, const char *name, double value, const char *unit)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_string_append_printf (css, "%s:%s%s;", name, g_ascii_formatd (buf, sizeof buf, "%.2f", value), unit);
}

static void
write_para_style (GString *out, const W42ParaFmt *pa)
{
  GString *css = g_string_new (NULL);

  switch (pa->align)
    {
    case W42_ALIGN_CENTER:  g_string_append (css, "text-align:center;"); break;
    case W42_ALIGN_RIGHT:   g_string_append (css, "text-align:right;"); break;
    case W42_ALIGN_JUSTIFY: g_string_append (css, "text-align:justify;"); break;
    default: break;
    }
  if (pa->indent_left)
    css_num (css, "margin-left", pa->indent_left / 1440.0, "in");
  if (pa->indent_right)
    css_num (css, "margin-right", pa->indent_right / 1440.0, "in");
  if (pa->indent_first && pa->list == W42_LIST_NONE)
    css_num (css, "text-indent", pa->indent_first / 1440.0, "in");
  if (pa->space_before)
    g_string_append_printf (css, "margin-top:%dpt;", pa->space_before / 20);
  if (pa->space_after)
    g_string_append_printf (css, "margin-bottom:%dpt;", pa->space_after / 20);
  if (pa->line_spacing_pct > 0 && pa->line_spacing_pct != 100)
    g_string_append_printf (css, "line-height:%d%%;", pa->line_spacing_pct + 20);
  if (pa->border != 0)
    {
      double w = (pa->border_width > 0 ? pa->border_width : 15) / 20.0;
      if (pa->border & W42_BORDER_TOP)    css_num (css, "border-top", w, "pt solid");
      if (pa->border & W42_BORDER_BOTTOM) css_num (css, "border-bottom", w, "pt solid");
      if (pa->border & W42_BORDER_LEFT)   css_num (css, "border-left", w, "pt solid");
      if (pa->border & W42_BORDER_RIGHT)  css_num (css, "border-right", w, "pt solid");
      g_string_append (css, "padding:2pt;");
    }
  if (pa->shading > 0)
    {
      int g = 255 - pa->shading * 255 / 100;
      g_string_append_printf (css, "background:rgb(%d,%d,%d);", g, g, g);
    }
  if (pa->page_break_before)
    g_string_append (css, "page-break-before:always;");

  if (css->len > 0)
    g_string_append_printf (out, " style=\"%s\"", css->str);
  g_string_free (css, TRUE);
}

static void
write_run (GString *out, W42PieceTable *pt, const W42Block *block,
           const W42Run *run, const W42CharFmt *ch, const W42CharFmt *base)
{
  GString *css = g_string_new (NULL);
  gboolean span;

  if (run->object != W42_OBJECT_NONE)
    {
      const W42Object *object = w42_object_table_get (w42_pt_object_table (pt), run->object);
      GBytes *png;

      if (object == NULL)
        return;
      png = w42_image_to_png (object->data);
      if (png != NULL)
        {
          char *b64 = g_base64_encode (g_bytes_get_data (png, NULL), g_bytes_get_size (png));

          g_string_append_printf (out, "<img src=\"data:image/png;base64,%s\" "
                                  "width=\"%d\" height=\"%d\" alt=\"\"%s>",
                                  b64, (int) (object->width / 15.0), (int) (object->height / 15.0),
                                  object->wrap == W42_WRAP_LEFT ? " style=\"float:left;margin:0 0.125in 0.125in 0\""
                                  : object->wrap == W42_WRAP_RIGHT ? " style=\"float:right;margin:0 0 0.125in 0.125in\"" : "");
          g_free (b64);
          g_bytes_unref (png);
        }
      g_string_free (css, TRUE);
      return;
    }

  if (run->footnote > 0)
    {
      char label[16];

      if (run->endnote)
        w42_roman_lower (run->footnote, label, sizeof label);
      else
        g_snprintf (label, sizeof label, "%d", run->footnote);
      g_string_append_printf (out, "<sup id=\"ref%s%d\"><a href=\"#note%s%d\">%s</a></sup>",
                              run->endnote ? "e" : "", run->footnote,
                              run->endnote ? "e" : "", run->footnote, label);
      g_string_free (css, TRUE);
      return;
    }

  if (ch->family != base->family && ch->family != NULL)
    {
      /* The name goes inside quotes inside an attribute: no quotes of
       * either kind, and the markup characters escaped. */
      g_string_append (css, "font-family:'");
      for (const char *p = ch->family; *p; p++)
        if (*p == '&') g_string_append (css, "&amp;");
        else if (*p == '<') g_string_append (css, "&lt;");
        else if (*p != '\'' && *p != '"') g_string_append_c (css, *p);
      g_string_append (css, "';");
    }
  if (ch->size != base->size)
    g_string_append_printf (css, "font-size:%dpt;", ch->size / 2);
  if (ch->color != 0 && ch->link == NULL)
    g_string_append_printf (css, "color:#%06x;", ch->color);
  if (ch->highlight)
    g_string_append_printf (css, "background:#%06x;", w42_highlight_rgb (ch->highlight));
  if (ch->smallcaps)
    g_string_append (css, "font-variant:small-caps;");
  if (ch->allcaps)
    g_string_append (css, "text-transform:uppercase;");
  if (ch->script > 0)
    g_string_append (css, "vertical-align:super;font-size:smaller;");
  if (ch->script < 0)
    g_string_append (css, "vertical-align:sub;font-size:smaller;");
  if (ch->spacing)
    css_num (css, "letter-spacing", ch->spacing / 20.0, "pt");

  if (ch->comment != NULL)
    {
      g_string_append (css, "background:#fff5b0;");
      g_string_append (out, "<span title=\"");
      append_escaped (out, ch->comment, strlen (ch->comment));
      g_string_append (out, "\">");
    }
  span = css->len > 0;
  if (ch->link != NULL)
    {
      g_string_append (out, "<a href=\"");
      append_escaped (out, ch->link, strlen (ch->link));
      g_string_append (out, "\">");
    }
  if (span)
    g_string_append_printf (out, "<span style=\"%s\">", css->str);
  if (ch->bold)      g_string_append (out, "<b>");
  if (ch->italic)    g_string_append (out, "<i>");
  if (ch->underline && ch->link == NULL)
    {
      static const char *const CSS[] = {
        "", "", "double", "", "dotted", "dashed", "solid", "wavy"
      };
      guint kind = MIN (ch->underline, G_N_ELEMENTS (CSS) - 1);

      if (*CSS[kind] != '\0')
        g_string_append_printf (out, "<u style=\"text-decoration-style:%s%s\">", CSS[kind],
                                ch->underline == W42_UNDERLINE_THICK
                                  ? ";text-decoration-thickness:2px" : "");
      else
        g_string_append (out, "<u>");
    }
  if (ch->strikeout) g_string_append (out, "<s>");
  if (ch->overline)  g_string_append (out, "<span style=\"text-decoration:overline\">");
  if (ch->revision == 1) g_string_append (out, "<ins>");
  if (ch->revision == 2) g_string_append (out, "<del>");

  append_escaped (out, block->text->str + run->byte_offset, run->n_bytes);

  if (ch->revision == 2) g_string_append (out, "</del>");
  if (ch->revision == 1) g_string_append (out, "</ins>");
  if (ch->overline)  g_string_append (out, "</span>");
  if (ch->strikeout) g_string_append (out, "</s>");
  if (ch->underline && ch->link == NULL) g_string_append (out, "</u>");
  if (ch->italic)    g_string_append (out, "</i>");
  if (ch->bold)      g_string_append (out, "</b>");
  if (span)
    g_string_append (out, "</span>");
  if (ch->link != NULL)
    g_string_append (out, "</a>");
  if (ch->comment != NULL)
    g_string_append (out, "</span>");

  g_string_free (css, TRUE);
}

static void
write_block_body (GString *out, W42PieceTable *pt, W42ApTable *aps,
                  const W42Block *block, const W42CharFmt *base)
{
  if (block->runs->len == 0)
    g_string_append (out, "&nbsp;");

  for (guint r = 0; r < block->runs->len; r++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, r);
      const W42Fmt *fmt = w42_ap_table_get (aps, run->ap);

      write_run (out, pt, block, run, &fmt->ch, base);
    }
}

gboolean
w42_html_export (W42PieceTable *pt, const W42PageSetup *page, GFile *file, GError **error)
{
  GString *out = g_string_new (NULL);
  GPtrArray *blocks;
  W42ApTable *aps;
  W42StyleSheet *styles;
  W42Fmt base;
  const W42ParaFmt *prev_pa = NULL;
  int list_stack[9];
  int list_depth = 0;
  int table_open = -1, row_open = -1;
  gboolean ok;
  char *title;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  blocks = w42_pt_snapshot_blocks (pt);
  aps = w42_pt_ap_table (pt);
  styles = w42_pt_stylesheet (pt);
  w42_fmt_init_default (&base);
  for (guint i = 0; i < w42_stylesheet_size (styles); i++)
    {
      const W42Style *style = w42_stylesheet_get (styles, i);

      if (g_ascii_strcasecmp (style->name, "Normal") == 0)
        {
          base.ch = style->ch;
          break;
        }
    }

  title = g_file_get_basename (file);
  g_string_append (out, "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n");
  g_string_append (out, "<meta name=\"generator\" content=\"Word42\">\n<title>");
  append_escaped (out, title, strlen (title));
  g_string_append (out, "</title>\n<style>\n.dropcap::first-letter{float:left;font-size:3em;line-height:0.8;margin:0.05em 0.05em 0 0}\n");
  g_string_append_printf (out,
    "body { font-family: '%s'; font-size: %dpt; max-width: %.2fin; margin: 1em auto; padding: 0 1em; }\n",
    base.ch.family != NULL ? base.ch.family : "Times New Roman", base.ch.size / 2,
    page != NULL ? (page->width - page->margin_left - page->margin_right) / 1440.0 : 6.5);
  g_string_append (out,
    "p { margin: 0; }\nh1, h2, h3 { margin: 0.5em 0 0.25em; }\n"
    "table { border-collapse: collapse; }\ntd { border: 1px solid #000; padding: 2pt 4pt; vertical-align: top; }\n"
    "a { color: #000080; }\n.notes { margin-top: 1em; border-top: 1px solid #000; width: 33%; padding-top: 0.5em; }\n"
    ".note { font-size: smaller; }\n");
  g_string_append (out, "</style>\n</head>\n<body>\n");
  g_free (title);

  /* The body: every paragraph that is not a footnote's. */
  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42Fmt *fmt = w42_ap_table_get (aps, block->ap);
      const W42ParaFmt *pa = &fmt->pa;
      const W42Block *next = b + 1 < blocks->len ? g_ptr_array_index (blocks, b + 1) : NULL;

      if (block->note >= 0)
        continue;

      /* Lists: <ul> and <ol> nested by level, one open list per level.
       * This comes before the table below, so a list that a table
       * follows is closed while we are still outside the table. */
      {
        int want = pa->list != W42_LIST_NONE && block->table < 0 && block->note < 0
                     ? MIN (pa->list_level, 8) + 1 : 0;

        while (list_depth > want ||
               (list_depth > 0 && list_depth == want &&
                (list_stack[list_depth - 1] != pa->list ||
                 (pa->list_start > 0 && w42_list_is_numbered (pa->list)))))
          {
            list_depth--;
            g_string_append (out, w42_list_is_bullet (list_stack[list_depth]) ? "</ul>\n" : "</ol>\n");
          }
        while (list_depth < want)
          {
            if (w42_list_is_bullet (pa->list))
              {
                const char *style = pa->list == W42_LIST_BULLET_CIRCLE ? "circle"
                                  : pa->list == W42_LIST_BULLET_SQUARE ? "square"
                                  : pa->list == W42_LIST_BULLET_DASH ? "'\\2013  '" : NULL;

                if (style != NULL)
                  g_string_append_printf (out, "<ul style=\"list-style-type:%s\">\n", style);
                else
                  g_string_append (out, "<ul>\n");
              }
            else
              {
                const char *type = pa->list == W42_LIST_LOWER_LETTER ? "a"
                                 : pa->list == W42_LIST_UPPER_LETTER ? "A"
                                 : pa->list == W42_LIST_LOWER_ROMAN ? "i"
                                 : pa->list == W42_LIST_UPPER_ROMAN ? "I" : NULL;

                g_string_append (out, "<ol");
                if (type != NULL)
                  g_string_append_printf (out, " type=\"%s\"", type);
                if (pa->list_start > 0 && list_depth + 1 == want)
                  g_string_append_printf (out, " start=\"%d\"", pa->list_start);
                g_string_append (out, ">\n");
              }
            list_stack[list_depth++] = pa->list;
          }
      }

      /* Tables: open and close rows and the table around the cells. */
      if (block->table >= 0 && block->table != table_open)
        {
          g_string_append (out, "<table>\n");
          table_open = block->table;
          row_open = -1;
        }
      if (block->table >= 0 && block->row != row_open)
        {
          if (row_open >= 0)
            g_string_append (out, "</tr>\n");
          g_string_append (out, "<tr>");
          row_open = block->row;
        }

      if (block->table >= 0)
        {
          gboolean cell_start = prev_pa == NULL || b == 0 ||
            ((const W42Block *) g_ptr_array_index (blocks, b - 1))->table != block->table ||
            ((const W42Block *) g_ptr_array_index (blocks, b - 1))->row != block->row ||
            ((const W42Block *) g_ptr_array_index (blocks, b - 1))->col != block->col;
          gboolean cell_end = next == NULL || next->table != block->table ||
            next->row != block->row || next->col != block->col;

          if (cell_start)
            {
              if (block->span > 1)
                g_string_append_printf (out, "<td colspan=\"%d\">", block->span);
              else
                g_string_append (out, "<td>");
            }
          g_string_append (out, "<p");
          if (pa->rtl)
            g_string_append (out, " dir=\"rtl\"");
          write_para_style (out, pa);
          g_string_append (out, ">");
          write_block_body (out, pt, aps, block, &base.ch);
          g_string_append (out, "</p>");
          if (cell_end)
            g_string_append (out, "</td>");
        }
      else if (pa->list != W42_LIST_NONE)
        {
          g_string_append (out, "<li");
          write_para_style (out, pa);
          g_string_append (out, ">");
          write_block_body (out, pt, aps, block, &base.ch);
          g_string_append (out, "</li>\n");
        }
      else
        {
          const char *tag = tag_for (styles, pa->style);

          if (pa->frame_side != W42_FRAME_NONE)
            g_string_append_printf (out, "<div style=\"float:%s;width:%.2fin;margin:0 %s\">",
                                    pa->frame_side == W42_FRAME_LEFT ? "left" : "right",
                                    (pa->frame_width > 0 ? pa->frame_width : 3120) / 1440.0,
                                    pa->frame_side == W42_FRAME_LEFT ? "0.125in 0.125in 0" : "0 0.125in 0.125in");
          g_string_append_printf (out, "<%s", tag);
          if (pa->rtl)
            g_string_append (out, " dir=\"rtl\"");
          if (pa->drop_cap > 0)
            g_string_append (out, " class=\"dropcap\"");
          write_para_style (out, pa);
          g_string_append (out, ">");
          write_block_body (out, pt, aps, block, &base.ch);
          g_string_append_printf (out, "</%s>\n", tag);
          if (pa->frame_side != W42_FRAME_NONE)
            g_string_append (out, "</div>\n");
        }

      /* The table closes after its last cell. */
      if (block->table >= 0 && (next == NULL || next->table != block->table))
        {
          g_string_append (out, "</tr>\n</table>\n");
          table_open = -1;
          row_open = -1;
        }
      prev_pa = pa;
    }
  while (list_depth > 0)
    {
      list_depth--;
      g_string_append (out, w42_list_is_bullet (list_stack[list_depth]) ? "</ul>\n" : "</ol>\n");
    }

  /* The footnotes, in order, at the end. */
  {
    gboolean any = FALSE;
    int last_note = -1;

    for (guint b = 0; b < blocks->len; b++)
      {
        const W42Block *block = g_ptr_array_index (blocks, b);

        if (block->note < 0)
          continue;
        if (!any)
          {
            g_string_append (out, "<div class=\"notes\">\n");
            any = TRUE;
          }
        g_string_append (out, "<p class=\"note\"");
        if (block->note != last_note)
          g_string_append_printf (out, " id=\"note%s%d\"", block->note_end ? "e" : "",
                                  block->note_number);
        g_string_append (out, ">");
        if (block->note != last_note)
          {
            char label[16];

            if (block->note_end)
              w42_roman_lower (block->note_number, label, sizeof label);
            else
              g_snprintf (label, sizeof label, "%d", block->note_number);
            g_string_append_printf (out, "<sup><a href=\"#ref%s%d\">%s</a></sup> ",
                                    block->note_end ? "e" : "", block->note_number, label);
          }
        write_block_body (out, pt, aps, block, &base.ch);
        g_string_append (out, "</p>\n");
        last_note = block->note;
      }
    if (any)
      g_string_append (out, "</div>\n");
  }

  g_string_append (out, "</body>\n</html>\n");

  ok = g_file_replace_contents (file, out->str, out->len, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);

  g_string_free (out, TRUE);
  g_ptr_array_free (blocks, TRUE);
  return ok;
}
