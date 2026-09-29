/* w42-wpd.c - reading and writing WordPerfect documents
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A WordPerfect 6 file (every WordPerfect from 1993 on writes it) is a
 * 16-byte header, an index of prefix packets -- font descriptors, the
 * text of headers, notes and comments, the document summary -- and the
 * document itself: characters and function codes to the end of the file.
 * Characters 0x21-0x7F are ASCII, a space is 0x80, and everything else a
 * character-set number and an index in an extended-character group.
 * Codes 0x80-0xCF are one byte each; 0xD0-0xEF are variable-length groups
 * framed by their size at both ends; 0xF0-0xFE fixed-length ones.  Most
 * formatting is state that holds until it changes: an attribute turned
 * on, a font chosen, a paragraph's justification.
 *
 * WordPerfect 5.x, from DOS, keeps the same header and a simpler stream;
 * its text, character formatting, paragraphs and page margins are read.
 *
 * The layouts follow the file formats as libwpd reads them.
 */

#include "w42-wpd.h"

#include <string.h>
#include <glib/gi18n.h>

#include "w42-build.h"
#include "w42-mathtex.h"
#include "w42-wpd-charsets.h"

#define FAIL(err, ...) \
  G_STMT_START { \
    g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, __VA_ARGS__); \
    return FALSE; \
  } G_STMT_END

/* WordPerfect measures in WPUs, 1200 to the inch; the model in twips.  A
 * WPU is sixteen bits in the file, and what another format let into the
 * model can be far more: such a measure is the most the file can say,
 * not a product that overflows. */
#define WPU_TO_TWIPS(v) ((int) ((v) * 6 / 5))
#define TWIPS_TO_WPU(v) ((guint) CLAMP ((gint64) (v) * 5 / 6, 0, 0xFFFF))
#define TWIPS_TO_WPU_SIGNED(v) ((guint16) (gint16) CLAMP ((gint64) (v) * 5 / 6, -0x8000, 0x7FFF))

static inline guint16 rd16 (const guint8 *p) { return (guint16) (p[0] | (p[1] << 8)); }
static inline guint32 rd32 (const guint8 *p)
{
  return (guint32) p[0] | ((guint32) p[1] << 8) | ((guint32) p[2] << 16) | ((guint32) p[3] << 24);
}

gboolean
w42_wpd_sniff (const guint8 *data, gsize len)
{
  return len >= 16 && data[0] == 0xFF && memcmp (data + 1, "WPC", 3) == 0 &&
         data[9] == 0x0A && (data[10] == 0x00 || data[10] == 0x02);
}

/* ---------------------------------------------------------------------- */
/* Characters                                                              */
/* ---------------------------------------------------------------------- */

/* The body's bytes 0x01-0x20: a shorthand for the commonest accented
 * letters, not control codes. */
static const guint16 WP6_SHORTHAND[32] = {
  229, 197, 230, 198, 228, 196, 225, 224, 226, 227, 195, 231, 199, 235, 233, 201,
  232, 234, 237, 241, 209, 248, 216, 245, 213, 246, 214, 252, 220, 250, 249, 223,
};

/* A character from a set, as up to two code points; 0 when there is
 * none.  Set 0 is ASCII, as in font names. */
static int
wp_char (gboolean wp5, guint set, guint index, gunichar out[2])
{
  if (set == 0)
    {
      out[0] = index >= 0x20 && index < 0x7F ? index : ' ';
      return 1;
    }
  if (set == 1)
    for (guint i = 0; i < G_N_ELEMENTS (WPD_MULTINATIONAL_PAIRS); i++)
      if (WPD_MULTINATIONAL_PAIRS[i].index == index)
        {
          gunichar a = WPD_MULTINATIONAL_PAIRS[i].first, b = WPD_MULTINATIONAL_PAIRS[i].second;

          /* A combining mark goes after its letter. */
          if (a >= 0x0300 && a < 0x0370)
            {
              out[0] = b;
              out[1] = a;
            }
          else
            {
              out[0] = a;
              out[1] = b;
            }
          return 2;
        }
  if (set < 15)
    {
      const gunichar *map = wp5 ? WPD_WP5_SETS[set].map : WPD_WP6_SETS[set].map;
      guint n = wp5 ? WPD_WP5_SETS[set].n : WPD_WP6_SETS[set].n;

      if (map != NULL && index < n && map[index] != 0)
        {
          out[0] = map[index];
          return 1;
        }
    }
  out[0] = ' ';
  return 1;
}

/* ---------------------------------------------------------------------- */
/* Reading WordPerfect 6                                                   */
/* ---------------------------------------------------------------------- */

typedef struct {
  guint8  type;
  guint8  flags;
  guint32 size, offset;
} Packet;

/* Attributes, by their numbers in F2/F3 (and C3/C4 in WordPerfect 5). */
enum {
  ATTR_EXTRA_LARGE = 0, ATTR_VERY_LARGE, ATTR_LARGE, ATTR_SMALL, ATTR_FINE,
  ATTR_SUPERSCRIPT, ATTR_SUBSCRIPT, ATTR_OUTLINE, ATTR_ITALIC, ATTR_SHADOW,
  ATTR_REDLINE, ATTR_DOUBLE_UNDERLINE, ATTR_BOLD, ATTR_STRIKEOUT, ATTR_UNDERLINE,
  ATTR_SMALL_CAPS, ATTR_N = 17
};

/* Where the stream's text goes: into the document, or into a string --
 * a header's words, a comment's. */
typedef enum { SINK_BODY, SINK_TEXT } Sink;

/* A style's parts: only the body of a paragraph style is text. */
typedef enum { STYLE_NORMAL, STYLE_BEGIN, STYLE_BODY, STYLE_END, STYLE_GLOBAL } StyleState;

typedef struct {
  const guint8 *data;
  gsize         len;
  gboolean      wp5;
  GArray       *packets;       /* Packet, by prefix ID - 1 */
  W42PieceTable *pt;
  W42PageSetup *page;
  W42Builder    b;
  int           depth;         /* notes and headers read inside the body */
  gsize         budget;        /* packet bytes still to be read as text */

  /* Character state. */
  gboolean      attr[ATTR_N];
  const char   *family;        /* interned */
  int           size;          /* half-points, before the size attributes */
  guint32       color;
  const char   *comment;       /* a comment on the next word, or NULL */

  /* Paragraph state, and the paragraph's own once its text has begun. */
  W42ParaFmt    pa;
  W42ParaFmt    para_pa;
  gboolean      para_open;
  gboolean      break_next;    /* the next paragraph starts a page */
  gboolean      any_text;      /* the page's settings are fixed by now */

  /* Tables. */
  GArray       *col_widths;    /* int twips, from the definition */
  gboolean      table_pending; /* defined, and waiting for its first row */
  gboolean      in_table;

  /* Undone text, and styles' own. */
  gboolean      undone;
  StyleState    style;

  /* The headers and footers met so far: the first of each is the
   * document's. */
  guint         page_texts_set;

  /* A text sink's string, its alignment, and the field being skipped. */
  GString      *text;
  W42Align      text_align;
  int           skip_digits;   /* inside a displayed number: its cached digits go */
} Reader;

static const Packet *
packet (Reader *r, guint pid, guint8 type)
{
  const Packet *p;

  if (pid == 0 || r->packets == NULL || pid > r->packets->len)
    return NULL;
  p = &g_array_index (r->packets, Packet, pid - 1);
  if (p->type != type || p->offset > r->len || p->size > r->len - p->offset)
    return NULL;
  return p;
}

/* A string of WP character words, as a font name or a summary field
 * keeps it: up to `n` bytes, or the first zero. */
static char *
wp_words (Reader *r, const guint8 *p, gsize n)
{
  GString *s = g_string_new (NULL);

  for (gsize i = 0; i + 1 < n; i += 2)
    {
      gunichar out[2];
      int k;

      if (p[i] == 0 && p[i + 1] == 0)
        break;
      k = wp_char (r->wp5, p[i + 1], p[i], out);
      for (int j = 0; j < k; j++)
        g_string_append_unichar (s, out[j]);
    }
  return g_string_free (s, FALSE);
}

/* A font descriptor's name, without the weights and widths WordPerfect
 * puts in it: "Times New Roman Regular" is Times New Roman. */
static const char *
font_name (Reader *r, guint pid)
{
  const Packet *p = packet (r, pid, 0x55);
  char *name, **words;
  GString *clean;
  static const char * const DROP[] = {
    "Bold", "Demi", "Extended", "Extra", "Headline", "Light", "Medium",
    "Normal", "Regular", "Standaard", "Standard", NULL,
  };
  const char *result;

  if (p == NULL || p->size < 24)
    return NULL;
  {
    guint len = rd16 (r->data + p->offset + 22);

    name = wp_words (r, r->data + p->offset + 24, MIN (len, p->size - 24));
  }
  words = g_strsplit (name, " ", -1);
  clean = g_string_new (NULL);
  for (guint i = 0; words[i] != NULL; i++)
    {
      gboolean drop = *words[i] == '\0';

      for (guint k = 0; DROP[k] != NULL && !drop; k++)
        drop = g_ascii_strcasecmp (words[i], DROP[k]) == 0;
      if (g_str_has_suffix (words[i], "-WP"))
        words[i][strlen (words[i]) - 3] = '\0';
      if (!drop)
        {
          if (clean->len > 0)
            g_string_append_c (clean, ' ');
          g_string_append (clean, words[i]);
        }
    }
  result = clean->len > 0 ? g_intern_string (clean->str) : NULL;
  g_string_free (clean, TRUE);
  g_strfreev (words);
  g_free (name);
  return result;
}

static void parse_stream (Reader *r, const guint8 *p, const guint8 *end, Sink sink);

/* A general text packet's words as a stream, read into `sink`. */
static void
parse_text_packet (Reader *r, guint pid, Sink sink)
{
  const Packet *p = packet (r, pid, 0x08);
  const guint8 *d;
  guint n;
  gsize total = 0;

  if (p == NULL || p->size < 6 || r->depth > 3)
    return;
  d = r->data + p->offset;
  n = rd16 (d);
  if (n == 0 || 6 + 4 * (gsize) n > p->size)
    return;
  for (guint i = 0; i < n; i++)
    total += rd32 (d + 6 + 4 * i);
  if (total > p->size - 6 - 4 * n)
    return;
  /* Each packet is some code's own, and read once, so together they are
   * no more than the file.  A file whose every note names one packet
   * would otherwise copy it once a note: kilobytes that read as
   * gigabytes, and notes whose text is comments naming another packet
   * multiply again. */
  if (total > r->budget)
    return;
  r->budget -= total;
  r->depth++;
  parse_stream (r, d + 6 + 4 * n, d + 6 + 4 * n + total, sink);
  r->depth--;
}

/* The text of a general text packet, as one line. */
static char *
text_of_packet (Reader *r, guint pid, W42Align *align)
{
  GString *saved = r->text;
  W42Align saved_align = r->text_align;
  StyleState saved_style = r->style;
  char *s;

  r->text = g_string_new (NULL);
  r->text_align = W42_ALIGN_LEFT;
  r->style = STYLE_NORMAL;
  parse_text_packet (r, pid, SINK_TEXT);
  if (align != NULL)
    *align = r->text_align;
  s = g_strstrip (g_string_free (r->text, FALSE));
  r->text = saved;
  r->text_align = saved_align;
  r->style = saved_style;
  return s;
}

/* The run's formatting: the font and size with the size attributes,
 * which are proportions of it, and the rest. */
static void
apply_char (Reader *r)
{
  W42CharFmt *ch = &r->b.ch;
  double scale = r->attr[ATTR_EXTRA_LARGE] ? 2.0 : r->attr[ATTR_VERY_LARGE] ? 1.5
               : r->attr[ATTR_LARGE] ? 1.2 : r->attr[ATTR_SMALL] ? 0.8
               : r->attr[ATTR_FINE] ? 0.6 : 1.0;

  ch->family    = r->family;
  ch->size      = CLAMP ((int) (r->size * scale + 0.5), 2, 3276);
  ch->bold      = r->attr[ATTR_BOLD] ? 1 : 0;
  ch->italic    = r->attr[ATTR_ITALIC] ? 1 : 0;
  ch->underline = r->attr[ATTR_DOUBLE_UNDERLINE] ? W42_UNDERLINE_DOUBLE
                : r->attr[ATTR_UNDERLINE] ? W42_UNDERLINE_SINGLE : W42_UNDERLINE_NONE;
  ch->strikeout = r->attr[ATTR_STRIKEOUT] ? 1 : 0;
  ch->outline   = r->attr[ATTR_OUTLINE] ? 1 : 0;
  ch->shadow    = r->attr[ATTR_SHADOW] ? 1 : 0;
  ch->smallcaps = r->attr[ATTR_SMALL_CAPS] ? 1 : 0;
  ch->script    = r->attr[ATTR_SUPERSCRIPT] ? 1 : r->attr[ATTR_SUBSCRIPT] ? -1 : 0;
  ch->revision  = r->attr[ATTR_REDLINE] ? 1 : 0;
  ch->color     = r->color;
  ch->comment   = r->comment;
}

/* Whether characters here are the document's. */
static gboolean
text_wanted (Reader *r)
{
  return !r->undone && r->skip_digits == 0 &&
         (r->style == STYLE_NORMAL || r->style == STYLE_BODY);
}

static void
put_text (Reader *r, Sink sink, const char *utf8)
{
  if (!text_wanted (r))
    return;
  if (sink == SINK_TEXT)
    {
      g_string_append (r->text, utf8);
      return;
    }
  if (!r->para_open)
    {
      r->para_pa = r->pa;
      r->para_open = TRUE;
    }
  r->any_text = TRUE;
  apply_char (r);
  w42_builder_text (&r->b, utf8);
  /* A comment is on the word after its mark: it ends at a space. */
  if (r->comment != NULL && g_str_equal (utf8, " "))
    r->comment = NULL;
}

static void
put_char (Reader *r, Sink sink, gunichar c)
{
  char buf[8];

  buf[g_unichar_to_utf8 (c, buf)] = '\0';
  put_text (r, sink, buf);
}

/* A field's result in the body: its code on the run, its text the
 * number Word42 will put in its place. */
static void
put_field (Reader *r, Sink sink, const char *code)
{
  if (sink == SINK_TEXT)
    {
      if (text_wanted (r))
        g_string_append_printf (r->text, "{%s}", code);
      return;
    }
  if (!text_wanted (r))
    return;
  r->b.ch.field = g_intern_string (code);
  put_text (r, sink, "1");
  r->b.ch.field = NULL;
}

static void
end_paragraph (Reader *r, Sink sink)
{
  if (sink == SINK_TEXT)
    {
      if (r->text->len > 0 && r->text->str[r->text->len - 1] != ' ')
        g_string_append_c (r->text, ' ');
      return;
    }
  if (!text_wanted (r) && r->style != STYLE_BEGIN && r->style != STYLE_END)
    return;
  r->b.pa = r->para_open ? r->para_pa : r->pa;
  r->b.pa.page_break_before = r->break_next ? 1 : 0;
  r->break_next = FALSE;
  apply_char (r);
  w42_builder_end_paragraph (&r->b);
  r->para_open = FALSE;
  r->comment = NULL;
  r->any_text = TRUE;
}

/* A hard page ends the paragraph that is open, if one is, and the next
 * one starts the page: a paragraph already ended leaves no empty one. */
static void
page_break (Reader *r, Sink sink)
{
  if (sink != SINK_BODY)
    return;
  if (r->para_open)
    end_paragraph (r, sink);
  r->break_next = TRUE;
}

/* ---- tables ----------------------------------------------------------- */

static void
table_row (Reader *r, int col_span, int row_span, gboolean header, int height_wpu)
{
  if (r->table_pending && !r->in_table)
    {
      int n = MAX ((int) r->col_widths->len, 1);

      if (r->para_open)
        end_paragraph (r, SINK_BODY);
      w42_builder_begin_table (&r->b, n, r->col_widths->len > 0 ? (int *) r->col_widths->data : NULL);
      r->in_table = TRUE;
      r->table_pending = FALSE;
    }
  else if (r->in_table)
    {
      w42_builder_end_cell (&r->b);
      w42_builder_end_row (&r->b);
    }
  else
    return;
  if (header)
    w42_pt_table_set_header_rows (r->pt, r->b.table, r->b.row + 1);
  if (height_wpu > 0)
    w42_pt_table_set_row_height (r->pt, r->b.table, r->b.row, WPU_TO_TWIPS (height_wpu));
  (void) col_span;
  (void) row_span;
}

static void
table_cell (Reader *r, int col_span, int row_span, gboolean covered,
            gboolean fill, guint32 rgb, int valign, gboolean new_row)
{
  if (!r->in_table)
    return;
  if (!new_row)
    w42_builder_end_cell (&r->b);
  w42_builder_begin_cell (&r->b, covered ? 1 : MAX (col_span, 1));
  if (r->b.cell_pos == (gsize) -1)
    return;
  if (covered)
    w42_pt_set_cell_vspan (r->pt, r->b.cell_pos, W42_CELL_COVERED);
  else if (row_span > 1)
    w42_pt_set_cell_vspan (r->pt, r->b.cell_pos, 2);
  if (fill)
    w42_pt_cell_set_fill_at (r->pt, r->b.cell_pos, TRUE, rgb);
  if (valign == 1)
    w42_pt_cell_set_valign_at (r->pt, r->b.cell_pos, W42_CELL_VALIGN_CENTER);
  else if (valign == 2)
    w42_pt_cell_set_valign_at (r->pt, r->b.cell_pos, W42_CELL_VALIGN_BOTTOM);
  r->para_open = FALSE;
}

static void
table_off (Reader *r)
{
  if (!r->in_table)
    {
      r->table_pending = FALSE;
      return;
    }
  w42_builder_end_cell (&r->b);
  w42_pt_resolve_vmerges (r->pt, r->b.table);
  w42_builder_end_table (&r->b);
  r->in_table = FALSE;
  r->para_open = FALSE;
  g_array_set_size (r->col_widths, 0);
}

/* ---- the groups -------------------------------------------------------- */

/* The EOL group: a hard return, a page break, a table's row or cell with
 * what its sub-functions say about it. */
static void
eol_group (Reader *r, Sink sink, guint sub, const guint8 *d, const guint8 *end)
{
  int col_span = 1, row_span = 1, valign = 0, height = 0;
  gboolean covered = FALSE, header = FALSE, fill = FALSE, line_break = FALSE;
  guint32 rgb = 0;

  if (d + 2 <= end)
    {
      const guint8 *q = d + 2 + rd16 (d);

      while (q < end && *q >= 0x80)
        {
          guint code = *q;
          gsize size;

          switch (code)
            {
            case 0x80:
              size = 5;
              if (q + 4 <= end)
                {
                  header = (q[1] & 0x04) != 0;
                  if (q[1] & 0x02)
                    height = rd16 (q + 2);
                }
              break;
            case 0x82: case 0x83: size = 4; break;
            case 0x84:
              size = 9;
              if (q + 4 <= end)
                valign = q[3] & 3;
              break;
            case 0x85:
              size = 4;
              if (q + 3 <= end)
                {
                  covered = q[1] >= 128;
                  col_span = q[1] & 0x7F;
                  row_span = q[2];
                }
              break;
            case 0x86:
              /* The fill: a foreground colour shaded over the background;
               * solid, the foreground is the colour. */
              size = 10;
              if (q + 9 <= end)
                {
                  const guint8 *c = q[4] > 0 ? q + 1 : q + 5;

                  rgb = ((guint32) c[0] << 16) | ((guint32) c[1] << 8) | c[2];
                  fill = rgb != 0xFFFFFF;
                }
              break;
            case 0x87: size = 6; break;
            case 0x88: size = 6; break;
            case 0x89: size = 11; break;
            case 0x8B: size = 3; break;
            case 0x8C: size = 3; break;
            case 0x8D: size = 1; line_break = TRUE; break;
            case 0x81: case 0x8E: case 0x8F:
              size = q + 3 <= end ? rd16 (q + 1) : 0;
              break;
            default:
              size = 0;
              break;
            }
          if (size == 0 || size > (gsize) (end - q))
            break;
          q += size;
        }
    }

  switch (sub)
    {
    case 0x01: case 0x02: case 0x03:
      put_text (r, sink, " ");
      break;
    case 0x04: case 0x05: case 0x06: case 0x17: case 0x18: case 0x19: case 0x1C:
      end_paragraph (r, sink);
      break;
    case 0x09:
      page_break (r, sink);
      break;
    case 0x07: case 0x08: case 0x1A: case 0x1B:
      end_paragraph (r, sink);
      break;
    case 0x0A:
      if (sink == SINK_BODY)
        table_cell (r, col_span, row_span, covered, fill, rgb, valign, FALSE);
      break;
    case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10:
      if (sink == SINK_BODY)
        {
          gboolean was = r->in_table;

          table_row (r, col_span, row_span, header, height);
          if (r->in_table)
            table_cell (r, col_span, row_span, covered, fill, rgb, valign, TRUE);
          (void) was;
        }
      break;
    case 0x11: case 0x12: case 0x13:
      if (sink == SINK_BODY)
        table_off (r);
      break;
    case 0x14:
      if (line_break)
        put_char (r, sink, 0x2028);
      break;
    default:
      break;
    }
}

static int
wpu_s16 (const guint8 *p)
{
  return (gint16) rd16 (p);
}

/* A 16.16 fixed-point proportion as a percentage. */
static int
proportion (const guint8 *p)
{
  guint32 v = rd32 (p);

  return (int) ((gint16) (v >> 16)) * 100 + (int) (((v & 0xFFFF) * 100 + 0x7FFF) / 0xFFFF);
}

static void
paragraph_group (Reader *r, Sink sink, guint sub, const guint8 *d, const guint8 *end, guint nondel)
{
  gsize n = (gsize) (end - d);

  switch (sub)
    {
    case 0x01:
      if (n >= 4)
        {
          int pct = proportion (d);

          r->pa.line_spacing = 0;
          r->pa.line_spacing_pct = pct > 0 && pct != 100 ? CLAMP (pct, 50, 500) : 0;
        }
      break;
    case 0x04:
      if (n >= 4)
        {
          /* Tab stops: absolute from the page's edge, or from the margin. */
          gboolean relative = d[0] != 0;
          guint count = d[3];
          int last = 0, base = relative ? 0 : (r->page != NULL ? r->page->margin_left : 1440);

          r->pa.n_tabs = 0;
          for (guint i = 0; i < count && 4 + 3 * (gsize) (i + 1) <= n; i++)
            {
              guint type = d[4 + 3 * i];
              guint pos = rd16 (d + 5 + 3 * i);

              if (pos == 0xFFFF)
                continue;
              if (type & 0x80)
                {
                  for (guint k = 0; k < (type & 0x7F) && r->pa.n_tabs < W42_MAX_TABS; k++)
                    {
                      last += WPU_TO_TWIPS (pos);
                      w42_para_fmt_set_tab (&r->pa, last, W42_TAB_LEFT);
                    }
                  continue;
                }
              last = WPU_TO_TWIPS (pos) - base;
              if (last > 0 && r->pa.n_tabs < W42_MAX_TABS)
                {
                  static const W42TabKind KINDS[] = { W42_TAB_LEFT, W42_TAB_CENTER, W42_TAB_RIGHT, W42_TAB_DECIMAL };
                  W42TabLeader leader = (type & 0x10) == 0 ? W42_TAB_LEAD_NONE
                                        : ((type & 0x60) >> 5) == 2 ? W42_TAB_LEAD_DASH
                                        : ((type & 0x60) >> 5) == 3 ? W42_TAB_LEAD_LINE : W42_TAB_LEAD_DOT;

                  w42_para_fmt_set_tab_leader (&r->pa, last, KINDS[(type & 0x0F) < 4 ? (type & 0x0F) : 0], leader);
                }
            }
        }
      break;
    case 0x05:
      if (n >= 1)
        {
          static const W42Align ALIGNS[] = { W42_ALIGN_LEFT, W42_ALIGN_JUSTIFY, W42_ALIGN_CENTER,
                                             W42_ALIGN_RIGHT, W42_ALIGN_JUSTIFY, W42_ALIGN_LEFT };

          r->pa.align = ALIGNS[MIN (d[0], 5)];
          if (sink == SINK_TEXT)
            r->text_align = r->pa.align;
          else if (r->para_open)
            r->para_pa.align = r->pa.align;   /* it closes the paragraph in WordPerfect */
        }
      break;
    case 0x0A:
      if (n >= 4)
        {
          int rel = proportion (d) - 100;
          int abs_wpu = nondel == 6 && n >= 6 ? rd16 (d + 4) : 0;

          /* Thousands of lines at the largest size overflow an int; no
           * paragraph wants more than a long page's height after it. */
          r->pa.space_after = (int) CLAMP ((gint64) rel * r->size * 10 / 100 + WPU_TO_TWIPS (abs_wpu),
                                           0, 31680);
        }
      break;
    case 0x0B:
      if (n >= 2)
        r->pa.indent_first = WPU_TO_TWIPS (wpu_s16 (d));
      break;
    case 0x0C:
      if (n >= 2)
        r->pa.indent_left = MAX (0, WPU_TO_TWIPS (wpu_s16 (d)));
      break;
    case 0x0D:
      if (n >= 2)
        r->pa.indent_right = MAX (0, WPU_TO_TWIPS (wpu_s16 (d)));
      break;
    default:
      break;
    }
}

static void
character_group (Reader *r, Sink sink, guint sub, const guint8 *d, const guint8 *end,
                 const guint16 *pids, guint n_pids)
{
  gsize n = (gsize) (end - d);

  switch (sub)
    {
    case 0x18:
      if (n >= 3)
        r->color = ((guint32) d[0] << 16) | ((guint32) d[1] << 8) | d[2];
      break;
    case 0x1A:
      if (n >= 8 && n_pids > 0)
        {
          const char *name = font_name (r, pids[0]);
          guint size = rd16 (d + 6);

          if (name != NULL)
            r->family = name;
          if (size > 0)
            r->size = CLAMP ((int) (size / 25), 2, 3276);
        }
      break;
    case 0x1B:
      if (n >= 2 && n_pids > 0 && rd16 (d) > 0)
        r->size = CLAMP ((int) (rd16 (d) / 25), 2, 3276);
      break;
    case 0x1D:
      /* A comment: its annotation packet names the text packet. */
      for (guint i = 0; i < n_pids && sink == SINK_BODY; i++)
        {
          const Packet *c = packet (r, pids[i], 0x1B);

          if (c != NULL && c->size >= 4 && rd16 (r->data + c->offset) == 1)
            {
              char *text = text_of_packet (r, rd16 (r->data + c->offset + 2), NULL);

              if (*text != '\0')
                r->comment = g_intern_string (text);
              g_free (text);
              break;
            }
        }
      break;
    case 0x2A:
      if (sink == SINK_BODY && !r->in_table)
        {
          g_array_set_size (r->col_widths, 0);
          r->table_pending = TRUE;
        }
      break;
    case 0x2C:
      if (sink == SINK_BODY && r->table_pending && n >= 3 && r->col_widths->len < 64)
        {
          int w = WPU_TO_TWIPS (rd16 (d + 1));

          g_array_append_val (r->col_widths, w);
        }
      break;
    default:
      break;
    }
}

/* A tab code: at the start of a paragraph WordPerfect's indents,
 * centring and flush right are the paragraph's; elsewhere a tab. */
static void
tab_group (Reader *r, Sink sink, guint sub, const guint8 *d, const guint8 *end, guint size)
{
  guint kind = (sub & 0xF8) >> 3;
  gboolean at_start = sink == SINK_BODY ? !r->para_open : r->text->len == 0;
  guint pos = 0;

  if ((sub & 0xC0) == 0 && end - d >= 2)
    pos = rd16 (d);
  else if (size >= 12 && size <= 18 && (gsize) (end - d) >= size - 10)
    pos = rd16 (d + size - 12);

  if (at_start && (kind == 0x08 || kind == 0x09))
    {
      /* Centred on the margins: this paragraph, not the ones after. */
      if (sink == SINK_TEXT)
        r->text_align = W42_ALIGN_CENTER;
      else
        {
          r->para_pa = r->pa;
          r->para_pa.align = W42_ALIGN_CENTER;
          r->para_open = TRUE;
        }
      return;
    }
  if (at_start && kind == 0x10)
    {
      if (sink == SINK_TEXT)
        r->text_align = W42_ALIGN_RIGHT;
      else
        {
          r->para_pa = r->pa;
          r->para_pa.align = W42_ALIGN_RIGHT;
          r->para_open = TRUE;
        }
      return;
    }
  if (at_start && (kind == 0x06 || kind == 0x07) && sink == SINK_BODY)
    {
      /* An indent: the paragraph's left edge moves to the stop. */
      int margin = r->page != NULL ? r->page->margin_left : 1440;

      r->para_pa = r->pa;
      if (pos > 0)
        r->para_pa.indent_left = MAX (0, WPU_TO_TWIPS (pos) - margin);
      else
        r->para_pa.indent_left += 720;
      if (kind == 0x07)
        r->para_pa.indent_right = r->para_pa.indent_left;
      r->para_open = TRUE;
      return;
    }
  if (kind == 0x04)
    {
      put_text (r, sink, "\t|");
      return;
    }
  put_text (r, sink, sink == SINK_TEXT ? " " : "\t");
}

/* ---- the stream ------------------------------------------------------- */

static const guint8 WP6_FIXED_SIZE[16] = { 4, 5, 3, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 8, 8, 0 };

static void
parse_stream (Reader *r, const guint8 *p, const guint8 *end, Sink sink)
{
  while (p < end)
    {
      guint c = *p;

      if (c == 0x00 || c == 0xFF)
        {
          p++;
          continue;
        }
      if (c <= 0x20)
        {
          put_char (r, sink, WP6_SHORTHAND[c - 1]);
          p++;
          continue;
        }
      if (c <= 0x7F)
        {
          const guint8 *s = p;
          char *run;

          while (p < end && *p >= 0x21 && *p <= 0x7F)
            p++;
          run = g_strndup ((const char *) s, (gsize) (p - s));
          put_text (r, sink, run);
          g_free (run);
          continue;
        }
      if (c <= 0xCF)
        {
          p++;
          switch (c)
            {
            case 0x80: put_text (r, sink, " "); break;
            case 0x81: put_char (r, sink, 0x00A0); break;
            case 0x82: case 0x83: put_char (r, sink, 0x00AD); break;
            case 0x84: put_text (r, sink, "-"); break;
            case 0x87: case 0xB7: case 0xB8: case 0xB9: case 0xCA: case 0xCB: case 0xCC:
              end_paragraph (r, sink);
              break;
            case 0xB4: case 0xC7:
              page_break (r, sink);
              break;
            case 0xB5: case 0xB6: case 0xC8: case 0xC9:
              end_paragraph (r, sink);
              break;
            case 0xBD: case 0xBE: case 0xBF:
              if (sink == SINK_BODY)
                table_off (r);
              break;
            case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
              if (sink == SINK_BODY)
                {
                  table_row (r, 1, 1, FALSE, 0);
                  table_cell (r, 1, 1, FALSE, FALSE, 0, 0, TRUE);
                }
              break;
            case 0xC6:
              if (sink == SINK_BODY)
                table_cell (r, 1, 1, FALSE, FALSE, 0, 0, FALSE);
              break;
            case 0xCD: case 0xCE: case 0xCF:
              put_text (r, sink, " ");
              break;
            default:
              break;
            }
          continue;
        }
      if (c >= 0xF0)
        {
          guint size = WP6_FIXED_SIZE[c - 0xF0];

          if (size == 0 || (gsize) (end - p) < size || p[size - 1] != c)
            {
              p++;
              continue;
            }
          switch (c)
            {
            case 0xF0:
              {
                gunichar out[2];
                int k = wp_char (FALSE, p[2], p[1], out);

                for (int i = 0; i < k; i++)
                  put_char (r, sink, out[i]);
              }
              break;
            case 0xF1:
              if (p[1] == 0)
                r->undone = TRUE;
              else if (p[1] == 1)
                r->undone = FALSE;
              break;
            case 0xF2: case 0xF3:
              if (p[1] < ATTR_N)
                r->attr[p[1]] = c == 0xF2;
              break;
            default:
              break;
            }
          p += size;
          continue;
        }

      /* A variable-length group, framed by its size at both ends. */
      {
        guint size, sub, flags, n_pids = 0, nondel;
        guint16 pids[16];
        const guint8 *q, *d, *dend;

        if ((gsize) (end - p) < 10)
          {
            p++;
            continue;
          }
        sub = p[1];
        size = rd16 (p + 2);
        if (size < 10 || size > (gsize) (end - p) || rd16 (p + size - 3) != size || p[size - 1] != c)
          {
            p++;
            continue;
          }
        flags = p[4];
        q = p + 5;
        dend = p + size - 3;
        if (flags & 0x80)
          {
            guint k = q < dend ? *q++ : 0;

            for (guint i = 0; i < k && q + 2 <= dend; i++, q += 2)
              if (n_pids < G_N_ELEMENTS (pids))
                pids[n_pids++] = rd16 (q);
          }
        if (q + 2 > dend)
          {
            p += size;
            continue;
          }
        nondel = rd16 (q);
        d = q + 2;

        switch (c)
          {
          case 0xD0:
            eol_group (r, sink, sub, d, MIN (d + nondel, dend));
            break;
          case 0xD1:
            /* The page: its margins and its paper, while nothing is on it. */
            if (sink == SINK_BODY && r->page != NULL && !r->any_text && r->depth == 0)
              {
                if ((sub == 0x00 || sub == 0x01) && dend - d >= 2)
                  {
                    int m = WPU_TO_TWIPS (rd16 (d));

                    if (sub == 0x00)
                      r->page->margin_top = m;
                    else
                      r->page->margin_bottom = m;
                  }
                else if (sub == 0x11 && dend - d >= 9)
                  {
                    int length = WPU_TO_TWIPS (rd16 (d + 3)), width = WPU_TO_TWIPS (rd16 (d + 5));

                    if (length >= 1440 && width >= 1440)
                      {
                        r->page->width = d[8] == 1 ? MAX (width, length) : width;
                        r->page->height = d[8] == 1 ? MIN (width, length) : length;
                      }
                  }
              }
            if (sub == 0x03 && sink == SINK_BODY && dend - d >= 8 && r->depth == 0)
              {
                /* WordPerfect's own page numbering, without a header: a
                 * header or footer holding the number, where it says. */
                guint where = d[7];
                W42Align align = (where == 1 || where == 5) ? W42_ALIGN_LEFT
                                 : (where == 3 || where == 7) ? W42_ALIGN_RIGHT : W42_ALIGN_CENTER;
                const W42PageText *have = where <= 4 ? w42_pt_get_header (r->pt) : w42_pt_get_footer (r->pt);

                if (where >= 1 && where <= 10 && (have == NULL || have->text == NULL || *have->text == '\0'))
                  {
                    if (where <= 4 || where == 9)
                      w42_pt_set_header (r->pt, "{PAGE}", align);
                    else
                      w42_pt_set_footer (r->pt, "{PAGE}", align);
                  }
              }
            break;
          case 0xD2:
            if (flags & 0x40)
              break;
            if ((sub == 0 || sub == 1) && sink == SINK_BODY && r->page != NULL && !r->any_text &&
                r->depth == 0 && dend - d >= 2)
              {
                int m = WPU_TO_TWIPS (rd16 (d));

                if (sub == 0)
                  r->page->margin_left = m;
                else
                  r->page->margin_right = m;
              }
            else if (sub == 2 && sink == SINK_BODY && r->page != NULL && dend - d >= 6 && r->depth == 0)
              {
                int cols = d[5];

                if (!r->any_text)
                  r->page->columns = cols > 1 ? MIN (cols, 6) : 1;
                else
                  {
                    /* Columns that change on the way: a new section. */
                    end_paragraph (r, sink);
                    r->pa.section_break = 1;
                    r->pa.columns = (guint8) (cols > 1 ? MIN (cols, 6) : 1);
                  }
              }
            break;
          case 0xD3:
            paragraph_group (r, sink, sub, d, dend, nondel);
            break;
          case 0xD4:
            character_group (r, sink, sub, d, dend, pids, n_pids);
            break;
          case 0xD6:
            /* A header or footer: its text is a general text packet. */
            if (sink == SINK_BODY && n_pids > 0 && sub <= 3 && r->depth == 0)
              {
                W42Align align = W42_ALIGN_LEFT;
                char *text;
                guint occurs = dend > d ? d[0] & 3 : 3;
                gboolean header = sub <= 1;
                guint bit = 1u << ((header ? 0 : 2) + (occurs == 2 ? 1 : 0));

                /* A header changed further on is the later pages'; the
                 * document's is its first. */
                if (r->page_texts_set & bit)
                  break;
                r->page_texts_set |= bit;
                text = text_of_packet (r, pids[0], &align);
                if (occurs == 2)
                  {
                    if (header)
                      w42_pt_set_header_kind (r->pt, W42_PAGE_TEXT_EVEN, text, align);
                    else
                      w42_pt_set_footer_kind (r->pt, W42_PAGE_TEXT_EVEN, text, align);
                    w42_pt_set_facing_pages (r->pt, TRUE);
                  }
                else if (header)
                  w42_pt_set_header (r->pt, text, align);
                else
                  w42_pt_set_footer (r->pt, text, align);
                g_free (text);
              }
            break;
          case 0xD7:
            /* A note: its mark here, its text in a packet. */
            if ((sub == 0x00 || sub == 0x02) && n_pids > 0 && sink == SINK_BODY && text_wanted (r) &&
                r->b.note_return == (gsize) -1 && !r->in_table)
              {
                gboolean saved_open = r->para_open;
                W42ParaFmt saved_pa = r->pa, saved_para_pa = r->para_pa;
                StyleState saved_style = r->style;

                if (!r->para_open)
                  {
                    r->para_pa = r->pa;
                    r->para_open = TRUE;
                  }
                apply_char (r);
                w42_builder_begin_note (&r->b, sub == 0x02);
                r->para_open = FALSE;
                r->pa = (W42ParaFmt) { 0 };
                r->pa.widow_control = 1;
                r->style = STYLE_NORMAL;
                parse_text_packet (r, pids[0], SINK_BODY);
                /* A table the note's text began ends with the note: left
                 * open, the body's text after it would go into its cells,
                 * at positions the builder no longer keeps straight. */
                if (r->in_table)
                  table_off (r);
                w42_builder_end_note (&r->b);
                r->para_open = saved_open;
                r->pa = saved_pa;
                r->para_pa = saved_para_pa;
                r->style = saved_style;
              }
            break;
          case 0xD8:
            if (sub == 0x02 && dend - d >= 4 && sink == SINK_BODY && r->depth == 0 && !r->any_text)
              w42_pt_set_page_numbering (r->pt, 1, rd16 (d + 2));
            break;
          case 0xDA:
            /* A displayed number: the page's or the page count become
             * fields; the digits cached with them are not text. */
            if (sub == 0x04 || sub == 0x14 || sub == 0x0E || sub == 0x10)
              r->skip_digits++;
            else if (sub == 0x05 || sub == 0x15 || sub == 0x0F || sub == 0x11)
              {
                if (r->skip_digits > 0)
                  r->skip_digits--;
                if (sub == 0x05)
                  put_field (r, sink, "PAGE");
                else if (sub == 0x15)
                  put_field (r, sink, "NUMPAGES");
              }
            break;
          case 0xDD:
            switch (sub)
              {
              case 0x04: r->style = STYLE_BEGIN; break;
              case 0x07: r->style = STYLE_BODY; break;
              case 0x08: r->style = STYLE_END; break;
              case 0x09: r->style = STYLE_NORMAL; break;
              case 0x0A: r->style = STYLE_GLOBAL; break;
              case 0x0B: r->style = STYLE_NORMAL; break;
              default: break;
              }
            break;
          case 0xE0:
            if (!(flags & 0x40))
              tab_group (r, sink, sub, d, dend, size);
            break;
          default:
            break;
          }
        p += size;
      }
    }
}

/* ---- the prefix: the index of packets, and what they say -------------- */

static void
read_packets (Reader *r)
{
  guint idx = MAX (rd16 (r->data + 14), 16), n;

  r->packets = g_array_new (FALSE, TRUE, sizeof (Packet));
  if ((gsize) idx + 14 > r->len)
    return;
  n = rd16 (r->data + idx + 2);
  for (guint i = 1; i < n && i < 65536 && (gsize) idx + 14 * (i + 1) <= r->len; i++)
    {
      const guint8 *e = r->data + idx + 14 * i;
      Packet p;

      p.flags = e[0];
      p.type = e[1];
      p.size = rd32 (e + 6);
      p.offset = rd32 (e + 10);
      g_array_append_val (r->packets, p);
    }
}

/* The document's first font, and the summary: title, subject, author,
 * keywords, abstract. */
static void
read_prefix (Reader *r)
{
  for (guint i = 0; i < r->packets->len; i++)
    {
      const Packet *p = &g_array_index (r->packets, Packet, i);
      const guint8 *d;

      if (p->offset > r->len || p->size > r->len - p->offset)
        continue;
      d = r->data + p->offset;
      if (p->type == 0x25 && p->size >= 6)
        {
          const char *name = font_name (r, rd16 (d + 2));

          if (name != NULL)
            r->family = name;
          if (rd16 (d + 4) > 0)
            r->size = CLAMP (rd16 (d + 4) / 25, 2, 3276);
        }
      else if (p->type == 0x12 && p->size <= r->budget)
        {
          W42DocInfo info = *w42_pt_get_info (r->pt);
          gsize at = 0;

          /* The index can name one summary thousands of times. */
          r->budget -= p->size;
          while (at + 6 <= p->size)
            {
              guint glen = rd16 (d + at), tag = rd16 (d + at + 2);
              gsize q = at + 6, end = MIN (at + glen, p->size);
              char *value;

              if (glen < 6 || at + glen > p->size)
                break;
              while (q + 1 < end && (d[q] || d[q + 1]))
                q += 2;                        /* past the name */
              q += 2;
              value = q < end ? wp_words (r, d + q, end - q) : g_strdup ("");
              if (*value != '\0')
                {
                  const char *v = g_intern_string (value);

                  switch (tag)
                    {
                    case 17: info.title = v; break;
                    case 5:  info.author = v; break;
                    case 46: info.subject = v; break;
                    case 26: info.keywords = v; break;
                    case 1:  info.comments = v; break;
                    default: break;
                    }
                }
              g_free (value);
              at += glen;
            }
          w42_pt_set_info (r->pt, &info);
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Reading WordPerfect 5                                                   */
/* ---------------------------------------------------------------------- */

static const guint8 WP5_FIXED_SIZE[16] = { 4, 9, 11, 3, 3, 5, 6, 7, 4, 5, 6, 6, 8, 10, 10, 12 };

static void
parse_wp5 (Reader *r, const guint8 *p, const guint8 *end)
{
  while (p < end)
    {
      guint c = *p;

      if (c >= 0x20 && c <= 0x7E)
        {
          const guint8 *s = p;
          char *run;

          while (p < end && *p >= 0x20 && *p <= 0x7E)
            p++;
          run = g_strndup ((const char *) s, (gsize) (p - s));
          put_text (r, SINK_BODY, run);
          g_free (run);
          continue;
        }
      if (c < 0x20 || (c >= 0x80 && c <= 0xBF) || c == 0x7F || c == 0xFF)
        {
          p++;
          switch (c)
            {
            case 0x0A: case 0x8C: case 0x90: case 0x99:
              end_paragraph (r, SINK_BODY);
              break;
            case 0x0C:
              page_break (r, SINK_BODY);
              break;
            case 0x0B: case 0x0D: case 0x93: case 0x94: case 0x95:
              put_text (r, SINK_BODY, " ");
              break;
            case 0xA0:
              put_char (r, SINK_BODY, 0x00A0);
              break;
            case 0xA9: case 0xAA: case 0xAB:
              put_text (r, SINK_BODY, "-");
              break;
            case 0xAC: case 0xAD: case 0xAE:
              put_char (r, SINK_BODY, 0x00AD);
              break;
            default:
              break;
            }
          continue;
        }
      if (c >= 0xC0 && c <= 0xCF)
        {
          guint size = WP5_FIXED_SIZE[c - 0xC0];

          if ((gsize) (end - p) < size || p[size - 1] != c)
            {
              p++;
              continue;
            }
          switch (c)
            {
            case 0xC0:
              {
                gunichar out[2];
                int k = wp_char (TRUE, p[2], p[1], out);

                for (int i = 0; i < k; i++)
                  put_char (r, SINK_BODY, out[i]);
              }
              break;
            case 0xC1:
              put_text (r, SINK_BODY, "\t");
              break;
            case 0xC2:
              if (!r->para_open)
                {
                  r->para_pa = r->pa;
                  r->para_pa.indent_left += 720;
                  r->para_open = TRUE;
                }
              break;
            case 0xC3: case 0xC4:
              if (p[1] < ATTR_N)
                r->attr[p[1]] = c == 0xC3;
              break;
            default:
              break;
            }
          p += size;
          continue;
        }

      /* A variable-length group: code, subgroup, length, the data, the
       * length again, the subgroup and the code. */
      {
        guint sub, size;
        const guint8 *d;

        if ((gsize) (end - p) < 8)
          {
            p++;
            continue;
          }
        sub = p[1];
        size = rd16 (p + 2) + 4u;
        if (size < 8 || size > (gsize) (end - p) || p[size - 1] != c || p[size - 2] != sub)
          {
            p++;
            continue;
          }
        d = p + 4;
        if (c == 0xD0 && r->page != NULL && !r->any_text)
          {
            if (sub == 0x01 && size >= 16)
              {
                r->page->margin_left = WPU_TO_TWIPS (rd16 (d + 4));
                r->page->margin_right = WPU_TO_TWIPS (rd16 (d + 6));
              }
            else if (sub == 0x05 && size >= 16)
              {
                r->page->margin_top = WPU_TO_TWIPS (rd16 (d + 4));
                r->page->margin_bottom = WPU_TO_TWIPS (rd16 (d + 6));
              }
          }
        if (c == 0xD0 && sub == 0x06 && size >= 10)
          {
            static const W42Align ALIGNS[] = { W42_ALIGN_LEFT, W42_ALIGN_JUSTIFY, W42_ALIGN_CENTER,
                                               W42_ALIGN_RIGHT, W42_ALIGN_JUSTIFY };

            r->pa.align = ALIGNS[MIN (d[1], 4)];
          }
        else if (c == 0xD0 && sub == 0x02 && size >= 12)
          {
            guint v = rd16 (d + 2);
            int pct = (int) ((gint8) (v >> 8)) * 100 + (int) ((v & 0xFF) * 100 / 0xFF);

            r->pa.line_spacing_pct = pct > 0 && pct != 100 ? CLAMP (pct, 50, 500) : 0;
          }
        else if (c == 0xD1 && sub == 0x01 && size >= 36)
          {
            guint v = rd16 (d + 28);

            if (v > 0)
              r->size = CLAMP ((int) (v / 25), 2, 3276);
          }
        p += size;
      }
    }
}

/* ---------------------------------------------------------------------- */

gboolean
w42_wpd_load (W42PieceTable *pt, W42PageSetup *page, GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  Reader r;
  guint32 doc;

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return FALSE;

  memset (&r, 0, sizeof r);
  r.data = (const guint8 *) contents;
  r.len = length;
  r.budget = length;
  if (!w42_wpd_sniff (r.data, r.len))
    {
      g_free (contents);
      FAIL (error, _("This is not a WordPerfect document Word42 can read: only "
                     "WordPerfect 5 and 6 and later are."));
    }
  if (rd16 (r.data + 12) != 0)
    {
      g_free (contents);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   _("The document is protected with a password, and Word42 cannot "
                     "open protected WordPerfect documents."));
      return FALSE;
    }
  doc = rd32 (r.data + 4);
  if (doc < 16 || doc > r.len)
    {
      g_free (contents);
      FAIL (error, _("The WordPerfect document's text is out of reach."));
    }

  r.wp5 = r.data[10] == 0x00;
  r.pt = pt;
  r.page = page;
  r.family = g_intern_static_string ("Times New Roman");
  r.size = 24;
  r.col_widths = g_array_new (FALSE, FALSE, sizeof (int));
  r.pa.widow_control = 1;
  if (page != NULL)
    {
      /* WordPerfect's own page: Letter, an inch all round. */
      page->width = 12240;
      page->height = 15840;
      page->margin_left = page->margin_right = page->margin_top = page->margin_bottom = 1440;
      page->columns = 0;
    }

  w42_pt_load_text (pt, "");
  w42_builder_init (&r.b, pt);
  if (!r.wp5)
    {
      read_packets (&r);
      read_prefix (&r);
      /* The first paragraph's mark wears the document's first font, as
       * every later one wears what is in force where the one before
       * ended. */
      apply_char (&r);
      w42_pt_set_mark_char_fmt (pt, w42_pt_first_caret_pos (pt), &r.b.ch);
      parse_stream (&r, r.data + doc, r.data + r.len, SINK_BODY);
    }
  else
    parse_wp5 (&r, r.data + doc, r.data + r.len);

  if (r.in_table)
    table_off (&r);
  if (r.para_open)
    end_paragraph (&r, SINK_BODY);
  w42_builder_finish (&r.b);
  w42_pt_clear_undo (pt);

  if (r.packets != NULL)
    g_array_free (r.packets, TRUE);
  g_array_free (r.col_widths, TRUE);
  g_free (contents);
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Writing WordPerfect 6                                                   */
/* ---------------------------------------------------------------------- */

static void put8 (GByteArray *o, guint v) { guint8 b = (guint8) v; g_byte_array_append (o, &b, 1); }
static void put16 (GByteArray *o, guint v) { put8 (o, v & 0xFF); put8 (o, (v >> 8) & 0xFF); }
static void put32 (GByteArray *o, guint32 v) { put16 (o, v & 0xFFFF); put16 (o, v >> 16); }

static void
set32 (GByteArray *o, gsize at, guint32 v)
{
  for (int i = 0; i < 4; i++)
    o->data[at + i] = (guint8) (v >> (8 * i));
}

typedef struct {
  guint8      type;
  guint8      flags;
  GByteArray *data;
} OutPacket;

typedef struct {
  W42PieceTable *pt;
  const W42PageSetup *page;
  W42ApTable   *aps;
  GArray       *packets;       /* OutPacket; the prefix ID is the place + 1 */
  GHashTable   *font_pid;      /* family -> PID */
  GHashTable   *reverse;       /* Unicode -> (set << 8 | index) + 1 */

  /* What the stream has in force. */
  gboolean      attr[ATTR_N];
  const char   *family;
  int           size;
  guint32       color;
  W42ParaFmt    pa;
  GPtrArray    *all;           /* every paragraph, notes' too */

  /* Each list level's count so far, and the kind it counts in. */
  int           level_n[9];
  W42ListKind   level_kind[9];
} Writer;

static void
out_packet_free (gpointer data)
{
  g_byte_array_free (((OutPacket *) data)->data, TRUE);
}

static guint
add_packet (Writer *w, guint8 type, guint8 flags, GByteArray *data)
{
  OutPacket p = { type, flags, data };

  g_array_append_val (w->packets, p);
  return w->packets->len;
}

/* A variable-length group: code, subgroup, its size, flags, the prefix
 * IDs it names, the size of its data, the data, the size again, the
 * code again. */
static void
group (GByteArray *o, guint code, guint sub, const guint16 *pids, guint n_pids,
       const guint8 *data, guint n, guint nondel)
{
  guint size = 5 + (n_pids > 0 ? 1 + 2 * n_pids : 0) + 2 + n + 3;

  put8 (o, code);
  put8 (o, sub);
  put16 (o, size);
  put8 (o, n_pids > 0 ? 0x80 : 0);
  if (n_pids > 0)
    {
      put8 (o, n_pids);
      for (guint i = 0; i < n_pids; i++)
        put16 (o, pids[i]);
    }
  put16 (o, nondel);
  if (n > 0)
    g_byte_array_append (o, data, n);
  put16 (o, size);
  put8 (o, code);
}

static void
group16 (GByteArray *o, guint code, guint sub, guint value)
{
  guint8 d[2] = { (guint8) value, (guint8) (value >> 8) };

  group (o, code, sub, NULL, 0, d, 2, 2);
}

static void
fixed (GByteArray *o, guint code, guint a)
{
  put8 (o, code);
  put8 (o, a);
  put8 (o, code);
}

/* WordPerfect's number for each character Unicode has that one of its
 * sets does: the first set that has it, the commonest sets first. */
static GHashTable *
reverse_map (void)
{
  static const guint ORDER[] = { 1, 4, 6, 8, 10, 2, 3, 5, 7, 9, 11, 13, 14 };
  GHashTable *map = g_hash_table_new (NULL, NULL);

  for (guint k = 0; k < G_N_ELEMENTS (ORDER); k++)
    {
      guint set = ORDER[k];
      const gunichar *t = WPD_WP6_SETS[set].map;

      for (guint i = 0; t != NULL && i < WPD_WP6_SETS[set].n && i < 256; i++)
        if (t[i] != 0 && t[i] != 0x20 && !g_hash_table_contains (map, GUINT_TO_POINTER (t[i])))
          g_hash_table_insert (map, GUINT_TO_POINTER ((guint) t[i]),
                               GUINT_TO_POINTER (((set << 8) | i) + 1));
    }
  return map;
}

/* A character as the stream has it: ASCII as itself, the space as 0x80,
 * the rest by set and index; '?' for what no set has. */
static void
put_uchar (Writer *w, GByteArray *o, gunichar c)
{
  gpointer v;

  if (c >= 0x21 && c <= 0x7E)
    put8 (o, c);
  else if (c == ' ')
    put8 (o, 0x80);
  else if (c == 0x00A0)
    put8 (o, 0x81);
  else if (c == 0x00AD)
    put8 (o, 0x82);
  else if (c == 0x2011)
    put8 (o, 0x84);
  else if ((v = g_hash_table_lookup (w->reverse, GUINT_TO_POINTER (c))) != NULL)
    {
      guint code = GPOINTER_TO_UINT (v) - 1;

      put8 (o, 0xF0);
      put8 (o, code & 0xFF);
      put8 (o, code >> 8);
      put8 (o, 0xF0);
    }
  else if (c >= 0x20)
    put8 (o, '?');
}

/* A font descriptor packet: WordPerfect's metrics left empty, and the
 * name. */
static guint
font_pid (Writer *w, const char *family)
{
  gpointer pid;
  GByteArray *d;
  const char *name = family != NULL && *family != '\0' ? family : "Times New Roman";

  name = g_intern_string (name);
  if ((pid = g_hash_table_lookup (w->font_pid, name)) != NULL)
    return GPOINTER_TO_UINT (pid);
  d = g_byte_array_new ();
  for (int i = 0; i < 22; i++)
    put8 (d, 0);
  {
    glong n16 = 0;
    gunichar2 *u = g_utf8_to_utf16 (name, -1, NULL, &n16, NULL);

    put16 (d, (guint) (n16 + 1) * 2);
    for (glong i = 0; i < n16; i++)
      {
        /* Charset 0, ASCII, as font names are kept; others by set. */
        gpointer v = u[i] >= 0x20 && u[i] < 0x7F ? NULL
                   : g_hash_table_lookup (w->reverse, GUINT_TO_POINTER ((guint) u[i]));

        if (v != NULL)
          {
            /* A character word: the index, then the set. */
            put8 (d, (GPOINTER_TO_UINT (v) - 1) & 0xFF);
            put8 (d, (GPOINTER_TO_UINT (v) - 1) >> 8);
          }
        else
          {
            put8 (d, u[i] < 0x7F ? u[i] : '?');
            put8 (d, 0);
          }
      }
    put16 (d, 0);
    g_free (u);
  }
  pid = GUINT_TO_POINTER (add_packet (w, 0x55, 0x00, d));
  g_hash_table_insert (w->font_pid, (gpointer) name, pid);
  return GPOINTER_TO_UINT (pid);
}

/* WordPerfect's attributes from the model's formatting. */
static void
char_attrs (const W42CharFmt *ch, gboolean attr[ATTR_N])
{
  memset (attr, 0, sizeof (gboolean) * ATTR_N);
  attr[ATTR_BOLD] = ch->bold;
  attr[ATTR_ITALIC] = ch->italic;
  attr[ATTR_UNDERLINE] = ch->underline != W42_UNDERLINE_NONE && ch->underline != W42_UNDERLINE_DOUBLE;
  attr[ATTR_DOUBLE_UNDERLINE] = ch->underline == W42_UNDERLINE_DOUBLE;
  attr[ATTR_STRIKEOUT] = ch->strikeout || ch->dstrike || ch->revision == 2;
  attr[ATTR_OUTLINE] = ch->outline;
  attr[ATTR_SHADOW] = ch->shadow;
  attr[ATTR_SMALL_CAPS] = ch->smallcaps != 0;
  attr[ATTR_SUPERSCRIPT] = ch->script > 0;
  attr[ATTR_SUBSCRIPT] = ch->script < 0;
  attr[ATTR_REDLINE] = ch->revision == 1;
}

/* The codes that change what is in force to `ch`: attributes off and
 * on, the font, its size, its colour. */
static void
set_char (Writer *w, GByteArray *o, const W42CharFmt *ch)
{
  gboolean want[ATTR_N];
  int size = ch->size > 0 ? ch->size : 24;

  char_attrs (ch, want);
  for (int a = 0; a < ATTR_N; a++)
    if (w->attr[a] && !want[a])
      {
        fixed (o, 0xF3, a);
        w->attr[a] = FALSE;
      }
  if (g_strcmp0 (ch->family, w->family) != 0)
    {
      guint16 pid = (guint16) font_pid (w, ch->family);
      guint8 d[8] = { 0 };
      guint wsize = (guint) size * 25;

      d[0] = (guint8) ((guint) w->size * 25);
      d[1] = (guint8) (((guint) w->size * 25) >> 8);
      d[6] = (guint8) wsize;
      d[7] = (guint8) (wsize >> 8);
      group (o, 0xD4, 0x1A, &pid, 1, d, 8, 8);
      w->family = ch->family;
      w->size = size;
    }
  else if (size != w->size)
    {
      guint16 pid = (guint16) font_pid (w, ch->family);
      guint8 d[2] = { (guint8) ((guint) size * 25), (guint8) (((guint) size * 25) >> 8) };

      group (o, 0xD4, 0x1B, &pid, 1, d, 2, 2);
      w->size = size;
    }
  if ((ch->color & 0xFFFFFF) != w->color)
    {
      guint8 d[4] = { (guint8) (ch->color >> 16), (guint8) (ch->color >> 8), (guint8) ch->color, 100 };

      group (o, 0xD4, 0x18, NULL, 0, d, 4, 4);
      w->color = ch->color & 0xFFFFFF;
    }
  for (int a = 0; a < ATTR_N; a++)
    if (!w->attr[a] && want[a])
      {
        fixed (o, 0xF2, a);
        w->attr[a] = TRUE;
      }
}

static void
put_proportion (guint8 *d, int pct)
{
  guint32 v = ((guint32) (pct / 100) << 16) | (guint32) ((pct % 100) * 0xFFFF / 100);

  d[0] = (guint8) v;
  d[1] = (guint8) (v >> 8);
  d[2] = (guint8) (v >> 16);
  d[3] = (guint8) (v >> 24);
}

/* The paragraph codes that differ from what is in force, before the
 * paragraph's text: justification, spacing, indents, tab stops. */
static void
set_para (Writer *w, GByteArray *o, const W42ParaFmt *pa)
{
  if (pa->align != w->pa.align)
    {
      static const guint8 JUST[] = { 0, 2, 3, 1 };   /* left, centre, right, full */
      guint8 j = JUST[pa->align & 3];

      group (o, 0xD3, 0x05, NULL, 0, &j, 1, 1);
    }
  if (pa->line_spacing_pct != w->pa.line_spacing_pct || pa->line_spacing != w->pa.line_spacing)
    {
      guint8 d[4];

      put_proportion (d, pa->line_spacing_pct > 0 ? pa->line_spacing_pct : 100);
      group (o, 0xD3, 0x01, NULL, 0, d, 4, 4);
    }
  if (pa->space_after != w->pa.space_after)
    {
      /* A proportion of 1, nothing, and the space as an amount. */
      guint8 d[6];
      guint wpu = TWIPS_TO_WPU (MAX (pa->space_after, 0));

      put_proportion (d, 100);
      d[4] = (guint8) wpu;
      d[5] = (guint8) (wpu >> 8);
      group (o, 0xD3, 0x0A, NULL, 0, d, 6, 6);
    }
  if (pa->indent_first != w->pa.indent_first)
    group16 (o, 0xD3, 0x0B, TWIPS_TO_WPU_SIGNED (pa->indent_first));
  if (pa->indent_left != w->pa.indent_left)
    group16 (o, 0xD3, 0x0C, TWIPS_TO_WPU_SIGNED (pa->indent_left));
  if (pa->indent_right != w->pa.indent_right)
    group16 (o, 0xD3, 0x0D, TWIPS_TO_WPU_SIGNED (pa->indent_right));
  if (pa->n_tabs != w->pa.n_tabs ||
      memcmp (pa->tab_pos, w->pa.tab_pos, sizeof pa->tab_pos[0] * pa->n_tabs) != 0 ||
      memcmp (pa->tab_kind, w->pa.tab_kind, pa->n_tabs) != 0)
    {
      /* Stops relative to the margin. */
      GByteArray *d = g_byte_array_new ();

      put8 (d, 1);
      put16 (d, 0);
      put8 (d, pa->n_tabs);
      for (guint i = 0; i < pa->n_tabs; i++)
        {
          static const guint8 ALIGN[] = { 0, 1, 2, 3 };
          static const guint8 LEADER[] = { 0, 0x10 | (1 << 5), 0x10 | (2 << 5), 0x10 | (3 << 5) };

          put8 (d, ALIGN[W42_TAB_KIND (pa->tab_kind[i]) & 3] | LEADER[W42_TAB_LEADER (pa->tab_kind[i]) & 3]);
          put16 (d, TWIPS_TO_WPU (MAX (pa->tab_pos[i], 0)));
        }
      group (o, 0xD3, 0x04, NULL, 0, d->data, d->len, d->len);
      g_byte_array_free (d, TRUE);
    }
  w->pa = *pa;
}

/* A stream of paragraphs into `o`: their text, their formatting, the
 * fields and notes in them.  Headers and notes are streams of their own
 * in text packets. */
static void write_blocks (Writer *w, GByteArray *o, GPtrArray *blocks, int note);

/* A general text packet holding a stream. */
static guint
text_packet (Writer *w, GByteArray *stream)
{
  GByteArray *d = g_byte_array_new ();

  put16 (d, 1);
  put32 (d, stream->len);
  put32 (d, stream->len);
  g_byte_array_append (d, stream->data, stream->len);
  return add_packet (w, 0x08, 0x08, d);
}

/* A displayed number with its cached digits: the page's, or the count. */
static void
put_number_field (GByteArray *o, guint on, const char *digits)
{
  guint8 level = 0;

  group (o, 0xDA, on, NULL, 0, &level, 1, 1);
  g_byte_array_append (o, (const guint8 *) digits, (guint) strlen (digits));
  group (o, 0xDA, on + 1, NULL, 0, NULL, 0, 0);
}

/* A header's or footer's one line, as a stream: its alignment, its
 * words, its fields. */
static guint
page_text_packet (Writer *w, const W42PageText *text)
{
  GByteArray *o = g_byte_array_new ();
  static const guint8 JUST[] = { 0, 2, 3, 1 };
  guint8 j = JUST[text->align & 3];
  guint pid;

  group (o, 0xD3, 0x05, NULL, 0, &j, 1, 1);
  for (const char *p = text->text; *p != '\0'; p = g_utf8_next_char (p))
    {
      if (g_str_has_prefix (p, "{PAGE}"))
        {
          put_number_field (o, 0x04, "1");
          p += 5;
          continue;
        }
      if (g_str_has_prefix (p, "{NUMPAGES}"))
        {
          put_number_field (o, 0x14, "1");
          p += 9;
          continue;
        }
      if (g_str_has_prefix (p, "{DATE}"))
        {
          GDateTime *now = g_date_time_new_now_local ();
          char *date = g_date_time_format (now, "%x");

          for (const char *q = date; *q != '\0'; q = g_utf8_next_char (q))
            put_uchar (w, o, g_utf8_get_char (q));
          g_free (date);
          g_date_time_unref (now);
          p += 5;
          continue;
        }
      put_uchar (w, o, g_utf8_get_char (p));
    }
  put8 (o, 0xCC);
  pid = text_packet (w, o);
  g_byte_array_free (o, TRUE);
  return pid;
}

/* A comment's words as a text packet, and the annotation packet that
 * names it. */
static guint
comment_packet (Writer *w, const char *text)
{
  GByteArray *o = g_byte_array_new ();
  GByteArray *d = g_byte_array_new ();
  guint tpid;

  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == '\n')
        put8 (o, 0xCC);
      else
        put_uchar (w, o, c);
    }
  put8 (o, 0xCC);
  tpid = text_packet (w, o);
  g_byte_array_free (o, TRUE);
  put16 (d, 1);
  put16 (d, tpid);
  put8 (d, 0);
  return add_packet (w, 0x1B, 0x08, d);
}

/* The cell of the table in the text that block `i` is in: WordPerfect
 * has no tables in cells, and the paragraphs of one are written as the
 * paragraphs of the cell it is in. */
static W42BlockCell
outer_cell (GPtrArray *blocks, guint i)
{
  W42BlockCell c = { -1, 0, 0, 1, 0 };

  w42_block_cell (g_ptr_array_index (blocks, i), 0, &c);
  return c;
}

/* A table's rows: the definition first -- where it sits and each
 * column's width -- then each row and cell with its span, its fill and
 * the row's height, the cells' paragraphs in them, and the end. */
static guint
write_table (Writer *w, GByteArray *o, GPtrArray *blocks, guint first)
{
  int table = outer_cell (blocks, first).table;
  const W42TableProps *props = w42_pt_table_props (w->pt, table);
  int n_cols = props != NULL ? MAX (props->n_cols, 1) : 1;
  int text_w = (w->page != NULL ? w->page->width - w->page->margin_left - w->page->margin_right : 9360);
  guint i = first, end = first;
  int rows = w42_pt_table_rows (w->pt, table);

  while (end < blocks->len && outer_cell (blocks, end).table == table)
    end++;

  {
    guint8 d[4] = { 0, 0, 0, 0 };
    guint left = TWIPS_TO_WPU (w->page != NULL ? w->page->margin_left : 1440);

    d[2] = (guint8) left;
    d[3] = (guint8) (left >> 8);
    group (o, 0xD4, 0x2A, NULL, 0, d, 4, 4);
  }
  for (int c = 0; c < n_cols; c++)
    {
      guint8 d[17] = { 0 };
      int cw = props != NULL && props->widths != NULL && (guint) c < props->widths->len
               ? g_array_index (props->widths, int, c) : 0;
      guint wpu = TWIPS_TO_WPU (cw > 0 ? cw : text_w / n_cols);

      d[1] = (guint8) wpu;
      d[2] = (guint8) (wpu >> 8);
      group (o, 0xD4, 0x2C, NULL, 0, d, 17, 17);
    }
  group (o, 0xD4, 0x2B, NULL, 0, NULL, 0, 0);

  while (i < end)
    {
      const W42Block *block = g_ptr_array_index (blocks, i);
      W42BlockCell here = outer_cell (blocks, i);
      W42BlockCell prev = i > first ? outer_cell (blocks, i - 1) : (W42BlockCell) { -1, -1, -1, 1, 0 };
      gboolean cell_start = i == first || prev.row != here.row || prev.col != here.col;

      if (cell_start)
        {
          /* A row's first cell starts the row; either way the cell says
           * its span, whether it is covered by the one above, and its
           * fill. */
          const W42ParaFmt *cell = &w42_ap_table_get (w->aps, here.cell_ap)->pa;
          gboolean row_start = i == first || prev.row != here.row;
          GByteArray *d = g_byte_array_new ();
          int height = w42_pt_table_get_row_height (w->pt, table, here.row);

          put16 (d, 0);
          if (row_start)
            {
              guint flags = (props != NULL && here.row < props->header_rows ? 0x04 : 0) |
                            (height > 0 ? 0x12 : 0);
              guint wpu = TWIPS_TO_WPU (height);

              put8 (d, 0x80); put8 (d, flags); put16 (d, wpu); put8 (d, 0x80);
            }
          put8 (d, 0x85);
          put8 (d, cell->cell_vspan == W42_CELL_COVERED ? 0x81 : MAX (here.span, 1));
          put8 (d, cell->cell_vspan > 1 && cell->cell_vspan != W42_CELL_COVERED ? cell->cell_vspan : 1);
          put8 (d, 0x85);
          if (cell->has_shading_color)
            {
              /* A solid fill: the colour in front, wholly, over white. */
              put8 (d, 0x86);
              put8 (d, cell->shading_color >> 16);
              put8 (d, cell->shading_color >> 8);
              put8 (d, cell->shading_color);
              put8 (d, 100);
              put8 (d, 0xFF); put8 (d, 0xFF); put8 (d, 0xFF); put8 (d, 100);
              put8 (d, 0x86);
            }
          if (cell->cell_valign != W42_CELL_VALIGN_TOP)
            {
              put8 (d, 0x84);
              put8 (d, 0); put8 (d, 0);
              put8 (d, cell->cell_valign == W42_CELL_VALIGN_CENTER ? 1 : 2);
              put16 (d, 0); put16 (d, 0);
              put8 (d, 0x84);
            }
          group (o, 0xD0, row_start ? 0x0B : 0x0A, NULL, 0, d->data, d->len, d->len);
          g_byte_array_free (d, TRUE);
        }
      else
        put8 (o, 0xCC);                    /* the cell's next paragraph */

      {
        GPtrArray *one = g_ptr_array_new ();

        g_ptr_array_add (one, (gpointer) block);
        write_blocks (w, o, one, -2);
        g_ptr_array_free (one, TRUE);
      }
      i++;
    }
  {
    guint8 d[2] = { 0, 0 };

    group (o, 0xD0, 0x11, NULL, 0, d, 2, 2);
  }
  (void) rows;
  return end;
}

/* The runs of one paragraph: `note` -2 writes no paragraph mark, for a
 * table's cell, whose next code ends it. */
static void
write_runs (Writer *w, GByteArray *o, const W42Block *block)
{
  GPtrArray *all = w->all;
  const char *open_comment = NULL;
  const char *field = NULL;

  for (guint r = 0; r < block->runs->len; r++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, r);
      const W42Fmt *rf = w42_ap_table_get (w->aps, run->ap);
      const char *text = block->text->str + run->byte_offset;

      set_char (w, o, &rf->ch);

      if (rf->ch.comment != NULL && rf->ch.comment != open_comment)
        {
          guint16 pid = (guint16) comment_packet (w, rf->ch.comment);

          group (o, 0xD4, 0x1D, &pid, 1, NULL, 0, 0);
        }
      open_comment = rf->ch.comment;

      if (run->footnote > 0)
        {
          /* A note: its mark, a text packet with its paragraphs, and its
           * number, which WordPerfect keeps up to date itself. */
          GByteArray *note = g_byte_array_new ();
          GPtrArray *mine = g_ptr_array_new ();
          Writer saved = *w;
          guint16 pid;
          char digits[16];

          for (guint k = 0; k < all->len; k++)
            if (((const W42Block *) g_ptr_array_index (all, k))->note == run->footnote_id)
              g_ptr_array_add (mine, g_ptr_array_index (all, k));
          memset (w->attr, 0, sizeof w->attr);
          w->family = NULL;
          w->pa = (W42ParaFmt) { 0 };
          /* The note's lists count on their own, and the list the note
           * is in counts on after it. */
          memset (w->level_n, 0, sizeof w->level_n);
          memset (w->level_kind, 0, sizeof w->level_kind);
          write_blocks (w, note, mine, run->footnote_id);
          w->family = saved.family;
          w->size = saved.size;
          w->color = saved.color;
          memcpy (w->attr, saved.attr, sizeof w->attr);
          w->pa = saved.pa;
          memcpy (w->level_n, saved.level_n, sizeof w->level_n);
          memcpy (w->level_kind, saved.level_kind, sizeof w->level_kind);
          pid = (guint16) text_packet (w, note);
          g_byte_array_free (note, TRUE);
          g_ptr_array_free (mine, TRUE);

          group (o, 0xD7, run->endnote ? 0x02 : 0x00, &pid, 1, NULL, 0, 0);
          g_snprintf (digits, sizeof digits, "%d", run->footnote);
          put_number_field (o, run->endnote ? 0x10 : 0x0E, digits);
          group (o, 0xD7, run->endnote ? 0x03 : 0x01, NULL, 0, NULL, 0, 0);
          continue;
        }
      if (run->object != W42_OBJECT_NONE)
        {
          /* Pictures are not written; an equation is, as it reads. */
          const W42Object *object = w42_object_table_get (w42_pt_object_table (w->pt), run->object);
          W42MathNode *root = object != NULL && object->mathml != NULL
                                ? w42_math_parse (object->mathml, -1, NULL) : NULL;

          if (root != NULL)
            {
              char *said = w42_mathml_to_text (root);

              for (const char *p = said; *p != '\0'; p = g_utf8_next_char (p))
                put_uchar (w, o, g_utf8_get_char (p));
              g_free (said);
              w42_math_node_free (root);
            }
          continue;
        }

      if (rf->ch.field != NULL && (g_str_equal (rf->ch.field, "PAGE") || g_str_equal (rf->ch.field, "NUMPAGES")))
        {
          if (field != rf->ch.field)
            put_number_field (o, g_str_equal (rf->ch.field, "PAGE") ? 0x04 : 0x14, "1");
          field = rf->ch.field;
          continue;
        }
      field = NULL;

      for (const char *p = text; p < text + run->n_bytes; p = g_utf8_next_char (p))
        {
          gunichar c = g_utf8_get_char (p);

          if (c == '\t')
            {
              guint8 d[2] = { 0, 0 };

              group (o, 0xE0, 0x10, NULL, 0, d, 2, 2);
            }
          else if (c == 0x2028)
            {
              guint8 d[3] = { 0, 0, 0x8D };

              group (o, 0xD0, 0x14, NULL, 0, d, 3, 3);
            }
          else if (c != 0xFFFC)
            put_uchar (w, o, c);
        }
    }
}

static void
write_blocks (Writer *w, GByteArray *o, GPtrArray *blocks, int note)
{
  for (guint i = 0; i < blocks->len; )
    {
      const W42Block *block = g_ptr_array_index (blocks, i);
      const W42Fmt *fmt = w42_ap_table_get (w->aps, block->ap);
      W42ParaFmt pa = fmt->pa;

      if (note == -1 && block->note >= 0)
        {
          i++;
          continue;
        }
      if (note == -1 && block->table >= 0)
        {
          i = write_table (w, o, blocks, i);
          continue;
        }

      if (note == -1 && pa.section_break && i > 0)
        {
          /* A new section's columns, on a new page. */
          GByteArray *d = g_byte_array_new ();
          int cols = pa.columns > 1 ? MIN (pa.columns, 6) : 1;
          int text_w = w->page != NULL ? w->page->width - w->page->margin_left - w->page->margin_right : 9360;
          /* No wider than the text: the model's gap is whatever the file
           * it came from said, and times the columns it would overflow. */
          int gap = pa.column_gap > 0 ? MIN (pa.column_gap, MAX (text_w, 0)) : 720;

          put8 (d, 0);
          put32 (d, 0x00010000);
          put8 (d, cols);
          for (int k = 0; cols > 1 && k < 2 * cols - 1; k++)
            {
              put8 (d, 1);
              put16 (d, TWIPS_TO_WPU (k % 2 ? gap : (text_w - gap * (cols - 1)) / cols));
            }
          group (o, 0xD2, 0x02, NULL, 0, d->data, d->len, d->len);
          g_byte_array_free (d, TRUE);
        }

      /* A list item's marker, as the text it shows: WordPerfect's lists
       * are outlines of its own. */
      set_para (w, o, &pa);
      if (pa.list == W42_LIST_NONE)
        {
          memset (w->level_n, 0, sizeof w->level_n);
          memset (w->level_kind, 0, sizeof w->level_kind);
        }
      else if (pa.list < W42_LIST_KINDS)
        {
          char marker[16];
          int n = 1;

          /* A numbered item counts on from the one before it at its
           * level, as the RTF writer counts: from its own start, or from
           * one when the kind changes; the levels inside it start again. */
          if (w42_list_is_numbered (pa.list))
            {
              int lv = MIN (pa.list_level, 8);

              if (pa.list_start > 0)
                w->level_n[lv] = pa.list_start;
              else if (pa.list != w->level_kind[lv])
                w->level_n[lv] = 1;
              else
                w->level_n[lv]++;
              w->level_kind[lv] = pa.list;
              for (int deeper = lv + 1; deeper < 9; deeper++)
                {
                  w->level_n[deeper] = 0;
                  w->level_kind[deeper] = W42_LIST_NONE;
                }
              n = w->level_n[lv];
            }
          w42_list_marker ((W42ListKind) pa.list, n, marker, sizeof marker);
          set_char (w, o, &fmt->ch);
          for (const char *p = marker; *p != '\0'; p = g_utf8_next_char (p))
            put_uchar (w, o, g_utf8_get_char (p));
          {
            guint8 d[2] = { 0, 0 };

            group (o, 0xE0, 0x10, NULL, 0, d, 2, 2);
          }
        }
      write_runs (w, o, block);

      if (note != -2)
        {
          /* The paragraph's end: a hard page when the next one starts a
           * page, else a hard return. */
          const W42Block *next = NULL;

          for (guint k = i + 1; k < blocks->len && next == NULL; k++)
            if (((const W42Block *) g_ptr_array_index (blocks, k))->note == block->note)
              next = g_ptr_array_index (blocks, k);
          if (note == -1 && next != NULL && next->table < 0 &&
              (w42_ap_table_get (w->aps, next->ap)->pa.page_break_before ||
               w42_ap_table_get (w->aps, next->ap)->pa.section_break))
            put8 (o, 0xC7);
          else
            put8 (o, 0xCC);
        }
      i++;
    }
}

/* The document summary: records of a tag, its name and its value. */
static GByteArray *
summary_packet (Writer *w)
{
  const W42DocInfo *info = w42_pt_get_info (w->pt);
  static const struct { guint tag; const char *name; gsize offset; } FIELDS[] = {
    { 17, "Descriptive Name", G_STRUCT_OFFSET (W42DocInfo, title) },
    { 46, "Subject",          G_STRUCT_OFFSET (W42DocInfo, subject) },
    { 5,  "Author",           G_STRUCT_OFFSET (W42DocInfo, author) },
    { 26, "Keywords",         G_STRUCT_OFFSET (W42DocInfo, keywords) },
    { 1,  "Abstract",         G_STRUCT_OFFSET (W42DocInfo, comments) },
  };
  GByteArray *d = NULL;

  for (guint i = 0; info != NULL && i < G_N_ELEMENTS (FIELDS); i++)
    {
      const char *value = G_STRUCT_MEMBER (const char *, info, FIELDS[i].offset);
      GByteArray *rec;

      if (value == NULL || *value == '\0')
        continue;
      if (d == NULL)
        d = g_byte_array_new ();
      rec = g_byte_array_new ();
      put16 (rec, 0);
      put16 (rec, FIELDS[i].tag);
      put16 (rec, 0);
      for (const char *p = FIELDS[i].name; *p != '\0'; p++)
        {
          put8 (rec, *p);
          put8 (rec, 0);
        }
      put16 (rec, 0);
      {
        GByteArray *chars = g_byte_array_new ();

        for (const char *p = value; *p != '\0'; p = g_utf8_next_char (p))
          {
            gunichar c = g_utf8_get_char (p);
            gpointer v;

            if (c >= 0x20 && c < 0x7F)
              {
                put8 (chars, c);
                put8 (chars, 0);
              }
            else if ((v = g_hash_table_lookup (w->reverse, GUINT_TO_POINTER (c))) != NULL)
              {
                put8 (chars, (GPOINTER_TO_UINT (v) - 1) & 0xFF);
                put8 (chars, (GPOINTER_TO_UINT (v) - 1) >> 8);
              }
          }
        g_byte_array_append (rec, chars->data, chars->len);
        g_byte_array_free (chars, TRUE);
      }
      put16 (rec, 0);
      rec->data[0] = (guint8) rec->len;
      rec->data[1] = (guint8) (rec->len >> 8);
      g_byte_array_append (d, rec->data, rec->len);
      g_byte_array_free (rec, TRUE);
    }
  if (d != NULL)
    put16 (d, 0);
  return d;
}

gboolean
w42_wpd_save (W42PieceTable *pt, const W42PageSetup *page, GFile *file, GError **error)
{
  Writer w;
  GByteArray *body = g_byte_array_new ();
  GByteArray *out;
  GPtrArray *blocks;
  gboolean ok;
  const W42Style *normal;

  memset (&w, 0, sizeof w);
  w.pt = pt;
  w.page = page;
  w.aps = w42_pt_ap_table (pt);
  w.packets = g_array_new (FALSE, FALSE, sizeof (OutPacket));
  g_array_set_clear_func (w.packets, out_packet_free);
  w.font_pid = g_hash_table_new (NULL, NULL);
  w.reverse = reverse_map ();
  w.pa.widow_control = 1;

  /* The document's first font, which WordPerfect starts in. */
  normal = w42_stylesheet_find (w42_pt_stylesheet (pt), "Normal");
  w.family = g_intern_string (normal != NULL && normal->ch.family != NULL ? normal->ch.family : "Times New Roman");
  w.size = normal != NULL && normal->ch.size > 0 ? normal->ch.size : 24;
  {
    GByteArray *d = g_byte_array_new ();

    put16 (d, 1);
    put16 (d, font_pid (&w, w.family));
    put16 (d, (guint) w.size * 25);
    add_packet (&w, 0x25, 0x00, d);
  }
  {
    GByteArray *summary = summary_packet (&w);

    if (summary != NULL)
      add_packet (&w, 0x12, 0x08, summary);
  }

  /* The page, before anything on it: its margins, its paper when it is
   * not Letter, its columns, where its numbers start. */
  if (page != NULL)
    {
      group16 (body, 0xD1, 0x00, TWIPS_TO_WPU (page->margin_top));
      group16 (body, 0xD1, 0x01, TWIPS_TO_WPU (page->margin_bottom));
      group16 (body, 0xD2, 0x00, TWIPS_TO_WPU (page->margin_left));
      group16 (body, 0xD2, 0x01, TWIPS_TO_WPU (page->margin_right));
      if (page->width != 12240 || page->height != 15840)
        {
          guint8 d[9] = { 0 };
          guint length = TWIPS_TO_WPU (MAX (page->width, page->height));
          guint width = TWIPS_TO_WPU (MIN (page->width, page->height));

          d[3] = (guint8) length; d[4] = (guint8) (length >> 8);
          d[5] = (guint8) width;  d[6] = (guint8) (width >> 8);
          d[8] = page->width > page->height ? 1 : 0;
          group (body, 0xD1, 0x11, NULL, 0, d, 9, 9);
        }
      if (w42_page_columns (page) > 1)
        {
          GByteArray *d = g_byte_array_new ();
          int cols = w42_page_columns (page), gap = w42_page_column_gap (page);
          int text_w = page->width - page->margin_left - page->margin_right;

          put8 (d, 0);
          put32 (d, 0x00010000);
          put8 (d, cols);
          for (int k = 0; k < 2 * cols - 1; k++)
            {
              put8 (d, 1);
              put16 (d, TWIPS_TO_WPU (k % 2 ? gap : (text_w - gap * (cols - 1)) / cols));
            }
          group (body, 0xD2, 0x02, NULL, 0, d->data, d->len, d->len);
          g_byte_array_free (d, TRUE);
        }
    }
  {
    int from, start;

    w42_pt_get_page_numbering (pt, &from, &start);
    if (from > 1 || start != 1)
      {
        guint n = (guint) MAX (start - (from - 1), 1);
        guint8 d[4] = { 1, 0, (guint8) n, (guint8) (n >> 8) };

        group (body, 0xD8, 0x02, NULL, 0, d, 4, 4);
      }
  }

  /* Headers and footers: A on every page, or on odd ones with B on the
   * even ones when the document has facing pages. */
  {
    gboolean facing = w42_pt_get_facing_pages (pt);
    const W42PageText *texts[4] = {
      w42_pt_get_header (pt), w42_pt_get_footer (pt),
      facing ? w42_pt_get_header_kind (pt, W42_PAGE_TEXT_EVEN) : NULL,
      facing ? w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_EVEN) : NULL,
    };
    static const guint SUB[4] = { 0, 2, 1, 3 };

    for (guint k = 0; k < 4; k++)
      if (texts[k] != NULL && texts[k]->text != NULL && *texts[k]->text != '\0')
        {
          guint16 pid = (guint16) page_text_packet (&w, texts[k]);
          guint8 occurs = k >= 2 ? 0x02 : facing ? 0x01 : 0x03;

          group (body, 0xD6, SUB[k], &pid, 1, &occurs, 1, 1);
        }
    if (w42_pt_get_title_page (pt))
      {
        /* A title page without its own: nothing at its head or foot. */
        const W42PageText *h = w42_pt_get_header_kind (pt, W42_PAGE_TEXT_FIRST);
        const W42PageText *f = w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_FIRST);
        guint8 code = 0;

        if (h == NULL || h->text == NULL || *h->text == '\0')
          code |= 4 | 8;
        if (f == NULL || f->text == NULL || *f->text == '\0')
          code |= 16 | 32;
        if (code != 0)
          group (body, 0xD1, 0x02, NULL, 0, &code, 1, 1);
      }
  }

  blocks = w42_pt_snapshot_blocks (pt);
  w.all = blocks;
  write_blocks (&w, body, blocks, -1);
  g_ptr_array_free (blocks, TRUE);

  /* The file: the header, the index at 0x200 as WordPerfect puts it, the
   * packets, and the document to the end. */
  out = g_byte_array_new ();
  {
    static const guint8 HEAD[16] = { 0xFF, 'W', 'P', 'C', 0, 0, 0, 0, 1, 0x0A, 2, 1, 0, 0, 0x00, 0x02 };
    guint n = w.packets->len;
    gsize at;

    g_byte_array_append (out, HEAD, 16);
    put32 (out, 5);
    put32 (out, 0);                        /* the file's size, below */
    put16 (out, 0);
    put16 (out, 0x0200);
    while (out->len < 0x200)
      put8 (out, 0);

    put8 (out, 2);
    put8 (out, 0);
    put16 (out, n + 1);
    for (int k = 0; k < 10; k++)
      put8 (out, 0);
    at = out->len + 14 * (gsize) n;
    for (guint i = 0; i < n; i++)
      {
        const OutPacket *p = &g_array_index (w.packets, OutPacket, i);

        put8 (out, p->flags);
        put8 (out, p->type);
        put16 (out, 1);
        put16 (out, 0);
        put32 (out, p->data->len);
        put32 (out, (guint32) at);
        at += p->data->len;
      }
    for (guint i = 0; i < n; i++)
      {
        const OutPacket *p = &g_array_index (w.packets, OutPacket, i);

        g_byte_array_append (out, p->data->data, p->data->len);
      }
    set32 (out, 4, out->len);
    g_byte_array_append (out, body->data, body->len);
    set32 (out, 20, out->len);
  }

  ok = g_file_replace_contents (file, (const char *) out->data, out->len, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);

  g_byte_array_free (out, TRUE);
  g_byte_array_free (body, TRUE);
  g_array_free (w.packets, TRUE);
  g_hash_table_destroy (w.font_pid);
  g_hash_table_destroy (w.reverse);
  return ok;
}
