/* w42-htmlin.c - see w42-htmlin.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The page is parsed by Lexbor, the HTML5 parser the browsers' own rules
 * specify, and this file walks the tree it builds: tokenizing hostile
 * bytes is a solved problem, and turning elements into a document is the
 * part that is word42's own.
 */

#include "w42-htmlin.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "w42-image.h"
#include "w42-lang.h"

/* ---------------------------------------------------------------------- */
/* The writer: text goes in at `pos` with the formatting on the stack      */
/* ---------------------------------------------------------------------- */

#define MAX_DEPTH 64

typedef struct {
  W42PieceTable *pt;
  W42PageSetup  *page;              /* the page, for what the body says */
  gsize          pos;

  W42CharFmt     ch[MAX_DEPTH];     /* a stack: inline elements push */
  int            depth;
  W42ParaFmt     pa;                /* the paragraph being built */
  gboolean       pa_dirty;          /* something was set on it */

  GString       *pending;           /* text not yet inserted */
  gboolean       in_para;           /* a paragraph has content */
  gboolean       space_pending;     /* collapsed whitespace to emit */
  gboolean       at_para_start;

  int            list_kind;         /* W42_LIST_* for <li> */
  int            list_kinds[9];     /* the kind at each open depth */
  int            list_start;        /* <ol start>: the first <li> restarts there */
  int            list_depth;

  int            table;             /* the table being read, or -1 */
  int            tables_opened;     /* how many the page has had so far */
  int            table_row, table_col, table_cols;
  int            covered[1024];   /* rows still covered from above, per column */
  int            cell_span;       /* the open cell's columns */
  int            col_widths[1023];  /* what its <colgroup> said, in twips */
  int            n_col_widths;
  gboolean       in_cell;
  gboolean       table_before_block;

  char          *base;              /* the page's own directory, for its pictures */
  GHashTable    *notes;             /* note id -> its text, found before the body */
  const char    *pending_bookmark;  /* an empty <a name>: a place, not a run */
  char          *meta[5];           /* title, subject, author, keywords, comments */
  int            pre_depth;         /* inside <pre>: whitespace kept */
  gboolean       cell_break_pending; /* a paragraph ended in a cell; the
                                      * next text starts a new one */
} Html;

static W42ApIdx
html_ap (Html *h)
{
  W42Fmt fmt;

  w42_fmt_init_default (&fmt);
  fmt.ch = h->ch[h->depth];
  fmt.pa = h->pa;
  return w42_ap_table_intern (w42_pt_ap_table (h->pt), &fmt);
}

static void
flush_text (Html *h)
{
  if (h->pending->len == 0)
    return;

  /* In a cell, a paragraph that ended waits for more text before its
   * successor is made, so a cell never ends with an empty one. */
  if (h->table >= 0 && h->in_cell && h->cell_break_pending)
    {
      w42_pt_insert_block (h->pt, h->pos, html_ap (h));
      h->pos += 1;
      h->cell_break_pending = FALSE;
    }

  w42_pt_insert_text (h->pt, h->pos, h->pending->str, html_ap (h));
  {
    gsize n = g_utf8_strlen (h->pending->str, -1);

    /* An empty <a name="..."> marks a place rather than a run, so the
     * bookmark goes on the text that follows it. */
    if (h->pending_bookmark != NULL && n > 0)
      {
        W42CharFmt want;

        memset (&want, 0, sizeof want);
        want.bookmark = h->pending_bookmark;
        w42_pt_apply_char_fmt (h->pt, h->pos, n, W42_CHAR_BOOKMARK, &want);
        h->pending_bookmark = NULL;
      }
    h->pos += n;
  }
  g_string_truncate (h->pending, 0);
  h->in_para = TRUE;
}

static void
push_char (Html *h)
{
  if (h->depth + 1 < MAX_DEPTH)
    {
      h->ch[h->depth + 1] = h->ch[h->depth];
      h->depth++;
    }
}

static void
pop_char (Html *h)
{
  if (h->depth > 0)
    h->depth--;
}

/* Text arrives in pieces; whitespace collapses to one space, and never
 * starts a paragraph. */
static void
add_text (Html *h, const char *text, gsize len)
{
  for (gsize i = 0; i < len; )
    {
      gunichar c = g_utf8_get_char_validated (text + i, len - i);
      gsize n;

      if (c == (gunichar) -1 || c == (gunichar) -2)
        {
          i++;
          continue;
        }
      n = g_utf8_skip[(guchar) text[i]];

      /* A no-break space is a character an author chose, not whitespace
       * to be collapsed: it is kept as it is. */

      if (h->pre_depth == 0 && (c == ' ' || c == '\t' || c == '\n' || c == '\r'))
        {
          if (!h->at_para_start)
            h->space_pending = TRUE;
        }
      else
        {
          if (h->pre_depth > 0 && c == '\n')
            {
              g_string_append_unichar (h->pending, 0x2028);
            }
          else
            {
              if (h->space_pending)
                g_string_append_c (h->pending, ' ');
              g_string_append_unichar (h->pending, c);
            }
          h->space_pending = FALSE;
          h->at_para_start = FALSE;
        }
      i += n;
    }
}

/* Ends the paragraph being built: its mark takes its formatting and the
 * next one begins. */
static void
end_paragraph (Html *h)
{
  flush_text (h);

  if (!h->in_para && !h->pa_dirty)
    {
      h->at_para_start = TRUE;
      h->space_pending = FALSE;
      return;
    }

  w42_pt_apply_para_fmt (h->pt, h->pos > 0 ? h->pos - 1 : 0, 0,
                         W42_PARA_ALL, &h->pa);

  if (h->table >= 0 && h->in_cell)
    {
      /* Another paragraph in the same cell, when more text comes. */
      h->cell_break_pending = TRUE;
    }
  else
    {
      w42_pt_insert_block (h->pt, h->pos, html_ap (h));
      h->pos += 1;
    }

  h->in_para = FALSE;
  h->pa_dirty = FALSE;
  h->at_para_start = TRUE;
  h->space_pending = FALSE;
  {
    W42Fmt def;
    w42_fmt_init_default (&def);
    h->pa = def.pa;
  }
}

/* ---- tables ----------------------------------------------------------- */

/* A cell a rowspan from above covers: it holds nothing, and is marked
 * so that the layout draws the covering cell over it. */
static void
open_covered_cell (Html *h)
{
  w42_pt_insert_cell (h->pt, h->pos, h->table, h->table_row, h->table_col,
                      html_ap (h));
  w42_pt_set_cell_vspan (h->pt, h->pos, W42_CELL_COVERED);
  h->pos += 2;
  h->covered[h->table_col]--;
  h->table_col++;
}

static void
open_cell_spanning (Html *h, int colspan, int rowspan)
{
  if (h->table < 0 || h->in_cell)
    return;

  flush_text (h);
  /* Columns a cell above still covers come first. */
  while (h->table_col < h->table_cols && h->table_col < 1024 && h->covered[h->table_col] > 0)
    open_covered_cell (h);
  /* More cells than the row definition has, or rows than the marks can
   * hold: dropped, as the other readers drop them. */
  if (h->table_col >= h->table_cols || h->table_row > 4095)
    {
      h->in_cell = TRUE;
      h->cell_span = 1;
      return;
    }
  colspan = CLAMP (colspan, 1, h->table_cols - h->table_col);
  w42_pt_insert_cell (h->pt, h->pos, h->table, h->table_row, h->table_col,
                      html_ap (h));
  if (colspan > 1)
    w42_pt_set_cell_span (h->pt, h->pos, colspan);
  if (rowspan > 1)
    {
      w42_pt_set_cell_vspan (h->pt, h->pos, MIN (rowspan, 254));
      for (int c = h->table_col; c < h->table_col + colspan && c < 1024; c++)
        h->covered[c] = rowspan - 1;
    }
  h->cell_span = colspan;
  h->pos += 2;
  h->in_cell = TRUE;
  h->at_para_start = TRUE;
  h->space_pending = FALSE;
}

static void
open_cell (Html *h)
{
  open_cell_spanning (h, 1, 1);
}

static void
close_cell (Html *h)
{
  if (h->table < 0 || !h->in_cell)
    return;

  flush_text (h);
  w42_pt_apply_para_fmt (h->pt, h->pos > 0 ? h->pos - 1 : 0, 0,
                         W42_PARA_ALL, &h->pa);
  h->in_cell = FALSE;
  h->in_para = FALSE;
  h->pa_dirty = FALSE;
  h->cell_break_pending = FALSE;
  h->table_col += MAX (h->cell_span, 1);
  h->cell_span = 1;
  {
    W42Fmt def;
    w42_fmt_init_default (&def);
    h->pa = def.pa;
  }
}

static void
end_row (Html *h)
{
  if (h->table < 0)
    return;

  close_cell (h);
  /* Cells the row is short of, so every row has the table's columns. */
  while (h->table_col < h->table_cols)
    {
      if (h->table_col < 1024 && h->covered[h->table_col] > 0)
        {
          open_covered_cell (h);
          continue;
        }
      open_cell (h);
      close_cell (h);
    }
  h->table_row++;
  h->table_col = 0;
}

static void
open_table (Html *h, int cols)
{
  /* Each table costs the model a pass over what came before it, so a page
   * of tens of thousands costs their square; past any document's worth,
   * the rest are read as their text. */
  if (h->table >= 0 || h->tables_opened >= 2048)
    return;
  h->tables_opened++;

  end_paragraph (h);
  flush_text (h);

  h->table_before_block = FALSE;
  if (h->pos >= 2 && h->pos == w42_pt_length (h->pt))
    {
      char *tail = w42_pt_get_text (h->pt, h->pos - 1, 1);
      h->table_before_block = (tail != NULL && *tail == '\n');
      g_free (tail);
    }
  if (h->table_before_block)
    h->pos -= 1;

  h->table = w42_pt_insert_table_start (h->pt, h->pos, CLAMP (cols, 1, 1023), NULL);
  h->pos += 1;
  h->table_row = 0;
  h->table_col = 0;
  h->table_cols = CLAMP (cols, 1, 1023);
  h->in_cell = FALSE;
}

static void
close_table (Html *h)
{
  if (h->table < 0)
    return;

  if (h->in_cell || h->table_col > 0)
    end_row (h);

  if (h->table_before_block)
    {
      w42_pt_insert_table_end_only (h->pt, h->pos);
      h->pos += 2;
    }
  else
    {
      w42_pt_insert_table_end (h->pt, h->pos, html_ap (h));
      h->pos += 2;
    }
  h->table = -1;
  h->in_para = FALSE;
  h->at_para_start = TRUE;
}

/* ---------------------------------------------------------------------- */
/* The tree Lexbor built, seen through small helpers                       */
/* ---------------------------------------------------------------------- */

/* The value of attribute `name`, entity-decoded by the parser; NULL when
 * the element does not carry it. */
static char *
elem_attr (lxb_dom_element_t *el, const char *name)
{
  size_t len = 0;
  const lxb_char_t *v = lxb_dom_element_get_attribute (el, (const lxb_char_t *) name,
                                                       strlen (name), &len);

  return v != NULL ? g_strndup ((const char *) v, len) : NULL;
}

/* The element's name, lowercased by the parser, into a small buffer: the
 * names this reader acts on all fit, and one that does not is one it
 * ignores anyway. */
static void
elem_name (lxb_dom_element_t *el, char *buf, gsize size)
{
  size_t len = 0;
  const lxb_char_t *n = lxb_dom_element_local_name (el, &len);
  gsize k = n != NULL ? MIN (len, size - 1) : 0;

  memcpy (buf, n, k);
  buf[k] = '\0';
}

/* The text of a whole subtree, tags left out: what a note says. */
static char *
node_text (lxb_dom_node_t *node)
{
  size_t len = 0;
  lxb_char_t *raw = lxb_dom_node_text_content (node, &len);
  char *out = g_strndup (raw != NULL ? (const char *) raw : "", raw != NULL ? len : 0);

  if (raw != NULL)
    lxb_dom_document_destroy_text (node->owner_document, raw);
  return g_strstrip (out);
}

/* The first element of tag `tag` under `root`, walked without recursion:
 * the tree's depth is the file's to choose, and the C stack is not. */
static lxb_dom_node_t *
first_descendant (lxb_dom_node_t *root, lxb_tag_id_t tag)
{
  lxb_dom_node_t *n = lxb_dom_node_first_child (root);

  while (n != NULL)
    {
      if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_dom_node_tag_id (n) == tag)
        return n;
      if (n->first_child != NULL)
        {
          n = n->first_child;
          continue;
        }
      while (n != root && n->next == NULL)
        n = n->parent;
      if (n == root)
        return NULL;
      n = n->next;
    }
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* CSS, as far as a word processor's HTML leans on it                      */
/* ---------------------------------------------------------------------- */

/* A CSS length in twips.  Points, inches, centimetres, millimetres and
 * pixels are what a word processor's HTML uses; anything else is left
 * alone, since guessing at ems without a font would be worse. */
static int
css_twips (const char *value)
{
  char *unit = NULL;
  double v = g_ascii_strtod (value, &unit);
  double per;

  /* The unit is the one that follows the number, not any two letters
   * elsewhere in the declaration: "margin" ends in an "in". */
  if (unit == NULL || unit == value)
    return 0;
  while (*unit == ' ')
    unit++;

  if      (g_str_has_prefix (unit, "in")) per = 1440.0;
  else if (g_str_has_prefix (unit, "cm")) per = 1440.0 / 2.54;
  else if (g_str_has_prefix (unit, "mm")) per = 1440.0 / 25.4;
  else if (g_str_has_prefix (unit, "pt")) per = 20.0;
  else if (g_str_has_prefix (unit, "px")) per = 15.0;
  else if (g_str_has_prefix (unit, "pc")) per = 240.0;
  else                                    return 0;

  /* "9e999in" is infinity by the time it is twips, and a cast that does
   * not fit an int is undefined; a million twips is more page than any
   * reader of this file allows anyway. */
  v *= per;
  if (isnan (v))
    return 0;
  v = CLAMP (v, -1000000.0, 1000000.0);
  return (int) (v < 0 ? v - 0.5 : v + 0.5);
}

/* The colour a CSS value names, or -1 if it names none. */
static gint64
css_colour (const char *value)
{
  const char *hash = strchr (value, '#');

  if (hash == NULL || strlen (hash) < 7)
    return -1;
  return (gint64) (strtoul (hash + 1, NULL, 16) & 0xFFFFFF);
}

/* The grey of a CSS colour, as a shading percentage: a pale background
 * behind a paragraph is what shading means to this model. */
static int
css_shading (const char *value)
{
  const char *hash = strchr (value, '#');
  guint32 rgb;
  int grey;

  if (hash == NULL || strlen (hash) < 7)
    return 0;
  rgb = (guint32) strtoul (hash + 1, NULL, 16);
  grey = (int) ((((rgb >> 16) & 0xFF) * 30 + ((rgb >> 8) & 0xFF) * 59 + (rgb & 0xFF) * 11) / 100);
  return CLAMP ((255 - grey) * 100 / 255, 0, 100);
}

/* The inline styles a word processor's HTML leans on. */
static void
apply_style (Html *h, const char *style, gboolean para)
{
  char **decls = g_strsplit (style, ";", -1);

  for (guint i = 0; decls[i] != NULL; i++)
    {
      char *colon = strchr (decls[i], ':');
      char *key, *value;

      if (colon == NULL)
        continue;
      *colon = '\0';
      key = g_strstrip (decls[i]);
      value = g_strstrip (colon + 1);

      if (g_ascii_strcasecmp (key, "font-family") == 0)
        {
          char *name = g_strdup (value);
          char *comma = strchr (name, ',');
          if (comma) *comma = '\0';
          g_strdelimit (name, "'\"", ' ');
          h->ch[h->depth].family = g_intern_string (g_strstrip (name));
          g_free (name);
        }
      else if (g_ascii_strcasecmp (key, "font-size") == 0)
        {
          double v = g_ascii_strtod (value, NULL);
          if (strstr (value, "px") != NULL) v = v * 0.75;
          if (v > 0)
            h->ch[h->depth].size = CLAMP ((int) (v * 2 + 0.5), 2, 3276);
        }
      else if (g_ascii_strcasecmp (key, "letter-spacing") == 0)
        h->ch[h->depth].spacing = (gint16) CLAMP (css_twips (value), -720, 720);
      else if (g_ascii_strcasecmp (key, "color") == 0 && value[0] == '#' && strlen (value) == 7)
        h->ch[h->depth].color = (guint32) strtoul (value + 1, NULL, 16);
      else if (g_ascii_strcasecmp (key, "font-weight") == 0)
        h->ch[h->depth].bold = (g_ascii_strcasecmp (value, "bold") == 0 || atoi (value) >= 600);
      else if (g_ascii_strcasecmp (key, "font-style") == 0)
        h->ch[h->depth].italic = g_ascii_strcasecmp (value, "italic") == 0;
      else if (g_ascii_strcasecmp (key, "text-decoration-style") == 0)
        {
          /* The shape of the line, when the page says one. */
          if (h->ch[h->depth].underline == W42_UNDERLINE_NONE)
            h->ch[h->depth].underline = W42_UNDERLINE_SINGLE;
          if (g_ascii_strcasecmp (value, "double") == 0)
            h->ch[h->depth].underline = W42_UNDERLINE_DOUBLE;
          else if (g_ascii_strcasecmp (value, "dotted") == 0)
            h->ch[h->depth].underline = W42_UNDERLINE_DOTTED;
          else if (g_ascii_strcasecmp (value, "dashed") == 0)
            h->ch[h->depth].underline = W42_UNDERLINE_DASHED;
          else if (g_ascii_strcasecmp (value, "wavy") == 0)
            h->ch[h->depth].underline = W42_UNDERLINE_WAVE;
        }
      else if (g_ascii_strcasecmp (key, "text-decoration") == 0)
        {
          if (strstr (value, "underline") && h->ch[h->depth].underline == W42_UNDERLINE_NONE)
            h->ch[h->depth].underline = W42_UNDERLINE_SINGLE;
          if (strstr (value, "double")) h->ch[h->depth].underline = W42_UNDERLINE_DOUBLE;
          if (strstr (value, "dotted")) h->ch[h->depth].underline = W42_UNDERLINE_DOTTED;
          if (strstr (value, "dashed")) h->ch[h->depth].underline = W42_UNDERLINE_DASHED;
          if (strstr (value, "wavy"))   h->ch[h->depth].underline = W42_UNDERLINE_WAVE;
          if (strstr (value, "line-through")) h->ch[h->depth].strikeout = 1;
          if (strstr (value, "overline")) h->ch[h->depth].overline = 1;
        }
      else if (para && g_ascii_strcasecmp (key, "text-align") == 0)
        {
          if (g_ascii_strcasecmp (value, "center") == 0) h->pa.align = W42_ALIGN_CENTER;
          else if (g_ascii_strcasecmp (value, "right") == 0) h->pa.align = W42_ALIGN_RIGHT;
          else if (g_ascii_strcasecmp (value, "justify") == 0) h->pa.align = W42_ALIGN_JUSTIFY;
          h->pa_dirty = TRUE;
        }
      else if (g_ascii_strcasecmp (key, "font-variant") == 0)
        h->ch[h->depth].smallcaps = g_ascii_strcasecmp (value, "small-caps") == 0;
      else if (g_ascii_strcasecmp (key, "text-transform") == 0)
        h->ch[h->depth].allcaps = g_ascii_strcasecmp (value, "uppercase") == 0;
      else if (!para && (g_ascii_strcasecmp (key, "background") == 0 ||
                         g_ascii_strcasecmp (key, "background-color") == 0))
        {
          /* On a run, a background is a highlight. */
          const char *hash = strchr (value, '#');

          if (hash != NULL && strlen (hash) >= 7)
            {
              guint32 rgb = (guint32) strtoul (hash + 1, NULL, 16);
              int best = 0;
              long best_away = 0;

              for (int k = 1; k <= 16; k++)
                {
                  guint32 c = w42_highlight_rgb (k);
                  long dr = (long) ((c >> 16) & 0xFF) - (long) ((rgb >> 16) & 0xFF);
                  long dg = (long) ((c >> 8) & 0xFF) - (long) ((rgb >> 8) & 0xFF);
                  long db = (long) (c & 0xFF) - (long) (rgb & 0xFF);
                  long away = dr * dr + dg * dg + db * db;

                  if (best == 0 || away < best_away)
                    {
                      best = k;
                      best_away = away;
                    }
                }
              if (rgb != 0xFFFFFF)
                h->ch[h->depth].highlight = (guint8) best;
            }
        }
      else if (para && g_ascii_strcasecmp (key, "margin-left") == 0)
        {
          h->pa.indent_left = css_twips (value);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "margin-right") == 0)
        {
          h->pa.indent_right = css_twips (value);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "text-indent") == 0)
        {
          h->pa.indent_first = css_twips (value);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "margin-top") == 0)
        {
          h->pa.space_before = css_twips (value);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "margin-bottom") == 0)
        {
          h->pa.space_after = css_twips (value);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "line-height") == 0)
        {
          if (strchr (value, '%') != NULL)
            {
              int pct = atoi (value);

              if (pct > 0 && pct != 100)
                h->pa.line_spacing_pct = pct;
            }
          else if (css_twips (value) > 0)
            h->pa.line_spacing = css_twips (value);
          h->pa_dirty = TRUE;
        }
      else if (g_ascii_strcasecmp (key, "--w42-line-height") == 0)
        {
          /* What the document says, rather than what the browser is being
           * asked to do with it; see the exporter. */
          int pct = atoi (value);

          if (strchr (value, '%') != NULL && pct >= 20 && pct <= 1000)
            {
              h->pa.line_spacing_pct = pct;
              h->pa.line_spacing = 0;
            }
          h->pa_dirty = TRUE;
        }
      else if (para && (g_ascii_strcasecmp (key, "background") == 0 ||
                        g_ascii_strcasecmp (key, "background-color") == 0))
        {
          gint64 rgb = css_colour (value);

          if (rgb >= 0)
            {
              h->pa.shading_color = (guint32) rgb;
              h->pa.has_shading_color = 1;
              h->pa.shading = 0;
            }
          else
            h->pa.shading = (guint8) css_shading (value);
          h->pa_dirty = TRUE;
        }
      else if (para && g_str_has_prefix (key, "border"))
        {
          /* "border: 1px solid #000000", or one side of it. */
          if (strstr (value, "none") == NULL && strstr (value, "0") != value)
            {
              int bits = g_ascii_strcasecmp (key, "border") == 0 ? W42_BORDER_BOX
                       : g_ascii_strcasecmp (key, "border-top") == 0 ? W42_BORDER_TOP
                       : g_ascii_strcasecmp (key, "border-bottom") == 0 ? W42_BORDER_BOTTOM
                       : g_ascii_strcasecmp (key, "border-left") == 0 ? W42_BORDER_LEFT
                       : g_ascii_strcasecmp (key, "border-right") == 0 ? W42_BORDER_RIGHT : 0;
              gint64 rgb = css_colour (value);
              int w = css_twips (value);
              W42BorderStyle line = w42_border_style_from_css (value);

              h->pa.border |= (guint8) bits;
              for (int e = 0; e < 4; e++)
                if (bits & (1 << e))
                  {
                    if (rgb >= 0)
                      h->pa.edge[e].color = (guint32) rgb;
                    if (w > 0)
                      h->pa.edge[e].width = (guint8) CLAMP (w, 5, 120);
                    h->pa.edge[e].style = (guint8) line;
                  }
              h->pa_dirty = TRUE;
            }
        }
    }
  g_strfreev (decls);
}

/* ---------------------------------------------------------------------- */
/* Notes                                                                   */
/* ---------------------------------------------------------------------- */

/* A word processor writing HTML puts its footnotes at the end and links
 * to them: LibreOffice as <div id="sdfootnote1">, Word42 as a paragraph
 * with id="note1".  Both are gathered here, before the body is walked,
 * so that a reference can become a real note when it is met. */
static void
harvest_notes (Html *h, lxb_dom_node_t *root)
{
  lxb_dom_node_t *n = lxb_dom_node_first_child (root);

  h->notes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  while (n != NULL)
    {
      if (n->type == LXB_DOM_NODE_TYPE_ELEMENT)
        {
          char *id = elem_attr (lxb_dom_interface_element (n), "id");

          if (id != NULL &&
              (g_str_has_prefix (id, "sdfootnote") ||
               g_str_has_prefix (id, "sdendnote") ||
               g_str_has_prefix (id, "note")) &&
              !g_hash_table_contains (h->notes, id))
            {
              char *text = node_text (n);

              /* A note's text begins with its own number, which is the
               * anchor back to the reference: that is not part of it. */
              {
                char *q = text;

                while (*q != '\0' && (g_ascii_isdigit (*q) || strchr (".)ivxIVX", *q) != NULL))
                  q++;
                while (*q == ' ')
                  q++;
                if (q != text && *q != '\0')
                  {
                    char *rest = g_strdup (q);

                    g_free (text);
                    text = rest;
                  }
              }

              if (*text != '\0')
                {
                  g_hash_table_insert (h->notes, id, text);
                  id = NULL;
                }
              else
                g_free (text);
            }
          g_free (id);
        }

      if (n->first_child != NULL)
        {
          n = n->first_child;
          continue;
        }
      while (n != root && n->next == NULL)
        n = n->parent;
      if (n == root)
        break;
      n = n->next;
    }
}

/* The id a reference points at: "#sdfootnote1sym" is the note
 * "sdfootnote1", and "#note1" is the note "note1". */
static char *
note_id_for (const char *href)
{
  char *id;
  gsize n;

  if (href == NULL || *href != '#')
    return NULL;
  id = g_strdup (href + 1);
  n = strlen (id);
  if (n > 3 && g_str_has_suffix (id, "sym"))
    id[n - 3] = '\0';
  else if (n > 3 && g_str_has_suffix (id, "anc"))
    id[n - 3] = '\0';
  return id;
}

/* ---------------------------------------------------------------------- */
/* Pictures                                                                */
/* ---------------------------------------------------------------------- */

/* A picture from a data: URI, or from a file kept beside the page: a
 * word processor writing HTML puts its pictures next to the document
 * rather than inside it. */
static void
picture (Html *h, const char *src, const char *width, const char *height)
{
  const char *comma;
  guchar *bytes;
  gsize n;
  GBytes *data;
  int pw = 0, ph = 0;
  const char *format = NULL;
  W42ObjectIdx idx;
  int w, hh;

  if (src == NULL || *src == '\0')
    return;

  if (g_str_has_prefix (src, "data:"))
    {
      if ((comma = strchr (src, ',')) == NULL ||
          strstr (src, ";base64") == NULL || (gsize) (comma - src) > 200)
        return;
      bytes = g_base64_decode (comma + 1, &n);
      data = g_bytes_new_take (bytes, n);
    }
  else
    {
      /* A file beside the page.  Only a plain relative name is followed:
       * nothing with a scheme, and nothing that climbs out of the
       * document's own directory. */
      char *unescaped, *path;
      char *contents = NULL;
      gsize len = 0;

      /* The checks must look at the decoded name, or "%2e%2e" walks
       * straight past them and out of the directory. */
      unescaped = g_uri_unescape_string (src, NULL);
      if (unescaped == NULL)
        return;
      if (h->base == NULL || strstr (unescaped, "://") != NULL ||
          strchr (unescaped, ':') != NULL ||
          g_str_has_prefix (unescaped, "/") || strstr (unescaped, "..") != NULL)
        {
          g_free (unescaped);
          return;
        }
      path = g_build_filename (h->base, unescaped, NULL);
      g_free (unescaped);
      if (!g_file_get_contents (path, &contents, &len, NULL) || len == 0)
        {
          g_free (path);
          g_free (contents);
          return;
        }
      g_free (path);
      data = g_bytes_new_take ((guint8 *) contents, len);
    }
  if (!w42_image_probe (data, &pw, &ph, &format))
    {
      g_bytes_unref (data);
      return;
    }
  /* A size with a unit is measured; a bare number is screen pixels, which
   * is what the width and height attributes hold. */
  w = width != NULL && css_twips (width) > 0 ? CLAMP (css_twips (width), 15, 30000)
    : width != NULL && atoi (width) > 0 ? CLAMP (atoi (width), 1, 2000) * 15
    : MIN (pw, 2000) * 15;
  hh = height != NULL && css_twips (height) > 0 ? CLAMP (css_twips (height), 15, 30000)
     : height != NULL && atoi (height) > 0 ? CLAMP (atoi (height), 1, 2000) * 15
     : MIN (ph, 2000) * 15;

  idx = w42_object_table_add (w42_pt_object_table (h->pt), data, format, pw, ph, w, hh);
  g_bytes_unref (data);

  flush_text (h);
  w42_pt_insert_object (h->pt, h->pos, idx, html_ap (h));
  h->pos += 1;
  h->in_para = TRUE;
  h->at_para_start = FALSE;
}

/* Columns of a table: the cells of its first row. */
static int
count_columns (lxb_dom_node_t *table)
{
  lxb_dom_node_t *tr = first_descendant (table, LXB_TAG_TR);
  int cols = 0;

  for (lxb_dom_node_t *n = tr != NULL ? lxb_dom_node_first_child (tr) : NULL;
       n != NULL; n = n->next)
    {
      lxb_tag_id_t tag = lxb_dom_node_tag_id (n);

      if (n->type == LXB_DOM_NODE_TYPE_ELEMENT &&
          (tag == LXB_TAG_TD || tag == LXB_TAG_TH))
        {
          char *span = elem_attr (lxb_dom_interface_element (n), "colspan");

          cols += span != NULL && atoi (span) > 1 ? CLAMP (atoi (span), 1, 1023) : 1;
          cols = MIN (cols, 1023);
          g_free (span);
        }
    }
  return cols;
}

/* ---------------------------------------------------------------------- */
/* The walk                                                                */
/* ---------------------------------------------------------------------- */

typedef enum {
  WALK_DESCEND,       /* walk the element's children */
  WALK_SKIP,          /* the whole subtree is handled, or not wanted */
} WalkEnter;

static gboolean
known_inline (const char *name)
{
  return g_str_equal (name, "b") || g_str_equal (name, "strong") || g_str_equal (name, "i") ||
         g_str_equal (name, "em") || g_str_equal (name, "u") || g_str_equal (name, "s") ||
         g_str_equal (name, "strike") || g_str_equal (name, "del") || g_str_equal (name, "a") ||
         g_str_equal (name, "sup") || g_str_equal (name, "sub") || g_str_equal (name, "span") ||
         g_str_equal (name, "font") || g_str_equal (name, "code") || g_str_equal (name, "tt") ||
         g_str_equal (name, "small") || g_str_equal (name, "big") || g_str_equal (name, "mark");
}

static gboolean
block_element (const char *name)
{
  return g_str_equal (name, "p") || g_str_equal (name, "div") ||
         (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') ||
         g_str_equal (name, "li") || g_str_equal (name, "blockquote") ||
         g_str_equal (name, "pre");
}

static WalkEnter
element_start (Html *h, const char *name, lxb_dom_element_t *el, gboolean *pushed)
{
  char *style;

  *pushed = FALSE;

  /* A script or a stylesheet is not text; a title was read before the
   * body was walked. */
  if (g_str_equal (name, "script") || g_str_equal (name, "style") ||
      g_str_equal (name, "title"))
    return WALK_SKIP;

  /* The page's own copy of a note, at its end: the document has the note
   * already -- harvest_notes gathered it before the body was walked -- so
   * the copy is passed over whole. */
  if (h->notes != NULL)
    {
      char *id = elem_attr (el, "id");

      if (id != NULL && g_hash_table_contains (h->notes, id))
        {
          end_paragraph (h);
          g_free (id);
          return WALK_SKIP;
        }
      g_free (id);
    }

  /* Block elements end the paragraph they are in. */
  if (block_element (name))
    {
      end_paragraph (h);
      if (h->table >= 0)
        open_cell (h);
      if (name[0] == 'h' && name[2] == '\0')
        {
          static const char *heads[] = { "Heading 1", "Heading 2", "Heading 3" };
          int level = name[1] - '0';
          const W42Style *st;

          h->pa.style = g_intern_string (heads[CLAMP (level, 1, 3) - 1]);
          h->pa_dirty = TRUE;
          /* the heading's own character formatting, as applying the
           * style would give it */
          for (guint i = 0; i < w42_stylesheet_size (w42_pt_stylesheet (h->pt)); i++)
            {
              st = w42_stylesheet_get (w42_pt_stylesheet (h->pt), i);
              if (g_ascii_strcasecmp (st->name, h->pa.style) == 0)
                {
                  h->ch[h->depth] = st->ch;
                  h->pa = st->pa;
                  h->pa.style = st->name;
                  break;
                }
            }
        }
      if (g_str_equal (name, "li"))
        {
          h->pa.list = h->list_kind != W42_LIST_NONE ? h->list_kind : W42_LIST_BULLET;
          h->pa.list_level = (guint8) CLAMP (h->list_depth - 1, 0, 8);
          h->pa.indent_left = 360 * (h->pa.list_level + 1);
          h->pa.list_start = (guint8) h->list_start;
          h->list_start = 0;
          h->pa.indent_left = 360 * MAX (h->list_depth, 1);
          h->pa.indent_first = -360;
          h->pa_dirty = TRUE;
        }
      if (g_str_equal (name, "blockquote"))
        {
          h->pa.indent_left = 720;
          h->pa.indent_right = 720;
          h->pa_dirty = TRUE;
        }
      if (g_str_equal (name, "pre"))
        {
          h->pre_depth++;
          h->ch[h->depth].family = g_intern_string ("Courier New");
        }
      if ((style = elem_attr (el, "style")) != NULL)
        {
          apply_style (h, style, TRUE);
          g_free (style);
        }
      if ((style = elem_attr (el, "align")) != NULL)
        {
          if (g_ascii_strcasecmp (style, "center") == 0) h->pa.align = W42_ALIGN_CENTER;
          else if (g_ascii_strcasecmp (style, "right") == 0) h->pa.align = W42_ALIGN_RIGHT;
          else if (g_ascii_strcasecmp (style, "justify") == 0) h->pa.align = W42_ALIGN_JUSTIFY;
          h->pa_dirty = TRUE;
          g_free (style);
        }
      if ((style = elem_attr (el, "dir")) != NULL)
        {
          h->pa.rtl = g_ascii_strcasecmp (style, "rtl") == 0;
          h->pa_dirty = TRUE;
          g_free (style);
        }
      return WALK_DESCEND;
    }

  if (g_str_equal (name, "br"))
    {
      if (h->space_pending)
        g_string_append_c (h->pending, ' ');
      g_string_append_unichar (h->pending, 0x2028);
      h->space_pending = FALSE;
      h->at_para_start = FALSE;
      return WALK_SKIP;
    }

  if (g_str_equal (name, "ul") || g_str_equal (name, "ol"))
    {
      char *type = elem_attr (el, "type");
      char *start = elem_attr (el, "start");
      char *ul_style = elem_attr (el, "style");

      end_paragraph (h);
      h->list_kind = g_str_equal (name, "ul") ? W42_LIST_BULLET : W42_LIST_NUMBER;
      if (g_str_equal (name, "ol") && type != NULL)
        h->list_kind = g_str_equal (type, "a") ? W42_LIST_LOWER_LETTER
                     : g_str_equal (type, "A") ? W42_LIST_UPPER_LETTER
                     : g_str_equal (type, "i") ? W42_LIST_LOWER_ROMAN
                     : g_str_equal (type, "I") ? W42_LIST_UPPER_ROMAN
                     : W42_LIST_NUMBER;
      if (g_str_equal (name, "ul") && ul_style != NULL)
        {
          if (strstr (ul_style, "circle") != NULL) h->list_kind = W42_LIST_BULLET_CIRCLE;
          else if (strstr (ul_style, "square") != NULL) h->list_kind = W42_LIST_BULLET_SQUARE;
          else if (strstr (ul_style, "2013") != NULL || strstr (ul_style, "'-'") != NULL)
            h->list_kind = W42_LIST_BULLET_DASH;
        }
      h->list_start = start != NULL ? CLAMP (atoi (start), 0, 255) : 0;
      g_free (type);
      g_free (start);
      g_free (ul_style);
      if (h->list_depth < 9)
        h->list_kinds[h->list_depth] = h->list_kind;
      h->list_depth++;
      return WALK_DESCEND;
    }

  if (g_str_equal (name, "table"))
    {
      char *cls = elem_attr (el, "class");

      if (h->table < 0)
        open_table (h, count_columns (lxb_dom_interface_node (el)));
      /* Word42 writes class="ruled" for a table that is; anything else
       * rules its cells itself, or not at all. */
      if (h->table >= 0)
        w42_pt_table_set_borders (h->pt, h->table,
                                  cls != NULL && strstr (cls, "ruled") != NULL);
      g_free (cls);
      return WALK_DESCEND;
    }
  if (g_str_equal (name, "col") && h->table >= 0)
    {
      /* A <colgroup> gives the columns their widths; one <col> with no
       * width of its own still takes its place among them. */
      char *cw = elem_attr (el, "style");
      const char *w = cw != NULL ? strstr (cw, "width:") : NULL;

      if (h->n_col_widths < 1023)
        h->col_widths[h->n_col_widths++] = w != NULL ? css_twips (w + 6) : 0;
      g_free (cw);
      return WALK_SKIP;
    }
  if (g_str_equal (name, "tr"))
    {
      if (h->table >= 0 && (h->in_cell || h->table_col > 0))
        end_row (h);
      return WALK_DESCEND;
    }
  if (g_str_equal (name, "td") || g_str_equal (name, "th"))
    {
      char *cs = elem_attr (el, "colspan"), *rs = elem_attr (el, "rowspan");

      close_cell (h);
      open_cell_spanning (h, cs != NULL ? atoi (cs) : 1, rs != NULL ? atoi (rs) : 1);
      g_free (cs);
      g_free (rs);
      if (g_str_equal (name, "th"))
        h->ch[h->depth].bold = 1;
      if ((style = elem_attr (el, "style")) != NULL)
        {
          /* A <td>'s style is the cell's, not the paragraph's inside
           * it: its rules and its background belong to the mark. */
          W42ParaFmt saved = h->pa;
          gboolean saved_dirty = h->pa_dirty;
          gsize cell_pos = h->pos >= 2 ? h->pos - 2 : 0;

          apply_style (h, style, TRUE);
          if (h->in_cell)
            {
              w42_pt_cell_set_borders_at (h->pt, cell_pos,
                                          (h->pa.border & W42_BORDER_BOX) |
                                          W42_BORDER_CELL_SET);
              w42_pt_cell_set_edges_at (h->pt, cell_pos, h->pa.edge);
              if (h->pa.has_shading_color)
                w42_pt_cell_set_fill_at (h->pt, cell_pos, TRUE,
                                         h->pa.shading_color);
              else if (h->pa.shading > 0)
                w42_pt_cell_set_shading_at (h->pt, cell_pos, h->pa.shading);
              if (strstr (style, "vertical-align:middle") != NULL || strstr (style, "vertical-align: middle") != NULL)
                w42_pt_cell_set_valign_at (h->pt, cell_pos, W42_CELL_VALIGN_CENTER);
              else if (strstr (style, "vertical-align:bottom") != NULL || strstr (style, "vertical-align: bottom") != NULL)
                w42_pt_cell_set_valign_at (h->pt, cell_pos, W42_CELL_VALIGN_BOTTOM);
            }
          h->pa = saved;
          h->pa_dirty = saved_dirty;
          g_free (style);
        }
      return WALK_DESCEND;
    }

  if (g_str_equal (name, "meta"))
    {
      /* What a word processor writing HTML says about the document. */
      static const struct { const char *name; int slot; } NAMES[] = {
        { "title", 0 }, { "subject", 1 }, { "classification", 1 },
        { "author", 2 }, { "changedby", 2 }, { "creator", 2 },
        { "keywords", 3 }, { "description", 4 }, { "comments", 4 },
      };
      char *what = elem_attr (el, "name");
      char *content = elem_attr (el, "content");

      if (what != NULL && content != NULL && *content != '\0')
        for (guint i = 0; i < G_N_ELEMENTS (NAMES); i++)
          if (g_ascii_strcasecmp (what, NAMES[i].name) == 0 &&
              h->meta[NAMES[i].slot] == NULL)
            h->meta[NAMES[i].slot] = g_strdup (content);
      g_free (what);
      g_free (content);
      return WALK_SKIP;
    }

  if (g_str_equal (name, "img"))
    {
      char *src = elem_attr (el, "src");
      char *w = elem_attr (el, "width");
      char *hh = elem_attr (el, "height");
      char *st = elem_attr (el, "style");

      /* A style says the size to the twip; the attributes only to the
       * screen pixel, so they are the fallback. */
      if (st != NULL)
        {
          const char *sw = strstr (st, "width:");
          const char *sh = strstr (st, "height:");

          if (sw != NULL) { g_free (w);  w  = g_strdup (sw + 6); }
          if (sh != NULL) { g_free (hh); hh = g_strdup (sh + 7); }
        }
      if (h->table >= 0)
        open_cell (h);
      picture (h, src, w, hh);
      g_free (src); g_free (w); g_free (hh); g_free (st);
      return WALK_SKIP;
    }

  if (!known_inline (name))
    {
      /* Something else, or unknown: no formatting of its own, but what
       * is inside it is still text. */
      return WALK_DESCEND;
    }

  flush_text (h);
  push_char (h);
  *pushed = TRUE;
  if (g_str_equal (name, "b") || g_str_equal (name, "strong"))
    h->ch[h->depth].bold = 1;
  else if (g_str_equal (name, "i") || g_str_equal (name, "em"))
    h->ch[h->depth].italic = 1;
  else if (g_str_equal (name, "u"))
    h->ch[h->depth].underline = 1;
  else if (g_str_equal (name, "s") || g_str_equal (name, "strike") || g_str_equal (name, "del"))
    h->ch[h->depth].strikeout = 1;
  else if (g_str_equal (name, "sup"))
    h->ch[h->depth].script = 1;
  else if (g_str_equal (name, "sub"))
    h->ch[h->depth].script = -1;
  else if (g_str_equal (name, "mark"))
    h->ch[h->depth].highlight = 7;
  else if (g_str_equal (name, "code") || g_str_equal (name, "tt"))
    h->ch[h->depth].family = g_intern_string ("Courier New");
  else if (g_str_equal (name, "small"))
    h->ch[h->depth].size = MAX (h->ch[h->depth].size - 4, 8);
  else if (g_str_equal (name, "big"))
    h->ch[h->depth].size = h->ch[h->depth].size + 4;
  else if (g_str_equal (name, "a"))
    {
      char *href = elem_attr (el, "href");
      char *anchor = elem_attr (el, "name");
      char *note_id = note_id_for (href);
      const char *note_text = note_id != NULL && h->notes != NULL
        ? g_hash_table_lookup (h->notes, note_id) : NULL;

      if (note_text != NULL)
        {
          /* A link to a note at the end of the page is a note: the
           * document gets the real thing, and the number the page shows
           * is left out, since a note numbers itself. */
          gsize body;

          body = g_str_has_prefix (note_id, "sdendnote") || g_str_has_prefix (note_id, "notee")
                   ? w42_pt_insert_endnote (h->pt, h->pos, html_ap (h))
                   : w42_pt_insert_footnote (h->pt, h->pos, html_ap (h));

          if (body != (gsize) -1)
            {
              W42Fmt nf;

              /* The reference is a superscript and may be a link; the note
               * it points at is neither. */
              w42_fmt_init_default (&nf);
              nf.ch = h->ch[0];
              nf.ch.script = 0;
              nf.ch.link = NULL;
              nf.ch.bookmark = NULL;
              nf.ch.comment = NULL;
              w42_pt_insert_text (h->pt, body, note_text,
                                  w42_ap_table_intern (w42_pt_ap_table (h->pt), &nf));
              h->pos += 1;             /* the mark the note left behind */
              h->at_para_start = FALSE;
              h->in_para = TRUE;
            }
          g_free (note_id);
          g_free (href);
          g_free (anchor);
          /* The anchor's own text is the number the page shows. */
          pop_char (h);
          *pushed = FALSE;
          return WALK_SKIP;
        }
      g_free (note_id);

      /* A link is somewhere to go, not something to run: a script scheme
       * would execute when the exported page is opened in a browser. */
      if (href != NULL && *href != '\0' && *href != '#' &&
          g_ascii_strncasecmp (href, "javascript:", 11) != 0 &&
          g_ascii_strncasecmp (href, "vbscript:", 9) != 0 &&
          g_ascii_strncasecmp (href, "data:", 5) != 0)
        h->ch[h->depth].link = g_intern_string (href);
      if (anchor == NULL)
        anchor = elem_attr (el, "id");
      /* <a name="x"> is where a link inside the page lands: a bookmark.
       * An empty one marks the place before what comes next. */
      if (anchor != NULL && *anchor != '\0')
        {
          h->ch[h->depth].bookmark = g_intern_string (anchor);
          h->pending_bookmark = h->ch[h->depth].bookmark;
        }
      g_free (href);
      g_free (anchor);
    }
  else if (g_str_equal (name, "font"))
    {
      char *face = elem_attr (el, "face");
      char *color = elem_attr (el, "color");

      if (face != NULL && *face != '\0') h->ch[h->depth].family = g_intern_string (face);
      if (color != NULL && color[0] == '#' && strlen (color) == 7)
        h->ch[h->depth].color = (guint32) strtoul (color + 1, NULL, 16);
      g_free (face); g_free (color);
    }

  {
    /* An element may say the language of what is in it, and any element
     * may: it is not the span's alone. */
    char *lang = elem_attr (el, "lang");
    const char *known = lang != NULL ? w42_lang_normalise (lang) : NULL;

    if (known != NULL)
      h->ch[h->depth].lang = known;
    g_free (lang);
  }

  if ((style = elem_attr (el, "style")) != NULL)
    {
      apply_style (h, style, FALSE);
      g_free (style);
    }

  return WALK_DESCEND;
}

static void
element_end (Html *h, const char *name, gboolean pushed)
{
  if (block_element (name))
    {
      end_paragraph (h);
      if (g_str_equal (name, "pre") && h->pre_depth > 0)
        h->pre_depth--;
      if (name[0] == 'h' && name[2] == '\0')
        {
          W42Fmt def;
          w42_fmt_init_default (&def);
          h->ch[h->depth] = def.ch;
        }
      return;
    }
  if (g_str_equal (name, "ul") || g_str_equal (name, "ol"))
    {
      end_paragraph (h);
      if (h->list_depth > 0)
        {
          h->list_depth--;
          /* Back out to the list around this one, with its own kind. */
          h->list_kind = h->list_depth > 0 && h->list_depth <= 9
                           ? h->list_kinds[h->list_depth - 1] : W42_LIST_NONE;
        }
      return;
    }
  if (g_str_equal (name, "table"))
    {
      close_table (h);
      return;
    }
  if (g_str_equal (name, "colgroup"))
    {
      if (h->table >= 0 && h->n_col_widths > 0)
        {
          w42_pt_table_set_widths (h->pt, h->table, h->col_widths,
                                   MIN (h->n_col_widths, 1023));
          h->n_col_widths = 0;
        }
      return;
    }
  if (g_str_equal (name, "tr"))
    {
      end_row (h);
      return;
    }
  if (g_str_equal (name, "td") || g_str_equal (name, "th"))
    {
      close_cell (h);
      h->ch[h->depth].bold = 0;
      return;
    }
  if (pushed)
    {
      flush_text (h);
      pop_char (h);
    }
}

/* The body, walked without recursion: how deep the file nests is its own
 * business, and must not become the C stack's. */
static void
walk_body (Html *h, lxb_dom_node_t *root)
{
  GByteArray *pushed = g_byte_array_new ();
  lxb_dom_node_t *node = lxb_dom_node_first_child (root);

  while (node != NULL && node != root)
    {
      gboolean descend = FALSE;

      if (node->type == LXB_DOM_NODE_TYPE_TEXT)
        {
          lexbor_str_t *s = &lxb_dom_interface_text (node)->char_data.data;

          add_text (h, (const char *) s->data, s->length);
        }
      else if (node->type == LXB_DOM_NODE_TYPE_ELEMENT)
        {
          char name[24];
          gboolean did_push = FALSE;

          elem_name (lxb_dom_interface_element (node), name, sizeof name);
          if (element_start (h, name, lxb_dom_interface_element (node), &did_push) == WALK_DESCEND &&
              node->first_child != NULL)
            {
              guint8 f = did_push ? 1 : 0;

              g_byte_array_append (pushed, &f, 1);
              descend = TRUE;
            }
          else
            element_end (h, name, did_push);
        }

      if (descend)
        {
          node = node->first_child;
          continue;
        }
      while (node->next == NULL)
        {
          node = node->parent;
          if (node == NULL || node == root)
            goto out;
          {
            char name[24];
            guint8 f = 0;

            if (pushed->len > 0)
              {
                f = pushed->data[pushed->len - 1];
                g_byte_array_set_size (pushed, pushed->len - 1);
              }
            elem_name (lxb_dom_interface_element (node), name, sizeof name);
            element_end (h, name, f != 0);
          }
        }
      node = node->next;
    }
out:
  g_byte_array_free (pushed, TRUE);
}

/* ---------------------------------------------------------------------- */

/* What the head says about the document: the title, and the <meta> names
 * a word processor writes when it saves a page. */
static void
read_head (Html *h, lxb_html_document_t *ldoc)
{
  size_t len = 0;
  const lxb_char_t *title = lxb_html_document_title (ldoc, &len);
  lxb_html_head_element_t *head = lxb_html_document_head_element (ldoc);

  if (title != NULL && len > 0 && h->meta[0] == NULL)
    {
      char *t = g_strstrip (g_strndup ((const char *) title, len));

      if (*t != '\0')
        h->meta[0] = t;
      else
        g_free (t);
    }

  for (lxb_dom_node_t *n = head != NULL
         ? lxb_dom_node_first_child (lxb_dom_interface_node (head)) : NULL;
       n != NULL; n = n->next)
    {
      if (n->type == LXB_DOM_NODE_TYPE_ELEMENT &&
          lxb_dom_node_tag_id (n) == LXB_TAG_META)
        {
          char name[24];
          gboolean did_push = FALSE;

          elem_name (lxb_dom_interface_element (n), name, sizeof name);
          element_start (h, name, lxb_dom_interface_element (n), &did_push);
        }
    }
}

/* True when the page's unclosed nesting is far past what any document
 * means.  The HTML5 tree builder walks its open elements for many a
 * token, so a file that is nothing but open tags costs the square of its
 * depth to parse; the browsers flatten a tree past a few hundred deep,
 * and a word processor can simply decline.  The estimate errs high --
 * a close pops only the open it names -- which only ever declines a
 * page no hand wrote. */
static gboolean
nests_too_deeply (const char *data, gsize len)
{
  /* Elements with no closing tag, those the parser refuses to repeat or
   * to nest -- the formatting elements, kept shallow by the spec's own
   * list -- and those the next of their own kind closes, do not stack
   * up, and must not count. */
  static const char *const UNCOUNTED[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr",
    "html", "head", "body",
    "a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small",
    "strike", "strong", "tt", "u", NULL };
  static const char *const SELF_CLOSING[] = {
    "p", "li", "td", "th", "tr", "dt", "dd", "option", NULL };
  GArray *open = g_array_new (FALSE, FALSE, sizeof (guint32));
  gsize i = 0;
  gboolean deep = FALSE;

  while (i < len)
    {
      gboolean closing = FALSE;
      char name[12] = { 0 };
      guint n = 0;
      guint32 packed;

      if (data[i] != '<')
        {
          i++;
          continue;
        }

      /* Comments, and the elements whose content is text that may hold
       * markup of its own to a byte scan. */
      if (i + 3 < len && memcmp (data + i, "<!--", 4) == 0)
        {
          const char *close = g_strstr_len (data + i + 4, len - i - 4, "-->");

          i = close != NULL ? (gsize) (close - data) + 3 : len;
          continue;
        }
      i++;
      if (i < len && (data[i] == '!' || data[i] == '?'))
        continue;
      if (i < len && data[i] == '/')
        {
          closing = TRUE;
          i++;
        }
      while (i < len && g_ascii_isalnum (data[i]) && n < sizeof name - 1)
        name[n++] = (char) g_ascii_tolower (data[i++]);
      name[n] = '\0';
      if (n == 0)
        continue;

      if (!closing && (g_str_equal (name, "script") || g_str_equal (name, "style")))
        {
          const char *close = g_strstr_len (data + i, len - i, "</");

          while (close != NULL &&
                 g_ascii_strncasecmp (close + 2, name, strlen (name)) != 0)
            close = g_strstr_len (close + 2, len - (close + 2 - data), "</");
          i = close != NULL ? (gsize) (close - data) + 2 : len;
          continue;
        }

      for (guint k = 0; UNCOUNTED[k] != NULL; k++)
        if (g_str_equal (name, UNCOUNTED[k]))
          {
            n = 0;
            break;
          }
      if (n == 0)
        continue;

      packed = ((guint32) (guchar) name[0]) | ((guint32) (guchar) name[1] << 8) |
               ((guint32) (guchar) name[2] << 16) | ((guint32) (guchar) name[3] << 24);
      if (closing)
        {
          if (open->len > 0 &&
              g_array_index (open, guint32, open->len - 1) == packed)
            g_array_set_size (open, open->len - 1);
          continue;
        }

      /* A new <p> is the old one's close; the pair stays one deep. */
      if (open->len > 0 &&
          g_array_index (open, guint32, open->len - 1) == packed)
        {
          gboolean replaces = FALSE;

          for (guint k = 0; SELF_CLOSING[k] != NULL; k++)
            if (g_str_equal (name, SELF_CLOSING[k]))
              {
                replaces = TRUE;
                break;
              }
          if (replaces)
            continue;
        }

      g_array_append_val (open, packed);
      if (open->len > 4096)
        {
          deep = TRUE;
          break;
        }
    }

  g_array_free (open, TRUE);
  return deep;
}

gboolean
w42_html_import (W42PieceTable *pt, W42PageSetup *page, GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  lxb_html_document_t *ldoc;
  lxb_html_body_element_t *body;
  Html h;
  W42Fmt def;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);


  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return FALSE;

  if (!g_utf8_validate (contents, length, NULL))
    {
      /* Not UTF-8: Latin-1 is the next best guess for an old page. */
      char *conv = g_convert (contents, length, "UTF-8", "ISO-8859-1", NULL, &length, NULL);
      g_free (contents);
      contents = conv != NULL ? conv : g_strdup ("");
      length = strlen (contents);
    }

  if (nests_too_deeply (contents, length))
    {
      g_free (contents);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "The page nests its elements too deeply to be a document.");
      return FALSE;
    }

  ldoc = lxb_html_document_create ();
  if (ldoc == NULL ||
      lxb_html_document_parse (ldoc, (const lxb_char_t *) contents, length) != LXB_STATUS_OK)
    {
      if (ldoc != NULL)
        lxb_html_document_destroy (ldoc);
      g_free (contents);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "The page could not be parsed.");
      return FALSE;
    }

  w42_pt_load_text (pt, "");

  memset (&h, 0, sizeof h);
  h.pt = pt;
  h.page = page;
  {
    GFile *dir = g_file_get_parent (file);

    h.base = dir != NULL ? g_file_get_path (dir) : NULL;
    g_clear_object (&dir);
  }
  h.pos = w42_pt_first_caret_pos (pt);
  h.pending = g_string_new (NULL);
  h.table = -1;
  h.at_para_start = TRUE;
  w42_fmt_init_default (&def);
  h.ch[0] = def.ch;
  h.pa = def.pa;

  /* What the stylesheet says the page is.  Word42 writes an @page rule,
   * and so does anything else that expects to be printed. */
  if (page != NULL)
    {
      const char *at = g_strstr_len (contents, length, "@page");

      if (at != NULL)
        {
          const char *brace = strchr (at, '{');
          const char *close = brace != NULL ? strchr (brace, '}') : NULL;

          if (close != NULL)
            {
              char *rule = g_strndup (brace + 1, close - brace - 1);
              const char *size = strstr (rule, "size:");
              const char *margin = strstr (rule, "margin:");

              if (size != NULL)
                {
                  const char *q = size + 5;
                  const char *sp;
                  int w, hh;

                  while (*q == ' ') q++;
                  w = css_twips (q);
                  sp = strchr (q, ' ');
                  hh = sp != NULL ? css_twips (sp) : 0;

                  if (w > 0 && hh > 0)
                    {
                      page->width = w;
                      page->height = hh;
                    }
                }
              if (margin != NULL)
                {
                  const char *q = margin + 7;
                  int m[4] = { 0, 0, 0, 0 };
                  int n = 0;

                  while (n < 4 && *q != '\0')
                    {
                      while (*q == ' ') q++;
                      if (*q == '\0') break;
                      m[n++] = css_twips (q);
                      while (*q != '\0' && *q != ' ') q++;
                    }
                  /* The CSS shorthand: one value for all four sides, two
                   * for the pairs, three or four naming them round. */
                  if (n == 1)      { m[1] = m[2] = m[3] = m[0]; }
                  else if (n == 2) { m[2] = m[0]; m[3] = m[1]; }
                  else if (n == 3) { m[3] = m[1]; }
                  if (n >= 1)
                    {
                      page->margin_top = m[0];
                      page->margin_right = m[1];
                      page->margin_bottom = m[2];
                      page->margin_left = m[3];
                    }
                }
              g_free (rule);
            }
        }
    }

  read_head (&h, ldoc);
  harvest_notes (&h, lxb_dom_interface_node (ldoc));

  body = lxb_html_document_body_element (ldoc);
  if (body != NULL)
    {
      /* What the body says about the colour behind the page. */
      char *style = elem_attr (lxb_dom_interface_element (body), "style");
      const char *hash = style != NULL ? strchr (style, '#') : NULL;

      if (page != NULL && style != NULL && hash != NULL && strlen (hash) >= 7 &&
          strstr (style, "background") != NULL)
        {
          page->background = (guint32) strtoul (hash + 1, NULL, 16);
          page->has_background = 1;
        }
      g_free (style);

      walk_body (&h, lxb_dom_interface_node (body));
    }

  close_table (&h);
  flush_text (&h);
  if (h.pa_dirty)
    w42_pt_apply_para_fmt (pt, h.pos > 0 ? h.pos - 1 : 0, 0,
                           W42_PARA_ALL, &h.pa);

  /* The last block element's close left an empty paragraph behind, as a
   * trailing newline would in a text file.  Drop it -- the body ends where
   * the notes begin, not where the document does, so a page with footnotes
   * drops it too rather than saving a blank line that grows a paragraph
   * every time the file goes out and comes back. */
  {
    gsize body_end = w42_pt_notes_start (pt);

    if (body_end == (gsize) -1)
      body_end = w42_pt_length (pt);

    if (!h.in_para && h.pos >= 1 && h.pos == body_end)
      {
        gsize first = w42_pt_first_caret_pos (pt);

        if (h.pos - 1 > first)
          w42_pt_delete (pt, h.pos - 1, 1);
      }
  }

  g_string_free (h.pending, TRUE);
  if (h.meta[0] != NULL || h.meta[1] != NULL || h.meta[2] != NULL ||
      h.meta[3] != NULL || h.meta[4] != NULL)
    {
      W42DocInfo info;

      memset (&info, 0, sizeof info);
      info.title    = h.meta[0];
      info.subject  = h.meta[1];
      info.author   = h.meta[2];
      info.keywords = h.meta[3];
      info.comments = h.meta[4];
      w42_pt_set_info (pt, &info);
    }
  for (guint i = 0; i < G_N_ELEMENTS (h.meta); i++)
    g_free (h.meta[i]);
  if (h.notes != NULL)
    g_hash_table_destroy (h.notes);
  g_free (h.base);
  g_free (contents);
  lxb_html_document_destroy (ldoc);
  w42_pt_clear_undo (pt);
  return TRUE;
}
