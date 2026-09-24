/* w42-html.c - see w42-html.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-html.h"

#include <string.h>

#include "w42-image.h"
#include "w42-lang.h"

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
        case '\t': g_string_append (out, "<span class=\"tab\">\t</span>"); break;
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
          else if ((guchar) text[i] == 0xC2 && i + 1 < len && (guchar) text[i + 1] == 0xA0)
            {
              g_string_append (out, "&nbsp;");
              i += 1;
            }
          else
            g_string_append_c (out, text[i]);
        }
    }
}

/* Text for an attribute's value or the <title>, where markup is not
 * markup: a tab or a line break is a character reference, not the span
 * or the <br> the body gets, whose quotes would end the attribute. */
static void
append_attr (GString *out, const char *text)
{
  for (const char *p = text; *p != '\0'; p++)
    switch (*p)
      {
      case '<':  g_string_append (out, "&lt;"); break;
      case '>':  g_string_append (out, "&gt;"); break;
      case '&':  g_string_append (out, "&amp;"); break;
      case '"':  g_string_append (out, "&quot;"); break;
      case '\t': g_string_append (out, "&#9;"); break;
      case '\n': g_string_append (out, "&#10;"); break;
      case '\r': g_string_append (out, "&#13;"); break;
      default:   g_string_append_c (out, *p);
      }
}

gboolean
w42_html_link_is_script (const char *href)
{
  char scheme[16];
  gsize n = 0;

  g_return_val_if_fail (href != NULL, FALSE);

  /* A browser skips the spaces and control characters before a URL and
   * the tabs and line breaks inside one, so "java\tscript:" is what it
   * runs; the scheme is read the way it reads it. */
  while (*href != '\0' && (guchar) *href <= ' ')
    href++;
  for (; *href != '\0' && *href != ':' && n < sizeof scheme - 1; href++)
    if (*href != '\t' && *href != '\n' && *href != '\r')
      scheme[n++] = g_ascii_tolower (*href);
  scheme[n] = '\0';
  return *href == ':' && (g_str_equal (scheme, "javascript") ||
                          g_str_equal (scheme, "vbscript") ||
                          g_str_equal (scheme, "data"));
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

/* The name goes inside quotes inside an attribute or a stylesheet: no
 * quotes of either kind, and the markup characters escaped, or a font
 * called "</style><script>" would be exactly that in the output.  In a
 * <style> element nothing is unescaped, so there a "<" is left out
 * rather than written as "&lt;", which would be read back as its name. */
static void
append_family (GString *css, const char *family, gboolean in_sheet)
{
  g_string_append (css, "font-family:'");
  for (const char *p = family; *p; p++)
    if (*p == '<' && in_sheet) continue;
    else if (*p == '&' && !in_sheet) g_string_append (css, "&amp;");
    else if (*p == '<') g_string_append (css, "&lt;");
    else if (*p != '\'' && *p != '"') g_string_append_c (css, *p);
  g_string_append (css, "';");
}

/* Whether white space may go after a list tag at `depth` lists deep:
 * not inside an item of the list round it, where it would be the item's
 * text, since that keeps its white space. */
static gboolean
list_breaks (const gboolean *li_open, int depth)
{
  return depth == 0 || !li_open[depth - 1];
}

/* A paragraph's style attribute, with `extra` declarations first when
 * there are any.  `style` is the paragraph's style when its tag stands
 * for one -- a heading's, which a reader applies whole and the page's
 * stylesheet gives margins of its own -- so that what the paragraph is
 * is said in full: its left alignment, its nought indents, its single
 * spacing and its margins.  The reader's heading need not be this
 * document's, so nothing can be left to it. */
static void
write_para_style (GString *out, const W42ParaFmt *pa, const W42ParaFmt *style,
                  const char *extra)
{
  GString *css = g_string_new (extra);

  switch (pa->align)
    {
    case W42_ALIGN_CENTER:  g_string_append (css, "text-align:center;"); break;
    case W42_ALIGN_RIGHT:   g_string_append (css, "text-align:right;"); break;
    case W42_ALIGN_JUSTIFY: g_string_append (css, "text-align:justify;"); break;
    default:
      if (style != NULL)
        g_string_append (css, "text-align:left;");
      break;
    }
#define SAY(field) (pa->field != 0 || style != NULL)
  /* In points, which hold a twip to two places; an inch to two places
   * is fourteen twips out. */
  if (SAY (indent_left))
    css_num (css, "margin-left", pa->indent_left / 20.0, "pt");
  if (SAY (indent_right))
    css_num (css, "margin-right", pa->indent_right / 20.0, "pt");
  if (SAY (indent_first) && pa->list == W42_LIST_NONE)
    css_num (css, "text-indent", pa->indent_first / 20.0, "pt");
  if (pa->space_before || style != NULL)
    css_num (css, "margin-top", pa->space_before / 20.0, "pt");
  if (pa->space_after || style != NULL)
    css_num (css, "margin-bottom", pa->space_after / 20.0, "pt");
#undef SAY
  if (pa->line_spacing_pct > 0 && pa->line_spacing_pct != 100)
    {
      /* A browser's line-height is a multiple of the type size; Word's is a
       * multiple of the line, which is about a fifth taller.  The browser
       * gets the taller figure so the page looks right, and the figure the
       * document actually holds rides along in a custom property, which
       * browsers ignore and this reader does not -- otherwise a document
       * saved twice would grow a fifth taller each time. */
      int pct = MIN (pa->line_spacing_pct, 10000);

      g_string_append_printf (css, "line-height:%d%%;", pct + 20);
      g_string_append_printf (css, "--w42-line-height:%d%%;", pct);
    }
  else if (pa->line_spacing > 0)
    css_num (css, "line-height", pa->line_spacing / 20.0, "pt");
  else if (style != NULL)
    g_string_append (css, "line-height:normal;");
  if (pa->border != 0)
    {
      static const char *names[4] = { "border-top", "border-bottom", "border-left", "border-right" };

      for (int i = 0; i < 4; i++)
        if (pa->border & (1 << i))
          {
            const W42BorderEdge *edge = &pa->edge[i];
            char unit[32];

            g_snprintf (unit, sizeof unit, "pt %s #%06x",
                        w42_border_style_css (edge->style), edge->color & 0xFFFFFF);
            css_num (css, names[i], W42_EDGE_WIDTH (edge) / 20.0, unit);
          }
      g_string_append (css, "padding:2pt;");
    }
  if (pa->has_shading_color)
    g_string_append_printf (css, "background:#%06x;", pa->shading_color & 0xFFFFFF);
  else if (pa->shading > 0)
    {
      int g = 255 - MIN (pa->shading, 100) * 255 / 100;
      g_string_append_printf (css, "background:rgb(%d,%d,%d);", g, g, g);
    }
  if (pa->page_break_before)
    g_string_append (css, "break-before:page;page-break-before:always;");
  if (pa->drop_cap > 0)
    g_string_append_printf (css, "--w42-drop-cap:%d;", pa->drop_cap);

  if (css->len > 0)
    g_string_append_printf (out, " style=\"%s\"", css->str);
  g_string_free (css, TRUE);
}

/* `base` is the body's type, which the page's stylesheet sets and the
 * run's font and size are measured against; `style` is the heading's
 * formatting when the run is in one, which a reader gives it whole, so
 * the run says all it is.  `bookmarks` holds the bookmarks whose id the
 * page has already given. */
static void
write_run (GString *out, W42PieceTable *pt, const W42Block *block,
           const W42Run *run, const W42CharFmt *ch, const W42CharFmt *base,
           const W42CharFmt *style, GHashTable *bookmarks)
{
  GString *css = g_string_new (NULL);
  gboolean span;
  /* A link that would run a script when the page is opened is not
   * written as one: a document from anywhere can carry one. */
  const char *link = ch->link != NULL && !w42_html_link_is_script (ch->link) ? ch->link : NULL;

  if (run->object != W42_OBJECT_NONE)
    {
      const W42Object *object = w42_object_table_get (w42_pt_object_table (pt), run->object);
      const char *mime = "image/png";
      GBytes *png;

      if (object == NULL)
        return;
      if (ch->family != NULL && (ch->family != base->family || style != NULL))
        append_family (css, ch->family, FALSE);
      if (ch->size != base->size || style != NULL)
        css_num (css, "font-size", ch->size / 2.0, "pt");
      if (ch->color != 0)
        g_string_append_printf (css, "color:#%06x;", ch->color & 0xFFFFFF);
      png = w42_image_for_container (object->data, NULL, &mime);
      if (png != NULL)
        {
          char *b64 = g_base64_encode (g_bytes_get_data (png, NULL), g_bytes_get_size (png));

          char iw[G_ASCII_DTOSTR_BUF_SIZE], ih[G_ASCII_DTOSTR_BUF_SIZE];

          span = css->len > 0 || ch->lang != NULL;
          if (span)
            {
              g_string_append (out, "<span");
              if (ch->lang != NULL)
                {
                  g_string_append (out, " lang=\"");
                  append_attr (out, ch->lang);
                  g_string_append_c (out, '"');
                }
              if (css->len > 0)
                g_string_append_printf (out, " style=\"%s\"", css->str);
              g_string_append_c (out, '>');
            }

          g_string_append_printf (out, "<img src=\"data:%s;base64,%s\" "
                                  "width=\"%d\" height=\"%d\" alt=\"\""
                                  " style=\"width:%sin;height:%sin;%s\">",
                                  mime, b64, (int) (object->width / 15.0), (int) (object->height / 15.0),
                                  g_ascii_formatd (iw, sizeof iw, "%.5f", object->width / 1440.0),
                                  g_ascii_formatd (ih, sizeof ih, "%.5f", object->height / 1440.0),
                                  object->wrap == W42_WRAP_LEFT ? "float:left;margin:0 0.125in 0.125in 0"
                                  : object->wrap == W42_WRAP_RIGHT ? "float:right;margin:0 0 0.125in 0.125in" : "");
          if (span)
            g_string_append (out, "</span>");
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
      g_string_append_printf (out, "<sup id=\"ref%s%d\"", run->endnote ? "e" : "", run->footnote);
      if (ch->lang != NULL)
        {
          g_string_append (out, " lang=\"");
          append_attr (out, ch->lang);
          g_string_append_c (out, '"');
        }
      g_string_append_printf (out, "><a href=\"#note%s%d\">%s</a></sup>",
                              run->endnote ? "e" : "", run->footnote, label);
      g_string_free (css, TRUE);
      return;
    }

  if (ch->family != NULL && (ch->family != base->family || style != NULL))
    append_family (css, ch->family, FALSE);
  if (ch->size != base->size || style != NULL)
    css_num (css, "font-size", ch->size / 2.0, "pt");
  if ((ch->color != 0 || style != NULL) && link == NULL)
    g_string_append_printf (css, "color:#%06x;", ch->color & 0xFFFFFF);
  if (ch->highlight)
    g_string_append_printf (css, "background:#%06x;", w42_highlight_rgb (ch->highlight));
  if (ch->smallcaps)
    g_string_append (css, "font-variant:small-caps;");
  else if (style != NULL)
    g_string_append (css, "font-variant:normal;");
  if (ch->allcaps)
    g_string_append (css, "text-transform:uppercase;");
  else if (style != NULL)
    g_string_append (css, "text-transform:none;");
  if (style != NULL)
    {
      /* A heading is bold to a browser as well as to its style, and to a
       * reader it is its style as the reader's own sheet has it, which
       * need not be this document's: so a run in one says all it is. */
      if (!ch->bold)
        g_string_append (css, "font-weight:normal;");
      if (!ch->italic)
        g_string_append (css, "font-style:normal;");
      if (!ch->underline && link == NULL)
        g_string_append (css, "text-decoration:none;");
    }
  if (ch->spacing)
    css_num (css, "letter-spacing", ch->spacing / 20.0, "pt");
  /* Word 97's effects, as far as CSS can say them: a shadow is one, the
   * relief is a white letter with a shadow to one side or the other,
   * and an outline is a stroke round a letter with no fill. */
  if (ch->emboss || ch->engrave)
    g_string_append_printf (css, "color:#ffffff;text-shadow:%s #808080;",
                            ch->emboss ? "1px 1px" : "-1px -1px");
  else if (ch->shadow)
    g_string_append (css, "text-shadow:1px 1px #808080;");
  if (ch->outline)
    g_string_append_printf (css, "-webkit-text-stroke:0.5px #%06x;-webkit-text-fill-color:transparent;",
                            ch->color & 0xFFFFFF);

  if (ch->comment != NULL)
    {
      /* An annotation is shown by its class's colour, which the run's own
       * highlight inside it wins over; its text is the span's title. */
      g_string_append (out, "<span class=\"comment\" title=\"");
      append_attr (out, ch->comment);
      g_string_append (out, "\">");
    }
  span = css->len > 0 || ch->lang != NULL;
  if (link != NULL || ch->bookmark != NULL)
    {
      /* A bookmark is an anchor with an id, as HTML has it.  An id is one
       * place, so it goes on the bookmark's first run only, and its later
       * runs -- in this paragraph or another -- say whose they are. */
      g_string_append (out, "<a");
      if (ch->bookmark != NULL && !g_hash_table_contains (bookmarks, ch->bookmark))
        {
          g_string_append (out, " id=\"");
          append_attr (out, ch->bookmark);
          g_string_append_c (out, '"');
          g_hash_table_add (bookmarks, (gpointer) ch->bookmark);
        }
      else if (ch->bookmark != NULL)
        {
          g_string_append (out, " data-w42-bookmark=\"");
          append_attr (out, ch->bookmark);
          g_string_append_c (out, '"');
        }
      if (link != NULL)
        {
          g_string_append (out, " href=\"");
          append_attr (out, link);
          g_string_append_c (out, '"');
        }
      g_string_append_c (out, '>');
    }
  if (span)
    {
      /* The language of the run, where HTML puts it. */
      g_string_append (out, "<span");
      if (ch->lang != NULL)
        {
          g_string_append (out, " lang=\"");
          append_attr (out, ch->lang);
          g_string_append_c (out, '"');
        }
      if (css->len > 0)
        g_string_append_printf (out, " style=\"%s\"", css->str);
      g_string_append_c (out, '>');
    }
  /* The elements, rather than vertical-align and a smaller size: a
   * browser sets them the same, and a reader gets the size back. */
  if (ch->script > 0) g_string_append (out, "<sup>");
  if (ch->script < 0) g_string_append (out, "<sub>");
  if (ch->bold)      g_string_append (out, "<b>");
  if (ch->italic)    g_string_append (out, "<i>");
  if (ch->underline && link == NULL)
    {
      static const char *const CSS[] = {
        "", "", "double", "", "dotted", "dashed", "solid", "wavy"
      };
      guint kind = MIN (ch->underline, G_N_ELEMENTS (CSS) - 1);

      if (*CSS[kind] != '\0')
        g_string_append_printf (out, "<u style=\"text-decoration-style:%s%s\">", CSS[kind],
                                ch->underline == W42_UNDERLINE_THICK
                                  ? ";text-decoration-thickness:2px" : "");
      else if (ch->underline == W42_UNDERLINE_WORDS)
        g_string_append (out, "<u style=\"text-decoration-skip:spaces\">");
      else
        g_string_append (out, "<u>");
    }
  if (ch->dstrike)
    g_string_append (out, "<s style=\"text-decoration-style:double\">");
  else if (ch->strikeout)
    g_string_append (out, "<s>");
  if (ch->overline)  g_string_append (out, "<span style=\"text-decoration:overline\">");
  if (ch->revision == 1) g_string_append (out, "<ins>");
  if (ch->revision == 2) g_string_append (out, "<del>");

  append_escaped (out, block->text->str + run->byte_offset, run->n_bytes);

  if (ch->revision == 2) g_string_append (out, "</del>");
  if (ch->revision == 1) g_string_append (out, "</ins>");
  if (ch->overline)  g_string_append (out, "</span>");
  if (ch->strikeout || ch->dstrike) g_string_append (out, "</s>");
  if (ch->underline && link == NULL) g_string_append (out, "</u>");
  if (ch->italic)    g_string_append (out, "</i>");
  if (ch->bold)      g_string_append (out, "</b>");
  if (ch->script < 0) g_string_append (out, "</sub>");
  if (ch->script > 0) g_string_append (out, "</sup>");
  if (span)
    g_string_append (out, "</span>");
  if (link != NULL || ch->bookmark != NULL)
    g_string_append (out, "</a>");
  if (ch->comment != NULL)
    g_string_append (out, "</span>");

  g_string_free (css, TRUE);
}

static void
write_block_body (GString *out, W42PieceTable *pt, W42ApTable *aps,
                  const W42Block *block, const W42CharFmt *base,
                  const W42CharFmt *style, GHashTable *bookmarks)
{
  if (block->runs->len == 0)
    g_string_append (out, "&nbsp;");

  for (guint r = 0; r < block->runs->len; r++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, r);
      const W42Fmt *fmt = w42_ap_table_get (aps, run->ap);

      write_run (out, pt, block, run, &fmt->ch, base, style, bookmarks);
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
  gboolean li_open[9] = { FALSE };  /* the list at that depth has an item open */
  int list_depth = 0;
  int table_open = -1, row_open = -1;
  GHashTable *bookmarks = g_hash_table_new (g_direct_hash, g_direct_equal);
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

  {
    /* What the document says about itself goes where a page says it:
     * its title in <title>, the rest in the <meta> names the reader
     * knows, so that File > Summary Info survives the trip. */
    const W42DocInfo *info = w42_pt_get_info (pt);
    static const char *const META[] = { "subject", "author", "keywords", "description" };
    const char *values[4];
    /* The page's language is the document's, when the document says
     * one; a document that leaves it to the desktop leaves the page to
     * the browser too, and comes back as it went. */
    const char *lang = base.ch.lang;

    if (info != NULL && info->title != NULL && *info->title != '\0')
      title = g_strdup (info->title);
    else
      {
        /* A page must have a title, and the file's name is the one it
         * has; in UTF-8, whatever the file system's names are in. */
        char *name = g_file_get_basename (file);

        title = g_filename_display_name (name != NULL ? name : "");
        g_free (name);
      }
    values[0] = info != NULL ? info->subject : NULL;
    values[1] = info != NULL ? info->author : NULL;
    values[2] = info != NULL ? info->keywords : NULL;
    values[3] = info != NULL ? info->comments : NULL;

    g_string_append (out, "<!DOCTYPE html>\n<html");
    if (lang != NULL && !g_str_equal (lang, W42_LANG_NONE))
      {
        g_string_append (out, " lang=\"");
        append_attr (out, lang);
        g_string_append_c (out, '"');
      }
    g_string_append (out, ">\n<head>\n<meta charset=\"utf-8\">\n");
    g_string_append (out, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n");
    g_string_append (out, "<meta name=\"generator\" content=\"Word42\">\n");
    for (guint i = 0; i < G_N_ELEMENTS (META); i++)
      if (values[i] != NULL && *values[i] != '\0')
        {
          g_string_append_printf (out, "<meta name=\"%s\" content=\"", META[i]);
          append_attr (out, values[i]);
          g_string_append (out, "\">\n");
        }
    g_string_append (out, "<title>");
    append_attr (out, title);
    g_string_append (out, "</title>\n");
  }
  g_string_append (out, "<style>\n.dropcap::first-letter{float:left;font-size:3em;line-height:0.8;margin:0.05em 0.05em 0 0}\n"
                        ".tab{white-space:pre}\n");
  g_string_append (out, "body { ");
  append_family (out, base.ch.family != NULL ? base.ch.family : "Times New Roman", TRUE);
  {
    /* Lengths with the C locale's full stop, whatever the user's: "6,50in"
     * is not a length to a browser. */
    char n[8][G_ASCII_DTOSTR_BUF_SIZE];

    g_string_append_printf (out,
      " font-size: %spt; max-width: %sin; margin: 1em auto; padding: 0 1em; }\n",
      g_ascii_formatd (n[0], sizeof n[0], "%.1f", base.ch.size / 2.0),
      g_ascii_formatd (n[1], sizeof n[1], "%.2f",
                       page != NULL ? (page->width - page->margin_left - page->margin_right) / 1440.0 : 6.5));
    if (page != NULL && page->width > 0 && page->height > 0)
      {
        g_string_append_printf (out,
          "@page { size: %sin %sin; margin: %sin %sin %sin %sin;",
          g_ascii_formatd (n[2], sizeof n[2], "%.4f", page->width / 1440.0),
          g_ascii_formatd (n[3], sizeof n[3], "%.4f", page->height / 1440.0),
          g_ascii_formatd (n[4], sizeof n[4], "%.4f", page->margin_top / 1440.0),
          g_ascii_formatd (n[5], sizeof n[5], "%.4f", page->margin_right / 1440.0),
          g_ascii_formatd (n[6], sizeof n[6], "%.4f", page->margin_bottom / 1440.0),
          g_ascii_formatd (n[7], sizeof n[7], "%.4f", page->margin_left / 1440.0));
        /* The page border, for a reader that prints pages; a browser
         * ignores it, having no pages to draw it round. */
        if (page->has_border)
          {
            char wb[G_ASCII_DTOSTR_BUF_SIZE], sb[G_ASCII_DTOSTR_BUF_SIZE];

            g_string_append_printf (out, " border: %spt %s #%06x; border-spacing: %spt;",
              g_ascii_formatd (wb, sizeof wb, "%.2f",
                               (page->border_width > 0 ? page->border_width : W42_BORDER_HAIRLINE) / 20.0),
              w42_border_style_css ((W42BorderStyle) page->border_style),
              page->border_color & 0xFFFFFF,
              g_ascii_formatd (sb, sizeof sb, "%.1f", page->border_space / 20.0));
          }
        g_string_append (out, " }\n");
      }
  }
  /* The spaces a document has are its own: a browser keeps them, and so
   * does the reader, rather than folding a run of them into one.  The
   * rules only the browser should follow -- a link's colour, the notes'
   * smaller type -- have selectors the reader passes over, which it would
   * otherwise take for the text's own formatting. */
  g_string_append (out,
    "p { margin: 0; }\nh1, h2, h3, h4, h5, h6 { margin: 0.5em 0 0.25em; }\n"
    "p, li, h1, h2, h3, h4, h5, h6 { white-space: pre-wrap; }\n"
    "table { border-collapse: collapse; }\ntd { padding: 2pt 4pt; vertical-align: top; }\n"
    "table.ruled td { border: 1px solid #000; }\n"
    "a[href] { color: #000080; }\n.comment { background: #fff5b0; }\n"
    ".notes { margin-top: 1em; border-top: 1px solid #000; width: 33%; padding-top: 0.5em; }\n"
    ".notes p { font-size: smaller; }\n");
  g_string_append (out, "</style>\n</head>\n");
  if (page != NULL && page->has_background)
    g_string_append_printf (out, "<body style=\"background: #%06x\">\n",
                            page->background & 0xFFFFFF);
  else
    g_string_append (out, "<body>\n");
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

      /* Lists: <ul> and <ol> nested by level, one open list per level,
       * a deeper list inside the item before it as HTML has it.  This
       * comes before the table below, so a list that a table follows is
       * closed while we are still outside the table. */
      {
        int want = pa->list != W42_LIST_NONE && block->table < 0 && block->note < 0
                     ? MIN (pa->list_level, 8) + 1 : 0;

        while (list_depth > want ||
               (list_depth > 0 && list_depth == want &&
                (list_stack[list_depth - 1] != pa->list ||
                 (pa->list_start > 0 && w42_list_is_numbered (pa->list)))))
          {
            list_depth--;
            if (li_open[list_depth])
              g_string_append (out, "</li>");
            li_open[list_depth] = FALSE;
            g_string_append (out, w42_list_is_bullet (list_stack[list_depth]) ? "</ul>" : "</ol>");
            if (list_breaks (li_open, list_depth))
              g_string_append_c (out, '\n');
          }
        /* The item before, at this one's level, ends here. */
        if (want > 0 && list_depth == want && li_open[want - 1])
          {
            g_string_append (out, "</li>");
            li_open[want - 1] = FALSE;
            if (list_breaks (li_open, want - 1))
              g_string_append_c (out, '\n');
          }
        while (list_depth < want)
          {
            if (w42_list_is_bullet (pa->list))
              {
                const char *style = pa->list == W42_LIST_BULLET_CIRCLE ? "circle"
                                  : pa->list == W42_LIST_BULLET_SQUARE ? "square"
                                  : pa->list == W42_LIST_BULLET_DASH ? "'\\2013  '" : NULL;

                if (style != NULL)
                  g_string_append_printf (out, "<ul style=\"list-style-type:%s\">", style);
                else
                  g_string_append (out, "<ul>");
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
                g_string_append (out, ">");
              }
            if (list_breaks (li_open, list_depth))
              g_string_append_c (out, '\n');
            li_open[list_depth] = FALSE;
            list_stack[list_depth++] = pa->list;
          }
      }

      /* Tables: open and close rows and the table around the cells. */
      if (block->table >= 0 && block->table != table_open)
        {
          const W42TableProps *tp = w42_pt_table_props (pt, block->table);

          g_string_append_printf (out, "<table%s>\n",
                                  (tp == NULL || tp->borders) ? " class=\"ruled\"" : "");
          if (tp != NULL && tp->widths != NULL)
            {
              g_string_append (out, "<colgroup>");
              for (int c = 0; c < tp->n_cols && c < (int) tp->widths->len; c++)
                {
                  int cw = g_array_index (tp->widths, int, c);

                  if (cw > 0)
                    {
                      char buf[G_ASCII_DTOSTR_BUF_SIZE];

                      g_string_append_printf (out, "<col style=\"width:%sin\">",
                                              g_ascii_formatd (buf, sizeof buf, "%.5f", cw / 1440.0));
                    }
                  else
                    g_string_append (out, "<col>");
                }
              g_string_append (out, "</colgroup>\n");
            }
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
          gboolean cell_covered = w42_ap_table_get (aps, block->cell_ap)->pa.cell_vspan == W42_CELL_COVERED;

          if (cell_start && cell_covered)
            {
              /* A cell a merge from above swallowed: the merging cell's
               * rowspan stands for it, so it is not written. */
            }
          else if (cell_start)
            {
              const W42ParaFmt *cpa = &w42_ap_table_get (aps, block->cell_ap)->pa;
              const W42TableProps *tp = w42_pt_table_props (pt, block->table);
              GString *css = g_string_new (NULL);
              static const char *names[4] = { "border-top", "border-bottom", "border-left", "border-right" };
              int sides = (cpa->border & W42_BORDER_CELL_SET) ? (cpa->border & W42_BORDER_BOX)
                        : (tp == NULL || tp->borders) ? W42_BORDER_BOX : 0;
              gboolean first_row = block->row == 0, last_row = TRUE;
              int n_cols = tp != NULL ? tp->n_cols : 1;

              for (guint k = b + 1; k < blocks->len; k++)
                {
                  const W42Block *rb = g_ptr_array_index (blocks, k);

                  if (rb->table != block->table)
                    break;
                  if (rb->row > block->row)
                    {
                      last_row = FALSE;
                      break;
                    }
                }
              /* Each side's line: the cell's own, else the table's --
               * its outer line round the outside, its inside rule
               * between the cells. */
              for (int k = 0; k < 4; k++)
                {
                  gboolean outer = (k == W42_EDGE_TOP && first_row) || (k == W42_EDGE_BOTTOM && last_row) ||
                                   (k == W42_EDGE_LEFT && block->col == 0) ||
                                   (k == W42_EDGE_RIGHT && block->col + block->span >= n_cols);
                  const W42BorderEdge *e = &cpa->edge[k];
                  W42BorderEdge edge;
                  char buf[G_ASCII_DTOSTR_BUF_SIZE];

                  if ((cpa->border & W42_BORDER_CELL_SET) && (e->width != 0 || e->style != 0 || e->color != 0))
                    edge = *e;
                  else if (tp != NULL)
                    edge = tp->edge[outer ? k : (k <= W42_EDGE_BOTTOM ? W42_EDGE_INSIDE_H : W42_EDGE_INSIDE_V)];
                  else
                    edge = (W42BorderEdge) { 0, 0, 0 };
                  if (!(sides & (1 << k)) || edge.style == W42_BORDER_NONE)
                    g_string_append_printf (css, "%s:none;", names[k]);
                  else
                    g_string_append_printf (css, "%s:%spt %s #%06x;", names[k],
                                            g_ascii_formatd (buf, sizeof buf, "%.2f", W42_EDGE_WIDTH (&edge) / 20.0),
                                            w42_border_style_css (edge.style), edge.color & 0xFFFFFF);
                }
              if (cpa->has_shading_color)
                g_string_append_printf (css, "background:#%06x;", cpa->shading_color & 0xFFFFFF);
              else if (cpa->shading > 0)
                {
                  int grey = 255 - (int) MIN (cpa->shading, 100) * 255 / 100;

                  g_string_append_printf (css, "background:rgb(%d,%d,%d);", grey, grey, grey);
                }
              if (cpa->cell_valign == W42_CELL_VALIGN_CENTER)
                g_string_append (css, "vertical-align:middle;");
              else if (cpa->cell_valign == W42_CELL_VALIGN_BOTTOM)
                g_string_append (css, "vertical-align:bottom;");
              g_string_append (out, "<td");
              if (block->span > 1)
                g_string_append_printf (out, " colspan=\"%d\"", block->span);
              if (cpa->cell_vspan > 1 && cpa->cell_vspan != W42_CELL_COVERED)
                {
                  /* No further than the table goes, whatever the mark
                   * says: a browser would make rows for the rest. */
                  int rows = w42_pt_table_rows (pt, block->table) - block->row;
                  int vspan = MIN ((int) cpa->cell_vspan, MAX (rows, 1));

                  if (vspan > 1)
                    g_string_append_printf (out, " rowspan=\"%d\"", vspan);
                }
              if (css->len > 0)
                g_string_append_printf (out, " style=\"%s\"", css->str);
              g_string_append (out, ">");
              g_string_free (css, TRUE);
            }
          if (cell_covered)
            {
              prev_pa = pa;
              if (cell_end && (next == NULL || next->table != block->table))
                {
                  g_string_append (out, "</tr>\n</table>\n");
                  table_open = -1;
                  row_open = -1;
                }
              continue;
            }
          g_string_append (out, "<p");
          if (pa->rtl)
            g_string_append (out, " dir=\"rtl\"");
          write_para_style (out, pa, NULL, NULL);
          g_string_append (out, ">");
          write_block_body (out, pt, aps, block, &base.ch, NULL, bookmarks);
          g_string_append (out, "</p>");
          if (cell_end)
            g_string_append (out, "</td>");
        }
      else if (pa->list != W42_LIST_NONE)
        {
          /* The item stays open, for a deeper list to go in; the next
           * item, or the list's end, closes it. */
          g_string_append (out, "<li");
          if (pa->rtl)
            g_string_append (out, " dir=\"rtl\"");
          write_para_style (out, pa, NULL, NULL);
          g_string_append (out, ">");
          write_block_body (out, pt, aps, block, &base.ch, NULL, bookmarks);
          li_open[list_depth - 1] = TRUE;
        }
      else
        {
          const char *tag = tag_for (styles, pa->style);
          const W42Style *hstyle = tag[0] == 'h' ? w42_stylesheet_find (styles, pa->style) : NULL;
          char frame[128] = "";

          /* A framed paragraph floats at its side of the column, the
           * text after it running down the other; the gap to the text
           * is padding, which is no indent when it is read back. */
          if (pa->frame_side != W42_FRAME_NONE)
            {
              char fw[G_ASCII_DTOSTR_BUF_SIZE];

              g_snprintf (frame, sizeof frame, "float:%s;width:%sin;padding:0 %s;",
                          pa->frame_side == W42_FRAME_LEFT ? "left" : "right",
                          g_ascii_formatd (fw, sizeof fw, "%.2f",
                                           (pa->frame_width > 0 ? pa->frame_width : 3120) / 1440.0),
                          pa->frame_side == W42_FRAME_LEFT ? "0.125in 0.125in 0" : "0 0.125in 0.125in");
            }
          g_string_append_printf (out, "<%s", tag);
          if (pa->rtl)
            g_string_append (out, " dir=\"rtl\"");
          if (pa->drop_cap > 0 || (pa->style != NULL && g_ascii_strcasecmp (pa->style, "Title") == 0))
            g_string_append_printf (out, " class=\"%s%s%s\"",
                                    pa->drop_cap > 0 ? "dropcap" : "",
                                    pa->drop_cap > 0 && pa->style != NULL && g_ascii_strcasecmp (pa->style, "Title") == 0 ? " " : "",
                                    pa->style != NULL && g_ascii_strcasecmp (pa->style, "Title") == 0 ? "title" : "");
          write_para_style (out, pa, hstyle != NULL ? &hstyle->pa : NULL,
                            *frame != '\0' ? frame : NULL);
          g_string_append (out, ">");
          write_block_body (out, pt, aps, block, &base.ch,
                            hstyle != NULL ? &hstyle->ch : NULL, bookmarks);
          g_string_append_printf (out, "</%s>\n", tag);
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
      if (li_open[list_depth])
        g_string_append (out, "</li>");
      li_open[list_depth] = FALSE;
      g_string_append (out, w42_list_is_bullet (list_stack[list_depth]) ? "</ul>" : "</ol>");
      if (list_breaks (li_open, list_depth))
        g_string_append_c (out, '\n');
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
        write_para_style (out, &w42_ap_table_get (aps, block->ap)->pa, NULL, NULL);
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
        write_block_body (out, pt, aps, block, &base.ch, NULL, bookmarks);
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
  g_hash_table_destroy (bookmarks);
  return ok;
}
