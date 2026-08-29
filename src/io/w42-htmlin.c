/* w42-htmlin.c - see w42-htmlin.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-htmlin.h"

#include <string.h>
#include <stdlib.h>

#include "w42-image.h"

/* ---------------------------------------------------------------------- */
/* The writer: text goes in at `pos` with the formatting on the stack      */
/* ---------------------------------------------------------------------- */

#define MAX_DEPTH 64

typedef struct {
  W42PieceTable *pt;
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
  int            table_row, table_col, table_cols;
  gboolean       in_cell;
  gboolean       table_before_block;

  int            skip_depth;        /* inside <script> or <style> */
  char          *base;              /* the page's own directory, for its pictures */
  GHashTable    *notes;             /* note id -> its text, found before the body */
  int            note_skip;         /* inside a note's own division: not body text */
  char           note_tag[8];       /* the element that opened it */
  gboolean       in_note_anchor;    /* inside the <a> that refers to a note */
  const char    *pending_bookmark;  /* an empty <a name>: a place, not a run */
  gboolean       in_title;          /* inside <title>, whose text is the title */
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
  if (h->in_title)
    {
      if (h->meta[0] == NULL && len > 0)
        h->meta[0] = g_strndup (text, len);
      return;
    }

  /* Inside a note's reference, or inside the note's own text at the end
   * of the page: neither belongs to the document's text. */
  if (h->in_note_anchor || h->note_skip > 0)
    return;

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

      /* A page's &nbsp; is mostly a placeholder for nothing; treat it as
       * the space it looks like. */
      if (c == 0xA0)
        c = ' ';

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

  w42_pt_apply_para_fmt (h->pt, h->pos, 0, W42_PARA_ALL, &h->pa);

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

static void
open_cell (Html *h)
{
  if (h->table < 0 || h->in_cell)
    return;

  flush_text (h);
  w42_pt_insert_cell (h->pt, h->pos, h->table, h->table_row, h->table_col,
                      html_ap (h));
  h->pos += 2;
  h->in_cell = TRUE;
  h->at_para_start = TRUE;
  h->space_pending = FALSE;
}

static void
close_cell (Html *h)
{
  if (h->table < 0 || !h->in_cell)
    return;

  flush_text (h);
  w42_pt_apply_para_fmt (h->pt, h->pos, 0, W42_PARA_ALL, &h->pa);
  h->in_cell = FALSE;
  h->in_para = FALSE;
  h->pa_dirty = FALSE;
  h->cell_break_pending = FALSE;
  h->table_col++;
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
      open_cell (h);
      close_cell (h);
    }
  h->table_row++;
  h->table_col = 0;
}

static void
open_table (Html *h, int cols)
{
  if (h->table >= 0)
    return;

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
/* The tokenizer                                                           */
/* ---------------------------------------------------------------------- */

/* The value of attribute `name` in a tag's attribute text, unescaped as
 * far as the entities in it; NULL when absent. */
static char *
attr_value (const char *attrs, const char *name)
{
  const char *p = attrs;
  gsize nlen = strlen (name);

  while ((p = strstr (p, name)) != NULL)
    {
      const char *q = p + nlen;
      gboolean word_start = p == attrs || g_ascii_isspace (p[-1]);

      p = q;
      if (!word_start)
        continue;
      while (g_ascii_isspace (*q)) q++;
      if (*q != '=')
        continue;
      q++;
      while (g_ascii_isspace (*q)) q++;
      if (*q == '"' || *q == '\'')
        {
          char quote = *q++;
          const char *e = strchr (q, quote);

          return g_strndup (q, e != NULL ? (gsize) (e - q) : strlen (q));
        }
      else
        {
          const char *e = q;
          while (*e && !g_ascii_isspace (*e) && *e != '>') e++;
          return g_strndup (q, (gsize) (e - q));
        }
    }
  return NULL;
}

static void
append_entity (GString *out, const char *ent, gsize len)
{
  static const struct { const char *name; gunichar c; } table[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "nbsp", 0xA0 }, { "emsp", 0x2003 }, { "ensp", 0x2002 }, { "thinsp", 0x2009 },
    { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "hellip", 0x2026 }, { "bull", 0x2022 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "euro", 0x20AC },
    { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "sect", 0xA7 },
    { "para", 0xB6 }, { "deg", 0xB0 }, { "plusmn", 0xB1 }, { "times", 0xD7 },
    { "divide", 0xF7 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "middot", 0xB7 },
    { "aring", 0xE5 }, { "Aring", 0xC5 }, { "aelig", 0xE6 }, { "AElig", 0xC6 },
    { "oslash", 0xF8 }, { "Oslash", 0xD8 }, { "auml", 0xE4 }, { "ouml", 0xF6 },
    { "uuml", 0xFC }, { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC },
    { "szlig", 0xDF }, { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "agrave", 0xE0 },
    { "ccedil", 0xE7 }, { "ntilde", 0xF1 },
  };

  if (len > 1 && ent[0] == '#')
    {
      gunichar c = (ent[1] == 'x' || ent[1] == 'X')
                     ? (gunichar) strtoul (ent + 2, NULL, 16)
                     : (gunichar) strtoul (ent + 1, NULL, 10);
      if (c > 0 && g_unichar_validate (c))
        g_string_append_unichar (out, c);
      return;
    }

  for (guint i = 0; i < G_N_ELEMENTS (table); i++)
    if (strlen (table[i].name) == len && strncmp (table[i].name, ent, len) == 0)
      {
        g_string_append_unichar (out, table[i].c);
        return;
      }

  /* Unknown: keep it as written. */
  g_string_append_c (out, '&');
  g_string_append_len (out, ent, len);
  g_string_append_c (out, ';');
}

/* Text with its entities resolved, onto the end of `out`. */
static void
text_run_to (GString *out, const char *text, gsize len)
{
  for (gsize i = 0; i < len; i++)
    {
      if (text[i] == '&')
        {
          const char *semi = memchr (text + i, ';', len - i);

          if (semi != NULL && semi - (text + i) < 12)
            {
              append_entity (out, text + i + 1, (gsize) (semi - text - i - 1));
              i = (gsize) (semi - text);
              continue;
            }
        }
      g_string_append_c (out, text[i]);
    }
}

/* Text between tags, with its entities resolved, handed to add_text. */
static void
text_run (Html *h, const char *text, gsize len)
{
  GString *out = g_string_new (NULL);

  text_run_to (out, text, len);
  add_text (h, out->str, out->len);
  g_string_free (out, TRUE);
}

/* A CSS length in twips.  Points, inches, centimetres, millimetres and
 * pixels are what a word processor's HTML uses; anything else is left
 * alone, since guessing at ems without a font would be worse. */
static int
css_twips (const char *value)
{
  double v = g_ascii_strtod (value, NULL);

  if (strstr (value, "in") != NULL)      return (int) (v * 1440.0);
  if (strstr (value, "cm") != NULL)      return (int) (v * 1440.0 / 2.54);
  if (strstr (value, "mm") != NULL)      return (int) (v * 1440.0 / 25.4);
  if (strstr (value, "pt") != NULL)      return (int) (v * 20.0);
  if (strstr (value, "px") != NULL)      return (int) (v * 15.0);
  return 0;
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
            h->ch[h->depth].size = CLAMP ((int) (v * 2), 2, 3276);
        }
      else if (g_ascii_strcasecmp (key, "color") == 0 && value[0] == '#' && strlen (value) == 7)
        h->ch[h->depth].color = (guint32) strtoul (value + 1, NULL, 16);
      else if (g_ascii_strcasecmp (key, "font-weight") == 0)
        h->ch[h->depth].bold = (g_ascii_strcasecmp (value, "bold") == 0 || atoi (value) >= 600);
      else if (g_ascii_strcasecmp (key, "font-style") == 0)
        h->ch[h->depth].italic = g_ascii_strcasecmp (value, "italic") == 0;
      else if (g_ascii_strcasecmp (key, "text-decoration") == 0)
        {
          if (strstr (value, "underline")) h->ch[h->depth].underline = 1;
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
      else if (para && (g_ascii_strcasecmp (key, "background") == 0 ||
                        g_ascii_strcasecmp (key, "background-color") == 0))
        {
          h->pa.shading = (guint8) css_shading (value);
          h->pa_dirty = TRUE;
        }
      else if (para && g_str_has_prefix (key, "border"))
        {
          /* "border: 1px solid #000000", or one side of it. */
          if (strstr (value, "none") == NULL && strstr (value, "0") != value)
            {
              if (g_ascii_strcasecmp (key, "border") == 0)
                h->pa.border |= W42_BORDER_BOX;
              else if (g_ascii_strcasecmp (key, "border-top") == 0)
                h->pa.border |= W42_BORDER_TOP;
              else if (g_ascii_strcasecmp (key, "border-bottom") == 0)
                h->pa.border |= W42_BORDER_BOTTOM;
              else if (g_ascii_strcasecmp (key, "border-left") == 0)
                h->pa.border |= W42_BORDER_LEFT;
              else if (g_ascii_strcasecmp (key, "border-right") == 0)
                h->pa.border |= W42_BORDER_RIGHT;
              h->pa_dirty = TRUE;
            }
        }
    }
  g_strfreev (decls);
}

/* The text of an element, with its tags taken out: what a note says. */
static char *
element_text (const char *start, const char *stop)
{
  GString *out = g_string_new (NULL);
  const char *p = start;

  while (p < stop)
    {
      const char *lt = memchr (p, '<', (gsize) (stop - p));
      const char *gt;

      if (lt == NULL)
        {
          text_run_to (out, p, (gsize) (stop - p));
          break;
        }
      text_run_to (out, p, (gsize) (lt - p));
      gt = memchr (lt, '>', (gsize) (stop - lt));
      if (gt == NULL)
        break;
      p = gt + 1;
    }
  return g_strstrip (g_string_free (out, FALSE));
}

/* A word processor writing HTML puts its footnotes at the end and links
 * to them: LibreOffice as <div id="sdfootnote1">, Word42 as a paragraph
 * with id="note1".  Both are gathered here, before the body is walked,
 * so that a reference can become a real note when it is met. */
static void
scan_notes (Html *h, const char *data, gsize len)
{
  static const char *const KEYS[] = { "id=\"sdfootnote", "id=\"sdendnote",
                                      "id=\"note", "id=\"notee" };
  const char *end = data + len;

  h->notes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (guint k = 0; k < G_N_ELEMENTS (KEYS); k++)
    {
      const char *p = data;

      while ((p = g_strstr_len (p, end - p, KEYS[k])) != NULL)
        {
          const char *quote = strchr (p, '"');
          const char *id_start = quote != NULL ? quote + 1 : NULL;
          const char *id_end = id_start != NULL ? strchr (id_start, '"') : NULL;
          const char *lt = p;
          const char *gt;
          char *id, *tag, *text;
          const char *close;

          if (id_end == NULL || id_end > end)
            break;

          /* Back to the element this attribute belongs to. */
          while (lt > data && *lt != '<')
            lt--;
          gt = memchr (lt, '>', (gsize) (end - lt));
          if (gt == NULL)
            break;

          {
            const char *q = lt + 1;
            GString *name = g_string_new (NULL);

            while (q < gt && g_ascii_isalnum (*q))
              g_string_append_c (name, g_ascii_tolower (*q++));
            tag = g_string_free (name, FALSE);
          }

          id = g_strndup (id_start, (gsize) (id_end - id_start));

          {
            char *closer = g_strdup_printf ("</%s", tag);

            close = g_strstr_len (gt, end - gt, closer);
            g_free (closer);
          }
          text = element_text (gt + 1, close != NULL ? close : end);

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

          if (*text != '\0' && !g_hash_table_contains (h->notes, id))
            g_hash_table_insert (h->notes, id, text);
          else
            {
              g_free (id);
              g_free (text);
            }
          g_free (tag);
          p = gt;
        }
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

      if (h->base == NULL || strstr (src, "://") != NULL || strchr (src, ':') != NULL ||
          g_str_has_prefix (src, "/") || strstr (src, "..") != NULL)
        return;

      unescaped = g_uri_unescape_string (src, NULL);
      path = g_build_filename (h->base, unescaped != NULL ? unescaped : src, NULL);
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
  w = width != NULL && atoi (width) > 0 ? CLAMP (atoi (width), 1, 2000) * 15 : MIN (pw, 2000) * 15;
  hh = height != NULL && atoi (height) > 0 ? CLAMP (atoi (height), 1, 2000) * 15 : MIN (ph, 2000) * 15;

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
count_columns (const char *p, const char *end)
{
  int cols = 0;
  const char *row_end;

  row_end = g_strstr_len (p, end - p, "</tr");
  if (row_end == NULL)
    row_end = end;
  while (p < row_end)
    {
      const char *lt = memchr (p, '<', row_end - p);

      if (lt == NULL)
        break;
      if ((g_ascii_strncasecmp (lt, "<td", 3) == 0 || g_ascii_strncasecmp (lt, "<th", 3) == 0) &&
          (lt[3] == '>' || g_ascii_isspace (lt[3]) || lt[3] == '/'))
        {
          const char *gt = memchr (lt, '>', row_end - lt);
          char *tag = gt != NULL ? g_strndup (lt, gt - lt) : NULL;
          char *span = tag != NULL ? attr_value (tag + 3, "colspan") : NULL;

          cols += span != NULL && atoi (span) > 1 ? CLAMP (atoi (span), 1, 1023) : 1;
          cols = MIN (cols, 1023);
          g_free (span);
          g_free (tag);
        }
      p = lt + 1;
    }
  return cols;
}

static void
handle_tag (Html *h, const char *name, const char *attrs, gboolean closing,
            gboolean self_closing, const char *after, const char *end)
{
  char *style;

  (void) self_closing;

  if (g_str_equal (name, "title") && closing)
    {
      h->in_title = FALSE;
      return;
    }

  if (h->skip_depth > 0)
    {
      if (closing && (g_str_equal (name, "script") || g_str_equal (name, "style")))
        h->skip_depth--;
      return;
    }

  if (!closing && (g_str_equal (name, "script") || g_str_equal (name, "style")))
    {
      h->skip_depth++;
      return;
    }

  /* Block elements end the paragraph they are in. */
  if (g_str_equal (name, "p") || g_str_equal (name, "div") ||
      (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') ||
      g_str_equal (name, "li") || g_str_equal (name, "blockquote") ||
      g_str_equal (name, "pre"))
    {
      if (!closing)
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
          if ((style = attr_value (attrs, "style")) != NULL)
            {
              apply_style (h, style, TRUE);
              g_free (style);
            }
          if ((style = attr_value (attrs, "align")) != NULL)
            {
              if (g_ascii_strcasecmp (style, "center") == 0) h->pa.align = W42_ALIGN_CENTER;
              else if (g_ascii_strcasecmp (style, "right") == 0) h->pa.align = W42_ALIGN_RIGHT;
              else if (g_ascii_strcasecmp (style, "justify") == 0) h->pa.align = W42_ALIGN_JUSTIFY;
              h->pa_dirty = TRUE;
              g_free (style);
            }
          if ((style = attr_value (attrs, "dir")) != NULL)
            {
              h->pa.rtl = g_ascii_strcasecmp (style, "rtl") == 0;
              h->pa_dirty = TRUE;
              g_free (style);
            }
        }
      else
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
        }
      return;
    }

  if (g_str_equal (name, "br"))
    {
      if (h->space_pending)
        g_string_append_c (h->pending, ' ');
      g_string_append_unichar (h->pending, 0x2028);
      h->space_pending = FALSE;
      h->at_para_start = FALSE;
      return;
    }

  if (g_str_equal (name, "ul") || g_str_equal (name, "ol"))
    {
      end_paragraph (h);
      if (!closing)
        {
          char *type = attr_value (attrs, "type");
          char *start = attr_value (attrs, "start");
          char *ul_style = attr_value (attrs, "style");

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
        }
      else if (h->list_depth > 0)
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
      if (!closing)
        open_table (h, count_columns (after, end));
      else
        close_table (h);
      return;
    }
  if (g_str_equal (name, "tr"))
    {
      if (closing)
        end_row (h);
      else if (h->table >= 0 && (h->in_cell || h->table_col > 0))
        end_row (h);
      return;
    }
  if (g_str_equal (name, "td") || g_str_equal (name, "th"))
    {
      if (!closing)
        {
          close_cell (h);
          open_cell (h);
          if (g_str_equal (name, "th"))
            h->ch[h->depth].bold = 1;
          if ((style = attr_value (attrs, "style")) != NULL)
            {
              apply_style (h, style, TRUE);
              g_free (style);
            }
        }
      else
        {
          close_cell (h);
          h->ch[h->depth].bold = 0;
        }
      return;
    }

  /* Inline elements push and pop the character formatting. */
  if (closing)
    {
      if (g_str_equal (name, "a") && h->in_note_anchor)
        {
          h->in_note_anchor = FALSE;
          return;
        }
      if (h->note_skip > 0 && g_str_equal (name, h->note_tag))
        {
          h->note_skip--;
          if (h->note_skip == 0)
            h->note_tag[0] = '\0';
          return;
        }
      if (g_str_equal (name, "b") || g_str_equal (name, "strong") || g_str_equal (name, "i") ||
          g_str_equal (name, "em") || g_str_equal (name, "u") || g_str_equal (name, "s") ||
          g_str_equal (name, "strike") || g_str_equal (name, "del") || g_str_equal (name, "a") ||
          g_str_equal (name, "sup") || g_str_equal (name, "sub") || g_str_equal (name, "span") ||
          g_str_equal (name, "font") || g_str_equal (name, "code") || g_str_equal (name, "tt") ||
          g_str_equal (name, "small") || g_str_equal (name, "big") || g_str_equal (name, "mark"))
        {
          flush_text (h);
          pop_char (h);
        }
      return;
    }

  /* The page's own copy of a note, at its end: the document has the note
   * already, so this is passed over. */
  {
    char *id = attr_value (attrs, "id");

    if (id != NULL && h->notes != NULL && g_hash_table_contains (h->notes, id) &&
        h->note_skip == 0)
      {
        g_strlcpy (h->note_tag, name, sizeof h->note_tag);
        h->note_skip = 1;
        end_paragraph (h);
        g_free (id);
        return;
      }
    g_free (id);
    if (h->note_skip > 0 && g_str_equal (name, h->note_tag))
      h->note_skip++;
  }

  if (g_str_equal (name, "meta"))
    {
      /* What a word processor writing HTML says about the document. */
      static const struct { const char *name; int slot; } NAMES[] = {
        { "title", 0 }, { "subject", 1 }, { "classification", 1 },
        { "author", 2 }, { "changedby", 2 }, { "creator", 2 },
        { "keywords", 3 }, { "description", 4 }, { "comments", 4 },
      };
      char *what = attr_value (attrs, "name");
      char *content = attr_value (attrs, "content");

      if (what != NULL && content != NULL && *content != '\0')
        for (guint i = 0; i < G_N_ELEMENTS (NAMES); i++)
          if (g_ascii_strcasecmp (what, NAMES[i].name) == 0 &&
              h->meta[NAMES[i].slot] == NULL)
            h->meta[NAMES[i].slot] = g_strdup (content);
      g_free (what);
      g_free (content);
      return;
    }

  if (g_str_equal (name, "title"))
    {
      h->in_title = TRUE;
      return;
    }

  if (g_str_equal (name, "img"))
    {
      char *src = attr_value (attrs, "src");
      char *w = attr_value (attrs, "width");
      char *hh = attr_value (attrs, "height");

      if (h->table >= 0)
        open_cell (h);
      picture (h, src, w, hh);
      g_free (src); g_free (w); g_free (hh);
      return;
    }

  flush_text (h);
  push_char (h);
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
      char *href = attr_value (attrs, "href");
      char *anchor = attr_value (attrs, "name");
      char *note_id = note_id_for (href);
      const char *note_text = note_id != NULL && h->notes != NULL
        ? g_hash_table_lookup (h->notes, note_id) : NULL;

      if (note_text != NULL)
        {
          /* A link to a note at the end of the page is a note: the
           * document gets the real thing, and the number the page shows
           * is left out, since a note numbers itself. */
          gsize body;

          flush_text (h);
          body = g_str_has_prefix (note_id, "sdendnote") || g_str_has_prefix (note_id, "notee")
                   ? w42_pt_insert_endnote (h->pt, h->pos, html_ap (h))
                   : w42_pt_insert_footnote (h->pt, h->pos, html_ap (h));

          if (body != (gsize) -1)
            {
              w42_pt_insert_text (h->pt, body, note_text, w42_pt_ap_at (h->pt, body));
              h->pos += 1;             /* the mark the note left behind */
              h->in_note_anchor = TRUE;
              h->at_para_start = FALSE;
              h->in_para = TRUE;
            }
          g_free (note_id);
          g_free (href);
          g_free (anchor);
          return;
        }
      g_free (note_id);

      if (href != NULL && *href != '\0' && *href != '#')
        h->ch[h->depth].link = g_intern_string (href);
      if (anchor == NULL)
        anchor = attr_value (attrs, "id");
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
      char *face = attr_value (attrs, "face");
      char *color = attr_value (attrs, "color");

      if (face != NULL) h->ch[h->depth].family = g_intern_string (face);
      if (color != NULL && color[0] == '#' && strlen (color) == 7)
        h->ch[h->depth].color = (guint32) strtoul (color + 1, NULL, 16);
      g_free (face); g_free (color);
    }
  else if (!g_str_equal (name, "span"))
    {
      /* Something else inline, or unknown: no formatting of its own, but
       * it still pops, so push the same. */
    }

  if ((style = attr_value (attrs, "style")) != NULL)
    {
      apply_style (h, style, FALSE);
      g_free (style);
    }

  /* Unknown elements were pushed too; their closing tag is ignored above,
   * so pop now unless it is one we track. */
  if (!(g_str_equal (name, "b") || g_str_equal (name, "strong") || g_str_equal (name, "i") ||
        g_str_equal (name, "em") || g_str_equal (name, "u") || g_str_equal (name, "s") ||
        g_str_equal (name, "strike") || g_str_equal (name, "del") || g_str_equal (name, "a") ||
        g_str_equal (name, "sup") || g_str_equal (name, "sub") || g_str_equal (name, "span") ||
        g_str_equal (name, "font") || g_str_equal (name, "code") || g_str_equal (name, "tt") ||
        g_str_equal (name, "small") || g_str_equal (name, "big") || g_str_equal (name, "mark")))
    pop_char (h);
}

/* ---------------------------------------------------------------------- */

/* The head is not walked with the body -- the reader starts at <body> --
 * so what it says about the document is picked out of it here: the title
 * and the <meta> names a word processor writes when it saves a page. */
static void
read_head (Html *h, const char *start, const char *stop)
{
  const char *p = start;

  while (p < stop)
    {
      const char *lt = memchr (p, '<', (gsize) (stop - p));
      const char *gt;

      if (lt == NULL)
        break;
      gt = memchr (lt, '>', (gsize) (stop - lt));
      if (gt == NULL)
        break;

      if (g_ascii_strncasecmp (lt, "<title", 6) == 0 && h->meta[0] == NULL)
        {
          const char *close = g_strstr_len (gt, stop - gt, "<");

          if (close != NULL && close > gt + 1)
            {
              char *text = g_strndup (gt + 1, (gsize) (close - gt - 1));

              h->meta[0] = g_strstrip (text);
            }
        }
      else if (g_ascii_strncasecmp (lt, "<meta", 5) == 0)
        {
          static const struct { const char *name; int slot; } NAMES[] = {
            { "title", 0 }, { "subject", 1 }, { "classification", 1 },
            { "author", 2 }, { "changedby", 2 }, { "creator", 2 },
            { "keywords", 3 }, { "description", 4 }, { "comments", 4 },
          };
          char *attrs = g_strndup (lt + 5, (gsize) (gt - lt - 5));
          char *what = attr_value (attrs, "name");
          char *content = attr_value (attrs, "content");

          if (what != NULL && content != NULL && *content != '\0')
            for (guint i = 0; i < G_N_ELEMENTS (NAMES); i++)
              if (g_ascii_strcasecmp (what, NAMES[i].name) == 0 && h->meta[NAMES[i].slot] == NULL)
                h->meta[NAMES[i].slot] = g_strdup (content);
          g_free (what);
          g_free (content);
          g_free (attrs);
        }
      p = gt + 1;
    }
}

gboolean
w42_html_import (W42PieceTable *pt, W42PageSetup *page, GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  const char *p, *end;
  Html h;
  W42Fmt def;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  (void) page;

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

  w42_pt_load_text (pt, "");

  memset (&h, 0, sizeof h);
  h.pt = pt;
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

  /* The body only, if there is one marked. */
  p = contents;
  end = contents + length;
  scan_notes (&h, contents, length);
  {
    const char *body = g_strstr_len (contents, length, "<body");
    if (body == NULL)
      body = g_strstr_len (contents, length, "<BODY");
    if (body != NULL)
      {
        const char *gt = memchr (body, '>', end - body);
        if (gt != NULL)
          p = gt + 1;
        read_head (&h, contents, body);
      }
  }

  while (p < end)
    {
      const char *lt = memchr (p, '<', end - p);

      if (lt == NULL)
        {
          if (h.skip_depth == 0)
            text_run (&h, p, end - p);
          break;
        }
      if (lt > p && h.skip_depth == 0)
        text_run (&h, p, lt - p);

      /* Comments, doctype and the like. */
      if (g_str_has_prefix (lt, "<!--"))
        {
          const char *close = g_strstr_len (lt, end - lt, "-->");
          p = close != NULL ? close + 3 : end;
          continue;
        }
      if (lt + 1 < end && (lt[1] == '!' || lt[1] == '?'))
        {
          const char *gt = memchr (lt, '>', end - lt);
          p = gt != NULL ? gt + 1 : end;
          continue;
        }

      {
        const char *gt = memchr (lt, '>', end - lt);
        const char *q = lt + 1;
        gboolean closing = FALSE, self_closing = FALSE;
        char name[16];
        guint n = 0;
        char *attrs;

        if (gt == NULL)
          break;
        if (*q == '/')
          {
            closing = TRUE;
            q++;
          }
        while (q < gt && (g_ascii_isalnum (*q)) && n < sizeof name - 1)
          name[n++] = (char) g_ascii_tolower (*q++);
        name[n] = '\0';
        if (gt > lt && gt[-1] == '/')
          self_closing = TRUE;
        attrs = g_strndup (q, (gsize) (gt - q));

        if (n > 0)
          {
            if (g_str_equal (name, "body") && closing)
              {
                g_free (attrs);
                break;
              }
            handle_tag (&h, name, attrs, closing, self_closing, gt + 1, end);
          }
        g_free (attrs);
        p = gt + 1;
      }
    }

  close_table (&h);
  flush_text (&h);
  if (h.pa_dirty)
    w42_pt_apply_para_fmt (pt, h.pos, 0, W42_PARA_ALL, &h.pa);

  /* The last block element's close left an empty paragraph behind, as a
   * trailing newline would in a text file.  Drop it. */
  if (!h.in_para && h.pos >= 1 && h.pos == w42_pt_length (pt))
    {
      gsize first = w42_pt_first_caret_pos (pt);
      if (h.pos - 1 > first)
        w42_pt_delete (pt, h.pos - 1, 1);
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
  w42_pt_clear_undo (pt);
  return TRUE;
}
