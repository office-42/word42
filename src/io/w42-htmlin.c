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
#include <glib/gi18n.h>

#include <lexbor/dom/dom.h>
#include <lexbor/html/html.h>

#include "w42-html.h"
#include "w42-image.h"
#include "w42-lang.h"

/* ---------------------------------------------------------------------- */
/* The writer: text goes in at `pos` with the formatting on the stack      */
/* ---------------------------------------------------------------------- */

#define MAX_DEPTH 128

/* What an element's start left for its end to undo. */
enum {
  FLAG_CHAR  = 1 << 0,    /* a formatting level was pushed */
  FLAG_PRE   = 1 << 1,    /* whitespace was being kept */
  FLAG_INNER = 1 << 2,    /* a part of a table read as paragraphs */
};

/* One rule of the page's own stylesheet, as far as this reader follows
 * it: a plain compound selector and its declarations. */
typedef struct {
  const char *tag;       /* interned lowercase element name, or NULL for any */
  const char *cls;       /* interned class, or NULL */
  const char *id;        /* interned id, or NULL */
  int         weight;    /* specificity: an id 100, a class 10, an element 1 */
  guint       order;     /* where it came in the sheet: ties go to the later */
  char       *decl;      /* the declarations, "a:b;c:d" */
} CssRule;

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
  int            space_depth;       /* the formatting level it was met at */
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
  int            table_nest;        /* tables open inside the one being
                                     * read, or in a note: their cells are
                                     * read as paragraphs */

  char          *base;              /* the page's own directory, for its pictures */
  GHashTable    *notes;             /* note id -> Note, found before the body */
  GHashTable    *note_parts;        /* the notes' elements, which the body
                                     * walk passes over */
  gboolean       in_note;           /* a note's own paragraphs are being read */
  gsize          note_start;        /* where that note's text begins */
  gboolean       note_skip_space;   /* the space after the note's number */
  const char    *pending_bookmark;  /* an empty <a name>: a place, not a run */
  char          *meta[5];           /* title, subject, author, keywords, comments */
  int            pre_depth;         /* inside <pre>: whitespace kept */
  gboolean       cell_break_pending; /* a paragraph ended in a cell; the
                                      * next text starts a new one */
  GPtrArray     *rules;             /* the page's <style> rules: CssRule */
  GHashTable    *rules_by_tag;      /* interned name -> GPtrArray of the
                                     * rules naming it and nothing else */
  GHashTable    *rules_by_class;    /* interned class -> GPtrArray */
  GHashTable    *rules_by_id;       /* interned id -> GPtrArray */
  W42Align       cell_align;        /* the open cell's, for each paragraph in it */
  guint          cell_rtl : 1;
  int            mso_item;          /* Word's HTML: the paragraph is a list
                                     * item whose marker is text to skip */
} Html;

static void open_cell (Html *h);

static W42ApIdx
html_ap (Html *h)
{
  W42Fmt fmt;

  w42_fmt_init_default (&fmt);
  fmt.ch = h->ch[h->depth];
  fmt.pa = h->pa;
  return w42_ap_table_intern (w42_pt_ap_table (h->pt), &fmt);
}

/* Makes the place text or a picture goes in. */
static void
open_run (Html *h)
{
  /* Text between a table's cells -- a <caption>, or a page that puts
   * words straight into a row -- goes into a cell, since the model has
   * nowhere else in a table to put it. */
  if (h->table >= 0 && !h->in_cell)
    open_cell (h);

  /* In a cell, a paragraph that ended waits for more before its
   * successor is made, so a cell never ends with an empty one. */
  if (h->table >= 0 && h->in_cell && h->cell_break_pending)
    {
      w42_pt_insert_block (h->pt, h->pos, html_ap (h));
      h->pos += 1;
      h->cell_break_pending = FALSE;
    }
}

static void
flush_text (Html *h)
{
  if (h->pending->len == 0)
    return;

  open_run (h);

  {
    /* What went in is measured, not counted from the string: the model
     * drops control characters, and a count that included them would
     * leave `pos` short of the text and the next paragraph inside it. */
    gsize before = w42_pt_length (h->pt);
    gsize n;

    w42_pt_insert_text (h->pt, h->pos, h->pending->str, html_ap (h));
    n = w42_pt_length (h->pt) - before;

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

/* True when a level was pushed; past the stack's depth the element
 * shares its parent's level, and its end must not pop one. */
static gboolean
push_char (Html *h)
{
  if (h->depth + 1 < MAX_DEPTH)
    {
      h->ch[h->depth + 1] = h->ch[h->depth];
      h->depth++;
      return TRUE;
    }
  return FALSE;
}

static void
pop_char (Html *h)
{
  if (h->depth > 0)
    h->depth--;
  /* A space met inside the element that ended is now between it and
   * what follows, and goes with the outer of the two. */
  if (h->space_pending && h->space_depth > h->depth)
    h->space_depth = h->depth;
}

/* Writes the collapsed space that is waiting.  It belongs to the outer of
 * the two runs it stands between -- in "a <b>b</b>" it is not bold, and
 * in "<u>a</u> b" not underlined -- as a browser draws it; a space met at
 * a level the text has since gone deeper than is written in that level's
 * formatting, as a run of its own. */
static void
emit_space (Html *h)
{
  if (!h->space_pending)
    return;
  h->space_pending = FALSE;
  if (h->space_depth < h->depth)
    {
      int depth = h->depth;
      const char *bookmark = h->pending_bookmark;

      flush_text (h);
      g_string_append_c (h->pending, ' ');
      h->depth = h->space_depth;
      h->pending_bookmark = NULL;       /* the place is the text's, after it */
      flush_text (h);
      h->pending_bookmark = bookmark;
      h->depth = depth;
    }
  else
    g_string_append_c (h->pending, ' ');
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

      /* The space Word42 writes after a note's number, which is not the
       * note's text. */
      if (h->note_skip_space)
        {
          h->note_skip_space = FALSE;
          if (c == ' ')
            {
              i += n;
              continue;
            }
        }

      /* White space between a table's cells is the page's layout, not
       * text, even where whitespace is kept: were it text, it would need
       * a cell to go in, between cells where there is none. */
      if ((h->pre_depth == 0 || (h->table >= 0 && !h->in_cell)) &&
          (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'))
        {
          if (!h->at_para_start && !h->space_pending)
            {
              h->space_pending = TRUE;
              h->space_depth = h->depth;
            }
        }
      else
        {
          if (h->pre_depth > 0 && c == '\n')
            {
              g_string_append_unichar (h->pending, 0x2028);
            }
          else
            {
              emit_space (h);
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
  /* Word42 writes an empty paragraph as a lone &nbsp;, which a browser
   * gives its line; read back, the paragraph is empty again. */
  if (!h->in_para && h->pending->len == 2 &&
      memcmp (h->pending->str, "\302\240", 2) == 0)
    {
      g_string_truncate (h->pending, 0);
      h->pa_dirty = TRUE;
    }
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
  h->mso_item = 0;
  {
    W42Fmt def;
    w42_fmt_init_default (&def);
    h->pa = def.pa;
    if (h->table >= 0 && h->in_cell)
      {
        /* The cell's own alignment, for every paragraph in it. */
        h->pa.align = h->cell_align;
        h->pa.rtl = h->cell_rtl;
      }
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

  /* No flush_text here: text waiting is what the cell is opened for, and
   * flushing it would open this cell for it, and so on for ever.  Columns
   * a cell above still covers come first. */
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
        h->covered[c] = MIN (rowspan, 254) - 1;
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
  h->cell_align = W42_ALIGN_LEFT;
  h->cell_rtl = 0;
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
  {
    /* The end of the body, which is not the end of the document once a
     * note has put the notes section after it. */
    gsize body_end = w42_pt_notes_start (h->pt);

    if (body_end == (gsize) -1)
      body_end = w42_pt_length (h->pt);
    if (h->pos >= 2 && h->pos == body_end)
      {
        char *tail = w42_pt_get_text (h->pt, h->pos - 1, 1);
        h->table_before_block = (tail != NULL && *tail == '\n');
        g_free (tail);
      }
  }
  if (h->table_before_block)
    h->pos -= 1;

  h->table = w42_pt_insert_table_start (h->pt, h->pos, CLAMP (cols, 1, 1023), NULL);
  h->pos += 1;
  h->table_row = 0;
  h->table_col = 0;
  h->table_cols = CLAMP (cols, 1, 1023);
  h->in_cell = FALSE;
  /* Nothing of the table before carries over: a rowspan that ran past
   * its last row, or a <colgroup> it never closed. */
  memset (h->covered, 0, sizeof h->covered);
  h->n_col_widths = 0;
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
  /* A rowspan says how far a cell reaches, and a page may say further
   * than its table goes: the merges are made what the rows have. */
  w42_pt_resolve_vmerges (h->pt, h->table);
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

/* ---------------------------------------------------------------------- */
/* CSS, as far as a word processor's HTML leans on it                      */
/* ---------------------------------------------------------------------- */

/* A CSS length in twips.  Points, inches, centimetres, millimetres and
 * pixels are what a word processor's HTML uses; an em is relative to the
 * font in force, which the caller says the size of in twips, or 0 when
 * there is none to measure by.  Anything else is left alone. */
static int
css_twips_em (const char *value, int em)
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
  else if (g_str_has_prefix (unit, "Q"))  per = 1440.0 / 25.4 / 4;
  else if (g_str_has_prefix (unit, "rem")) per = 240.0;   /* the root's 12pt */
  else if (g_str_has_prefix (unit, "em") && em > 0) per = em;
  else if (g_str_has_prefix (unit, "ex") && em > 0) per = em / 2.0;
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

static int
css_twips (const char *value)
{
  return css_twips_em (value, 0);
}

/* The named colours of CSS Color Level 4, and Word's "windowtext". */
static const struct { const char *name; guint32 rgb; } CSS_COLOURS[] = {
  { "aliceblue", 0xF0F8FF }, { "antiquewhite", 0xFAEBD7 }, { "aqua", 0x00FFFF },
  { "aquamarine", 0x7FFFD4 }, { "azure", 0xF0FFFF }, { "beige", 0xF5F5DC },
  { "bisque", 0xFFE4C4 }, { "black", 0x000000 }, { "blanchedalmond", 0xFFEBCD },
  { "blue", 0x0000FF }, { "blueviolet", 0x8A2BE2 }, { "brown", 0xA52A2A },
  { "burlywood", 0xDEB887 }, { "cadetblue", 0x5F9EA0 }, { "chartreuse", 0x7FFF00 },
  { "chocolate", 0xD2691E }, { "coral", 0xFF7F50 }, { "cornflowerblue", 0x6495ED },
  { "cornsilk", 0xFFF8DC }, { "crimson", 0xDC143C }, { "cyan", 0x00FFFF },
  { "darkblue", 0x00008B }, { "darkcyan", 0x008B8B }, { "darkgoldenrod", 0xB8860B },
  { "darkgray", 0xA9A9A9 }, { "darkgreen", 0x006400 }, { "darkgrey", 0xA9A9A9 },
  { "darkkhaki", 0xBDB76B }, { "darkmagenta", 0x8B008B }, { "darkolivegreen", 0x556B2F },
  { "darkorange", 0xFF8C00 }, { "darkorchid", 0x9932CC }, { "darkred", 0x8B0000 },
  { "darksalmon", 0xE9967A }, { "darkseagreen", 0x8FBC8F }, { "darkslateblue", 0x483D8B },
  { "darkslategray", 0x2F4F4F }, { "darkslategrey", 0x2F4F4F }, { "darkturquoise", 0x00CED1 },
  { "darkviolet", 0x9400D3 }, { "deeppink", 0xFF1493 }, { "deepskyblue", 0x00BFFF },
  { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "dodgerblue", 0x1E90FF },
  { "firebrick", 0xB22222 }, { "floralwhite", 0xFFFAF0 }, { "forestgreen", 0x228B22 },
  { "fuchsia", 0xFF00FF }, { "gainsboro", 0xDCDCDC }, { "ghostwhite", 0xF8F8FF },
  { "gold", 0xFFD700 }, { "goldenrod", 0xDAA520 }, { "gray", 0x808080 },
  { "green", 0x008000 }, { "greenyellow", 0xADFF2F }, { "grey", 0x808080 },
  { "honeydew", 0xF0FFF0 }, { "hotpink", 0xFF69B4 }, { "indianred", 0xCD5C5C },
  { "indigo", 0x4B0082 }, { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C },
  { "lavender", 0xE6E6FA }, { "lavenderblush", 0xFFF0F5 }, { "lawngreen", 0x7CFC00 },
  { "lemonchiffon", 0xFFFACD }, { "lightblue", 0xADD8E6 }, { "lightcoral", 0xF08080 },
  { "lightcyan", 0xE0FFFF }, { "lightgoldenrodyellow", 0xFAFAD2 }, { "lightgray", 0xD3D3D3 },
  { "lightgreen", 0x90EE90 }, { "lightgrey", 0xD3D3D3 }, { "lightpink", 0xFFB6C1 },
  { "lightsalmon", 0xFFA07A }, { "lightseagreen", 0x20B2AA }, { "lightskyblue", 0x87CEFA },
  { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 }, { "lightsteelblue", 0xB0C4DE },
  { "lightyellow", 0xFFFFE0 }, { "lime", 0x00FF00 }, { "limegreen", 0x32CD32 },
  { "linen", 0xFAF0E6 }, { "magenta", 0xFF00FF }, { "maroon", 0x800000 },
  { "mediumaquamarine", 0x66CDAA }, { "mediumblue", 0x0000CD }, { "mediumorchid", 0xBA55D3 },
  { "mediumpurple", 0x9370DB }, { "mediumseagreen", 0x3CB371 }, { "mediumslateblue", 0x7B68EE },
  { "mediumspringgreen", 0x00FA9A }, { "mediumturquoise", 0x48D1CC }, { "mediumvioletred", 0xC71585 },
  { "midnightblue", 0x191970 }, { "mintcream", 0xF5FFFA }, { "mistyrose", 0xFFE4E1 },
  { "moccasin", 0xFFE4B5 }, { "navajowhite", 0xFFDEAD }, { "navy", 0x000080 },
  { "oldlace", 0xFDF5E6 }, { "olive", 0x808000 }, { "olivedrab", 0x6B8E23 },
  { "orange", 0xFFA500 }, { "orangered", 0xFF4500 }, { "orchid", 0xDA70D6 },
  { "palegoldenrod", 0xEEE8AA }, { "palegreen", 0x98FB98 }, { "paleturquoise", 0xAFEEEE },
  { "palevioletred", 0xDB7093 }, { "papayawhip", 0xFFEFD5 }, { "peachpuff", 0xFFDAB9 },
  { "peru", 0xCD853F }, { "pink", 0xFFC0CB }, { "plum", 0xDDA0DD },
  { "powderblue", 0xB0E0E6 }, { "purple", 0x800080 }, { "rebeccapurple", 0x663399 },
  { "red", 0xFF0000 }, { "rosybrown", 0xBC8F8F }, { "royalblue", 0x4169E1 },
  { "saddlebrown", 0x8B4513 }, { "salmon", 0xFA8072 }, { "sandybrown", 0xF4A460 },
  { "seagreen", 0x2E8B57 }, { "seashell", 0xFFF5EE }, { "sienna", 0xA0522D },
  { "silver", 0xC0C0C0 }, { "skyblue", 0x87CEEB }, { "slateblue", 0x6A5ACD },
  { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "snow", 0xFFFAFA },
  { "springgreen", 0x00FF7F }, { "steelblue", 0x4682B4 }, { "tan", 0xD2B48C },
  { "teal", 0x008080 }, { "thistle", 0xD8BFD8 }, { "tomato", 0xFF6347 },
  { "turquoise", 0x40E0D0 }, { "violet", 0xEE82EE }, { "wheat", 0xF5DEB3 },
  { "white", 0xFFFFFF }, { "whitesmoke", 0xF5F5F5 }, { "yellow", 0xFFFF00 },
  { "yellowgreen", 0x9ACD32 }, { "windowtext", 0x000000 },
};

/* One channel of rgb(): a number, or a percentage of 255. */
static int
css_channel (const char **p)
{
  char *end = NULL;
  double v;

  while (**p == ' ' || **p == ',' || **p == '/')
    (*p)++;
  v = g_ascii_strtod (*p, &end);
  if (end == *p || isnan (v))
    return -1;
  if (*end == '%')
    {
      v = v * 255.0 / 100.0;
      end++;
    }
  *p = end;
  /* Clamped as a double: "1e999" is infinity, which no int holds. */
  return (int) (CLAMP (v, 0.0, 255.0) + 0.5);
}

/* The colour a CSS value names, wherever in the value it is -- "1px solid
 * red" names one -- as 0x00RRGGBB, or -1 if it names none.  "#rgb" and
 * "#rrggbb", rgb() and rgba(), and the named colours; "transparent" and
 * "none" are none. */
static gint64
css_colour (const char *value)
{
  const char *p;

  if ((p = strchr (value, '#')) != NULL)
    {
      int n = 0;

      while (n < 9 && g_ascii_isxdigit (p[1 + n]))
        n++;
      if (n == 6 || n == 8)
        {
          char hex[7];

          memcpy (hex, p + 1, 6);
          hex[6] = '\0';
          return (gint64) strtoul (hex, NULL, 16);
        }
      if (n == 3 || n == 4)
        {
          guint32 r = g_ascii_xdigit_value (p[1]);
          guint32 g = g_ascii_xdigit_value (p[2]);
          guint32 b = g_ascii_xdigit_value (p[3]);

          return (gint64) ((r * 0x110000) | (g * 0x1100) | (b * 0x11));
        }
    }
  if ((p = strstr (value, "rgb")) != NULL && (p = strchr (p, '(')) != NULL)
    {
      int r, g, b;

      p++;
      r = css_channel (&p);
      g = css_channel (&p);
      b = css_channel (&p);
      if (r >= 0 && g >= 0 && b >= 0)
        return (gint64) (((guint32) r << 16) | ((guint32) g << 8) | (guint32) b);
    }

  /* A name, among whatever else the value says. */
  for (p = value; *p != '\0'; )
    {
      char word[24];
      guint n = 0;

      while (*p != '\0' && !g_ascii_isalpha (*p))
        p++;
      while (g_ascii_isalpha (*p))
        {
          if (n < sizeof word - 1)
            word[n++] = g_ascii_tolower (*p);
          p++;
        }
      word[n] = '\0';
      if (n == 0)
        break;
      for (guint i = 0; i < G_N_ELEMENTS (CSS_COLOURS); i++)
        if (g_str_equal (word, CSS_COLOURS[i].name))
          return CSS_COLOURS[i].rgb;
    }
  return -1;
}

/* True when a border value says there is no line: "none", "hidden", or
 * a width of nought -- "0", "0px" -- and not "0.75pt", which begins
 * with a nought and is a line. */
static gboolean
css_border_none (const char *value)
{
  if (strstr (value, "none") != NULL || strstr (value, "hidden") != NULL)
    return TRUE;
  for (const char *p = value; *p != '\0'; p++)
    if ((g_ascii_isdigit (*p) || *p == '.') && (p == value || *(p - 1) == ' '))
      {
        char *end = NULL;
        double v = g_ascii_strtod (p, &end);

        return end != p && v == 0.0;
      }
  return FALSE;
}

/* The value a list of declarations gives property `name` -- the last, as
 * the cascade has it -- or NULL; freed by the caller.  The name is matched
 * whole, so that "width" is not found in "border-width" or "max-width". */
static char *
css_value (const char *decls, const char *name)
{
  char **list = g_strsplit (decls, ";", -1);
  char *out = NULL;

  for (guint i = 0; list[i] != NULL; i++)
    {
      char *colon = strchr (list[i], ':');
      char *bang;

      if (colon == NULL)
        continue;
      *colon = '\0';
      if (g_ascii_strcasecmp (g_strstrip (list[i]), name) != 0)
        continue;
      g_free (out);
      out = g_strdup (colon + 1);
      if ((bang = strstr (out, "!important")) != NULL)
        *bang = '\0';
      g_strstrip (out);
    }
  g_strfreev (list);
  return out;
}

/* A font size in half-points from a CSS value, against the size in force
 * -- "larger", "120%" and "1.5em" are relative to it -- or 0 for a value
 * that says no size. */
static int
css_font_size (const char *value, int current)
{
  /* The keywords, against a medium of 12pt. */
  static const struct { const char *name; int hp; } KEYS[] = {
    { "xx-small", 14 }, { "x-small", 15 }, { "small", 20 }, { "medium", 24 },
    { "large", 27 }, { "x-large", 36 }, { "xx-large", 48 }, { "xxx-large", 72 },
  };
  char *unit = NULL;
  double v;

  if (current <= 0)
    current = 24;
  for (guint i = 0; i < G_N_ELEMENTS (KEYS); i++)
    if (g_ascii_strcasecmp (value, KEYS[i].name) == 0)
      return KEYS[i].hp;
  if (g_ascii_strcasecmp (value, "smaller") == 0)
    return MAX (current * 5 / 6, 2);
  /* Nested, "larger" compounds; past the largest size there is, it stops. */
  if (g_ascii_strcasecmp (value, "larger") == 0)
    return MIN (current * 6 / 5, 3276);

  v = g_ascii_strtod (value, &unit);
  if (unit == value || v <= 0 || isnan (v))
    return 0;
  while (*unit == ' ')
    unit++;
  if (g_str_has_prefix (unit, "pt"))       v = v * 2;
  else if (g_str_has_prefix (unit, "px"))  v = v * 1.5;
  else if (*unit == '%')                   v = current * v / 100.0;
  else if (g_str_has_prefix (unit, "rem")) v = 24 * v;
  else if (g_str_has_prefix (unit, "em"))  v = current * v;
  else if (*unit == '\0' || *unit == ';')  v = v * 1.5;   /* a bare number: pixels */
  else
    {
      int tw = css_twips (value);

      if (tw <= 0)
        return 0;
      v = tw / 10.0;
    }
  /* Clamped as a double: "1e999pt" is infinity, which no int holds. */
  return (int) (CLAMP (v, 2.0, 3276.0) + 0.5);
}

/* Word's <font size="3">, and "+1": the seven steps, medium the third. */
static int
font_size_attr (const char *value, int current)
{
  static const int STEPS[7] = { 15, 20, 24, 27, 36, 48, 72 };
  int n = (int) CLAMP (g_ascii_strtoll (value, NULL, 10), -100, 100);

  if (*value == '+' || *value == '-')
    n = 3 + n;
  if (n < 1 || n > 7)
    return current;
  return STEPS[n - 1];
}

typedef enum {
  STYLE_SHOWN = 0,
  STYLE_HIDDEN,        /* display:none: not read */
  STYLE_MARKER         /* Word's dialect: the number a list paragraph
                        * carries as text, and Word's tab of spaces */
} StyleShows;

/* The inline styles a word processor's HTML leans on. */
static StyleShows
apply_style (Html *h, const char *style, gboolean para)
{
  char **decls = g_strsplit (style, ";", -1);
  W42CharFmt *ch = &h->ch[h->depth];
  StyleShows hidden = STYLE_SHOWN;

  for (guint i = 0; decls[i] != NULL; i++)
    {
      char *colon = strchr (decls[i], ':');
      char *key, *value, *bang;
      int em;

      if (colon == NULL)
        continue;
      *colon = '\0';
      key = g_strstrip (decls[i]);
      value = g_strstrip (colon + 1);
      /* "!important" weighs one rule against another; by the time a value
       * is here the weighing is done. */
      if ((bang = strstr (value, "!important")) != NULL)
        {
          *bang = '\0';
          g_strstrip (value);
        }
      if (*key == '\0' || *value == '\0')
        continue;
      em = ch->size > 0 ? ch->size * 10 : 240;

      if (g_ascii_strcasecmp (key, "font-family") == 0)
        {
          char *name = g_strdup (value);
          char *comma = strchr (name, ',');
          if (comma) *comma = '\0';
          g_strdelimit (name, "'\"", ' ');
          g_strstrip (name);
          if (*name != '\0')
            ch->family = g_intern_string (name);
          g_free (name);
        }
      else if (g_ascii_strcasecmp (key, "font-size") == 0)
        {
          int hp = css_font_size (value, ch->size);

          if (hp > 0)
            ch->size = hp;
        }
      else if (g_ascii_strcasecmp (key, "font") == 0)
        {
          /* The shorthand: style, weight and variant in any order, then
           * the size (with the line height after a slash), then the
           * family; "font:7.0pt Symbol" is how Word writes a marker. */
          char **tok = g_strsplit (value, " ", -1);
          guint k;

          for (k = 0; tok[k] != NULL; k++)
            {
              char *t = tok[k];
              int hp;

              if (*t == '\0')
                continue;
              if (g_ascii_isdigit (*t) || *t == '.')
                {
                  char *slash = strchr (t, '/');

                  if (slash != NULL)
                    *slash = '\0';
                  if ((hp = css_font_size (t, ch->size)) > 0)
                    ch->size = hp;
                  k++;
                  break;
                }
              if (g_ascii_strcasecmp (t, "italic") == 0 || g_ascii_strcasecmp (t, "oblique") == 0)
                ch->italic = 1;
              else if (g_ascii_strcasecmp (t, "bold") == 0 || g_ascii_strcasecmp (t, "bolder") == 0)
                ch->bold = 1;
              else if (g_ascii_strcasecmp (t, "small-caps") == 0)
                ch->smallcaps = 1;
              else if (g_ascii_strcasecmp (t, "normal") == 0)
                ;
              else
                break;    /* a keyword size, or the family already */
            }
          if (tok[k] != NULL)
            {
              char *family = g_strjoinv (" ", tok + k);
              char *comma = strchr (family, ',');

              if (comma) *comma = '\0';
              g_strdelimit (family, "'\"", ' ');
              g_strstrip (family);
              if (*family != '\0')
                ch->family = g_intern_string (family);
              g_free (family);
            }
          g_strfreev (tok);
        }
      else if (g_ascii_strcasecmp (key, "letter-spacing") == 0)
        ch->spacing = (gint16) CLAMP (css_twips_em (value, em), -720, 720);
      else if (g_ascii_strcasecmp (key, "color") == 0)
        {
          gint64 rgb = css_colour (value);

          if (rgb >= 0)
            ch->color = (guint32) rgb;
        }
      else if (g_ascii_strcasecmp (key, "font-weight") == 0)
        ch->bold = (g_ascii_strcasecmp (value, "bold") == 0 ||
                    g_ascii_strcasecmp (value, "bolder") == 0 || atoi (value) >= 600);
      else if (g_ascii_strcasecmp (key, "font-style") == 0)
        ch->italic = (g_ascii_strcasecmp (value, "italic") == 0 ||
                      g_ascii_strcasecmp (value, "oblique") == 0);
      else if (g_ascii_strcasecmp (key, "text-decoration-style") == 0)
        {
          /* The shape of the line, when the page says one.  On a
           * struck-through element the doubling is the strikethrough's,
           * as Word42's own pages write it. */
          if (ch->strikeout && g_ascii_strcasecmp (value, "double") == 0)
            {
              ch->dstrike = 1;
              ch->strikeout = 0;
            }
          else if (ch->underline == W42_UNDERLINE_NONE && !ch->strikeout)
            ch->underline = W42_UNDERLINE_SINGLE;
          if (ch->dstrike)
            ;
          else if (g_ascii_strcasecmp (value, "double") == 0)
            ch->underline = W42_UNDERLINE_DOUBLE;
          else if (g_ascii_strcasecmp (value, "dotted") == 0)
            ch->underline = W42_UNDERLINE_DOTTED;
          else if (g_ascii_strcasecmp (value, "dashed") == 0)
            ch->underline = W42_UNDERLINE_DASHED;
          else if (g_ascii_strcasecmp (value, "wavy") == 0)
            ch->underline = W42_UNDERLINE_WAVE;
        }
      else if (g_ascii_strcasecmp (key, "text-decoration-thickness") == 0)
        {
          /* Word's thick underline is a line of two pixels or more, as
           * Word42 writes it. */
          if (ch->underline != W42_UNDERLINE_NONE && css_twips (value) >= 30)
            ch->underline = W42_UNDERLINE_THICK;
        }
      else if (g_ascii_strcasecmp (key, "text-decoration-skip") == 0)
        {
          /* Words only: the line leaves the spaces out. */
          if (ch->underline == W42_UNDERLINE_SINGLE && strstr (value, "spaces") != NULL)
            ch->underline = W42_UNDERLINE_WORDS;
        }
      else if (g_ascii_strcasecmp (key, "text-decoration") == 0 ||
               g_ascii_strcasecmp (key, "text-decoration-line") == 0)
        {
          if (g_ascii_strcasecmp (value, "none") == 0)
            {
              ch->underline = W42_UNDERLINE_NONE;
              ch->strikeout = 0;
              ch->overline = 0;
            }
          if (strstr (value, "underline") && ch->underline == W42_UNDERLINE_NONE)
            ch->underline = W42_UNDERLINE_SINGLE;
          if (strstr (value, "double")) ch->underline = W42_UNDERLINE_DOUBLE;
          if (strstr (value, "dotted")) ch->underline = W42_UNDERLINE_DOTTED;
          if (strstr (value, "dashed")) ch->underline = W42_UNDERLINE_DASHED;
          if (strstr (value, "wavy"))   ch->underline = W42_UNDERLINE_WAVE;
          if (strstr (value, "line-through")) ch->strikeout = 1;
          if (strstr (value, "overline")) ch->overline = 1;
        }
      else if (g_ascii_strcasecmp (key, "text-shadow") == 0)
        {
          if (g_ascii_strcasecmp (value, "none") == 0)
            ch->shadow = ch->emboss = ch->engrave = 0;
          else if (ch->color == 0xffffff)
            {
              /* A white letter with a grey shadow is our own relief. */
              if (*value == '-')
                ch->engrave = 1;
              else
                ch->emboss = 1;
              ch->color = 0;
            }
          else
            ch->shadow = 1;
        }
      else if (g_ascii_strcasecmp (key, "-webkit-text-stroke") == 0)
        ch->outline = g_ascii_strcasecmp (value, "none") != 0 && g_ascii_strtod (value, NULL) > 0.0;
      else if (g_ascii_strcasecmp (key, "-webkit-text-fill-color") == 0)
        {
          /* Transparent fill is the other half of our outline; nothing
           * to keep. */
        }
      else if (g_ascii_strcasecmp (key, "vertical-align") == 0)
        {
          if (para)
            {
              /* On a cell: where its text sits. */
              if (g_ascii_strcasecmp (value, "middle") == 0)
                h->pa.cell_valign = W42_CELL_VALIGN_CENTER;
              else if (g_ascii_strcasecmp (value, "bottom") == 0)
                h->pa.cell_valign = W42_CELL_VALIGN_BOTTOM;
              else if (g_ascii_strcasecmp (value, "top") == 0)
                h->pa.cell_valign = W42_CELL_VALIGN_TOP;
            }
          else if (g_ascii_strcasecmp (value, "super") == 0)
            ch->script = 1;
          else if (g_ascii_strcasecmp (value, "sub") == 0)
            ch->script = -1;
          else if (g_ascii_strcasecmp (value, "baseline") == 0)
            ch->script = 0;
        }
      else if (para && g_ascii_strcasecmp (key, "text-align") == 0)
        {
          if (g_ascii_strcasecmp (value, "center") == 0) h->pa.align = W42_ALIGN_CENTER;
          else if (g_ascii_strcasecmp (value, "right") == 0 ||
                   g_ascii_strcasecmp (value, "end") == 0) h->pa.align = W42_ALIGN_RIGHT;
          else if (g_ascii_strcasecmp (value, "justify") == 0) h->pa.align = W42_ALIGN_JUSTIFY;
          else if (g_ascii_strcasecmp (value, "left") == 0 ||
                   g_ascii_strcasecmp (value, "start") == 0) h->pa.align = W42_ALIGN_LEFT;
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "direction") == 0)
        {
          h->pa.rtl = g_ascii_strcasecmp (value, "rtl") == 0;
          h->pa_dirty = TRUE;
        }
      else if (g_ascii_strcasecmp (key, "font-variant") == 0 ||
               g_ascii_strcasecmp (key, "font-variant-caps") == 0)
        ch->smallcaps = strstr (value, "small-caps") != NULL;
      else if (g_ascii_strcasecmp (key, "text-transform") == 0)
        ch->allcaps = g_ascii_strcasecmp (value, "uppercase") == 0;
      else if (g_ascii_strcasecmp (key, "display") == 0)
        {
          if (g_ascii_strcasecmp (value, "none") == 0)
            hidden = STYLE_HIDDEN;
        }
      else if (g_ascii_strcasecmp (key, "visibility") == 0)
        {
          if (g_ascii_strcasecmp (value, "hidden") == 0)
            hidden = STYLE_HIDDEN;
        }
      else if (g_ascii_strcasecmp (key, "mso-list") == 0)
        {
          /* Word's HTML numbers its lists in text: the paragraph says
           * "mso-list:l0 level1 lfo1", and a span inside it, marked
           * Ignore, holds the "1." or the bullet a browser shows. */
          const char *level = strstr (value, "level");

          if (g_ascii_strcasecmp (value, "ignore") == 0)
            hidden = STYLE_MARKER;
          else if (para && level != NULL)
            {
              h->mso_item = CLAMP (atoi (level + 5), 1, 9);
              h->pa.list = W42_LIST_BULLET;
              h->pa.list_level = (guint8) (h->mso_item - 1);
              h->pa_dirty = TRUE;
            }
        }
      else if (g_ascii_strcasecmp (key, "mso-tab-count") == 0)
        {
          /* A tab, written as a span of spaces for the browser's sake. */
          int n = CLAMP (atoi (value), 1, 32);

          for (int k = 0; k < n; k++)
            g_string_append_c (h->pending, '\t');
          h->space_pending = FALSE;
          h->at_para_start = FALSE;
          hidden = STYLE_MARKER;     /* the spaces stand for the tab */
        }
      else if (g_ascii_strcasecmp (key, "white-space") == 0)
        {
          /* Kept whitespace is the element's to the end; the caller
           * notes it from the count. */
          if (g_ascii_strncasecmp (value, "pre", 3) == 0)
            h->pre_depth++;
        }
      else if (!para && (g_ascii_strcasecmp (key, "background") == 0 ||
                         g_ascii_strcasecmp (key, "background-color") == 0))
        {
          /* On a run, a background is a highlight: the nearest of
           * Word's sixteen. */
          gint64 rgb = css_colour (value);

          if (rgb >= 0 && rgb != 0xFFFFFF)
            ch->highlight = (guint8) w42_highlight_nearest ((guint32) rgb);
        }
      else if (para && g_ascii_strcasecmp (key, "margin") == 0)
        {
          /* The shorthand: one value for all four sides, two for the
           * pairs, three or four naming them round from the top. */
          char **tok = g_strsplit (value, " ", -1);
          int m[4] = { 0, 0, 0, 0 };
          int n = 0;

          for (guint k = 0; tok[k] != NULL && n < 4; k++)
            if (*tok[k] != '\0')
              m[n++] = css_twips_em (tok[k], em);
          g_strfreev (tok);
          if (n == 1)      { m[1] = m[2] = m[3] = m[0]; }
          else if (n == 2) { m[2] = m[0]; m[3] = m[1]; }
          else if (n == 3) { m[3] = m[1]; }
          if (n >= 1)
            {
              h->pa.space_before = m[0];
              h->pa.indent_right = m[1];
              h->pa.space_after  = m[2];
              h->pa.indent_left  = m[3];
              h->pa_dirty = TRUE;
            }
        }
      else if (para && g_ascii_strcasecmp (key, "margin-left") == 0)
        {
          h->pa.indent_left = css_twips_em (value, em);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "margin-right") == 0)
        {
          h->pa.indent_right = css_twips_em (value, em);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "text-indent") == 0)
        {
          h->pa.indent_first = css_twips_em (value, em);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "margin-top") == 0)
        {
          h->pa.space_before = css_twips_em (value, em);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "margin-bottom") == 0)
        {
          h->pa.space_after = css_twips_em (value, em);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "line-height") == 0)
        {
          char *unit = NULL;
          double v = g_ascii_strtod (value, &unit);

          if (g_ascii_strcasecmp (value, "normal") == 0)
            {
              h->pa.line_spacing = 0;
              h->pa.line_spacing_pct = 0;
            }
          else if (isnan (v))
            ;
          else if (strchr (value, '%') != NULL)
            {
              /* A hundred lines to the line is more than any document
               * asks, and what OpenDocument's reader allows. */
              int pct = (int) (CLAMP (v, 0.0, 10000.0) + 0.5);

              /* A hundred per cent is single spacing, said outright over
               * what a stylesheet rule said before it. */
              if (pct == 100)
                h->pa.line_spacing_pct = h->pa.line_spacing = 0;
              else if (pct > 0)
                h->pa.line_spacing_pct = pct;
            }
          else if (unit != value && (*unit == '\0' || *unit == ' '))
            {
              /* A bare number is a multiple of the type size. */
              int pct = (int) (CLAMP (v, 0.0, 100.0) * 100 + 0.5);

              if (pct == 100)
                h->pa.line_spacing_pct = h->pa.line_spacing = 0;
              else if (pct > 0)
                h->pa.line_spacing_pct = pct;
            }
          else if (css_twips_em (value, em) > 0)
            h->pa.line_spacing = css_twips_em (value, em);
          h->pa_dirty = TRUE;
        }
      else if (g_ascii_strcasecmp (key, "--w42-line-height") == 0)
        {
          /* What the document says, rather than what the browser is being
           * asked to do with it; see the exporter. */
          int pct = atoi (value);

          if (strchr (value, '%') != NULL && pct >= 20 && pct <= 10000)
            {
              h->pa.line_spacing_pct = pct;
              h->pa.line_spacing = 0;
            }
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "--w42-drop-cap") == 0)
        {
          h->pa.drop_cap = (guint8) CLAMP (atoi (value), 0, 10);
          h->pa_dirty = TRUE;
        }
      else if (para && (g_ascii_strcasecmp (key, "page-break-before") == 0 ||
                        g_ascii_strcasecmp (key, "break-before") == 0))
        {
          h->pa.page_break_before = (g_ascii_strcasecmp (value, "always") == 0 ||
                                     g_ascii_strcasecmp (value, "page") == 0 ||
                                     g_ascii_strcasecmp (value, "left") == 0 ||
                                     g_ascii_strcasecmp (value, "right") == 0);
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "float") == 0)
        {
          /* A paragraph set at the side of the column: a frame. */
          if (g_ascii_strcasecmp (value, "left") == 0)
            h->pa.frame_side = W42_FRAME_LEFT;
          else if (g_ascii_strcasecmp (value, "right") == 0)
            h->pa.frame_side = W42_FRAME_RIGHT;
          else
            h->pa.frame_side = W42_FRAME_NONE;
          h->pa_dirty = TRUE;
        }
      else if (para && g_ascii_strcasecmp (key, "width") == 0)
        {
          /* Only a frame has a width of its own in this model. */
          int w = css_twips_em (value, em);

          if (w > 0)
            h->pa.frame_width = w;
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
          else if (strstr (value, "transparent") != NULL || g_ascii_strcasecmp (value, "none") == 0)
            {
              h->pa.has_shading_color = 0;
              h->pa.shading = 0;
            }
          h->pa_dirty = TRUE;
        }
      else if (para && (g_ascii_strcasecmp (key, "border-width") == 0 ||
                        g_ascii_strcasecmp (key, "border-style") == 0 ||
                        g_ascii_strcasecmp (key, "border-color") == 0))
        {
          /* One property of every side at once.  A style of its own
           * turns the sides on; "none" turns them off. */
          if (key[7] == 's' && g_ascii_strcasecmp (value, "none") == 0)
            h->pa.border &= (guint8) ~W42_BORDER_BOX;
          else
            {
              if (key[7] == 's' && (h->pa.border & W42_BORDER_BOX) == 0)
                h->pa.border |= W42_BORDER_BOX;
              for (int e = 0; e < 4; e++)
                {
                  if (key[7] == 'w')
                    {
                      int w = css_twips_em (value, em);

                      if (w > 0)
                        h->pa.edge[e].width = (guint8) CLAMP (w, 5, 120);
                    }
                  else if (key[7] == 's')
                    h->pa.edge[e].style = (guint8) w42_border_style_from_css (value);
                  else
                    {
                      gint64 rgb = css_colour (value);

                      if (rgb >= 0)
                        h->pa.edge[e].color = (guint32) rgb;
                    }
                }
            }
          h->pa_dirty = TRUE;
        }
      else if (para && g_str_has_prefix (key, "border"))
        {
          /* "border: 1px solid #000000", or one side of it. */
          int bits = g_ascii_strcasecmp (key, "border") == 0 ? W42_BORDER_BOX
                   : g_ascii_strcasecmp (key, "border-top") == 0 ? W42_BORDER_TOP
                   : g_ascii_strcasecmp (key, "border-bottom") == 0 ? W42_BORDER_BOTTOM
                   : g_ascii_strcasecmp (key, "border-left") == 0 ? W42_BORDER_LEFT
                   : g_ascii_strcasecmp (key, "border-right") == 0 ? W42_BORDER_RIGHT : 0;

          if (bits == 0)
            continue;
          if (css_border_none (value))
            h->pa.border &= (guint8) ~bits;
          else
            {
              gint64 rgb = css_colour (value);
              int w = css_twips_em (value, em);
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
            }
          h->pa_dirty = TRUE;
        }
    }
  g_strfreev (decls);
  return hidden;
}

/* ---------------------------------------------------------------------- */
/* The page's own stylesheet                                               */
/* ---------------------------------------------------------------------- */

/* A rule's selector, if it is one this reader follows: an element, a
 * class, an id, or an element with one of those.  Combinators, attribute
 * selectors and pseudo-classes are more CSS than a document needs, and
 * their rules are left to the browser. */
static void
add_rule (Html *h, const char *sel, gsize sel_len, const char *decl, gsize decl_len)
{
  char *s = g_strstrip (g_strndup (sel, sel_len));
  CssRule *rule;
  char *dot, *hash;

  if (*s == '\0' || strpbrk (s, " \t\r\n>+~[]:()*") != NULL)
    {
      g_free (s);
      return;
    }
  rule = g_new0 (CssRule, 1);
  dot = strchr (s, '.');
  hash = strchr (s, '#');
  if (dot != NULL && strchr (dot + 1, '.') != NULL)
    {
      /* Two classes at once: not followed. */
      g_free (rule);
      g_free (s);
      return;
    }
  if (dot != NULL)
    {
      char *cls_end = hash != NULL && hash > dot ? hash : dot + strlen (dot);
      char *cls = g_strndup (dot + 1, cls_end - dot - 1);

      rule->cls = g_intern_string (cls);
      rule->weight += 10;
      g_free (cls);
    }
  if (hash != NULL)
    {
      char *id_end = dot != NULL && dot > hash ? dot : hash + strlen (hash);
      char *id = g_strndup (hash + 1, id_end - hash - 1);

      rule->id = g_intern_string (id);
      rule->weight += 100;
      g_free (id);
    }
  {
    char *tag_end = s + strlen (s);

    if (dot != NULL) tag_end = MIN (tag_end, dot);
    if (hash != NULL) tag_end = MIN (tag_end, hash);
    if (tag_end > s)
      {
        char *tag = g_ascii_strdown (s, tag_end - s);

        rule->tag = g_intern_string (tag);
        rule->weight += 1;
        g_free (tag);
      }
  }
  if (rule->weight == 0 || (rule->cls != NULL && *rule->cls == '\0') ||
      (rule->id != NULL && *rule->id == '\0'))
    {
      g_free (rule);
      g_free (s);
      return;
    }
  /* A sheet past any document's size: the rest are left to the browser,
   * before matching them costs more than reading the page. */
  if (h->rules->len >= 8192)
    {
      g_free (rule);
      g_free (s);
      return;
    }
  rule->order = h->rules->len;
  rule->decl = g_strndup (decl, decl_len);
  g_ptr_array_add (h->rules, rule);
  {
    /* Filed under the most particular thing it names, which an element
     * must carry to match it. */
    GHashTable *index = rule->id != NULL ? h->rules_by_id
                      : rule->cls != NULL ? h->rules_by_class : h->rules_by_tag;
    const char *key = rule->id != NULL ? rule->id : rule->cls != NULL ? rule->cls : rule->tag;
    GPtrArray *list = g_hash_table_lookup (index, key);

    if (list == NULL)
      {
        list = g_ptr_array_new ();
        g_hash_table_insert (index, (gpointer) key, list);
      }
    g_ptr_array_add (list, rule);
  }
  g_free (s);
}

static void
rule_list_free (gpointer data)
{
  g_ptr_array_free (data, TRUE);
}

static void
css_rule_free (gpointer data)
{
  CssRule *rule = data;

  g_free (rule->decl);
  g_free (rule);
}

/* An @page rule's declarations: the sheet's size, its margins, and the
 * border Word42 writes round it, "0.75pt solid #000000" with its distance
 * from the edge in border-spacing. */
static void
read_page_rule (Html *h, const char *decls)
{
  char *size, *margin, *border, *spacing;

  if (h->page == NULL)
    return;
  size = css_value (decls, "size");
  margin = css_value (decls, "margin");
  border = css_value (decls, "border");
  spacing = css_value (decls, "border-spacing");

  if (size != NULL)
    {
      const char *sp = strchr (size, ' ');
      int w = css_twips (size);
      int hh = sp != NULL ? css_twips (sp) : 0;

      if (w > 0 && hh > 0)
        {
          h->page->width = w;
          h->page->height = hh;
        }
    }
  if (margin != NULL)
    {
      char **tok = g_strsplit (margin, " ", -1);
      int m[4] = { 0, 0, 0, 0 };
      int n = 0;

      for (guint k = 0; tok[k] != NULL && n < 4; k++)
        if (*tok[k] != '\0')
          m[n++] = css_twips (tok[k]);
      g_strfreev (tok);
      /* The CSS shorthand: one value for all four sides, two for the
       * pairs, three or four naming them round from the top. */
      if (n == 1)      { m[1] = m[2] = m[3] = m[0]; }
      else if (n == 2) { m[2] = m[0]; m[3] = m[1]; }
      else if (n == 3) { m[3] = m[1]; }
      if (n >= 1)
        {
          h->page->margin_top = m[0];
          h->page->margin_right = m[1];
          h->page->margin_bottom = m[2];
          h->page->margin_left = m[3];
        }
    }
  if (border != NULL && !css_border_none (border))
    {
      gint64 rgb = css_colour (border);

      h->page->has_border = 1;
      h->page->border_width = (guint8) CLAMP (css_twips (border), 5, 120);
      h->page->border_style = (guint8) w42_border_style_from_css (border);
      h->page->border_color = rgb >= 0 ? (guint32) rgb : 0;
      h->page->border_space = spacing != NULL ? CLAMP (css_twips (spacing), 0, 4000) : 480;
    }
  g_free (size);
  g_free (margin);
  g_free (border);
  g_free (spacing);
}

/* The text of one <style> element: its rules, in order, minus the
 * comments and the at-rules -- @page is the page's, @media and
 * @font-face are the browser's. */
static void
parse_stylesheet (Html *h, const char *css, gsize len)
{
  GString *clean = g_string_sized_new (len);
  const char *p, *end;

  /* Comments, and the HTML comment markers old pages wrap a sheet in. */
  for (gsize i = 0; i < len; )
    {
      if (i + 1 < len && css[i] == '/' && css[i + 1] == '*')
        {
          const char *close = g_strstr_len (css + i + 2, len - i - 2, "*/");

          i = close != NULL ? (gsize) (close - css) + 2 : len;
          continue;
        }
      if (i + 3 < len && memcmp (css + i, "<!--", 4) == 0)
        {
          i += 4;
          continue;
        }
      if (i + 2 < len && memcmp (css + i, "-->", 3) == 0)
        {
          i += 3;
          continue;
        }
      g_string_append_c (clean, css[i]);
      i++;
    }

  p = clean->str;
  end = p + clean->len;
  while (p < end)
    {
      const char *brace, *close;

      while (p < end && g_ascii_isspace (*p))
        p++;
      if (p >= end)
        break;
      if (*p == '@')
        {
          /* An at-rule: to its semicolon, or over its block. */
          const char *semi = memchr (p, ';', end - p);
          const char *open = memchr (p, '{', end - p);

          if (open == NULL || (semi != NULL && semi < open))
            {
              p = semi != NULL ? semi + 1 : end;
              continue;
            }
          {
            int depth = 0;
            const char *at = p;

            for (p = open; p < end; p++)
              {
                if (*p == '{') depth++;
                else if (*p == '}' && --depth == 0) { p++; break; }
              }
            /* The page's own rule, and only in a stylesheet: the words
             * "@page" in the text of a page about CSS are not one. */
            if (g_ascii_strncasecmp (at, "@page", 5) == 0 && depth == 0)
              {
                char *decls = g_strndup (open + 1, p - open - 2);

                read_page_rule (h, decls);
                g_free (decls);
              }
          }
          continue;
        }
      brace = memchr (p, '{', end - p);
      if (brace == NULL)
        break;
      close = memchr (brace, '}', end - brace);
      if (close == NULL)
        break;
      {
        /* Each selector of the list gets the declarations. */
        const char *sel = p;

        while (sel < brace)
          {
            const char *comma = memchr (sel, ',', brace - sel);
            const char *sel_end = comma != NULL ? comma : brace;

            add_rule (h, sel, sel_end - sel, brace + 1, close - brace - 1);
            sel = sel_end + 1;
          }
      }
      p = close + 1;
    }
  g_string_free (clean, TRUE);
}

/* Every <style> in the page, in order, wherever it is: a word processor
 * puts its sheet in the head, and a page written by hand anywhere. */
static void
collect_styles (Html *h, lxb_dom_node_t *root)
{
  lxb_dom_node_t *n = lxb_dom_node_first_child (root);

  h->rules = g_ptr_array_new_with_free_func (css_rule_free);
  h->rules_by_tag = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, rule_list_free);
  h->rules_by_class = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, rule_list_free);
  h->rules_by_id = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, rule_list_free);
  while (n != NULL)
    {
      if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_dom_node_tag_id (n) == LXB_TAG_STYLE)
        {
          size_t len = 0;
          lxb_char_t *raw = lxb_dom_node_text_content (n, &len);

          if (raw != NULL)
            {
              parse_stylesheet (h, (const char *) raw, len);
              lxb_dom_document_destroy_text (n->owner_document, raw);
            }
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

/* The declarations that apply to an element: the sheet's matching rules
 * by weight and then by order, and its own style attribute last, as the
 * cascade has it.  Freed by the caller; NULL when there are none. */
static char *
elem_style (Html *h, lxb_dom_element_t *el)
{
  char *inline_style = elem_attr (el, "style");
  char *cls, *id, **classes = NULL;
  const char *id_i = NULL;
  char name[24];
  GPtrArray *hits;
  GString *out;

  if (h->rules == NULL || h->rules->len == 0)
    return inline_style;

  elem_name (el, name, sizeof name);
  cls = elem_attr (el, "class");
  id = elem_attr (el, "id");
  if (cls != NULL)
    classes = g_strsplit_set (cls, " \t\r\n", -1);
  if (id != NULL && *id != '\0')
    id_i = g_intern_string (id);

  /* The rules that could match: those filed under the element's name,
   * each of its classes, and its id. */
  hits = g_ptr_array_new ();
  for (int pass = 0; pass < 2 + (classes != NULL ? (int) g_strv_length (classes) : 0); pass++)
    {
      GPtrArray *list;

      if (pass == 0)
        list = g_hash_table_lookup (h->rules_by_tag, g_intern_string (name));
      else if (pass == 1)
        list = id_i != NULL ? g_hash_table_lookup (h->rules_by_id, id_i) : NULL;
      else
        list = *classes[pass - 2] != '\0'
                 ? g_hash_table_lookup (h->rules_by_class, g_intern_string (classes[pass - 2])) : NULL;
      for (guint i = 0; list != NULL && i < list->len; i++)
        {
          CssRule *rule = g_ptr_array_index (list, i);
          gboolean ok = TRUE;

          if (rule->tag != NULL && !g_str_equal (rule->tag, name))
            ok = FALSE;
          if (ok && rule->id != NULL && rule->id != id_i)
            ok = FALSE;
          if (ok && rule->cls != NULL)
            {
              ok = FALSE;
              for (guint k = 0; classes != NULL && classes[k] != NULL; k++)
                if (g_str_equal (classes[k], rule->cls))
                  {
                    ok = TRUE;
                    break;
                  }
            }
          if (ok)
            {
              /* Kept in order of weight, then of the sheet. */
              guint at = hits->len;

              while (at > 0)
                {
                  const CssRule *before = g_ptr_array_index (hits, at - 1);

                  if (before->weight < rule->weight ||
                      (before->weight == rule->weight && before->order < rule->order))
                    break;
                  at--;
                }
              g_ptr_array_insert (hits, (gint) at, rule);
            }
        }
    }

  if (hits->len == 0)
    {
      g_ptr_array_free (hits, TRUE);
      g_strfreev (classes);
      g_free (cls);
      g_free (id);
      return inline_style;
    }
  out = g_string_new (NULL);
  {
    /* The rules that win are the last, so a sheet that would give one
     * element megabytes of declarations -- the cost of a hostile page,
     * paid again for every element -- gives it only its last 8 KiB,
     * which is a few dozen rules, more than any real sheet matches. */
    guint first = hits->len;
    gsize total = 0;

    while (first > 0)
      {
        gsize n = strlen (((CssRule *) g_ptr_array_index (hits, first - 1))->decl) + 1;

        if (total + n > 8192)
          break;
        total += n;
        first--;
      }
    for (guint i = first; i < hits->len; i++)
      {
        g_string_append (out, ((CssRule *) g_ptr_array_index (hits, i))->decl);
        g_string_append_c (out, ';');
      }
  }
  if (inline_style != NULL)
    g_string_append (out, inline_style);
  g_ptr_array_free (hits, TRUE);
  g_strfreev (classes);
  g_free (cls);
  g_free (id);
  g_free (inline_style);
  return g_string_free (out, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Notes                                                                   */
/* ---------------------------------------------------------------------- */

/* A note the page keeps at its end: its elements, and whether a reference
 * has made it one of the document's notes yet. */
typedef struct {
  GPtrArray *parts;     /* lxb_dom_node_t *, in the page's order */
  gboolean   used;
} Note;

static void
note_free (gpointer data)
{
  Note *note = data;

  g_ptr_array_free (note->parts, TRUE);
  g_free (note);
}

/* Whether a text node is only white space. */
static gboolean
blank_text (lxb_dom_node_t *node)
{
  lexbor_str_t *s = &lxb_dom_interface_text (node)->char_data.data;

  for (size_t i = 0; i < s->length; i++)
    if (!g_ascii_isspace (s->data[i]))
      return FALSE;
  return TRUE;
}

/* Whether a subtree holds anything to read: text that is not white space,
 * or a picture. */
static gboolean
has_content (lxb_dom_node_t *root)
{
  lxb_dom_node_t *n = root;

  while (n != NULL)
    {
      if (n->type == LXB_DOM_NODE_TYPE_TEXT && !blank_text (n))
        return TRUE;
      if (n->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_dom_node_tag_id (n) == LXB_TAG_IMG)
        return TRUE;
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
  return FALSE;
}

/* The ids the notes are given: Word42's "note1" and "notee1", and
 * LibreOffice's "sdfootnote1" and "sdendnote1" -- a name and a number,
 * nothing else, so that a section called "notes" is not taken for one. */
static gboolean
is_note_id (const char *id)
{
  static const char *const PREFIX[] = { "sdfootnote", "sdendnote", "notee", "note" };

  for (guint i = 0; i < G_N_ELEMENTS (PREFIX); i++)
    if (g_str_has_prefix (id, PREFIX[i]) && g_ascii_isdigit (id[strlen (PREFIX[i])]))
      {
        const char *p = id + strlen (PREFIX[i]);

        while (g_ascii_isdigit (*p))
          p++;
        return *p == '\0';
      }
  return FALSE;
}

/* A word processor writing HTML puts its footnotes at the end and links
 * to them: LibreOffice as <div id="sdfootnote1">, Word42 as a paragraph
 * with id="note1" and any more of the note's paragraphs after it.  Their
 * elements are gathered here, before the body is walked, so that a
 * reference can become a real note when it is met. */
static void
harvest_notes (Html *h, lxb_dom_node_t *root)
{
  lxb_dom_node_t *n = lxb_dom_node_first_child (root);

  h->notes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, note_free);
  h->note_parts = g_hash_table_new (g_direct_hash, g_direct_equal);

  while (n != NULL)
    {
      gboolean descend = n->first_child != NULL;

      if (n->type == LXB_DOM_NODE_TYPE_ELEMENT)
        {
          char *id = elem_attr (lxb_dom_interface_element (n), "id");

          if (id != NULL && is_note_id (id) &&
              !g_hash_table_contains (h->notes, id) && has_content (n))
            {
              Note *note = g_new0 (Note, 1);

              note->parts = g_ptr_array_new ();
              g_ptr_array_add (note->parts, n);
              g_hash_table_add (h->note_parts, n);
              /* Word42 writes a note's later paragraphs straight after its
               * first, with the class and without the id. */
              if (g_str_has_prefix (id, "note"))
                for (lxb_dom_node_t *s = n->next; s != NULL; s = s->next)
                  {
                    char *cls, *sid;
                    gboolean part;

                    if (s->type == LXB_DOM_NODE_TYPE_TEXT && blank_text (s))
                      continue;
                    if (s->type != LXB_DOM_NODE_TYPE_ELEMENT)
                      break;
                    cls = elem_attr (lxb_dom_interface_element (s), "class");
                    sid = elem_attr (lxb_dom_interface_element (s), "id");
                    part = sid == NULL && cls != NULL && g_str_equal (cls, "note");
                    g_free (cls);
                    g_free (sid);
                    if (!part)
                      break;
                    g_ptr_array_add (note->parts, s);
                    g_hash_table_add (h->note_parts, s);
                  }
              g_hash_table_insert (h->notes, id, note);
              id = NULL;
              /* What is inside a note is the note's: an id there that looks
               * like another's is not one, and the note is not gone over
               * again for it -- a page of notes nested in notes would
               * otherwise cost the square of its size. */
              descend = FALSE;
            }
          g_free (id);
        }

      if (descend)
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

/* Whether an element holds the page's copies of its notes and nothing
 * else: Word42's <div class="notes">, a rule over the notes drawn by its
 * stylesheet.  Its notes are in the document already, and the box they
 * came in is not a paragraph of it. */
static gboolean
holds_only_notes (Html *h, lxb_dom_node_t *node)
{
  gboolean any = FALSE;

  if (h->note_parts == NULL || g_hash_table_size (h->note_parts) == 0)
    return FALSE;
  for (lxb_dom_node_t *c = lxb_dom_node_first_child (node); c != NULL; c = c->next)
    {
      if (c->type == LXB_DOM_NODE_TYPE_ELEMENT)
        {
          if (!g_hash_table_contains (h->note_parts, c))
            return FALSE;
          any = TRUE;
        }
      else if (c->type == LXB_DOM_NODE_TYPE_TEXT && !blank_text (c))
        return FALSE;
    }
  return any;
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
picture (Html *h, const char *src, const char *width, const char *height,
         W42Wrap wrap)
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

  /* A space before the picture is before it, not after. */
  emit_space (h);
  flush_text (h);
  open_run (h);
  h->note_skip_space = FALSE;
  w42_pt_insert_object (h->pt, h->pos, idx, html_ap (h));
  if (wrap != W42_WRAP_INLINE)
    w42_pt_set_object_wrap (h->pt, h->pos, wrap);
  h->pos += 1;
  h->in_para = TRUE;
  h->at_para_start = FALSE;
}

/* The cells of one row, spans counted. */
static int
count_row_cells (lxb_dom_node_t *tr)
{
  int cols = 0;

  for (lxb_dom_node_t *n = lxb_dom_node_first_child (tr); n != NULL; n = n->next)
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

/* Columns of a table: as many as its widest row has, a heading row that
 * spans them all or a first row that is short notwithstanding.  A table
 * nested in a cell counts for its own rows, not this one's. */
static int
count_columns (lxb_dom_node_t *table)
{
  lxb_dom_node_t *n = lxb_dom_node_first_child (table);
  int cols = 0;

  while (n != NULL)
    {
      gboolean descend = n->first_child != NULL;

      if (n->type == LXB_DOM_NODE_TYPE_ELEMENT)
        {
          lxb_tag_id_t tag = lxb_dom_node_tag_id (n);

          if (tag == LXB_TAG_TR)
            {
              cols = MAX (cols, count_row_cells (n));
              descend = FALSE;
            }
          else if (tag == LXB_TAG_TABLE)
            descend = FALSE;
        }
      if (descend)
        {
          n = n->first_child;
          continue;
        }
      while (n != table && n->next == NULL)
        n = n->parent;
      if (n == table)
        break;
      n = n->next;
    }
  return cols;
}

/* A width attribute in twips: pixels when bare, a share of the text's
 * width when a percentage, or a length with its unit. */
static int
attr_width_twips (Html *h, const char *value)
{
  if (value == NULL || *value == '\0')
    return 0;
  if (strchr (value, '%') != NULL)
    {
      int text_width = h->page != NULL && h->page->width > 0
                         ? h->page->width - h->page->margin_left - h->page->margin_right
                         : 9360;
      double pct = g_ascii_strtod (value, NULL);

      return pct > 0 && pct <= 100 ? (int) (text_width * pct / 100.0 + 0.5) : 0;
    }
  if (css_twips (value) > 0)
    return css_twips (value);
  return atoi (value) > 0 ? CLAMP (atoi (value), 1, 2000) * 15 : 0;
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
  static const char *const NAMES[] = {
    "b", "strong", "i", "em", "u", "s", "strike", "del", "ins", "a",
    "sup", "sub", "span", "font", "code", "tt", "kbd", "samp", "var",
    "cite", "dfn", "q", "abbr", "small", "big", "mark", "label", "bdi",
    "bdo", "time", "data", "output", NULL };

  for (guint i = 0; NAMES[i] != NULL; i++)
    if (g_str_equal (name, NAMES[i]))
      return TRUE;
  return FALSE;
}

/* The elements that begin a paragraph of their own: the classic blocks,
 * and HTML5's sectioning elements, which a page written today puts its
 * paragraphs inside. */
static gboolean
block_element (const char *name)
{
  static const char *const NAMES[] = {
    "p", "div", "li", "blockquote", "pre", "center", "address",
    "dt", "dd", "figcaption", "summary", "caption",
    "article", "section", "header", "footer", "main", "nav", "aside",
    "figure", "details", "fieldset", "legend", "form", NULL };

  if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0')
    return TRUE;
  for (guint i = 0; NAMES[i] != NULL; i++)
    if (g_str_equal (name, NAMES[i]))
      return TRUE;
  return FALSE;
}

/* Word's list markers, read as text: "1." numbers, "a." letters, "iv."
 * romans; a bullet is anything else. */
static int
list_kind_of_marker (const char *text)
{
  gsize n = strlen (text);

  if (n == 0)
    return W42_LIST_BULLET;
  if (g_ascii_isdigit (text[0]))
    return W42_LIST_NUMBER;
  if (n >= 2 && (text[1] == '.' || text[1] == ')'))
    {
      if (strchr ("ivx", text[0]) != NULL) return W42_LIST_LOWER_ROMAN;
      if (strchr ("IVX", text[0]) != NULL) return W42_LIST_UPPER_ROMAN;
      if (g_ascii_islower (text[0])) return W42_LIST_LOWER_LETTER;
      if (g_ascii_isupper (text[0])) return W42_LIST_UPPER_LETTER;
    }
  if (strspn (text, "ivx") == n - 1 && (text[n - 1] == '.' || text[n - 1] == ')'))
    return W42_LIST_LOWER_ROMAN;
  if (strspn (text, "IVX") == n - 1 && (text[n - 1] == '.' || text[n - 1] == ')'))
    return W42_LIST_UPPER_ROMAN;
  if (g_str_equal (text, "o"))
    return W42_LIST_BULLET_CIRCLE;
  if (g_str_equal (text, "\302\247") || g_str_equal (text, "\342\226\252"))    /* Wingdings' square */
    return W42_LIST_BULLET_SQUARE;
  if (g_str_equal (text, "-") || g_str_equal (text, "\342\200\223"))
    return W42_LIST_BULLET_DASH;
  return W42_LIST_BULLET;
}

/* The list kind a list-style-type names, or W42_LIST_NONE for one it
 * does not. */
static int
list_kind_of_css (const char *style)
{
  const char *v = strstr (style, "list-style");

  if (v == NULL)
    return W42_LIST_NONE;
  if (strstr (v, "lower-alpha") || strstr (v, "lower-latin")) return W42_LIST_LOWER_LETTER;
  if (strstr (v, "upper-alpha") || strstr (v, "upper-latin")) return W42_LIST_UPPER_LETTER;
  if (strstr (v, "lower-roman")) return W42_LIST_LOWER_ROMAN;
  if (strstr (v, "upper-roman")) return W42_LIST_UPPER_ROMAN;
  if (strstr (v, "decimal")) return W42_LIST_NUMBER;
  if (strstr (v, "circle")) return W42_LIST_BULLET_CIRCLE;
  if (strstr (v, "square")) return W42_LIST_BULLET_SQUARE;
  if (strstr (v, "disc")) return W42_LIST_BULLET;
  if (strstr (v, "2013") || strstr (v, "'-'") || strstr (v, "\"-\"")) return W42_LIST_BULLET_DASH;
  return W42_LIST_NONE;
}

/* An annotation, as Word42 writes one: a span of class "comment" with
 * the annotation's text in its title.  NULL for any other span. */
static char *
span_comment (lxb_dom_element_t *el)
{
  char *cls = elem_attr (el, "class");
  char *title = NULL;

  if (cls != NULL)
    {
      char **names = g_strsplit_set (cls, " \t\r\n", -1);

      for (guint i = 0; names[i] != NULL; i++)
        if (g_str_equal (names[i], "comment"))
          {
            title = elem_attr (el, "title");
            break;
          }
      g_strfreev (names);
      g_free (cls);
    }
  if (title != NULL && *title == '\0')
    g_clear_pointer (&title, g_free);
  return title;
}

static void read_note (Html *h, const Note *note, gsize body);

/* An element keeps whitespace once, however many ways it says so -- a
 * <pre>, white-space:pre in its style, or both -- and its end gives
 * back exactly that one level.  A styled <pre> gave back two, and the
 * text after it, still inside another element that kept whitespace,
 * had its spaces run together. */
static void
note_pre (Html *h, int pre_before, guint8 *flags)
{
  if (h->pre_depth > pre_before)
    {
      h->pre_depth = pre_before + 1;
      *flags |= FLAG_PRE;
    }
}

static WalkEnter
element_start (Html *h, const char *name, lxb_dom_element_t *el, guint8 *flags)
{
  char *style, *comment = NULL;
  int pre_before = h->pre_depth;

  *flags = 0;

  /* A script or a stylesheet is not text; a title was read before the
   * body was walked. */
  if (g_str_equal (name, "script") || g_str_equal (name, "style") ||
      g_str_equal (name, "title"))
    return WALK_SKIP;

  /* The page's own copy of a note, at its end: a note is read where its
   * reference is, into the document's notes, so the copy is passed over
   * whole here. */
  if (h->note_parts != NULL &&
      (g_hash_table_contains (h->note_parts, lxb_dom_interface_node (el)) ||
       holds_only_notes (h, lxb_dom_interface_node (el))))
    {
      end_paragraph (h);
      return WALK_SKIP;
    }

  /* A rule across the page: an empty paragraph with a line under it. */
  if (g_str_equal (name, "hr"))
    {
      end_paragraph (h);
      if (h->table >= 0)
        open_cell (h);
      h->pa.border |= W42_BORDER_BOTTOM;
      h->pa.edge[W42_EDGE_BOTTOM].style = W42_BORDER_SINGLE;
      h->pa.edge[W42_EDGE_BOTTOM].width = W42_BORDER_HAIRLINE;
      h->pa_dirty = TRUE;
      end_paragraph (h);
      return WALK_SKIP;
    }

  /* Block elements end the paragraph they are in.  Each gets a formatting
   * level of its own, so that what its style says of the type stays in
   * it and the next paragraph starts from its parent's. */
  if (block_element (name))
    {
      W42Fmt def;

      end_paragraph (h);
      if (h->table >= 0)
        open_cell (h);
      if (push_char (h))
        *flags |= FLAG_CHAR;
      w42_fmt_init_default (&def);
      if (name[0] == 'h' && name[2] == '\0')
        {
          int level = name[1] - '0';
          const W42Style *st;
          char *cls = elem_attr (el, "class");
          char *head = g_strdup_printf ("Heading %d", level);
          W42StyleSheet *sheet = w42_pt_stylesheet (h->pt);

          /* The sheet a document starts with has three headings.  A deeper
           * one -- Word42 writes an <h4> for a style of the fourth level --
           * is made from the third, one level down, so that it is still a
           * heading of its own level when it is written out again. */
          if (w42_stylesheet_find (sheet, head) == NULL &&
              (st = w42_stylesheet_find (sheet, "Heading 3")) != NULL)
            {
              W42Style deeper = *st;

              deeper.name = g_intern_string (head);
              deeper.pa.style = deeper.name;
              deeper.outline = level;
              deeper.based_on = st->name;
              deeper.pa_own = 0;
              deeper.ch_own = 0;
              w42_stylesheet_set (sheet, &deeper);
            }
          h->pa.style = g_intern_string (w42_stylesheet_find (sheet, head) != NULL ? head : "Heading 3");
          g_free (head);
          /* Word42 writes its Title as an <h1 class="title">. */
          if (cls != NULL && level == 1 && strstr (cls, "title") != NULL)
            h->pa.style = g_intern_string ("Title");
          g_free (cls);
          h->pa_dirty = TRUE;
          /* the heading's own character formatting, as applying the
           * style would give it */
          for (guint i = 0; i < w42_stylesheet_size (w42_pt_stylesheet (h->pt)); i++)
            {
              st = w42_stylesheet_get (w42_pt_stylesheet (h->pt), i);
              if (g_ascii_strcasecmp (st->name, h->pa.style) == 0)
                {
                  const char *lang = h->ch[h->depth].lang;

                  h->ch[h->depth] = st->ch;
                  h->ch[h->depth].lang = lang;
                  h->pa = st->pa;
                  h->pa.style = st->name;
                  break;
                }
            }
        }
      if (g_str_equal (name, "li"))
        {
          char *value = elem_attr (el, "value");

          h->pa.list = h->list_kind != W42_LIST_NONE ? h->list_kind : W42_LIST_BULLET;
          h->pa.list_level = (guint8) CLAMP (h->list_depth - 1, 0, 8);
          h->pa.indent_left = 360 * (h->pa.list_level + 1);
          h->pa.list_start = (guint8) h->list_start;
          if (value != NULL && atoi (value) > 0)
            h->pa.list_start = (guint8) CLAMP (atoi (value), 1, 255);
          g_free (value);
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
      if (g_str_equal (name, "dd"))
        {
          h->pa.indent_left = 720;
          h->pa_dirty = TRUE;
        }
      if (g_str_equal (name, "center"))
        {
          h->pa.align = W42_ALIGN_CENTER;
          h->pa_dirty = TRUE;
        }
      if (g_str_equal (name, "address"))
        h->ch[h->depth].italic = 1;
      if (g_str_equal (name, "pre"))
        {
          h->pre_depth++;
          note_pre (h, pre_before, flags);
          h->ch[h->depth].family = g_intern_string ("Courier New");
        }
      if ((style = elem_style (h, el)) != NULL)
        {
          StyleShows hidden = apply_style (h, style, TRUE);

          g_free (style);
          note_pre (h, pre_before, flags);
          if (hidden != STYLE_SHOWN)
            {
              /* Not shown: not read.  The paragraph it would have been
               * is not begun, and its end has nothing to close. */
              h->pa = def.pa;
              h->pa_dirty = FALSE;
              return WALK_SKIP;
            }
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
      emit_space (h);
      g_string_append_unichar (h->pending, 0x2028);
      h->space_pending = FALSE;
      h->at_para_start = FALSE;
      return WALK_SKIP;
    }

  if (g_str_equal (name, "ul") || g_str_equal (name, "ol"))
    {
      char *type = elem_attr (el, "type");
      char *start = elem_attr (el, "start");
      char *ul_style = elem_style (h, el);

      end_paragraph (h);
      h->list_kind = g_str_equal (name, "ul") ? W42_LIST_BULLET : W42_LIST_NUMBER;
      if (g_str_equal (name, "ol") && type != NULL)
        h->list_kind = g_str_equal (type, "a") ? W42_LIST_LOWER_LETTER
                     : g_str_equal (type, "A") ? W42_LIST_UPPER_LETTER
                     : g_str_equal (type, "i") ? W42_LIST_LOWER_ROMAN
                     : g_str_equal (type, "I") ? W42_LIST_UPPER_ROMAN
                     : W42_LIST_NUMBER;
      if (g_str_equal (name, "ul") && type != NULL)
        h->list_kind = g_ascii_strcasecmp (type, "circle") == 0 ? W42_LIST_BULLET_CIRCLE
                     : g_ascii_strcasecmp (type, "square") == 0 ? W42_LIST_BULLET_SQUARE
                     : W42_LIST_BULLET;
      if (ul_style != NULL && list_kind_of_css (ul_style) != W42_LIST_NONE)
        h->list_kind = list_kind_of_css (ul_style);
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
      char *cls, *border, *tstyle, *b;
      int before = h->table;
      gboolean ruled;

      if (before < 0 && !h->in_note)
        open_table (h, count_columns (lxb_dom_interface_node (el)));
      if (before >= 0 || h->table < 0)
        {
          /* A table in a table's cell, in a note, or past as many as a
           * document has: the model has no table to make of it there, so
           * each of its cells is read as a paragraph of what holds it,
           * rather than its rows and cells being taken for the outer
           * table's and closing it. */
          h->table_nest++;
          *flags |= FLAG_INNER;
          end_paragraph (h);
          return WALK_DESCEND;
        }
      cls = elem_attr (el, "class");
      border = elem_attr (el, "border");
      tstyle = elem_style (h, el);
      /* Word42 writes class="ruled" for a table that is; the old
       * border attribute and a border in the style say the same, and
       * anything else rules its cells itself, or not at all. */
      ruled = (cls != NULL && strstr (cls, "ruled") != NULL) ||
              (border != NULL && atoi (border) > 0);
      if (!ruled && tstyle != NULL && (b = css_value (tstyle, "border")) != NULL)
        {
          ruled = !css_border_none (b);
          g_free (b);
        }
      w42_pt_table_set_borders (h->pt, h->table, ruled);
      g_free (cls);
      g_free (border);
      g_free (tstyle);
      return WALK_DESCEND;
    }
  if (h->table_nest > 0 &&
      (g_str_equal (name, "tr") || g_str_equal (name, "td") || g_str_equal (name, "th") ||
       g_str_equal (name, "col") || g_str_equal (name, "colgroup")))
    {
      /* A row or a cell of a table read as paragraphs: each begins one. */
      *flags |= FLAG_INNER;
      end_paragraph (h);
      return g_str_equal (name, "col") ? WALK_SKIP : WALK_DESCEND;
    }
  if (g_str_equal (name, "col") && h->table >= 0)
    {
      /* A <colgroup> gives the columns their widths; one <col> with no
       * width of its own still takes its place among them. */
      char *cw = elem_style (h, el);
      char *wa = elem_attr (el, "width");
      char *w = cw != NULL ? css_value (cw, "width") : NULL;
      int twips = w != NULL ? css_twips (w) : attr_width_twips (h, wa);

      if (h->n_col_widths < 1023)
        h->col_widths[h->n_col_widths++] = twips;
      g_free (w);
      g_free (cw);
      g_free (wa);
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
      char *align = elem_attr (el, "align"), *valign = elem_attr (el, "valign");
      char *bgcolor = elem_attr (el, "bgcolor"), *width = elem_attr (el, "width");
      gboolean had_style;

      close_cell (h);
      open_cell_spanning (h, cs != NULL ? atoi (cs) : 1, rs != NULL ? atoi (rs) : 1);
      g_free (cs);
      g_free (rs);
      /* The cell's type is the cell's: what its style and a <th> say of
       * it ends with it, not with the table or the document. */
      if (push_char (h))
        *flags |= FLAG_CHAR;
      if (g_str_equal (name, "th"))
        h->ch[h->depth].bold = 1;
      style = elem_style (h, el);
      had_style = style != NULL;
      if (style != NULL || align != NULL || valign != NULL || bgcolor != NULL)
        {
          /* A <td>'s style is the cell's, not the paragraph's inside
           * it: its rules and its background belong to the mark; its
           * alignment is the paragraphs'.  The old attributes say the
           * same things the style does. */
          W42ParaFmt saved = h->pa;
          gboolean saved_dirty = h->pa_dirty;
          gsize cell_pos = h->pos >= 2 ? h->pos - 2 : 0;
          gboolean hidden = FALSE;

          h->pa.cell_valign = W42_CELL_VALIGN_TOP;
          if (style != NULL)
            hidden = apply_style (h, style, TRUE) != STYLE_SHOWN;
          if (align != NULL)
            {
              if (g_ascii_strcasecmp (align, "center") == 0) h->pa.align = W42_ALIGN_CENTER;
              else if (g_ascii_strcasecmp (align, "right") == 0) h->pa.align = W42_ALIGN_RIGHT;
              else if (g_ascii_strcasecmp (align, "justify") == 0) h->pa.align = W42_ALIGN_JUSTIFY;
            }
          if (valign != NULL)
            {
              if (g_ascii_strcasecmp (valign, "middle") == 0) h->pa.cell_valign = W42_CELL_VALIGN_CENTER;
              else if (g_ascii_strcasecmp (valign, "bottom") == 0) h->pa.cell_valign = W42_CELL_VALIGN_BOTTOM;
            }
          if (bgcolor != NULL && css_colour (bgcolor) >= 0)
            {
              h->pa.shading_color = (guint32) css_colour (bgcolor);
              h->pa.has_shading_color = 1;
            }
          if (h->in_cell)
            {
              if (had_style)
                {
                  w42_pt_cell_set_borders_at (h->pt, cell_pos,
                                              (h->pa.border & W42_BORDER_BOX) |
                                              W42_BORDER_CELL_SET);
                  w42_pt_cell_set_edges_at (h->pt, cell_pos, h->pa.edge);
                }
              if (h->pa.has_shading_color)
                w42_pt_cell_set_fill_at (h->pt, cell_pos, TRUE,
                                         h->pa.shading_color);
              else if (h->pa.shading > 0)
                w42_pt_cell_set_shading_at (h->pt, cell_pos, h->pa.shading);
              if (h->pa.cell_valign != W42_CELL_VALIGN_TOP)
                w42_pt_cell_set_valign_at (h->pt, cell_pos, h->pa.cell_valign);
            }
          note_pre (h, pre_before, flags);
          /* The alignment is every paragraph's in the cell, without
           * making a paragraph of its own. */
          h->cell_align = h->pa.align;
          h->cell_rtl = h->pa.rtl;
          saved.align = h->pa.align;
          saved.rtl = h->pa.rtl;
          h->pa = saved;
          h->pa_dirty = saved_dirty;
          g_free (style);
          if (hidden)
            {
              g_free (align); g_free (valign); g_free (bgcolor); g_free (width);
              return WALK_SKIP;
            }
        }
      /* A cell's width, when the columns were not given theirs. */
      if (width != NULL && h->table >= 0 && h->table_row == 0 && h->n_col_widths == 0)
        {
          int twips = attr_width_twips (h, width);
          int col = h->table_col;      /* this cell's: it moves on at its end */

          if (twips > 0 && col >= 0 && col < 1023 && h->cell_span == 1)
            {
              static int widths[1023];
              W42TableProps const *tp = w42_pt_table_props (h->pt, h->table);

              memset (widths, 0, sizeof widths);
              if (tp != NULL && tp->widths != NULL)
                for (guint k = 0; k < tp->widths->len && k < 1023; k++)
                  widths[k] = g_array_index (tp->widths, int, k);
              widths[col] = twips;
              w42_pt_table_set_widths (h->pt, h->table, widths, MIN (h->table_cols, 1023));
            }
        }
      g_free (align); g_free (valign); g_free (bgcolor); g_free (width);
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
      char *st = elem_style (h, el);
      char *align = elem_attr (el, "align");
      W42Wrap wrap = W42_WRAP_INLINE;

      /* A style says the size to the twip; the attributes only to the
       * screen pixel, so they are the fallback. */
      if (st != NULL)
        {
          char *sw = css_value (st, "width");
          char *sh = css_value (st, "height");
          char *fl = css_value (st, "float");
          char *shown = css_value (st, "display");
          gboolean none = shown != NULL && g_ascii_strcasecmp (shown, "none") == 0;

          if (sw != NULL && css_twips (sw) > 0) { g_free (w);  w  = g_steal_pointer (&sw); }
          if (sh != NULL && css_twips (sh) > 0) { g_free (hh); hh = g_steal_pointer (&sh); }
          if (fl != NULL)
            {
              /* A picture the text runs beside. */
              if (g_ascii_strcasecmp (fl, "left") == 0) wrap = W42_WRAP_LEFT;
              else if (g_ascii_strcasecmp (fl, "right") == 0) wrap = W42_WRAP_RIGHT;
            }
          g_free (sw); g_free (sh); g_free (fl); g_free (shown);
          if (none)
            {
              g_free (src); g_free (w); g_free (hh); g_free (st); g_free (align);
              return WALK_SKIP;
            }
        }
      if (align != NULL && wrap == W42_WRAP_INLINE)
        {
          if (g_ascii_strcasecmp (align, "left") == 0) wrap = W42_WRAP_LEFT;
          else if (g_ascii_strcasecmp (align, "right") == 0) wrap = W42_WRAP_RIGHT;
        }
      if (h->table >= 0)
        open_cell (h);
      picture (h, src, w, hh, wrap);
      g_free (src); g_free (w); g_free (hh); g_free (st); g_free (align);
      return WALK_SKIP;
    }

  if (!known_inline (name))
    {
      /* Something else, or unknown: no formatting of its own, but what
       * is inside it is still text. */
      return WALK_DESCEND;
    }

  flush_text (h);
  if (push_char (h))
    *flags |= FLAG_CHAR;
  if (g_str_equal (name, "b") || g_str_equal (name, "strong"))
    h->ch[h->depth].bold = 1;
  else if (g_str_equal (name, "i") || g_str_equal (name, "em") || g_str_equal (name, "cite") ||
           g_str_equal (name, "var") || g_str_equal (name, "dfn"))
    h->ch[h->depth].italic = 1;
  else if (g_str_equal (name, "u"))
    h->ch[h->depth].underline = 1;
  else if (g_str_equal (name, "s") || g_str_equal (name, "strike"))
    h->ch[h->depth].strikeout = 1;
  else if (g_str_equal (name, "del"))
    h->ch[h->depth].revision = 2;      /* a deletion, marked as one */
  else if (g_str_equal (name, "ins"))
    h->ch[h->depth].revision = 1;
  else if (g_str_equal (name, "sup"))
    h->ch[h->depth].script = 1;
  else if (g_str_equal (name, "sub"))
    h->ch[h->depth].script = -1;
  else if (g_str_equal (name, "mark"))
    h->ch[h->depth].highlight = 7;
  else if (g_str_equal (name, "code") || g_str_equal (name, "tt") ||
           g_str_equal (name, "kbd") || g_str_equal (name, "samp"))
    h->ch[h->depth].family = g_intern_string ("Courier New");
  else if (g_str_equal (name, "small"))
    h->ch[h->depth].size = MAX (h->ch[h->depth].size * 5 / 6, 8);
  else if (g_str_equal (name, "big"))
    h->ch[h->depth].size = MIN (h->ch[h->depth].size * 6 / 5, 3276);
  else if (g_str_equal (name, "q"))
    {
      /* The quotation marks a browser draws round it. */
      emit_space (h);
      g_string_append (h->pending, "\342\200\234");
      h->at_para_start = FALSE;
    }
  else if (g_str_equal (name, "a"))
    {
      char *href = elem_attr (el, "href");
      char *anchor = elem_attr (el, "name");
      char *note_id = note_id_for (href);
      Note *note = note_id != NULL && h->notes != NULL
        ? g_hash_table_lookup (h->notes, note_id) : NULL;
      gboolean skip = FALSE;

      if (h->in_note && href != NULL && *href == '#' &&
          h->pos == h->note_start && h->pending->len == 0)
        {
          /* A note begins with its number, a link back to its reference;
           * the document numbers its notes itself. */
          h->note_skip_space = TRUE;
          skip = TRUE;
        }
      else if (note != NULL && !note->used)
        {
          /* A link to a note at the end of the page is a note: the
           * document gets the real thing, and the number the page shows
           * is left out, since a note numbers itself. */
          W42Fmt mark;
          W42ApIdx ap;
          gsize body;

          emit_space (h);
          flush_text (h);
          open_run (h);
          /* The note's paragraph is a plain one, whatever the paragraph
           * its reference stands in looks like.  The mark is drawn raised
           * whatever it carries: the <sup> round it is the page's way of
           * showing that, not a superscript of the text's. */
          w42_fmt_init_default (&mark);
          mark.ch = h->ch[h->depth];
          mark.ch.script = 0;
          ap = w42_ap_table_intern (w42_pt_ap_table (h->pt), &mark);
          body = g_str_has_prefix (note_id, "sdendnote") || g_str_has_prefix (note_id, "notee")
                   ? w42_pt_insert_endnote (h->pt, h->pos, ap)
                   : w42_pt_insert_footnote (h->pt, h->pos, ap);

          if (body != (gsize) -1)
            {
              /* One reference, one note: a page that points at a note
               * again and again does not get its text again and again. */
              note->used = TRUE;
              h->pos += 1;             /* the mark the note left behind */
              h->at_para_start = FALSE;
              h->in_para = TRUE;
              read_note (h, note, body);
            }
          skip = TRUE;
        }
      g_free (note_id);
      if (skip)
        {
          g_free (href);
          g_free (anchor);
          /* The anchor's own text is the number the page shows. */
          if (*flags & FLAG_CHAR)
            pop_char (h);
          *flags &= (guint8) ~FLAG_CHAR;
          return WALK_SKIP;
        }

      /* A link is somewhere to go, not something to run: a script scheme
       * would execute when the exported page is opened in a browser. */
      if (href != NULL && *href != '\0' && *href != '#' && !w42_html_link_is_script (href))
        h->ch[h->depth].link = g_intern_string (href);
      if (anchor == NULL)
        anchor = elem_attr (el, "id");
      /* The later runs of a bookmark Word42 wrote: the id, which a page
       * has once, went on its first. */
      if (anchor == NULL)
        anchor = elem_attr (el, "data-w42-bookmark");
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
  else if (g_str_equal (name, "span"))
    comment = span_comment (el);
  else if (g_str_equal (name, "font"))
    {
      char *face = elem_attr (el, "face");
      char *color = elem_attr (el, "color");
      char *size = elem_attr (el, "size");

      if (face != NULL && *face != '\0')
        {
          char *comma = strchr (face, ',');

          if (comma) *comma = '\0';
          h->ch[h->depth].family = g_intern_string (g_strstrip (face));
        }
      if (color != NULL && css_colour (color) >= 0)
        h->ch[h->depth].color = (guint32) css_colour (color);
      if (size != NULL && *size != '\0')
        h->ch[h->depth].size = font_size_attr (size, h->ch[h->depth].size);
      g_free (face); g_free (color); g_free (size);
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

  if ((style = elem_style (h, el)) != NULL)
    {
      StyleShows hidden = apply_style (h, style, FALSE);

      g_free (style);
      note_pre (h, pre_before, flags);
      if (hidden != STYLE_SHOWN)
        {
          /* Word's list marker: the number is the paragraph's, in text. */
          if (hidden == STYLE_MARKER && h->mso_item > 0 && h->pending->len == 0)
            {
              char *marker = node_text (lxb_dom_interface_node (el));

              h->pa.list = (guint8) list_kind_of_marker (marker);
              g_free (marker);
            }
          g_free (comment);
          return WALK_SKIP;
        }
    }

  if (comment != NULL)
    {
      /* The yellow the page's stylesheet gives an annotation shows it in
       * a browser; it is not a highlight of the text. */
      h->ch[h->depth].comment = g_intern_string (comment);
      if (*flags & FLAG_CHAR)
        h->ch[h->depth].highlight = h->ch[h->depth - 1].highlight;
      g_free (comment);
    }

  return WALK_DESCEND;
}

static void
element_end (Html *h, const char *name, guint8 flags)
{
  if ((flags & FLAG_PRE) && h->pre_depth > 0)
    h->pre_depth--;
  if (flags & FLAG_INNER)
    {
      end_paragraph (h);
      if (g_str_equal (name, "table") && h->table_nest > 0)
        h->table_nest--;
      return;
    }
  if (block_element (name))
    {
      end_paragraph (h);
      if (flags & FLAG_CHAR)
        pop_char (h);
      return;
    }
  if (g_str_equal (name, "q"))
    {
      g_string_append (h->pending, "\342\200\235");
      h->space_pending = FALSE;
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
      if (flags & FLAG_CHAR)
        pop_char (h);
      return;
    }
  if (flags & FLAG_CHAR)
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
          guint8 f = 0;

          elem_name (lxb_dom_interface_element (node), name, sizeof name);
          if (element_start (h, name, lxb_dom_interface_element (node), &f) == WALK_DESCEND &&
              node->first_child != NULL)
            {
              g_byte_array_append (pushed, &f, 1);
              descend = TRUE;
            }
          else
            element_end (h, name, f);
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
            element_end (h, name, f);
          }
        }
      node = node->next;
    }
out:
  g_byte_array_free (pushed, TRUE);
}

/* One element and what is in it. */
static void
walk_element (Html *h, lxb_dom_node_t *node)
{
  char name[24];
  guint8 f = 0;

  elem_name (lxb_dom_interface_element (node), name, sizeof name);
  if (element_start (h, name, lxb_dom_interface_element (node), &f) == WALK_DESCEND)
    walk_body (h, node);
  element_end (h, name, f);
}

/* A note's paragraphs, read where its reference put the note: its
 * elements are walked as the body is, so that the note keeps its
 * paragraphs, line breaks and formatting, by a reader of their own --
 * the body's is in the middle of a paragraph, perhaps of a table, and
 * picks up there when the note is done. */
static void
read_note (Html *h, const Note *note, gsize body)
{
  Html *n = g_new0 (Html, 1);
  W42Fmt def;

  w42_fmt_init_default (&def);
  n->pt = h->pt;
  n->pos = body;
  /* The page's type, and none of the reference's superscript or link. */
  n->ch[0] = h->ch[0];
  n->ch[0].script = 0;
  n->ch[0].link = NULL;
  n->ch[0].bookmark = NULL;
  n->ch[0].comment = NULL;
  n->pa = def.pa;
  n->pending = g_string_new (NULL);
  n->at_para_start = TRUE;
  n->table = -1;
  n->base = h->base;
  n->rules = h->rules;
  n->rules_by_tag = h->rules_by_tag;
  n->rules_by_class = h->rules_by_class;
  n->rules_by_id = h->rules_by_id;
  /* No notes in a note, and no tables: the model has neither there. */
  n->in_note = TRUE;
  n->note_start = body;
  memcpy (n->meta, h->meta, sizeof n->meta);

  for (guint i = 0; i < note->parts->len; i++)
    walk_element (n, g_ptr_array_index (note->parts, i));

  /* The last paragraph's end began another, which the note does not
   * have. */
  flush_text (n);
  if (n->in_para || n->pa_dirty)
    w42_pt_apply_para_fmt (n->pt, n->pos - 1, 0, W42_PARA_ALL, &n->pa);
  else if (n->pos > body && w42_pt_is_block_mark (n->pt, n->pos - 1))
    w42_pt_delete (n->pt, n->pos - 1, 1);

  memcpy (h->meta, n->meta, sizeof h->meta);
  g_string_free (n->pending, TRUE);
  g_free (n);
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
          guint8 f = 0;

          elem_name (lxb_dom_interface_element (n), name, sizeof name);
          element_start (h, name, lxb_dom_interface_element (n), &f);
        }
    }
}

/* The first four letters of a lowercase element name as one number:
 * enough to tell apart the elements the estimate below cares about. */
static guint32
pack_name (const char *name)
{
  guint32 packed = 0;

  for (guint i = 0; i < 4 && name[i] != '\0'; i++)
    packed |= (guint32) (guchar) name[i] << (8 * i);
  return packed;
}

/* A NULL-ended list of names, packed, into a 0-ended array. */
static void
pack_names (const char *const *names, guint32 *out)
{
  guint k;

  for (k = 0; names[k] != NULL; k++)
    out[k] = pack_name (names[k]);
  out[k] = 0;
}

static gboolean
packed_in (guint32 packed, const guint32 *set)
{
  for (; *set != 0; set++)
    if (*set == packed)
      return TRUE;
  return FALSE;
}

static gboolean
name_in (const char *name, const char *const *names)
{
  for (guint k = 0; names[k] != NULL; k++)
    if (g_str_equal (name, names[k]))
      return TRUE;
  return FALSE;
}

/* Where on the stack the open element a tag ends is -- one packed as `a`
 * or `b` -- looking down past anything but what is in `stop`, or past
 * only what is in `pass` when that is given, and no further than a few
 * dozen: -1 when it is not found so.  A miss only leaves the count higher
 * than the tree builder's, never lower. */
static int
find_open (GArray *open, guint32 a, guint32 b, const guint32 *stop, const guint32 *pass)
{
  guint seen = 0;

  for (guint k = open->len; k > 0 && seen < 64; k--, seen++)
    {
      guint32 e = g_array_index (open, guint32, k - 1);

      if (e == a || e == b)
        return (int) k - 1;
      if (stop != NULL && packed_in (e, stop))
        return -1;
      if (pass != NULL && !packed_in (e, pass))
        return -1;
    }
  return -1;
}

/* True when the page's unclosed nesting is far past what any document
 * means.  The HTML5 tree builder walks its open elements for many a
 * token, so a file that is nothing but open tags costs the square of its
 * depth to parse; the browsers flatten a tree past a few hundred deep,
 * and a word processor can simply decline.  The count follows the tree
 * builder where a page leaves an end tag out -- a cell ends the cell
 * before it, a block's end tag what was left open in it -- and elsewhere
 * errs high, which only ever declines a page no hand wrote. */
static gboolean
nests_too_deeply (const char *data, gsize len)
{
  /* Elements with no closing tag, those the parser refuses to repeat or
   * to nest -- the formatting elements, kept shallow by the spec's own
   * list -- do not stack up, and must not count. */
  static const char *const UNCOUNTED[] = {
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr",
    "html", "head", "body",
    "a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small",
    "strike", "strong", "tt", "u", NULL };
  /* The blocks: an end tag of one of them closes what was left open
   * inside it, and a start tag of one closes an open paragraph. */
  static const char *const BLOCKS[] = {
    "address", "article", "aside", "blockquote", "center", "details",
    "dialog", "dir", "div", "dl", "fieldset", "figcaption", "figure",
    "footer", "header", "hgroup", "listing", "main", "menu", "nav", "ol",
    "pre", "search", "section", "summary", "ul", "form", "h1", "h2", "h3",
    "h4", "h5", "h6", "p", "li", "dd", "dt", NULL };
  static const char *const TABLE_PARTS[] = {
    "table", "tbody", "thead", "tfoot", "tr", "td", "th", "caption", NULL };
  /* What bounds a block's reach, and a table's rows and cells', and the
   * little an item looks past for the item before it. */
  static const char *const SCOPE[] = {
    "table", "td", "th", "caption", "button", "object", "marquee",
    "applet", "template", NULL };
  static const char *const TABLE[] = { "table", NULL };
  static const char *const ROW[] = { "table", "tr", NULL };
  static const char *const ITEM[] = { "div", "p", "address", NULL };
  guint32 scope[G_N_ELEMENTS (SCOPE)], table[G_N_ELEMENTS (TABLE)];
  guint32 row[G_N_ELEMENTS (ROW)], item[G_N_ELEMENTS (ITEM)];
  GArray *open = g_array_new (FALSE, FALSE, sizeof (guint32));
  gsize i = 0;
  gboolean deep = FALSE;

  pack_names (SCOPE, scope);
  pack_names (TABLE, table);
  pack_names (ROW, row);
  pack_names (ITEM, item);

  while (i < len)
    {
      gboolean closing = FALSE;
      char name[12] = { 0 };
      guint n = 0;
      guint32 packed;
      int at = -1;

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

      if (name_in (name, UNCOUNTED))
        continue;

      packed = pack_name (name);
      if (closing)
        {
          if (name_in (name, TABLE_PARTS))
            at = find_open (open, packed, 0, g_str_equal (name, "table") ? NULL : table, NULL);
          else if (name_in (name, BLOCKS))
            at = find_open (open, packed, 0, scope, NULL);
          else if (open->len > 0 && g_array_index (open, guint32, open->len - 1) == packed)
            at = (int) open->len - 1;
          if (at >= 0)
            g_array_set_size (open, (guint) at);
          continue;
        }

      /* A start tag whose element ends the one before it: a cell the cell,
       * a row the row, an item the item. */
      if (g_str_equal (name, "td") || g_str_equal (name, "th"))
        at = find_open (open, pack_name ("td"), pack_name ("th"), row, NULL);
      else if (g_str_equal (name, "tr"))
        at = find_open (open, packed, 0, table, NULL);
      else if (g_str_equal (name, "li"))
        at = find_open (open, packed, 0, NULL, item);
      else if (g_str_equal (name, "dd") || g_str_equal (name, "dt"))
        at = find_open (open, pack_name ("dd"), pack_name ("dt"), NULL, item);
      else if (g_str_equal (name, "option") && open->len > 0 &&
               g_array_index (open, guint32, open->len - 1) == packed)
        at = (int) open->len - 1;
      if (at >= 0)
        g_array_set_size (open, (guint) at);
      /* And a block ends an open paragraph. */
      if (name_in (name, BLOCKS) &&
          (at = find_open (open, pack_name ("p"), 0, scope, NULL)) >= 0)
        g_array_set_size (open, (guint) at);

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

/* The encoding a page declares, in its first bytes as the browsers'
 * prescan reads it: <meta charset="x">, or the charset in a Content-Type
 * <meta>; NULL when it says nothing.  Lowercased. */
static char *
declared_charset (const char *data, gsize len)
{
  gsize n = MIN (len, 8192);
  char *head = g_ascii_strdown (data, (gssize) n);
  const char *m = head;
  char *out = NULL;

  /* Only a <meta> says it: "charset=" in the text of a page that is
   * about encodings is not the page's own. */
  while (out == NULL && (m = strstr (m, "<meta")) != NULL)
    {
      const char *end = strchr (m, '>');
      const char *p = m;

      if (end == NULL)
        break;
      while ((p = g_strstr_len (p, end - p, "charset")) != NULL)
        {
          const char *q = p + 7;
          gsize k = 0;

          while (*q == ' ' || *q == '\t')
            q++;
          if (*q != '=')
            {
              p = q;
              continue;
            }
          q++;
          while (*q == ' ' || *q == '\t' || *q == '"' || *q == '\'')
            q++;
          while (q + k < end && (g_ascii_isalnum (q[k]) || q[k] == '-' || q[k] == '_' || q[k] == '.' || q[k] == ':'))
            k++;
          if (k > 0)
            out = g_strndup (q, k);
          break;
        }
      m = end;
    }
  g_free (head);
  return out;
}

/* The page's bytes as UTF-8.  A byte order mark says what it is; failing
 * that the page's own declaration; failing that UTF-8 if it is valid as
 * that, and otherwise Windows-1252 -- which is what the HTML standard
 * makes of Latin-1 too, since the pages that say Latin-1 mean it, curly
 * quotes and all. */
static char *
page_as_utf8 (char *contents, gsize *length)
{
  gsize len = *length;
  char *out = NULL;
  gsize out_len = 0;
  char *charset;

  /* A last odd byte is half a character, and would fail the whole. */
  if (len >= 2 && ((guchar) contents[0] == 0xFF && (guchar) contents[1] == 0xFE))
    out = g_convert (contents + 2, (len - 2) & ~(gsize) 1, "UTF-8", "UTF-16LE", NULL, &out_len, NULL);
  else if (len >= 2 && ((guchar) contents[0] == 0xFE && (guchar) contents[1] == 0xFF))
    out = g_convert (contents + 2, (len - 2) & ~(gsize) 1, "UTF-8", "UTF-16BE", NULL, &out_len, NULL);
  if (out != NULL)
    {
      g_free (contents);
      *length = out_len;
      return out;
    }
  if (len >= 3 && (guchar) contents[0] == 0xEF && (guchar) contents[1] == 0xBB && (guchar) contents[2] == 0xBF)
    {
      memmove (contents, contents + 3, len - 3);
      len -= 3;
      contents[len] = '\0';
      *length = len;
      if (g_utf8_validate (contents, len, NULL))
        return contents;
    }

  charset = declared_charset (contents, len);
  if (charset != NULL)
    {
      const char *from = charset;

      /* A page that says UTF-16 in its own ASCII bytes is not UTF-16 --
       * it could not have been read to find that out if it were -- and
       * the HTML standard takes such a page for UTF-8. */
      if (g_str_equal (charset, "utf-8") || g_str_equal (charset, "utf8") ||
          g_str_equal (charset, "unicode-1-1-utf-8") ||
          g_str_has_prefix (charset, "utf-16") || g_str_has_prefix (charset, "unicode") ||
          g_str_equal (charset, "ucs-2"))
        from = NULL;
      else if (g_str_has_prefix (charset, "iso-8859-1") || g_str_has_prefix (charset, "iso8859-1") ||
               g_str_equal (charset, "latin1") || g_str_equal (charset, "l1") ||
               g_str_equal (charset, "us-ascii") || g_str_equal (charset, "ascii") ||
               g_str_equal (charset, "iso-ir-100") || g_str_equal (charset, "cp1252") ||
               g_str_equal (charset, "x-cp1252") || g_str_equal (charset, "windows-1252"))
        from = "WINDOWS-1252";
      if (from != NULL)
        out = g_convert (contents, len, "UTF-8", from, NULL, &out_len, NULL);
      g_free (charset);
      if (out != NULL)
        {
          g_free (contents);
          *length = out_len;
          return out;
        }
    }
  if (g_utf8_validate (contents, len, NULL))
    return contents;

  out = g_convert (contents, len, "UTF-8", "WINDOWS-1252", NULL, &out_len, NULL);
  if (out == NULL)
    out = g_convert (contents, len, "UTF-8", "ISO-8859-1", NULL, &out_len, NULL);
  g_free (contents);
  if (out == NULL)
    {
      out = g_strdup ("");
      out_len = 0;
    }
  *length = out_len;
  return out;
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

  contents = page_as_utf8 (contents, &length);

  if (nests_too_deeply (contents, length))
    {
      g_free (contents);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   /* Translators: "the page" is the web page (HTML file)
                    * being opened. */
                   _("The page nests its elements too deeply to be a document."));
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
                   /* Translators: "the page" is the web page (HTML file)
                    * being opened. */
                   _("The page could not be parsed."));
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

  read_head (&h, ldoc);
  /* Word42 names a page after its file when the document has no title
   * of its own, and that is not a title to read back. */
  if (h.meta[0] != NULL)
    {
      char *name = g_file_get_basename (file);
      char *shown = name != NULL ? g_filename_display_name (name) : NULL;

      if (g_strcmp0 (h.meta[0], name) == 0 || g_strcmp0 (h.meta[0], shown) == 0)
        g_clear_pointer (&h.meta[0], g_free);
      g_free (name);
      g_free (shown);
    }
  collect_styles (&h, lxb_dom_interface_node (ldoc));
  harvest_notes (&h, lxb_dom_interface_node (ldoc));

  body = lxb_html_document_body_element (ldoc);
  if (body != NULL)
    {
      lxb_dom_node_t *html = lxb_dom_interface_node (body)->parent;
      char *style = elem_style (&h, lxb_dom_interface_element (body));
      char *bgcolor = elem_attr (lxb_dom_interface_element (body), "bgcolor");
      char *lang = html != NULL && html->type == LXB_DOM_NODE_TYPE_ELEMENT
                     ? elem_attr (lxb_dom_interface_element (html), "lang") : NULL;

      /* The language of the page, until an element says its own. */
      if (lang == NULL || *lang == '\0')
        {
          g_free (lang);
          lang = elem_attr (lxb_dom_interface_element (body), "lang");
        }
      if (lang != NULL && w42_lang_normalise (lang) != NULL)
        h.ch[0].lang = w42_lang_normalise (lang);
      g_free (lang);

      /* What the body says about the type of the page, and the colour
       * behind it. */
      if (style != NULL)
        {
          char *bg = css_value (style, "background-color");

          if (bg == NULL)
            bg = css_value (style, "background");
          apply_style (&h, style, FALSE);
          if (page != NULL && bg != NULL && css_colour (bg) >= 0)
            {
              page->background = (guint32) css_colour (bg);
              page->has_background = 1;
            }
          g_free (bg);
        }
      if (page != NULL && bgcolor != NULL && css_colour (bgcolor) >= 0)
        {
          page->background = (guint32) css_colour (bgcolor);
          page->has_background = 1;
        }
      h.pre_depth = 0;
      g_free (style);
      g_free (bgcolor);

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
  if (h.note_parts != NULL)
    g_hash_table_destroy (h.note_parts);
  if (h.rules != NULL)
    g_ptr_array_free (h.rules, TRUE);
  if (h.rules_by_tag != NULL)
    g_hash_table_destroy (h.rules_by_tag);
  if (h.rules_by_class != NULL)
    g_hash_table_destroy (h.rules_by_class);
  if (h.rules_by_id != NULL)
    g_hash_table_destroy (h.rules_by_id);
  g_free (h.base);
  g_free (contents);
  lxb_html_document_destroy (ldoc);
  w42_pt_clear_undo (pt);
  return TRUE;
}
