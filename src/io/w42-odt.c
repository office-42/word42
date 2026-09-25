/* w42-odt.c - see w42-odt.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-odt.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "w42-build.h"
#include "w42-image.h"
#include "w42-lang.h"
#include "w42-zip.h"

#define ODT_MIME "application/vnd.oasis.opendocument.text"

/* ---------------------------------------------------------------------- */
/* Little helpers                                                          */
/* ---------------------------------------------------------------------- */

static const char *
local (const char *name)
{
  const char *colon = strchr (name, ':');

  return colon != NULL ? colon + 1 : name;
}

static const char *
attr (const char **names, const char **values, const char *want)
{
  for (int i = 0; names != NULL && names[i] != NULL; i++)
    if (g_str_equal (names[i], want))
      return values[i];
  return NULL;
}

/* "2.54cm", "1in", "12pt", "0.5in", "10mm" -> twips. */
static int
length_twips (const char *value)
{
  double v, per;

  if (value == NULL)
    return 0;
  v = g_ascii_strtod (value, NULL);
  if      (strstr (value, "pt") != NULL) per = 20.0;
  else if (strstr (value, "cm") != NULL) per = 1440.0 / 2.54;
  else if (strstr (value, "mm") != NULL) per = 1440.0 / 25.4;
  else if (strstr (value, "in") != NULL) per = 1440.0;
  else if (strstr (value, "px") != NULL) per = 15.0;
  else                                   per = 20.0;

  /* To the nearest twip.  Truncating loses one on nearly every round trip:
   * a 1 cm margin is written as 0.3937in and read back as 566.  Clamped
   * first, because a cast that does not fit an int is undefined. */
  v *= per;
  if (isnan (v))
    return 0;
  v = CLAMP (v, -1000000.0, 1000000.0);
  return (int) (v < 0 ? v - 0.5 : v + 0.5);
}

/* The text as XML: the markup characters escaped, and nothing that is
 * not a character in XML at all -- a control, a byte that is not UTF-8,
 * a non-character -- since one of those makes LibreOffice refuse the
 * whole document rather than the run. */
static void
xml_escape (GString *out, const char *text, gsize len)
{
  for (gsize i = 0; i < len; )
    {
      gunichar c = g_utf8_get_char_validated (text + i, (gssize) (len - i));
      gsize n;

      if (c == (gunichar) -1 || c == (gunichar) -2)
        {
          g_string_append (out, "\357\277\275");     /* U+FFFD */
          i++;
          continue;
        }
      n = g_utf8_skip[(guchar) text[i]];
      switch (c)
        {
        case '<': g_string_append (out, "&lt;"); break;
        case '>': g_string_append (out, "&gt;"); break;
        case '&': g_string_append (out, "&amp;"); break;
        case '"': g_string_append (out, "&quot;"); break;
        default:
          if (c >= 0x20 || c == '\t' || c == '\n')
            {
              if (c == 0xFFFE || c == 0xFFFF)
                g_string_append (out, "\357\277\275");
              else
                g_string_append_len (out, text + i, (gssize) n);
            }
        }
      i += n;
    }
}

/* The fo:margin shorthand: one length for every side, two for the
 * pairs, three or four naming them round from the top. */
static void
margin_shorthand (const char *v, int *top, int *right, int *bottom, int *left)
{
  char **tok = g_strsplit (v, " ", -1);
  int m[4] = { 0, 0, 0, 0 };
  int n = 0;

  for (guint k = 0; tok[k] != NULL && n < 4; k++)
    if (*tok[k] != '\0')
      m[n++] = length_twips (tok[k]);
  g_strfreev (tok);
  if (n == 0)
    return;
  if (n == 1)      { m[1] = m[2] = m[3] = m[0]; }
  else if (n == 2) { m[2] = m[0]; m[3] = m[1]; }
  else if (n == 3) { m[3] = m[1]; }
  *top = m[0];
  *right = m[1];
  *bottom = m[2];
  *left = m[3];
}

static void
twips_out (GString *s, int twips)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_string_append (s, g_ascii_formatd (buf, sizeof buf, "%.5f", twips / 1440.0));
  g_string_append (s, "in");
}

/* ====================================================================== */
/* Reading                                                                 */
/* ====================================================================== */

/* A style, resolved: what its chain of parents adds up to. */
typedef struct {
  W42ParaFmt pa;
  W42CharFmt ch;
  char      *parent;
  char      *list_style;
  char      *display;      /* the display name, for the named styles */
  int        outline;
  int        own_outline;  /* the level the style gives itself, not inherited */
  gboolean   outline_said; /* and that it gives one, nought included */
  gboolean   tabs_said;    /* its tab stops are its own, none included */
  gboolean   resolved;
  gboolean   has_pa, has_ch;
  gboolean   text_family;  /* a character style */
  gboolean   named;        /* waiting to join Word42's sheet */
  gboolean   master_page;  /* it names a page style: its paragraph starts a page */
  /* The attributes of its paragraph-properties and text-properties, as
   * the file gave them: resolving puts them over the parent's values, so
   * that what the style says -- "start", "0in", "normal" included -- is
   * what it gets, and what it does not say comes from the parent. */
  char     **pa_names, **pa_values;
  char     **ch_names, **ch_values;
} OdtStyle;

/* What a graphic style says about a shape: its fill, its outline, and
 * whether it sits behind the text. */
typedef struct {
  gboolean filled, stroked, arrow, behind;
  guint32  fill, stroke;
  double   stroke_pt;
} OdtGraphic;

/* What a table-cell style says beyond its sides and background: the
 * line of each side, and where the text sits. */
typedef struct {
  W42BorderEdge edge[4];
  int           valign;    /* W42CellVAlign */
} OdtCellLines;

typedef struct {
  W42ListKind kind[10];
} OdtListStyle;

/* A section style's columns. */
typedef struct {
  int count, gap;
} OdtColumns;

typedef struct {
  W42Builder     b;
  W42PieceTable *pt;
  W42PageSetup  *page;
  W42Zip        *zip;

  GHashTable    *styles;        /* name -> OdtStyle */
  GHashTable    *list_styles;   /* name -> OdtListStyle */
  GHashTable    *fonts;         /* font-face name -> family */
  GHashTable    *col_widths;    /* table-column style -> twips */
  GHashTable    *row_heights;   /* table-row style -> least height in twips */
  GHashTable    *cell_sides;    /* table-cell style -> W42_BORDER_CELL_SET | sides */
  GHashTable    *cell_fills;    /* table-cell style -> 0x1000000 | 0x00RRGGBB */
  GHashTable    *cell_lines;    /* table-cell style -> OdtCellLines */
  const OdtCellLines *pending_cell_lines;
  char          *cur_cell_style;
  int            pending_cell_sides;   /* for the cell about to begin; -1 none */
  guint          pending_cell_fill;    /* and its background; 0 none */
  char          *cur_row_style;
  int            table_row;     /* rows begun in the table being read */
  gboolean       in_header_rows;

  /* while a style is being read */
  OdtStyle      *cur_style;
  char          *cur_style_name;   /* and its name and family */
  gboolean       cur_style_text;
  gboolean       in_named_styles;  /* inside office:styles: the named ones */
  GPtrArray     *named;            /* char*: the named styles for the sheet */
  gboolean       ours;             /* Word42 wrote the file */
  int            n_masters;        /* master pages seen */
  gboolean       master_standard;  /* the one read is the Standard one */
  OdtListStyle  *cur_list;
  char          *cur_col_style;
  gboolean       in_page_layout, page_seen;
  int            in_hf_style;      /* 1 in a header-style, 2 a footer-style */
  int            hf_box[2];        /* the header's and footer's height and
                                    * spacing, which the page's margin in
                                    * OpenDocument does not count */
  gboolean       in_header, in_footer;
  W42PageTextKind hf_kind;      /* which of the three is being read */
  GString       *hf_text;
  W42Align       hf_align;

  /* the body */
  gboolean       in_body;
  GString       *text;
  gboolean       para_open;
  W42CharFmt     para_ch;       /* the paragraph's own text properties */
  GArray        *span_stack;    /* W42CharFmt, for nested spans */
  const char    *link;
  gboolean       after_space;   /* white space here would be collapsed */
  GHashTable    *bookmark_start;
  int            list_depth;
  GPtrArray     *list_style_stack;   /* char*, one per open list */
  gboolean       item_fresh[10];     /* the item open at each depth has had
                                      * no paragraph yet: the next is its
                                      * numbered one */
  int            item_start;         /* text:start-value of that item, or 0 */
  int            list_n[9];          /* the number of the last item at each level */
  gboolean       list_gap;           /* an unnumbered paragraph since it */
  gboolean       list_restart;       /* a list that does not continue began */
  gboolean       last_numbered;      /* the last paragraph was a numbered item, */
  W42ListKind    last_kind;          /* of this kind at this level */
  int            last_level;
  int            in_table;
  GArray        *table_widths;
  gboolean       table_started;
  gboolean       cell_pending;
  int            cell_repeat;     /* table:number-columns-repeated on it */
  int            cell_span;
  int            cell_vspan;         /* rows the pending cell spans, or W42_CELL_COVERED */
  int            skip_covered;       /* covered cells the last sideways span accounts for */
  int            in_note;
  gboolean       note_first_para;
  W42ParaFmt     note_outer_pa;
  W42CharFmt     note_outer_ch;
  GArray        *note_outer_spans;  /* the spans the note stands inside */
  const char    *note_outer_link;
  gboolean       in_annotation;
  GString       *annotation;
  gsize          annotation_pos;
  char          *annotation_name;
  GHashTable    *annotation_start;
  char          *pending_comment;   /* ODF 1.1's, for the run after it */
  int            skip_depth;
  const char    *field;           /* inside a field element */
  char          *index_term;      /* between the marks of an index entry */
  gsize          index_start;
  GString       *field_text;
  gboolean       frame_pending;
  gboolean       frame_positioned;
  int            frame_x, frame_y;
  int            frame_w, frame_h;
  char          *frame_href;
  W42Wrap        frame_wrap;
  int            tb_depth;         /* inside a draw:text-box */
  int            tb_side, tb_width;
  W42ParaFmt     tb_saved_pa;      /* the anchoring paragraph, to go on with */
  W42CharFmt     tb_saved_ch;
  GArray        *tb_saved_spans;
  const char    *tb_saved_link;
  int            tb_saved_depth;
  gboolean       tb_saved_fresh[10];
  gboolean       tb_reopened;      /* it was reopened after the box, empty */
  GHashTable    *pictures;         /* href -> GBytes, read once however
                                    * often the text names it */
  GHashTable    *graphic_wraps;  /* graphic style name -> W42Wrap + 1 */
  GHashTable    *graphics;       /* graphic style name -> OdtGraphic */
  gboolean       shape_open;     /* inside a draw:custom-shape, rect, ellipse or line */
  W42ShapeKind   shape_kind;
  int            shape_x, shape_y, shape_w, shape_h;   /* twips */
  gboolean       shape_positioned;
  W42Wrap        shape_wrap;
  const OdtGraphic *shape_style;
  GString       *shape_text;
  gboolean       shape_primitive_known;
  char          *cur_graphic;    /* the graphic style being read */
  char          *cur_section;    /* the section style being read */
  GHashTable    *section_cols;   /* section style name -> OdtColumns */
  int            section_depth;  /* text:sections open in the body */
  gboolean       section_pending;  /* one has begun: its first paragraph starts it */
  OdtColumns     section;          /* and its columns */
  gboolean       section_reset;  /* one of several columns has ended: the next
                                  * paragraph goes back to one */
  char          *frame_first_href;  /* a frame's first picture, which a reader
                                     * is to take when it can */
} Odt;

static void
style_free (gpointer data)
{
  OdtStyle *s = data;

  g_free (s->parent);
  g_free (s->list_style);
  g_free (s->display);
  g_strfreev (s->pa_names);
  g_strfreev (s->pa_values);
  g_strfreev (s->ch_names);
  g_strfreev (s->ch_values);
  g_free (s);
}

/* Adds an element's attributes to those a style has kept. */
static void
keep_attrs (char ***names, char ***values, const char **an, const char **av)
{
  guint have = *names != NULL ? g_strv_length (*names) : 0;
  guint more = 0;

  while (an != NULL && an[more] != NULL)
    more++;
  *names = g_renew (char *, *names, have + more + 1);
  *values = g_renew (char *, *values, have + more + 1);
  for (guint i = 0; i < more; i++)
    {
      (*names)[have + i] = g_strdup (an[i]);
      (*values)[have + i] = g_strdup (av[i]);
    }
  (*names)[have + more] = NULL;
  (*values)[have + more] = NULL;
}

/* ---- properties into formats -------------------------------------------- */

static void
para_props (Odt *o, W42ParaFmt *pa, const char **an, const char **av)
{
  for (int i = 0; an != NULL && an[i] != NULL; i++)
    {
      const char *k = an[i], *v = av[i];

      /* "start" and "end" are the left and the right whichever way the
       * paragraph runs: that is how LibreOffice sets them, a right-to-left
       * paragraph's "start" included. */
      if (g_str_equal (k, "fo:text-align"))
        pa->align = g_str_equal (v, "center") ? W42_ALIGN_CENTER
                  : g_str_equal (v, "end") || g_str_equal (v, "right") ? W42_ALIGN_RIGHT
                  : g_str_equal (v, "justify") ? W42_ALIGN_JUSTIFY : W42_ALIGN_LEFT;
      else if (g_str_equal (k, "fo:margin"))
        margin_shorthand (v, &pa->space_before, &pa->indent_right, &pa->space_after, &pa->indent_left);
      else if (g_str_equal (k, "fo:margin-left"))   pa->indent_left = length_twips (v);
      else if (g_str_equal (k, "fo:margin-right"))  pa->indent_right = length_twips (v);
      else if (g_str_equal (k, "fo:text-indent"))   pa->indent_first = length_twips (v);
      else if (g_str_equal (k, "fo:margin-top"))    pa->space_before = length_twips (v);
      else if (g_str_equal (k, "fo:margin-bottom")) pa->space_after = length_twips (v);
      else if (g_str_equal (k, "fo:line-height"))
        {
          /* Either kind replaces the other, which a parent style may
           * have set; 100% and "normal" are single spacing, the model's 0. */
          pa->line_spacing = 0;
          pa->line_spacing_pct = 0;
          if (strchr (v, '%') != NULL)
            pa->line_spacing_pct = CLAMP (atoi (v), 0, 10000);
          else if (!g_str_equal (v, "normal"))
            pa->line_spacing = length_twips (v);
          if (pa->line_spacing_pct == 100)
            pa->line_spacing_pct = 0;
        }
      else if (g_str_equal (k, "fo:break-before"))  pa->page_break_before = g_str_equal (v, "page");
      else if (g_str_equal (k, "fo:keep-with-next")) pa->keep_next = g_str_equal (v, "always");
      else if (g_str_equal (k, "fo:keep-together")) pa->keep_together = g_str_equal (v, "always");
      else if (g_str_equal (k, "fo:widows"))        pa->widow_control = atoi (v) > 0;
      else if (g_str_equal (k, "style:writing-mode")) pa->rtl = g_str_has_prefix (v, "rl");
      else if (g_str_equal (k, "fo:border") || g_str_equal (k, "fo:border-top") ||
               g_str_equal (k, "fo:border-bottom") || g_str_equal (k, "fo:border-left") ||
               g_str_equal (k, "fo:border-right"))
        {
          int bit = g_str_equal (k, "fo:border") ? W42_BORDER_BOX
                  : g_str_equal (k, "fo:border-top") ? W42_BORDER_TOP
                  : g_str_equal (k, "fo:border-bottom") ? W42_BORDER_BOTTOM
                  : g_str_equal (k, "fo:border-left") ? W42_BORDER_LEFT : W42_BORDER_RIGHT;

          /* "none" takes away a side a parent style drew. */
          if (g_str_equal (v, "none"))
            pa->border &= ~bit;
          else
            {
              const char *hash = strchr (v, '#');
              W42BorderEdge edge;

              /* "0.75pt solid #14828c": the width, the style, the colour. */
              edge.width = (guint8) CLAMP (length_twips (v), 5, 120);
              edge.style = (guint8) w42_border_style_from_css (v);
              edge.color = (hash != NULL && strlen (hash) >= 7)
                             ? (guint32) strtoul (hash + 1, NULL, 16) & 0xFFFFFF : 0;
              pa->border |= bit;
              for (int e = 0; e < 4; e++)
                if (bit & (1 << e))
                  pa->edge[e] = edge;
            }
        }
      else if (g_str_equal (k, "fo:background-color"))
        {
          if (v[0] == '#' && strlen (v) >= 7)
            {
              pa->shading_color = (guint32) strtoul (v + 1, NULL, 16) & 0xFFFFFF;
              pa->has_shading_color = 1;
              pa->shading = 0;
            }
          else if (g_str_equal (v, "transparent"))
            {
              pa->has_shading_color = 0;
              pa->shading = 0;
            }
        }
    }
  (void) o;
}

static void
text_props (Odt *o, W42CharFmt *ch, const char **an, const char **av)
{
  const char *lang = NULL, *country = NULL, *script = NULL, *rfc = NULL;

  for (int i = 0; an != NULL && an[i] != NULL; i++)
    {
      const char *k = an[i], *v = av[i];

      if (g_str_equal (k, "fo:language"))      lang = v;
      else if (g_str_equal (k, "fo:country"))  country = v;
      else if (g_str_equal (k, "fo:script"))   script = v;
      else if (g_str_equal (k, "style:rfc-language-tag")) rfc = v;

      if (g_str_equal (k, "fo:font-weight"))        ch->bold = g_str_equal (v, "bold") || atoi (v) >= 600;
      else if (g_str_equal (k, "fo:font-style"))    ch->italic = g_str_equal (v, "italic") || g_str_equal (v, "oblique");
      else if (g_str_equal (k, "style:text-underline-style"))
        {
          /* OpenDocument says the line's shape here and, for a double
           * line, its number in style:text-underline-type. */
          if (g_str_equal (v, "none"))            ch->underline = W42_UNDERLINE_NONE;
          else if (g_str_equal (v, "dotted"))     ch->underline = W42_UNDERLINE_DOTTED;
          else if (g_str_has_prefix (v, "dash") ||
                   g_str_has_prefix (v, "long-dash") ||
                   g_str_has_prefix (v, "dot-dash")) ch->underline = W42_UNDERLINE_DASHED;
          else if (g_str_equal (v, "wave"))       ch->underline = W42_UNDERLINE_WAVE;
          else if (ch->underline == W42_UNDERLINE_NONE) ch->underline = W42_UNDERLINE_SINGLE;
        }
      else if (g_str_equal (k, "style:text-underline-type") && g_str_equal (v, "double"))
        ch->underline = W42_UNDERLINE_DOUBLE;
      else if (g_str_equal (k, "style:text-underline-width") &&
               (g_str_equal (v, "bold") || g_str_equal (v, "thick")))
        ch->underline = W42_UNDERLINE_THICK;
      else if (g_str_equal (k, "style:text-underline-mode") && g_str_equal (v, "skip-white-space"))
        ch->underline = W42_UNDERLINE_WORDS;
      else if (g_str_equal (k, "style:text-line-through-style")) ch->strikeout = !g_str_equal (v, "none");
      else if (g_str_equal (k, "style:text-line-through-type")) ch->dstrike = g_str_equal (v, "double");
      else if (g_str_equal (k, "fo:text-shadow"))      ch->shadow = !g_str_equal (v, "none");
      else if (g_str_equal (k, "style:text-outline"))  ch->outline = g_str_equal (v, "true");
      else if (g_str_equal (k, "style:font-relief"))
        {
          ch->emboss = g_str_equal (v, "embossed");
          ch->engrave = g_str_equal (v, "engraved");
        }
      else if (g_str_equal (k, "style:text-overline-style")) ch->overline = !g_str_equal (v, "none");
      else if (g_str_equal (k, "fo:font-size"))
        {
          if (strchr (v, '%') == NULL)
            {
              /* Half-points, as the model counts them, from however many
               * decimals the file gave.  Clamped before the cast, which
               * would be undefined for a size that does not fit. */
              double pt = g_ascii_strtod (v, NULL);

              if (pt > 0.0 && pt < 1700.0)
                ch->size = CLAMP ((int) (pt * 2.0 + 0.5), 2, 3276);
            }
        }
      else if (g_str_equal (k, "fo:color"))
        {
          if (v[0] == '#' && strlen (v) >= 7)
            ch->color = (guint32) strtoul (v + 1, NULL, 16) & 0xFFFFFF;
        }
      else if (g_str_equal (k, "fo:background-color"))
        {
          /* The nearest of Word's sixteen, which is what a highlight is. */
          if (g_str_equal (v, "transparent"))
            ch->highlight = 0;
          else if (v[0] == '#' && strlen (v) >= 7)
            ch->highlight = (guint8) w42_highlight_nearest ((guint32) strtoul (v + 1, NULL, 16));
          else
            ch->highlight = 7;
        }
      else if (g_str_equal (k, "fo:font-family"))
        {
          char *name = g_strdup (v);

          g_strdelimit (name, "'\"", ' ');
          ch->family = g_intern_string (g_strstrip (name));
          g_free (name);
        }
      else if (g_str_equal (k, "style:font-name"))
        {
          const char *family = g_hash_table_lookup (o->fonts, v);

          ch->family = g_intern_string (family != NULL ? family : v);
        }
      else if (g_str_equal (k, "style:text-position"))
        ch->script = g_str_has_prefix (v, "super") ? 1 : g_str_has_prefix (v, "sub") ? -1
                   : g_ascii_strtod (v, NULL) > 0 ? 1 : g_ascii_strtod (v, NULL) < 0 ? -1 : 0;
      else if (g_str_equal (k, "fo:font-variant"))  ch->smallcaps = g_str_equal (v, "small-caps");
      else if (g_str_equal (k, "fo:text-transform")) ch->allcaps = g_str_equal (v, "uppercase");
      else if (g_str_equal (k, "fo:letter-spacing"))
        {
          ch->spacing = g_str_equal (v, "normal")
                          ? 0 : (gint16) CLAMP (length_twips (v), -720, 720);
        }
    }

  /* The language, the script and the country are attributes apart and
   * one tag; a tag they cannot say comes whole in rfc-language-tag. */
  if (lang != NULL && *lang != '\0')
    {
      if (g_str_equal (lang, W42_LANG_NONE))
        ch->lang = g_intern_static_string (W42_LANG_NONE);
      else
        {
          GString *built = g_string_new (lang);
          char *tag;
          const char *known;

          if (script != NULL && *script != '\0')
            g_string_append_printf (built, "-%s", script);
          if (country != NULL && *country != '\0' && !g_str_equal (country, "none"))
            g_string_append_printf (built, "-%s", country);
          tag = rfc != NULL && *rfc != '\0' ? g_strdup (rfc) : g_strdup (built->str);
          g_string_free (built, TRUE);
          known = w42_lang_normalise (tag);

          if (known != NULL)
            ch->lang = known;
          g_free (tag);
        }
    }
}

/* The style with its parents folded in. */
static OdtStyle *
resolve_style (Odt *o, const char *name, int depth)
{
  OdtStyle *s = name != NULL ? g_hash_table_lookup (o->styles, name) : NULL;

  if (s == NULL)
    return NULL;
  if (!s->resolved && depth < 16)
    {
      OdtStyle *parent = s->parent != NULL ? resolve_style (o, s->parent, depth + 1) : NULL;

      if (parent != NULL)
        {
          /* What the style itself set stays; the rest comes from the
           * parent: the parent's values, then the style's own attributes
           * read again over them. */
          W42ParaFmt pa = parent->pa;
          W42CharFmt ch = parent->ch;

          if (s->pa_names != NULL)
            para_props (o, &pa, (const char **) s->pa_names, (const char **) s->pa_values);
          /* Tab stops and a drop cap come in elements of their own, so
           * they are taken whole: a style's stops replace its parent's,
           * and an empty list of them says it has none. */
          if (s->tabs_said)
            {
              pa.n_tabs = s->pa.n_tabs;
              memcpy (pa.tab_pos, s->pa.tab_pos, sizeof pa.tab_pos);
              memcpy (pa.tab_kind, s->pa.tab_kind, sizeof pa.tab_kind);
            }
          if (s->pa.drop_cap)
            pa.drop_cap = s->pa.drop_cap;
          {
            /* A named style of the file's own keeps its name; an automatic
             * style takes its parent's. */
            W42Fmt def;

            w42_fmt_init_default (&def);
            if (s->pa.style != NULL && s->pa.style != def.pa.style)
              pa.style = s->pa.style;
          }
          if (s->ch_names != NULL)
            text_props (o, &ch, (const char **) s->ch_names, (const char **) s->ch_values);
          s->pa = pa;
          s->ch = ch;
          if (s->outline == 0)
            s->outline = parent->outline;
          if (s->list_style == NULL && parent->list_style != NULL)
            s->list_style = g_strdup (parent->list_style);
          if (s->pa.style == NULL || g_str_equal (s->pa.style, "Normal"))
            s->pa.style = parent->pa.style;
        }
      s->resolved = TRUE;
    }
  return s;
}

/* A text style over the text it is applied to: its named parents' text
 * properties first, then its own, each saying what it says -- "normal"
 * as much as "bold".  The family's default style is not among them: the
 * paragraph's text has already had it. */
static void
span_props (Odt *o, OdtStyle *s, W42CharFmt *ch, int depth)
{
  if (s->parent != NULL && s->parent[0] != '@' && depth < 16)
    {
      OdtStyle *parent = g_hash_table_lookup (o->styles, s->parent);

      if (parent != NULL)
        span_props (o, parent, ch, depth + 1);
    }
  if (s->ch_names != NULL)
    text_props (o, ch, (const char **) s->ch_names, (const char **) s->ch_values);
}

/* The Word42 style a named style stands for, by its display name.  In a
 * file Word42 wrote, only the style named "Standard" is Normal: another it
 * shows as "Standard" is one of that name. */
static const char *
our_style_name (Odt *o, const char *name, const char *display)
{
  W42StyleSheet *sheet = w42_pt_stylesheet (o->pt);

  if (display == NULL)
    return NULL;
  if (o->ours && name != NULL && g_str_equal (name, "Standard"))
    return g_intern_static_string ("Normal");
  if (!o->ours &&
      (g_str_equal (display, "Standard") || g_str_equal (display, "Default Paragraph Style") ||
       g_str_equal (display, "Text body") || g_str_equal (display, "Default")))
    return g_intern_static_string ("Normal");
  for (guint i = 0; i < w42_stylesheet_size (sheet); i++)
    {
      const W42Style *st = w42_stylesheet_get (sheet, i);

      if (g_ascii_strcasecmp (st->name, display) == 0)
        return st->name;
    }
  return NULL;
}

/* ---- the style parts (styles.xml and the automatic styles) --------------- */

static void
styles_start (Odt *o, const char *tag, const char **an, const char **av)
{
  if (g_str_equal (tag, "font-face"))
    {
      const char *name = attr (an, av, "style:name");
      const char *family = attr (an, av, "svg:font-family");

      if (name != NULL && family != NULL)
        {
          char *clean = g_strdup (family);

          g_strdelimit (clean, "'\"", ' ');
          g_hash_table_insert (o->fonts, g_strdup (name), g_strdup (g_strstrip (clean)));
          g_free (clean);
        }
    }
  else if (g_str_equal (tag, "style") || g_str_equal (tag, "default-style"))
    {
      const char *name = attr (an, av, "style:name");
      const char *family = attr (an, av, "style:family");
      OdtStyle *s;
      W42Fmt def;

      if (family != NULL && g_str_equal (family, "table-column"))
        {
          g_free (o->cur_col_style);
          o->cur_col_style = g_strdup (name);
          return;
        }
      if (family != NULL && g_str_equal (family, "table-row"))
        {
          g_free (o->cur_row_style);
          o->cur_row_style = g_strdup (name);
          return;
        }
      if (family != NULL && g_str_equal (family, "table-cell"))
        {
          g_free (o->cur_cell_style);
          o->cur_cell_style = g_strdup (name);
          return;
        }
      if (family != NULL && g_str_equal (family, "graphic") && name != NULL)
        {
          g_free (o->cur_graphic);
          o->cur_graphic = g_strdup (name);
          return;
        }
      if (family != NULL && g_str_equal (family, "section") && name != NULL)
        {
          g_free (o->cur_section);
          o->cur_section = g_strdup (name);
          return;
        }
      if (family == NULL || !(g_str_equal (family, "paragraph") || g_str_equal (family, "text")))
        return;
      if (name == NULL)
        name = g_str_equal (family, "paragraph") ? "@default-paragraph" : "@default-text";

      s = g_new0 (OdtStyle, 1);
      w42_fmt_init_default (&def);
      s->pa = def.pa;
      s->ch = def.ch;
      s->parent = g_strdup (attr (an, av, "style:parent-style-name"));
      /* A style with no parent inherits from the family's default style,
       * where LibreOffice puts the document's font, size and language. */
      if (s->parent == NULL && name[0] != '@')
        s->parent = g_strdup (g_str_equal (family, "paragraph") ? "@default-paragraph" : "@default-text");
      s->list_style = g_strdup (attr (an, av, "style:list-style-name"));
      {
        const char *display = attr (an, av, "style:display-name");
        char *shown = display != NULL ? g_strdup (display) : g_strdup (name);
        char *p;

        /* Names come with spaces as _20_ */
        while ((p = strstr (shown, "_20_")) != NULL)
          {
            *p = ' ';
            memmove (p + 1, p + 4, strlen (p + 4) + 1);
          }
        s->display = shown;
      }
      {
        const char *lvl = attr (an, av, "style:default-outline-level");
        const char *ours = our_style_name (o, name, s->display);

        if (lvl != NULL)
          {
            s->outline = s->own_outline = CLAMP (atoi (lvl), 0, 9);
            s->outline_said = TRUE;
          }
        if (ours != NULL)
          s->pa.style = ours;
        else if (s->outline >= 1 && s->outline <= 3)
          {
            char *hn = g_strdup_printf ("Heading %d", s->outline);
            s->pa.style = g_intern_string (hn);
            g_free (hn);
          }
      }
      s->text_family = g_str_equal (family, "text");
      {
        const char *master = attr (an, av, "style:master-page-name");

        s->master_page = master != NULL && *master != '\0';
      }
      g_hash_table_insert (o->styles, g_strdup (name), s);
      o->cur_style = s;
      g_free (o->cur_style_name);
      o->cur_style_name = g_strdup (name);
      o->cur_style_text = s->text_family;
    }
  else if (g_str_equal (tag, "styles"))
    o->in_named_styles = TRUE;
  else if (g_str_equal (tag, "graphic-properties") && o->cur_graphic != NULL)
    {
      /* style:wrap says where the text goes; the picture sits on the
       * other side, or where horizontal-pos puts it. */
      const char *wrap = attr (an, av, "style:wrap");
      const char *hpos = attr (an, av, "style:horizontal-pos");
      W42Wrap w = W42_WRAP_INLINE;

      /* "none" is nothing beside it, which is Word's top and bottom;
       * "run-through" is left inline here, for the frame to say whether
       * the text runs over it or under. */
      if (wrap != NULL && g_str_equal (wrap, "none"))
        w = W42_WRAP_TOP_BOTTOM;
      else if (wrap != NULL && !g_str_equal (wrap, "run-through"))
        {
          if (g_str_equal (wrap, "left"))
            w = W42_WRAP_RIGHT;
          else if (g_str_equal (wrap, "right"))
            w = W42_WRAP_LEFT;
          else
            w = (hpos != NULL && g_str_equal (hpos, "right")) ? W42_WRAP_RIGHT : W42_WRAP_LEFT;
        }
      g_hash_table_insert (o->graphic_wraps, g_strdup (o->cur_graphic), GINT_TO_POINTER ((int) w + 1));
      {
        /* And for a shape: its fill and its outline. */
        OdtGraphic *g = g_new0 (OdtGraphic, 1);
        const char *fill = attr (an, av, "draw:fill");
        const char *fill_color = attr (an, av, "draw:fill-color");
        const char *stroke = attr (an, av, "draw:stroke");
        const char *stroke_w = attr (an, av, "svg:stroke-width");
        const char *stroke_c = attr (an, av, "svg:stroke-color");
        const char *marker = attr (an, av, "draw:marker-end");
        const char *marker2 = attr (an, av, "draw:marker-start");
        const char *through = attr (an, av, "style:run-through");

        g->filled = fill != NULL ? !g_str_equal (fill, "none") : fill_color != NULL;
        g->fill = fill_color != NULL && fill_color[0] == '#' ? (guint32) strtoul (fill_color + 1, NULL, 16) & 0xFFFFFF : 0x4472C4;
        g->stroked = stroke == NULL || !g_str_equal (stroke, "none");
        g->stroke_pt = stroke_w != NULL ? length_twips (stroke_w) / 20.0 : 0.75;
        if (g->stroked && g->stroke_pt <= 0.0)
          g->stroke_pt = 0.75;
        g->stroke = stroke_c != NULL && stroke_c[0] == '#' ? (guint32) strtoul (stroke_c + 1, NULL, 16) & 0xFFFFFF : 0;
        g->arrow = (marker != NULL && *marker != '\0') || (marker2 != NULL && *marker2 != '\0');
        g->behind = through != NULL && g_str_equal (through, "background");
        g_hash_table_insert (o->graphics, g_strdup (o->cur_graphic), g);
      }
    }
  else if (g_str_equal (tag, "drop-cap") && o->cur_style != NULL)
    {
      const char *lines = attr (an, av, "style:lines");

      o->cur_style->pa.drop_cap = (guint8) CLAMP (lines != NULL ? atoi (lines) : 3, 1, 10);
      o->cur_style->has_pa = TRUE;
    }
  else if (g_str_equal (tag, "paragraph-properties") && o->cur_style != NULL)
    {
      para_props (o, &o->cur_style->pa, an, av);
      keep_attrs (&o->cur_style->pa_names, &o->cur_style->pa_values, an, av);
      o->cur_style->has_pa = TRUE;
    }
  else if (g_str_equal (tag, "tab-stops") && o->cur_style != NULL)
    o->cur_style->tabs_said = TRUE;
  else if (g_str_equal (tag, "tab-stop") && o->cur_style != NULL)
    {
      const char *pos = attr (an, av, "style:position");
      const char *type = attr (an, av, "style:type");
      const char *lead = attr (an, av, "style:leader-style");
      const char *lead_text = attr (an, av, "style:leader-text");

      if (lead == NULL && lead_text != NULL && *lead_text != '\0')
        lead = *lead_text == '.' ? "dotted" : *lead_text == '-' ? "dash" : "solid";

      if (pos != NULL)
        w42_para_fmt_set_tab_leader (&o->cur_style->pa, length_twips (pos),
                              type == NULL ? W42_TAB_LEFT
                              : g_str_equal (type, "center") ? W42_TAB_CENTER
                              : g_str_equal (type, "right") ? W42_TAB_RIGHT
                              : g_str_equal (type, "char") ? W42_TAB_DECIMAL : W42_TAB_LEFT,
                              lead == NULL || g_str_equal (lead, "none") ? W42_TAB_LEAD_NONE
                              : g_str_has_prefix (lead, "dot") ? W42_TAB_LEAD_DOT
                              : g_str_has_prefix (lead, "dash") || g_str_has_prefix (lead, "long-dash")
                                ? W42_TAB_LEAD_DASH : W42_TAB_LEAD_LINE);
    }
  else if (g_str_equal (tag, "text-properties") && o->cur_style != NULL)
    {
      text_props (o, &o->cur_style->ch, an, av);
      keep_attrs (&o->cur_style->ch_names, &o->cur_style->ch_values, an, av);
      o->cur_style->has_ch = TRUE;
    }
  else if (g_str_equal (tag, "table-cell-properties") && o->cur_cell_style != NULL)
    {
      /* fo:border sets all four sides; fo:border-* one each; "none" clears.
       * "1.5pt double #0000ff" is the width, the style and the colour;
       * a double line's width is the whole of it, and its own lines'
       * widths come in style:border-line-width-*. */
      int sides = -1;
      OdtCellLines *lines = g_new0 (OdtCellLines, 1);
      const char *valign = attr (an, av, "style:vertical-align");

      for (int i = 0; an != NULL && an[i] != NULL; i++)
        {
          const char *k = an[i], *v = av[i];
          int bit = g_str_equal (k, "fo:border") ? W42_BORDER_BOX
                  : g_str_equal (k, "fo:border-top") ? W42_BORDER_TOP
                  : g_str_equal (k, "fo:border-bottom") ? W42_BORDER_BOTTOM
                  : g_str_equal (k, "fo:border-left") ? W42_BORDER_LEFT
                  : g_str_equal (k, "fo:border-right") ? W42_BORDER_RIGHT : 0;
          W42BorderEdge edge;

          if (bit == 0)
            continue;
          if (sides < 0)
            sides = 0;
          if (g_str_equal (v, "none"))
            {
              sides &= ~bit;
              continue;
            }
          sides |= bit;
          edge.style = (guint8) w42_border_style_from_css (v);
          edge.width = (guint8) CLAMP (length_twips (v), 5, 255);
          if (edge.style == W42_BORDER_DOUBLE)
            edge.width = (guint8) MAX (edge.width / 3, 5);
          {
            const char *hash = strchr (v, '#');

            edge.color = (hash != NULL && strlen (hash) >= 7)
                           ? (guint32) strtoul (hash + 1, NULL, 16) & 0xFFFFFF : 0;
          }
          for (int e = 0; e < 4; e++)
            if (bit & (1 << e))
              lines->edge[e] = edge;
        }
      for (int i = 0; an != NULL && an[i] != NULL; i++)
        {
          /* "0.0208in 0.0208in 0.0208in": inner line, gap, outer line. */
          const char *k = an[i];
          int e = g_str_equal (k, "style:border-line-width") ? -1
                : g_str_equal (k, "style:border-line-width-top") ? W42_EDGE_TOP
                : g_str_equal (k, "style:border-line-width-bottom") ? W42_EDGE_BOTTOM
                : g_str_equal (k, "style:border-line-width-left") ? W42_EDGE_LEFT
                : g_str_equal (k, "style:border-line-width-right") ? W42_EDGE_RIGHT : -2;
          int w;

          if (e == -2)
            continue;
          w = CLAMP (length_twips (av[i]), 5, 255);
          for (int k2 = 0; k2 < 4; k2++)
            if ((e < 0 || e == k2) && lines->edge[k2].style == W42_BORDER_DOUBLE)
              lines->edge[k2].width = (guint8) w;
        }
      lines->valign = valign == NULL ? W42_CELL_VALIGN_TOP
                    : g_str_equal (valign, "middle") ? W42_CELL_VALIGN_CENTER
                    : g_str_equal (valign, "bottom") ? W42_CELL_VALIGN_BOTTOM : W42_CELL_VALIGN_TOP;
      g_hash_table_insert (o->cell_lines, g_strdup (o->cur_cell_style), lines);
      if (sides >= 0)
        g_hash_table_insert (o->cell_sides, g_strdup (o->cur_cell_style),
                             GINT_TO_POINTER (W42_BORDER_CELL_SET | sides));
      for (int i = 0; an != NULL && an[i] != NULL; i++)
        if (g_str_equal (an[i], "fo:background-color") && av[i][0] == '#' &&
            strlen (av[i]) >= 7)
          g_hash_table_insert (o->cell_fills, g_strdup (o->cur_cell_style),
                               GUINT_TO_POINTER (
                                 (guint) (strtoul (av[i] + 1, NULL, 16) & 0xFFFFFF) | 0x1000000u));
    }
  else if (g_str_equal (tag, "table-row-properties") && o->cur_row_style != NULL)
    {
      const char *h = attr (an, av, "style:min-row-height");

      if (h == NULL)
        h = attr (an, av, "style:row-height");
      if (h != NULL)
        g_hash_table_insert (o->row_heights, g_strdup (o->cur_row_style),
                             GINT_TO_POINTER (length_twips (h)));
    }
  else if (g_str_equal (tag, "table-column-properties") && o->cur_col_style != NULL)
    {
      const char *w = attr (an, av, "style:column-width");

      if (w != NULL)
        g_hash_table_insert (o->col_widths, g_strdup (o->cur_col_style), GINT_TO_POINTER (length_twips (w)));
    }
  else if (g_str_equal (tag, "list-style"))
    {
      const char *name = attr (an, av, "style:name");

      if (name != NULL)
        {
          OdtListStyle *ls = g_new0 (OdtListStyle, 1);

          for (int i = 0; i < 10; i++)
            ls->kind[i] = W42_LIST_NUMBER;
          g_hash_table_insert (o->list_styles, g_strdup (name), ls);
          o->cur_list = ls;
        }
    }
  else if (o->cur_list != NULL && (g_str_equal (tag, "list-level-style-number") ||
                                   g_str_equal (tag, "list-level-style-bullet")))
    {
      int level = CLAMP (atoi (attr (an, av, "text:level") != NULL ? attr (an, av, "text:level") : "1") - 1, 0, 9);
      W42ListKind kind = W42_LIST_NUMBER;

      if (g_str_equal (tag, "list-level-style-bullet"))
        {
          const char *bc = attr (an, av, "text:bullet-char");
          gunichar c = bc != NULL ? g_utf8_get_char (bc) : 0x2022;

          kind = c == 0x25E6 || c == 'o' ? W42_LIST_BULLET_CIRCLE
               : c == 0x25AA || c == 0x25A0 || c == 0xA7 ? W42_LIST_BULLET_SQUARE
               : c == '-' || c == 0x2013 ? W42_LIST_BULLET_DASH : W42_LIST_BULLET;
        }
      else
        {
          const char *fmt = attr (an, av, "style:num-format");

          if (fmt != NULL)
            kind = g_str_equal (fmt, "a") ? W42_LIST_LOWER_LETTER : g_str_equal (fmt, "A") ? W42_LIST_UPPER_LETTER
                 : g_str_equal (fmt, "i") ? W42_LIST_LOWER_ROMAN : g_str_equal (fmt, "I") ? W42_LIST_UPPER_ROMAN
                 : W42_LIST_NUMBER;
        }
      o->cur_list->kind[level] = kind;
    }
  else if (g_str_equal (tag, "page-layout-properties") && !o->page_seen)
    {
      const char *w = attr (an, av, "fo:page-width"), *h = attr (an, av, "fo:page-height");

      if (w != NULL) o->page->width = length_twips (w);
      if (h != NULL) o->page->height = length_twips (h);
      if (attr (an, av, "fo:margin"))
        margin_shorthand (attr (an, av, "fo:margin"), &o->page->margin_top, &o->page->margin_right,
                          &o->page->margin_bottom, &o->page->margin_left);
      if (attr (an, av, "fo:margin-top")) o->page->margin_top = length_twips (attr (an, av, "fo:margin-top"));
      if (attr (an, av, "fo:margin-bottom")) o->page->margin_bottom = length_twips (attr (an, av, "fo:margin-bottom"));
      if (attr (an, av, "fo:margin-left")) o->page->margin_left = length_twips (attr (an, av, "fo:margin-left"));
      if (attr (an, av, "fo:margin-right")) o->page->margin_right = length_twips (attr (an, av, "fo:margin-right"));
      {
        /* A border round the page.  OpenDocument draws it inside the
         * margins with the padding between it and the text, where Word
         * measures the margin to the text and the border from the edge:
         * the margins read are the border's distance, and the text's
         * margin is what the padding adds to them. */
        const char *border = attr (an, av, "fo:border");
        int t = 0, r = 0, b = 0, l = 0;
        const char *pad = attr (an, av, "fo:padding");

        if (border == NULL) border = attr (an, av, "fo:border-top");
        if (border == NULL) border = attr (an, av, "fo:border-left");
        if (pad != NULL) margin_shorthand (pad, &t, &r, &b, &l);
        if (attr (an, av, "fo:padding-top")) t = length_twips (attr (an, av, "fo:padding-top"));
        if (attr (an, av, "fo:padding-right")) r = length_twips (attr (an, av, "fo:padding-right"));
        if (attr (an, av, "fo:padding-bottom")) b = length_twips (attr (an, av, "fo:padding-bottom"));
        if (attr (an, av, "fo:padding-left")) l = length_twips (attr (an, av, "fo:padding-left"));
        if (border != NULL && !g_str_equal (border, "none"))
          {
            const char *hash = strchr (border, '#');
            int width = CLAMP (length_twips (border), 5, 120);

            o->page->has_border = 1;
            o->page->border_style = (guint8) w42_border_style_from_css (border);
            o->page->border_width = (guint8) width;
            o->page->border_color = (hash != NULL && strlen (hash) >= 7)
                                      ? (guint32) strtoul (hash + 1, NULL, 16) & 0xFFFFFF : 0;
            o->page->border_space = o->page->margin_top;
            o->page->margin_top += t + width;
            o->page->margin_bottom += b + width;
            o->page->margin_left += l + width;
            o->page->margin_right += r + width;
          }
      }
      {
        /* The colour behind the page. */
        const char *bg = attr (an, av, "fo:background-color");

        if (bg != NULL && *bg == '#' && strlen (bg) >= 7)
          {
            o->page->background = (guint32) strtoul (bg + 1, NULL, 16) & 0xFFFFFF;
            o->page->has_background = 1;
          }
      }
      o->in_page_layout = TRUE;
    }
  else if ((g_str_equal (tag, "header-style") || g_str_equal (tag, "footer-style")) &&
           !o->page_seen)
    o->in_hf_style = tag[0] == 'h' ? 1 : 2;
  else if (g_str_equal (tag, "header-footer-properties") && o->in_hf_style != 0 && !o->page_seen)
    {
      /* OpenDocument's page margin runs to the header; the header's
       * height and its gap to the text are the rest of Word's margin. */
      const char *height = attr (an, av, "svg:height");
      const char *gap = attr (an, av, o->in_hf_style == 1 ? "fo:margin-bottom" : "fo:margin-top");

      if (height == NULL)
        height = attr (an, av, "fo:min-height");
      o->hf_box[o->in_hf_style - 1] = CLAMP ((height != NULL ? length_twips (height) : 0) +
                                             (gap != NULL ? length_twips (gap) : 0), 0, 31680);
    }
  else if (g_str_equal (tag, "columns") && o->cur_section != NULL)
    {
      /* A section's columns, which Word42 keeps with the section break
       * that starts it. */
      const char *n = attr (an, av, "fo:column-count"), *gap = attr (an, av, "fo:column-gap");
      OdtColumns *c = g_new0 (OdtColumns, 1);

      c->count = n != NULL ? CLAMP (atoi (n), 1, 9) : 1;
      c->gap = gap != NULL ? CLAMP (length_twips (gap), 0, 31680) : 0;
      g_hash_table_insert (o->section_cols, g_strdup (o->cur_section), c);
    }
  else if (g_str_equal (tag, "columns") && o->in_page_layout)
    {
      const char *n = attr (an, av, "fo:column-count"), *gap = attr (an, av, "fo:column-gap");

      if (n != NULL) o->page->columns = CLAMP (atoi (n), 1, 9);
      if (gap != NULL) o->page->column_gap = length_twips (gap);
    }
  else if (g_str_equal (tag, "master-page"))
    {
      /* One master page's headers are the document's: the Standard one,
       * which the text uses unless it says otherwise, or the first when
       * there is none.  Taking every one's, the last page style in the
       * file -- a landscape one, an envelope -- set the headers. */
      const char *name = attr (an, av, "style:name");
      gboolean standard = name != NULL && g_str_equal (name, "Standard");

      if (o->n_masters == 0 || (standard && !o->master_standard))
        {
          if (o->n_masters > 0)
            {
              for (int k = 0; k < W42_PAGE_TEXT_KINDS; k++)
                {
                  w42_pt_set_header_kind (o->pt, (W42PageTextKind) k, "", W42_ALIGN_LEFT);
                  w42_pt_set_footer_kind (o->pt, (W42PageTextKind) k, "", W42_ALIGN_LEFT);
                }
              w42_pt_set_title_page (o->pt, FALSE);
              w42_pt_set_facing_pages (o->pt, FALSE);
            }
          o->master_standard = standard;
        }
      else
        o->skip_depth = 1;
      o->n_masters++;
    }
  else if (g_str_equal (tag, "header") || g_str_equal (tag, "footer") ||
           g_str_equal (tag, "header-left") || g_str_equal (tag, "footer-left") ||
           g_str_equal (tag, "header-first") || g_str_equal (tag, "footer-first"))
    {
      const char *display = attr (an, av, "style:display");

      /* One that is there but switched off is not shown. */
      if (display != NULL && g_str_equal (display, "false"))
        {
          o->skip_depth = 1;
          return;
        }
      if (tag[0] == 'h') o->in_header = TRUE; else o->in_footer = TRUE;
      o->hf_kind = strstr (tag, "-left") != NULL ? W42_PAGE_TEXT_EVEN
                 : strstr (tag, "-first") != NULL ? W42_PAGE_TEXT_FIRST : W42_PAGE_TEXT_DEFAULT;
      g_string_truncate (o->hf_text, 0);
      o->hf_align = W42_ALIGN_LEFT;
    }
  else if ((o->in_header || o->in_footer) && g_str_equal (tag, "page-number"))
    {
      g_string_append (o->hf_text, "{PAGE}");
      o->skip_depth = 1;
    }
  else if ((o->in_header || o->in_footer) && g_str_equal (tag, "page-count"))
    {
      g_string_append (o->hf_text, "{NUMPAGES}");
      o->skip_depth = 1;
    }
  else if ((o->in_header || o->in_footer) && (g_str_equal (tag, "date") || g_str_equal (tag, "time")))
    {
      g_string_append (o->hf_text, "{DATE}");
      o->skip_depth = 1;
    }
  else if ((o->in_header || o->in_footer) && g_str_equal (tag, "p"))
    {
      const char *sn = attr (an, av, "text:style-name");
      OdtStyle *s = resolve_style (o, sn, 0);

      if (s != NULL)
        o->hf_align = s->pa.align;
      if (o->hf_text->len > 0)
        g_string_append_c (o->hf_text, ' ');
    }
  else if ((o->in_header || o->in_footer) && g_str_equal (tag, "s"))
    {
      const char *c = attr (an, av, "text:c");
      int n = c != NULL ? CLAMP (atoi (c), 0, 1000) : 1;

      for (int i = 0; i < n; i++)
        g_string_append_c (o->hf_text, ' ');
    }
  /* A header's tab is what sets its page number at the right; its line
   * break has no line to go to in one line of text. */
  else if ((o->in_header || o->in_footer) && g_str_equal (tag, "tab"))
    g_string_append_c (o->hf_text, '\t');
  else if ((o->in_header || o->in_footer) && g_str_equal (tag, "line-break"))
    g_string_append_c (o->hf_text, ' ');
}

/* The styles LibreOffice writes for its own machinery -- list labels, note
 * anchors, index entries -- which nobody wants in Format > Style. */
static gboolean
odt_internal_style (const char *display)
{
  static const char *const prefixes[] = {
    "ListLabel", "Footnote", "Endnote", "Internet link", "Visited Internet",
    "Bullet", "Numbering", "Contents ", "Index", "Header", "Footer",
    "Table Contents", "Table Heading", "Frame contents", "Drawing",
    "Illustration", "List ", "Text body", "Default", "Standard", "Line numbering",
    "Page number", "Rubies", "Placeholder", "Source Text", "Definition",
    "Strong Emphasis", "Emphasis", "Variable", "User Entry", "Citation",
    "Teletype", "Quotation", "Preformatted", "Hanging", "Salutation",
    "Signature", "Sender", "Addressee", "Marginalia", "Horizontal Line",
    "Heading", "Title", "Subtitle", "Caption", "Graphics", "Frame", "OLE", "Formula"
  };

  for (guint i = 0; i < G_N_ELEMENTS (prefixes); i++)
    if (g_str_has_prefix (display, prefixes[i]))
      return TRUE;
  return FALSE;
}

/* The name word42's own sheet already has for this one, when the file names
 * the same style: "Heading 1" is "Heading 1" in both.  The aliases
 * our_style_name accepts -- a file's "Standard" or "Text body" standing in
 * for Normal -- are deliberately not among them: those say where a
 * paragraph belongs, not what Normal itself looks like.  Except in a file
 * Word42 wrote, whose "Standard" is its Normal: without it a document's
 * Normal -- Calibri, 115% -- came back as Word42's Times. */
static const char *
sheet_style_named (Odt *o, const char *name, const char *display)
{
  W42StyleSheet *sheet = w42_pt_stylesheet (o->pt);

  if (display == NULL)
    return NULL;
  if (o->ours && name != NULL && g_str_equal (name, "Standard"))
    return g_intern_static_string ("Normal");
  for (guint i = 0; i < w42_stylesheet_size (sheet); i++)
    {
      const W42Style *st = w42_stylesheet_get (sheet, i);

      if (g_ascii_strcasecmp (st->name, display) == 0)
        return st->name;
    }
  return NULL;
}

/* The style a named one is based on, by Word42's name for it: its parent,
 * or the nearest of that parent's own parents the sheet has or is to
 * have.  The family's default style is not a style anyone chose. */
static const char *
odt_based_on (Odt *o, const OdtStyle *s)
{
  for (int depth = 0; depth < 16 && s->parent != NULL && s->parent[0] != '@'; depth++)
    {
      const OdtStyle *ps = g_hash_table_lookup (o->styles, s->parent);
      const char *ours;

      if (ps == NULL)
        return NULL;
      ours = our_style_name (o, s->parent, ps->display);
      if (ours != NULL)
        return ours;
      if (ps->named)
        return g_intern_string (ps->display);
      s = ps;
    }
  return NULL;
}

/* The named styles into Word42's sheet, so that the document keeps them
 * and Format > Style shows them.  Each goes in resolved -- what its parents
 * give it, with what it says itself over that -- since the sheet has no
 * parents to ask: taken as read, a heading that said no more than "bold"
 * came in as Times at the default size.  One that is a name Word42 already
 * has -- Heading 1, Title, the Normal a Word42 file writes -- is updated,
 * not skipped: a document that sets its headings in Times must not get
 * Word42's Arial back when it is read. */
static void
odt_register_styles (Odt *o)
{
  W42StyleSheet *sheet = w42_pt_stylesheet (o->pt);

  for (guint i = 0; i < o->named->len; i++)
    {
      const char *name = g_ptr_array_index (o->named, i);
      OdtStyle *s = resolve_style (o, name, 0);
      const char *mine;
      const W42Style *have;
      W42Style st;

      if (s == NULL)
        continue;
      mine = sheet_style_named (o, name, s->display);
      if (mine == NULL && w42_stylesheet_size (sheet) >= 128)
        continue;
      have = mine != NULL ? w42_stylesheet_find (sheet, mine) : NULL;
      if (have != NULL)
        st = *have;
      else
        memset (&st, 0, sizeof st);
      st.name = g_intern_string (mine != NULL ? mine : s->display);
      st.pa = s->pa;
      st.ch = s->ch;
      st.pa.style = st.name;
      if (s->outline_said || have == NULL)
        st.outline = s->own_outline;
      st.character = s->text_family ? 1 : 0;
      st.pa_own = W42_STYLE_PA_ALL;    /* read resolved: all its own */
      st.ch_own = W42_STYLE_CH_ALL;
      st.based_on = odt_based_on (o, s);
      w42_stylesheet_set (sheet, &st);
      if (!st.character)
        s->pa.style = st.name;
    }
  g_ptr_array_set_size (o->named, 0);
}

static void
styles_end (Odt *o, const char *tag)
{
  if (g_str_equal (tag, "styles"))
    o->in_named_styles = FALSE;
  if (g_str_equal (tag, "style") && o->cur_style != NULL && o->in_named_styles &&
      o->cur_style->display != NULL && strlen (o->cur_style->display) < 64 &&
      (o->ours || sheet_style_named (o, o->cur_style_name, o->cur_style->display) != NULL ||
       !odt_internal_style (o->cur_style->display)))
    {
      /* A named style of the file's own joins the sheet -- once the whole
       * of the part is read, since what a style is depends on a parent
       * that may come after it. */
      o->cur_style->named = TRUE;
      g_ptr_array_add (o->named, g_strdup (o->cur_style_name));
    }
  if (g_str_equal (tag, "style") || g_str_equal (tag, "default-style"))
    {
      o->cur_style = NULL;
      g_free (o->cur_col_style);
      o->cur_col_style = NULL;
      g_clear_pointer (&o->cur_section, g_free);
    }
  else if (g_str_equal (tag, "list-style"))
    o->cur_list = NULL;
  else if (g_str_equal (tag, "page-layout"))
    {
      if (o->in_page_layout)
        o->page_seen = TRUE;
      o->in_page_layout = FALSE;
    }
  else if (g_str_equal (tag, "header-style") || g_str_equal (tag, "footer-style"))
    o->in_hf_style = 0;
  else if (g_str_equal (tag, "header") || g_str_equal (tag, "footer") ||
           g_str_equal (tag, "header-left") || g_str_equal (tag, "footer-left") ||
           g_str_equal (tag, "header-first") || g_str_equal (tag, "footer-first"))
    {
      if (tag[0] == 'h')
        w42_pt_set_header_kind (o->pt, o->hf_kind, o->hf_text->str, o->hf_align);
      else
        w42_pt_set_footer_kind (o->pt, o->hf_kind, o->hf_text->str, o->hf_align);

      /* A file that has one says so by having it, an empty one included:
       * a title page with no header is a title page. */
      if (o->hf_kind == W42_PAGE_TEXT_FIRST)
        w42_pt_set_title_page (o->pt, TRUE);
      else if (o->hf_kind == W42_PAGE_TEXT_EVEN)
        w42_pt_set_facing_pages (o->pt, TRUE);
      o->hf_kind = W42_PAGE_TEXT_DEFAULT;
      o->in_header = o->in_footer = FALSE;
    }
}

/* ---- the body --------------------------------------------------------------- */

static W42CharFmt
current_ch (Odt *o)
{
  if (o->span_stack->len > 0)
    return g_array_index (o->span_stack, W42CharFmt, o->span_stack->len - 1);
  return o->para_ch;
}

static void
odt_flush (Odt *o)
{
  if (o->text->len == 0)
    return;
  if (o->field != NULL)
    {
      g_string_append (o->field_text, o->text->str);
      g_string_truncate (o->text, 0);
      return;
    }
  o->b.ch = current_ch (o);
  o->b.ch.link = o->link;
  {
    gsize start = o->b.pos;

    w42_builder_text (&o->b, o->text->str);
    if (o->pending_comment != NULL && o->b.pos > start)
      {
        /* An annotation from before ODF 1.2 has no end: it marks the
         * run that follows it. */
        W42CharFmt want;

        memset (&want, 0, sizeof want);
        want.comment = g_intern_string (o->pending_comment);
        w42_pt_apply_char_fmt (o->pt, start, o->b.pos - start, W42_CHAR_COMMENT, &want);
        g_clear_pointer (&o->pending_comment, g_free);
      }
  }
  g_string_truncate (o->text, 0);
}

/* The cell whose element has been seen but whose first paragraph has not:
 * it is begun here, so that its properties have a mark to sit on. */
static void
open_pending_cell (Odt *o)
{
  if (!o->cell_pending)
    return;
  w42_builder_begin_cell (&o->b, o->cell_span);
  o->cell_pending = FALSE;
  if (o->cell_span > 1)
    o->skip_covered += o->cell_span - 1;   /* the covered cells it stands for */
  if (o->b.cell_pos == (gsize) -1)
    return;
  if (o->pending_cell_sides >= 0)
    w42_pt_cell_set_borders_at (o->pt, o->b.cell_pos, o->pending_cell_sides);
  if (o->pending_cell_lines != NULL)
    {
      w42_pt_cell_set_edges_at (o->pt, o->b.cell_pos, o->pending_cell_lines->edge);
      if (o->pending_cell_lines->valign != W42_CELL_VALIGN_TOP)
        w42_pt_cell_set_valign_at (o->pt, o->b.cell_pos, (W42CellVAlign) o->pending_cell_lines->valign);
    }
  if (o->pending_cell_fill != 0)
    w42_pt_cell_set_fill_at (o->pt, o->b.cell_pos, TRUE,
                             o->pending_cell_fill & 0xFFFFFF);
  if (o->cell_vspan != 1)
    w42_pt_set_cell_vspan (o->pt, o->b.cell_pos, o->cell_vspan);
}

/* The kind of shape a draw:enhanced-geometry describes: by its type, or
 * for the "non-primitive" ones Word writes, by the look of its path. */
static W42ShapeKind
enhanced_geometry_kind (const char **an, const char **av)
{
  const char *type = attr (an, av, "draw:type");
  const char *path = attr (an, av, "draw:enhanced-path");

  if (type != NULL)
    {
      if (g_str_equal (type, "ellipse")) return W42_SHAPE_ELLIPSE;
      if (g_str_equal (type, "round-rectangle")) return W42_SHAPE_ROUNDED_RECTANGLE;
      if (g_str_equal (type, "rectangle")) return W42_SHAPE_RECTANGLE;
      if (g_str_equal (type, "line")) return W42_SHAPE_LINE;
    }
  if (path != NULL)
    {
      if (strstr (path, " A ") != NULL || strstr (path, " W ") != NULL || strstr (path, " U ") != NULL)
        return W42_SHAPE_ELLIPSE;
      if (g_str_has_prefix (path, "M ?f0 ?f2 L ?f1 ?f3") || strstr (path, "L") == NULL)
        return W42_SHAPE_LINE;
      {
        /* A rectangle's path has four corners; a line's, two. */
        int points = 0;

        for (const char *c = path; *c != '\0'; c++)
          if (*c == 'L' || *c == 'M')
            points++;
        if (points <= 2 && strstr (path, "Z") == NULL)
          return W42_SHAPE_LINE;
      }
    }
  return W42_SHAPE_RECTANGLE;
}

/* An item's number, kept as Word42 counts it.  Word42 numbers a run of
 * items and starts again after anything else, where OpenDocument numbers
 * a list's items whatever stands between them: an unnumbered paragraph in
 * an item, a header, and a list that starts over are said here as the
 * number the next item starts at. */
static void
list_number (Odt *o, W42ListKind kind, int level)
{
  if (w42_list_is_numbered (kind))
    {
      if (o->item_start > 0)
        o->b.pa.list_start = (guint8) CLAMP (o->item_start, 1, 255);
      else if (o->list_restart && o->last_numbered && o->last_kind == kind &&
               o->last_level == level)
        o->b.pa.list_start = 1;
      else if (o->list_gap && o->list_n[level] > 0)
        o->b.pa.list_start = (guint8) CLAMP (o->list_n[level] + 1, 1, 255);
      o->list_n[level] = o->b.pa.list_start > 0 ? o->b.pa.list_start : o->list_n[level] + 1;
      for (int deeper = level + 1; deeper < 9; deeper++)
        o->list_n[deeper] = 0;
    }
  o->item_start = 0;
  o->list_restart = FALSE;
  o->list_gap = FALSE;
  o->last_numbered = w42_list_is_numbered (kind);
  o->last_kind = kind;
  o->last_level = level;
}

/* A paragraph, with the style `s`, has begun: if it is the first of a
 * text:section, it starts the section, with the section's columns.
 * Word42 writes every section break so; another program's section is a
 * section break when it starts a page, and otherwise just a part of the
 * text with a name, which Word42 has no section for: it breaks no page. */
static void
odt_section_para (Odt *o, const OdtStyle *s)
{
  if (o->section_pending)
    {
      if (o->ours || o->b.pa.page_break_before || (s != NULL && s->master_page))
        {
          o->b.pa.section_break = 1;
          o->b.pa.columns = (guint8) CLAMP (o->section.count, 1, 9);
          o->b.pa.column_gap = o->section.gap;
        }
      else
        o->section.count = 1;         /* not taken: nothing to end either */
      o->section_pending = FALSE;
      o->section_reset = FALSE;
    }
  else if (o->section_reset && o->section_depth == 0)
    {
      o->b.pa.section_break = 1;
      o->b.pa.columns = 1;
      o->b.pa.column_gap = 0;
      o->section_reset = FALSE;
    }
}

static void
body_start (Odt *o, const char *tag, const char **an, const char **av)
{
  if (o->in_annotation)
    {
      /* Its paragraphs are the comment's text, spaces and all. */
      if (g_str_equal (tag, "s"))
        {
          const char *c = attr (an, av, "text:c");
          int n = c != NULL ? CLAMP (atoi (c), 1, 1000) : 1;

          for (int i = 0; i < n; i++)
            g_string_append_c (o->annotation, ' ');
        }
      else if (g_str_equal (tag, "tab"))
        g_string_append_c (o->annotation, '\t');
      else if (g_str_equal (tag, "line-break"))
        g_string_append_c (o->annotation, '\n');
      return;
    }
  if (o->shape_open)
    {
      /* Inside a shape: its geometry, and its text as a label. */
      if (g_str_equal (tag, "enhanced-geometry"))
        {
          if (!o->shape_primitive_known)
            o->shape_kind = enhanced_geometry_kind (an, av);
        }
      else if (g_str_equal (tag, "line-break"))
        g_string_append_c (o->shape_text, '\n');
      else if (g_str_equal (tag, "s") || g_str_equal (tag, "tab"))
        g_string_append_c (o->shape_text, ' ');
      return;
    }
  if (g_str_equal (tag, "custom-shape") || g_str_equal (tag, "rect") ||
      g_str_equal (tag, "ellipse") || g_str_equal (tag, "line"))
    {
      const char *anchor = attr (an, av, "text:anchor-type");
      const char *sname = attr (an, av, "draw:style-name");
      gpointer w = sname != NULL ? g_hash_table_lookup (o->graphic_wraps, sname) : NULL;

      o->shape_open = TRUE;
      o->shape_primitive_known = !g_str_equal (tag, "custom-shape");
      o->shape_kind = g_str_equal (tag, "rect") ? W42_SHAPE_RECTANGLE
                    : g_str_equal (tag, "ellipse") ? W42_SHAPE_ELLIPSE
                    : g_str_equal (tag, "line") ? W42_SHAPE_LINE : W42_SHAPE_RECTANGLE;
      o->shape_style = sname != NULL ? g_hash_table_lookup (o->graphics, sname) : NULL;
      if (g_str_equal (tag, "line"))
        {
          int x1 = length_twips (attr (an, av, "svg:x1")), y1 = length_twips (attr (an, av, "svg:y1"));
          int x2 = length_twips (attr (an, av, "svg:x2")), y2 = length_twips (attr (an, av, "svg:y2"));

          o->shape_x = MIN (x1, x2);
          o->shape_y = MIN (y1, y2);
          o->shape_w = ABS (x2 - x1);
          o->shape_h = ABS (y2 - y1);
        }
      else
        {
          o->shape_x = length_twips (attr (an, av, "svg:x"));
          o->shape_y = length_twips (attr (an, av, "svg:y"));
          o->shape_w = length_twips (attr (an, av, "svg:width"));
          o->shape_h = length_twips (attr (an, av, "svg:height"));
        }
      o->shape_wrap = W42_WRAP_INLINE;
      o->shape_positioned = FALSE;
      if (anchor != NULL && !g_str_equal (anchor, "as-char"))
        {
          o->shape_positioned = TRUE;
          if (w != NULL && (W42Wrap) (GPOINTER_TO_INT (w) - 1) != W42_WRAP_INLINE)
            o->shape_wrap = (W42Wrap) (GPOINTER_TO_INT (w) - 1);
          else
            o->shape_wrap = o->shape_style != NULL && o->shape_style->behind ? W42_WRAP_BEHIND : W42_WRAP_FRONT;
        }
      g_string_truncate (o->shape_text, 0);
      return;
    }
  if (g_str_equal (tag, "h") || g_str_equal (tag, "p"))
    {
      const char *sn = attr (an, av, "text:style-name");
      OdtStyle *s = resolve_style (o, sn, 0);
      W42Fmt def;

      if (s == NULL)
        s = resolve_style (o, "@default-paragraph", 0);
      open_pending_cell (o);
      if (o->in_note > 0)
        {
          if (!o->note_first_para)
            w42_builder_end_paragraph (&o->b);
          o->note_first_para = FALSE;
        }
      w42_fmt_init_default (&def);
      w42_builder_reset_para (&o->b);
      o->para_ch = def.ch;
      if (s != NULL)
        {
          o->b.pa = s->pa;
          o->para_ch = s->ch;
          if (o->b.pa.style == NULL)
            o->b.pa.style = def.pa.style;
        }
      if (o->in_note == 0)
        odt_section_para (o, s);
      if (g_str_equal (tag, "h"))
        {
          const char *lvl = attr (an, av, "text:outline-level");
          int level = lvl != NULL ? CLAMP (atoi (lvl), 1, 9) : (s != NULL && s->outline > 0 ? s->outline : 1);

          /* The level makes a heading of a paragraph in a body style.  One
           * in a heading style of the file's own keeps that style and the
           * text formatting it resolved to: made Heading 1, a custom
           * heading came back in Word42's Arial. */
          if (level <= 3 && (s == NULL || s->pa.style == NULL ||
                             g_str_equal (s->pa.style, "Normal")))
            {
              char *hn = g_strdup_printf ("Heading %d", level);
              const W42Style *st = w42_stylesheet_find (w42_pt_stylesheet (o->pt), hn);

              if (st != NULL)
                {
                  o->b.pa.style = st->name;
                  if (s == NULL)
                    o->para_ch = st->ch;
                }
              g_free (hn);
            }
        }
      /* In a list: the kind from the list style, the level from the
       * nesting.  Only an item's first paragraph has the number; the rest
       * of the item, and a list's header, are set in with it unnumbered. */
      if (o->list_depth > 0)
        {
          const char *ls_name = o->list_style_stack->len > 0 ? g_ptr_array_index (o->list_style_stack, o->list_style_stack->len - 1) : NULL;
          OdtListStyle *ls = ls_name != NULL ? g_hash_table_lookup (o->list_styles, ls_name) : NULL;
          int level = CLAMP (o->list_depth - 1, 0, 8);
          int item = MIN (o->list_depth, 10) - 1;

          if (ls == NULL && s != NULL && s->list_style != NULL)
            ls = g_hash_table_lookup (o->list_styles, s->list_style);
          if (o->b.pa.indent_left == 0)
            o->b.pa.indent_left = 360 * (level + 1);
          if (o->item_fresh[item])
            {
              W42ListKind kind = ls != NULL ? ls->kind[level] : W42_LIST_NUMBER;

              o->item_fresh[item] = FALSE;
              o->b.pa.list = (guint8) kind;
              o->b.pa.list_level = (guint8) level;
              if (o->b.pa.indent_first == 0)
                o->b.pa.indent_first = -360;
              list_number (o, kind, level);
            }
          else
            o->list_gap = TRUE;
        }
      else if (o->tb_depth > 0 && o->tb_saved_depth > 0)
        o->list_gap = TRUE;           /* a box beside the list it stands in */
      else
        {
          /* A paragraph outside every list ends the numbering, in Word42's
           * count as in OpenDocument's. */
          memset (o->list_n, 0, sizeof o->list_n);
          o->list_gap = FALSE;
          o->last_numbered = FALSE;
        }
      g_array_set_size (o->span_stack, 0);
      o->after_space = TRUE;
      if (o->tb_depth > 0)
        {
          o->b.pa.frame_side = (guint8) o->tb_side;
          o->b.pa.frame_width = CLAMP (o->tb_width, 0, 31680);
        }
      o->para_open = TRUE;
      o->tb_reopened = FALSE;
    }
  else if (!o->para_open && !g_str_equal (tag, "list") && !g_str_equal (tag, "list-item") &&
           !g_str_equal (tag, "table") && !g_str_equal (tag, "table-row") &&
           !g_str_equal (tag, "table-cell") && !g_str_equal (tag, "covered-table-cell") &&
           !g_str_equal (tag, "table-column") && !g_str_equal (tag, "section") &&
           !g_str_equal (tag, "bookmark-start") && !g_str_equal (tag, "bookmark-end") &&
           !g_str_equal (tag, "annotation") && !g_str_equal (tag, "annotation-end"))
    {
      /* Text-level things outside a paragraph: skipped. */
    }
  if (g_str_equal (tag, "span"))
    {
      const char *sn = attr (an, av, "text:style-name");
      OdtStyle *s = resolve_style (o, sn, 0);
      W42CharFmt ch = current_ch (o);

      odt_flush (o);
      if (s != NULL)
        span_props (o, s, &ch, 0);
      g_array_append_val (o->span_stack, ch);
    }
  else if (g_str_equal (tag, "a"))
    {
      const char *href = attr (an, av, "xlink:href");

      odt_flush (o);
      o->link = href != NULL ? g_intern_string (href) : NULL;
    }
  else if (g_str_equal (tag, "s"))
    {
      const char *c = attr (an, av, "text:c");
      int n = c != NULL ? CLAMP (atoi (c), 1, 1000) : 1;

      for (int i = 0; i < n; i++)
        g_string_append_c (o->text, ' ');
      /* A space written after text:s is a space of its own, as it is
       * after a tab or a break: LibreOffice keeps it. */
      o->after_space = FALSE;
    }
  else if (g_str_equal (tag, "tab"))
    {
      g_string_append_c (o->text, '\t');
      o->after_space = FALSE;
    }
  else if (g_str_equal (tag, "line-break"))
    {
      g_string_append (o->text, "\342\200\250");
      o->after_space = FALSE;
    }
  else if (g_str_equal (tag, "soft-hyphen"))
    {
      g_string_append (o->text, "\302\255");
      o->after_space = FALSE;
    }
  else if (g_str_equal (tag, "bookmark-start"))
    {
      const char *name = attr (an, av, "text:name");

      odt_flush (o);
      if (name != NULL)
        g_hash_table_insert (o->bookmark_start, g_strdup (name), GSIZE_TO_POINTER (o->b.pos));
    }
  else if (g_str_equal (tag, "bookmark-end"))
    {
      const char *name = attr (an, av, "text:name");
      gpointer start = name != NULL ? g_hash_table_lookup (o->bookmark_start, name) : NULL;

      odt_flush (o);
      if (start != NULL && o->b.pos > GPOINTER_TO_SIZE (start))
        {
          W42CharFmt want;

          memset (&want, 0, sizeof want);
          want.bookmark = g_intern_string (name);
          w42_pt_apply_char_fmt (o->pt, GPOINTER_TO_SIZE (start), o->b.pos - GPOINTER_TO_SIZE (start),
                                 W42_CHAR_BOOKMARK, &want);
        }
    }
  else if (g_str_equal (tag, "annotation"))
    {
      const char *name = attr (an, av, "office:name");

      odt_flush (o);
      o->in_annotation = TRUE;
      g_string_truncate (o->annotation, 0);
      o->annotation_pos = o->b.pos;
      g_free (o->annotation_name);
      o->annotation_name = g_strdup (name);
    }
  else if (g_str_equal (tag, "annotation-end"))
    {
      const char *name = attr (an, av, "office:name");
      char *text = name != NULL ? g_hash_table_lookup (o->annotation_start, name) : NULL;
      gpointer start = name != NULL ? g_hash_table_lookup (o->bookmark_start, name) : NULL;

      odt_flush (o);
      if (text != NULL && start != NULL && o->b.pos > GPOINTER_TO_SIZE (start))
        {
          W42CharFmt want;

          memset (&want, 0, sizeof want);
          want.comment = g_intern_string (text);
          w42_pt_apply_char_fmt (o->pt, GPOINTER_TO_SIZE (start), o->b.pos - GPOINTER_TO_SIZE (start),
                                 W42_CHAR_COMMENT, &want);
        }
    }
  else if (g_str_equal (tag, "note"))
    {
      const char *cls = attr (an, av, "text:note-class");

      odt_flush (o);
      if (o->in_note == 0 && !w42_builder_in_table (&o->b))
        {
          o->note_outer_pa = o->b.pa;
          o->note_outer_ch = o->para_ch;
          /* The spans and the link the mark stands in are the text's
           * after the note, not the note's own. */
          if (o->note_outer_spans != NULL)
            g_array_unref (o->note_outer_spans);
          o->note_outer_spans = g_array_copy (o->span_stack);
          o->note_outer_link = o->link;
          /* The mark is set in the text around it. */
          o->b.ch = current_ch (o);
          o->b.ch.link = o->link;
          o->link = NULL;
          w42_builder_begin_note (&o->b, cls != NULL && g_str_equal (cls, "endnote"));
          o->in_note = 1;
          o->note_first_para = TRUE;
        }
      else
        o->skip_depth = 1;
    }
  else if (g_str_equal (tag, "note-citation"))
    o->skip_depth = 1;
  else if (g_str_equal (tag, "list"))
    {
      const char *sn = attr (an, av, "text:style-name");

      odt_flush (o);
      if (o->para_open)
        {
          w42_builder_end_paragraph (&o->b);
          o->para_open = FALSE;
        }
      if (o->list_depth > 0)
        {
          /* A list that opens an item: what follows it in the item is
           * not the item's numbered paragraph. */
          o->item_fresh[MIN (o->list_depth, 10) - 1] = FALSE;
        }
      else
        {
          /* A list starts over unless it says it goes on. */
          const char *cont = attr (an, av, "text:continue-numbering");

          o->list_restart = !(cont != NULL && g_str_equal (cont, "true")) &&
                            attr (an, av, "text:continue-list") == NULL;
        }
      o->list_depth++;
      o->item_fresh[MIN (o->list_depth, 10) - 1] = FALSE;
      if (sn == NULL && o->list_style_stack->len > 0)
        sn = g_ptr_array_index (o->list_style_stack, o->list_style_stack->len - 1);
      g_ptr_array_add (o->list_style_stack, g_strdup (sn != NULL ? sn : ""));
    }
  else if (g_str_equal (tag, "section"))
    {
      /* A section in the body -- not one in a note, a cell or a box --
       * begins with its first paragraph, which starts it. */
      if (o->in_note == 0 && o->in_table == 0 && o->tb_depth == 0 && o->list_depth == 0 &&
          ++o->section_depth == 1)
        {
          const char *sn = attr (an, av, "text:style-name");
          const OdtColumns *c = sn != NULL ? g_hash_table_lookup (o->section_cols, sn) : NULL;

          o->section_pending = TRUE;
          o->section.count = c != NULL ? c->count : 1;
          o->section.gap = c != NULL ? c->gap : 0;
        }
    }
  else if (g_str_equal (tag, "list-item") && o->list_depth > 0)
    {
      const char *start = attr (an, av, "text:start-value");

      o->item_fresh[MIN (o->list_depth, 10) - 1] = TRUE;
      o->item_start = start != NULL ? CLAMP (atoi (start), 0, 255) : 0;
    }
  else if (g_str_equal (tag, "list-header") && o->list_depth > 0)
    o->item_fresh[MIN (o->list_depth, 10) - 1] = FALSE;
  else if (g_str_equal (tag, "table"))
    {
      odt_flush (o);
      if (o->para_open)
        {
          w42_builder_end_paragraph (&o->b);
          o->para_open = FALSE;
        }
      o->in_table++;
      if (o->in_table == 1)
        {
          g_array_set_size (o->table_widths, 0);
          o->table_started = FALSE;
          o->table_row = 0;
          o->in_header_rows = FALSE;
        }
    }
  else if (g_str_equal (tag, "table-header-rows") && o->in_table == 1)
    o->in_header_rows = TRUE;
  else if (g_str_equal (tag, "table-column") && o->in_table == 1)
    {
      const char *sn = attr (an, av, "table:style-name");
      const char *rep = attr (an, av, "table:number-columns-repeated");
      int n = rep != NULL ? CLAMP (atoi (rep), 1, 63) : 1;
      int w = sn != NULL ? GPOINTER_TO_INT (g_hash_table_lookup (o->col_widths, sn)) : 0;

      for (int i = 0; i < n; i++)
        g_array_append_val (o->table_widths, w);
    }
  else if (g_str_equal (tag, "table-row") && o->in_table == 1)
    {
      const char *sn = attr (an, av, "table:style-name");
      int h = sn != NULL ? GPOINTER_TO_INT (g_hash_table_lookup (o->row_heights, sn)) : 0;

      if (!o->table_started)
        {
          int n = (int) o->table_widths->len;

          w42_builder_begin_table (&o->b, n > 0 ? n : 1, n > 0 ? (const int *) o->table_widths->data : NULL);
          /* ODF has no rules of its own on a table: every rule belongs to
           * a cell style, so the table is left unruled and the cells say
           * what they want. */
          w42_pt_table_set_borders (o->pt, o->b.table, FALSE);
          o->table_started = TRUE;
        }
      if (h > 0)
        w42_pt_table_set_row_height (o->b.pt, o->b.table, o->table_row, h);
      if (o->in_header_rows)
        w42_pt_table_set_header_rows (o->b.pt, o->b.table, o->table_row + 1);
      o->table_row++;
      o->skip_covered = 0;
    }
  else if ((g_str_equal (tag, "table-cell") || g_str_equal (tag, "covered-table-cell")) &&
           o->in_table == 1)
    {
      const char *span = attr (an, av, "table:number-columns-spanned");
      const char *rows = attr (an, av, "table:number-rows-spanned");

      if (g_str_equal (tag, "covered-table-cell") && o->skip_covered > 0)
        {
          o->skip_covered--;          /* a cell swallowed sideways: it is already there */
          return;
        }
      o->cell_pending = TRUE;
      o->cell_span = span != NULL ? CLAMP (atoi (span), 1, 63) : 1;
      {
        const char *rep = attr (an, av, "table:number-columns-repeated");

        o->cell_repeat = rep != NULL ? CLAMP (atoi (rep), 1, 63) : 1;
      }
      /* A covered cell is one the merge above it has swallowed; the cell
       * that owns the merge says how many rows it takes. */
      if (g_str_equal (tag, "covered-table-cell"))
        o->cell_vspan = W42_CELL_COVERED;
      else if (rows != NULL && atoi (rows) > 1)
        o->cell_vspan = CLAMP (atoi (rows), 1, 254);
      else
        o->cell_vspan = 1;
      {
        const char *sn = attr (an, av, "table:style-name");
        gpointer v = sn != NULL ? g_hash_table_lookup (o->cell_sides, sn) : NULL;

        o->pending_cell_sides = v != NULL ? (GPOINTER_TO_INT (v) & W42_BORDER_BOX) : -1;
        o->pending_cell_lines = sn != NULL ? g_hash_table_lookup (o->cell_lines, sn) : NULL;
        {
          gpointer f = sn != NULL ? g_hash_table_lookup (o->cell_fills, sn) : NULL;

          o->pending_cell_fill = f != NULL ? GPOINTER_TO_UINT (f) : 0;
        }
      }
    }
  else if (g_str_equal (tag, "frame"))
    {
      const char *anchor = attr (an, av, "text:anchor-type");
      const char *sname = attr (an, av, "draw:style-name");
      gpointer w = sname != NULL ? g_hash_table_lookup (o->graphic_wraps, sname) : NULL;

      o->frame_pending = TRUE;
      o->frame_w = length_twips (attr (an, av, "svg:width"));
      o->frame_h = length_twips (attr (an, av, "svg:height"));
      o->frame_wrap = W42_WRAP_INLINE;
      o->frame_x = length_twips (attr (an, av, "svg:x"));
      o->frame_y = length_twips (attr (an, av, "svg:y"));
      o->frame_positioned = anchor != NULL && !g_str_equal (anchor, "as-char") &&
                            (attr (an, av, "svg:x") != NULL || attr (an, av, "svg:y") != NULL);
      if (anchor != NULL && !g_str_equal (anchor, "as-char") && w != NULL)
        o->frame_wrap = (W42Wrap) (GPOINTER_TO_INT (w) - 1);
      if (anchor != NULL && !g_str_equal (anchor, "as-char") && o->frame_wrap == W42_WRAP_INLINE)
        {
          const OdtGraphic *g = sname != NULL ? g_hash_table_lookup (o->graphics, sname) : NULL;

          /* No wrapping said, or run-through: the text runs on under or
           * over it. */
          if (w != NULL || g != NULL)
            o->frame_wrap = g != NULL && g->behind ? W42_WRAP_BEHIND : W42_WRAP_FRONT;
        }
      g_free (o->frame_href);
      o->frame_href = NULL;
      g_clear_pointer (&o->frame_first_href, g_free);
    }
  else if (g_str_equal (tag, "text-box") && o->frame_pending && o->tb_depth == 0)
    {
      /* A text box: its paragraphs are framed at the side the frame's
       * style puts it, and the paragraph it hangs on goes on after it. */
      o->tb_depth = 1;
      o->tb_side = o->frame_wrap == W42_WRAP_RIGHT ? W42_FRAME_RIGHT : W42_FRAME_LEFT;
      o->tb_width = o->frame_w;
      o->tb_saved_pa = o->b.pa;
      if (o->tb_saved_pa.section_break && !o->b.in_para)
        {
          /* The paragraph the box hangs on started a section, and has
           * nothing of its own to keep it: the box's first paragraph is
           * the one Word42 has it on. */
          o->section_pending = TRUE;
          o->section.count = MAX (o->tb_saved_pa.columns, 1);
          o->section.gap = o->tb_saved_pa.column_gap;
          o->tb_saved_pa.section_break = 0;
          o->tb_saved_pa.columns = 0;
          o->tb_saved_pa.column_gap = 0;
        }
      o->tb_saved_ch = o->para_ch;
      /* The box's text is not in the spans, the link or the list the
       * paragraph it hangs on is in; they go on after it. */
      if (o->tb_saved_spans != NULL)
        g_array_unref (o->tb_saved_spans);
      o->tb_saved_spans = g_array_copy (o->span_stack);
      o->tb_saved_link = o->link;
      o->link = NULL;
      o->tb_saved_depth = o->list_depth;
      memcpy (o->tb_saved_fresh, o->item_fresh, sizeof o->item_fresh);
      o->list_depth = 0;
      o->frame_pending = FALSE;
      if (o->para_open)
        {
          odt_flush (o);
          if (o->b.in_para)
            w42_builder_end_paragraph (&o->b);
          o->para_open = FALSE;
        }
    }
  else if (g_str_equal (tag, "text-box"))
    o->tb_depth++;                  /* a box inside a box: counted, not started */
  else if (g_str_equal (tag, "image") && o->frame_pending)
    {
      /* A frame may hold the picture more than once: the one to use, then
       * others for a reader that cannot use it.  The last is the one read,
       * as it always was; the first is kept when it is a picture only it
       * holds, a metafile beside its replacement. */
      g_free (o->frame_href);
      o->frame_href = g_strdup (attr (an, av, "xlink:href"));
      if (o->frame_first_href == NULL && o->frame_href != NULL)
        o->frame_first_href = g_strdup (o->frame_href);
    }
  else if (g_str_equal (tag, "alphabetical-index-mark-start"))
    {
      const char *term = attr (an, av, "text:string-value");

      odt_flush (o);
      g_free (o->index_term);
      o->index_term = g_strdup (term != NULL ? term : "");
      o->index_start = o->b.pos;
    }
  else if (g_str_equal (tag, "page-number") || g_str_equal (tag, "page-count") ||
           g_str_equal (tag, "date") || g_str_equal (tag, "time") ||
           g_str_equal (tag, "file-name") || g_str_equal (tag, "word-count"))
    {
      odt_flush (o);
      o->field = g_str_equal (tag, "page-number") ? "PAGE" : g_str_equal (tag, "page-count") ? "NUMPAGES"
               : g_str_equal (tag, "date") ? "DATE" : g_str_equal (tag, "time") ? "TIME"
               : g_str_equal (tag, "file-name") ? "FILENAME" : "NUMWORDS";
      g_string_truncate (o->field_text, 0);
    }
}

/* A picture the frame names, from the package.  One the text names again
 * and again -- a logo on every page -- is unpacked once and shared, not
 * once a time: forty of one 20 MB part took 800 MB.  Empty when it is not
 * there. */
static GBytes *
odt_picture (Odt *o, const char *href)
{
  GBytes *bytes = g_hash_table_lookup (o->pictures, href);

  if (bytes == NULL)
    {
      bytes = w42_zip_read (o->zip, href);
      if (bytes == NULL)
        bytes = g_bytes_new (NULL, 0);      /* not there: not looked for again */
      g_hash_table_insert (o->pictures, g_strdup (href), bytes);
    }
  return bytes;
}

/* The kind of picture a part is, from its name: its extension, letters
 * and digits only, since it goes back out into a part's name and a media
 * type; "picture" when it has none. */
static char *
odt_picture_kind (const char *href)
{
  const char *dot = strrchr (href, '.'), *slash = strrchr (href, '/');
  GString *ext = g_string_new (NULL);

  if (dot != NULL && (slash == NULL || dot > slash))
    for (const char *q = dot + 1; *q != '\0' && ext->len < 8; q++)
      if (g_ascii_isalnum (*q))
        g_string_append_c (ext, g_ascii_tolower (*q));
  if (ext->len == 0)
    g_string_assign (ext, "picture");
  return g_string_free (ext, FALSE);
}

/* The picture a frame holds goes in where the text has got to: the last
 * of its pictures this machine can draw, as a frame's picture was always
 * read.  A first one it cannot draw -- a metafile, beside its replacement
 * or alone -- is kept as it came, for the file to have back when it is
 * saved: behind the replacement when there is one, else behind a box with
 * its kind written in it, as the .docx reader keeps one. */
static void
odt_frame_picture (Odt *o)
{
  GBytes *last = odt_picture (o, o->frame_href);
  const char *first_href = o->frame_first_href != NULL && !g_str_equal (o->frame_first_href, o->frame_href)
                             ? o->frame_first_href : o->frame_href;
  GBytes *first = first_href != o->frame_href ? odt_picture (o, first_href) : last;
  char *kind = odt_picture_kind (first_href);
  int pw = 0, ph = 0;
  const char *format = NULL;
  GBytes *use = NULL, *kept = NULL;
  /* Decoding a picture to learn that it is one is not free: a first one
   * named as an ordinary picture is taken to be one. */
  static const char *const ordinary[] = { "png", "jpg", "jpeg", "gif", "bmp", "svg", "svgz",
                                          "webp", "tif", "tiff", "avif", NULL };

  if (g_bytes_get_size (last) > 0 && w42_image_probe (last, &pw, &ph, &format))
    use = last;
  if (g_bytes_get_size (first) > 0 && (use == NULL || first != last))
    {
      if (use == NULL && first != last && w42_image_probe (first, &pw, &ph, &format))
        use = first;
      else if (use == NULL || !g_strv_contains (ordinary, kind))
        kept = use == NULL || !w42_image_probe (first, NULL, NULL, NULL) ? first : NULL;
    }

  o->b.ch = current_ch (o);
  o->b.last_object = W42_OBJECT_NONE;
  if (use != NULL)
    w42_builder_object (&o->b, use, format, pw, ph, o->frame_w, o->frame_h);
  else if (kept != NULL)
    {
      char *label = g_strdup_printf ("%s picture", kind);

      for (char *q = label; *q != '\0' && *q != ' '; q++)
        *q = g_ascii_toupper (*q);
      w42_builder_shape (&o->b, W42_SHAPE_RECTANGLE,
                         o->frame_w > 0 ? o->frame_w : 1440, o->frame_h > 0 ? o->frame_h : 1440,
                         0.75, 0x999999, FALSE, 0xFFFFFF, label);
      g_free (label);
    }
  if (o->b.last_object != W42_OBJECT_NONE)
    {
      if (kept != NULL)
        w42_object_table_set_original (w42_pt_object_table (o->pt), o->b.last_object, kept, kind);
      w42_builder_object_wrap (&o->b, o->frame_wrap);
      if (o->frame_wrap != W42_WRAP_INLINE && o->frame_positioned)
        w42_builder_object_position (&o->b, o->frame_x, o->frame_y);
      o->after_space = FALSE;
    }
  g_free (kind);
}

static void
body_end (Odt *o, const char *tag)
{
  if (o->in_annotation && !g_str_equal (tag, "annotation"))
    {
      if (g_str_equal (tag, "p"))
        g_string_append_c (o->annotation, ' ');
      return;
    }
  if (o->shape_open)
    {
      if (g_str_equal (tag, "p"))
        g_string_append_c (o->shape_text, '\n');
      else if (g_str_equal (tag, "custom-shape") || g_str_equal (tag, "rect") ||
               g_str_equal (tag, "ellipse") || g_str_equal (tag, "line"))
        {
          const OdtGraphic *g = o->shape_style;
          W42ShapeKind kind = o->shape_kind;

          o->shape_open = FALSE;
          while (o->shape_text->len > 0 && o->shape_text->str[o->shape_text->len - 1] == '\n')
            g_string_truncate (o->shape_text, o->shape_text->len - 1);
          if (kind == W42_SHAPE_LINE && g != NULL && g->arrow)
            kind = W42_SHAPE_ARROW;
          odt_flush (o);
          open_pending_cell (o);
          o->b.ch = current_ch (o);
          w42_builder_shape (&o->b, kind, o->shape_w, o->shape_h,
                             g == NULL || g->stroked ? (g != NULL ? g->stroke_pt : 0.75) : 0.0,
                             g != NULL ? g->stroke : 0,
                             g != NULL ? g->filled : FALSE, g != NULL ? g->fill : 0xFFFFFF,
                             o->shape_text->len > 0 ? o->shape_text->str : NULL);
          if (o->shape_wrap != W42_WRAP_INLINE)
            {
              w42_builder_object_wrap (&o->b, o->shape_wrap);
              if (o->shape_positioned)
                w42_builder_object_position (&o->b, o->shape_x, o->shape_y);
            }
          g_string_truncate (o->shape_text, 0);
        }
      return;
    }
  if (g_str_equal (tag, "text-box") && o->tb_depth > 1)
    {
      o->tb_depth--;
      return;
    }
  if (g_str_equal (tag, "text-box") && o->tb_depth > 0)
    {
      odt_flush (o);
      if (o->para_open && o->b.in_para)
        w42_builder_end_paragraph (&o->b);
      o->tb_depth = 0;
      o->b.pa = o->tb_saved_pa;
      o->para_ch = o->tb_saved_ch;
      g_array_set_size (o->span_stack, 0);
      if (o->tb_saved_spans != NULL)
        {
          g_array_append_vals (o->span_stack, o->tb_saved_spans->data, o->tb_saved_spans->len);
          g_clear_pointer (&o->tb_saved_spans, g_array_unref);
        }
      o->link = o->tb_saved_link;
      o->list_depth = o->tb_saved_depth;
      memcpy (o->item_fresh, o->tb_saved_fresh, sizeof o->item_fresh);
      o->para_open = TRUE;
      o->tb_reopened = TRUE;
      o->after_space = FALSE;
      return;
    }
  if (g_str_equal (tag, "h") || g_str_equal (tag, "p"))
    {
      odt_flush (o);
      if (o->tb_reopened && !o->b.in_para)
        {
          /* The paragraph the box hung on had nothing else: no empty
           * paragraph for it. */
          o->tb_reopened = FALSE;
          o->para_open = FALSE;
          return;
        }
      o->tb_reopened = FALSE;
      open_pending_cell (o);
      if (o->in_note == 0)
        w42_builder_end_paragraph (&o->b);
      o->para_open = FALSE;
    }
  else if (g_str_equal (tag, "span"))
    {
      odt_flush (o);
      if (o->span_stack->len > 0)
        g_array_set_size (o->span_stack, o->span_stack->len - 1);
    }
  else if (g_str_equal (tag, "a"))
    {
      odt_flush (o);
      o->link = NULL;
    }
  else if (g_str_equal (tag, "annotation"))
    {
      /* The text is kept until annotation-end says where it ends; an
       * annotation with no end marks the run that follows it. */
      char *clean = g_strstrip (g_strdup (o->annotation->str));

      o->in_annotation = FALSE;
      if (o->annotation_name != NULL)
        {
          g_hash_table_insert (o->annotation_start, g_strdup (o->annotation_name), g_strdup (clean));
          g_hash_table_insert (o->bookmark_start, g_strdup (o->annotation_name), GSIZE_TO_POINTER (o->annotation_pos));
          g_free (clean);
        }
      else if (*clean != '\0')
        {
          g_free (o->pending_comment);
          o->pending_comment = clean;
        }
      else
        g_free (clean);
    }
  else if (g_str_equal (tag, "note"))
    {
      odt_flush (o);
      if (o->in_note > 0)
        {
          w42_builder_end_note (&o->b);
          o->in_note = 0;
          o->b.pa = o->note_outer_pa;
          o->para_ch = o->note_outer_ch;
          g_array_set_size (o->span_stack, 0);
          if (o->note_outer_spans != NULL)
            {
              g_array_append_vals (o->span_stack, o->note_outer_spans->data,
                                   o->note_outer_spans->len);
              g_clear_pointer (&o->note_outer_spans, g_array_unref);
            }
          o->link = o->note_outer_link;
          o->para_open = TRUE;
          o->after_space = FALSE;
        }
    }
  else if (g_str_equal (tag, "section"))
    {
      if (o->in_note == 0 && o->in_table == 0 && o->tb_depth == 0 && o->list_depth == 0 &&
          o->section_depth > 0 && --o->section_depth == 0)
        {
          /* Columns that end with their section end on the page: another
           * program's text after it is in one column again, which Word42
           * says with a section of one.  A file of Word42's own has every
           * section it wants in a text:section. */
          if (!o->section_pending && o->section.count > 1 && !o->ours)
            o->section_reset = TRUE;
          o->section_pending = FALSE;
          o->section.count = 1;
        }
    }
  else if (g_str_equal (tag, "list"))
    {
      odt_flush (o);
      if (o->list_depth > 0)
        o->list_depth--;
      if (o->list_style_stack->len > 0)
        g_ptr_array_remove_index (o->list_style_stack, o->list_style_stack->len - 1);
    }
  else if ((g_str_equal (tag, "table-cell") || g_str_equal (tag, "covered-table-cell")) &&
           o->in_table == 1)
    {
      odt_flush (o);
      open_pending_cell (o);
      w42_builder_end_cell (&o->b);
      o->para_open = FALSE;
      /* One element standing for several cells alike: the rest are
       * empty ones with the same style. */
      for (int i = 1; i < o->cell_repeat; i++)
        {
          o->cell_pending = TRUE;
          open_pending_cell (o);
          w42_builder_end_cell (&o->b);
        }
      o->cell_repeat = 1;
    }
  else if (g_str_equal (tag, "table-row") && o->in_table == 1)
    w42_builder_end_row (&o->b);
  else if (g_str_equal (tag, "table-header-rows"))
    o->in_header_rows = FALSE;
  else if (g_str_equal (tag, "table"))
    {
      if (o->in_table == 1)
        {
          int t = o->b.table;

          w42_builder_end_table (&o->b);
          if (t >= 0)
            w42_pt_resolve_vmerges (o->pt, t);   /* merges tidied, strays freed */
        }
      if (o->in_table > 0)
        o->in_table--;
    }
  else if (g_str_equal (tag, "frame"))
    {
      if (o->frame_pending && o->frame_href != NULL)
        {
          odt_flush (o);
          odt_frame_picture (o);
        }
      o->frame_pending = FALSE;
    }
  else if (g_str_equal (tag, "alphabetical-index-mark-start") && o->index_term != NULL)
    {
      /* An empty element: the words it marks come after it, and the
       * closing mark says where they end. */
    }
  else if (g_str_equal (tag, "alphabetical-index-mark-end") && o->index_term != NULL)
    {
      W42CharFmt want;
      char *code;

      odt_flush (o);
      {
        /* The term is kept in the code only where it is not the words
         * marked, as Word42 keeps it. */
        char *marked = o->b.pos > o->index_start
                         ? w42_pt_get_text (o->pt, o->index_start, o->b.pos - o->index_start) : NULL;

        code = *o->index_term != '\0' && (marked == NULL || !g_str_equal (marked, o->index_term))
                 ? g_strconcat ("XE:", o->index_term, NULL) : g_strdup ("XE");
        g_free (marked);
      }
      memset (&want, 0, sizeof want);
      want.field = g_intern_string (code);
      if (o->b.pos > o->index_start)
        w42_pt_apply_char_fmt (o->pt, o->index_start, o->b.pos - o->index_start,
                               W42_CHAR_FIELD, &want);
      g_free (code);
      g_clear_pointer (&o->index_term, g_free);
    }
  else if (o->field != NULL && (g_str_equal (tag, "page-number") || g_str_equal (tag, "page-count") ||
                                g_str_equal (tag, "date") || g_str_equal (tag, "time") ||
                                g_str_equal (tag, "file-name") || g_str_equal (tag, "word-count")))
    {
      const char *code = o->field;
      W42CharFmt want;
      gsize start = o->b.pos;

      g_string_append (o->field_text, o->text->str);
      g_string_truncate (o->text, 0);
      o->field = NULL;
      if (o->field_text->len == 0)
        g_string_append (o->field_text, g_str_equal (code, "PAGE") ? "1" : "?");
      o->b.ch = current_ch (o);
      o->b.ch.link = o->link;
      w42_builder_text (&o->b, o->field_text->str);
      memset (&want, 0, sizeof want);
      want.field = g_intern_string (code);
      w42_pt_apply_char_fmt (o->pt, start, o->b.pos - start, W42_CHAR_FIELD, &want);
      o->after_space = FALSE;
    }
}

static void
odt_start (GMarkupParseContext *ctx, const char *name, const char **an,
           const char **av, gpointer data, GError **error)
{
  Odt *o = data;
  const char *tag = local (name);

  (void) ctx; (void) error;
  if (o->skip_depth > 0)
    {
      o->skip_depth++;
      return;
    }
  if (g_str_equal (tag, "body"))
    o->in_body = TRUE;
  if (o->in_body &&
      (g_str_has_prefix (name, "dc:") || g_str_has_prefix (name, "meta:") ||
       g_str_has_prefix (name, "svg:") || g_str_equal (name, "text:tracked-changes")))
    {
      /* Not the text: an annotation's author and date, a picture's title
       * and description, the record of the changes tracked -- whose
       * deletions came back as paragraphs at the top, and whose dc:date
       * passed for a date field. */
      o->skip_depth = 1;
      return;
    }
  if (o->in_body)
    body_start (o, tag, an, av);
  else
    styles_start (o, tag, an, av);
}

static void
odt_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  Odt *o = data;
  const char *tag = local (name);

  (void) ctx; (void) error;
  if (o->skip_depth > 0)
    {
      o->skip_depth--;
      return;
    }
  if (o->in_body)
    body_end (o, tag);
  else
    styles_end (o, tag);
}

static void
odt_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  Odt *o = data;

  (void) ctx; (void) error;
  if (o->skip_depth > 0)
    return;
  if (o->in_annotation)
    {
      g_string_append_len (o->annotation, text, len);
      return;
    }
  if (o->in_header || o->in_footer)
    {
      for (gsize i = 0; i < len; i++)
        if (text[i] != '\n' && text[i] != '\r')
          g_string_append_c (o->hf_text, text[i]);
      return;
    }
  if (o->shape_open)
    {
      for (gsize i = 0; i < len; i++)
        if (text[i] != '\n' && text[i] != '\r')
          g_string_append_c (o->shape_text, text[i]);
      return;
    }
  if (!o->in_body || !o->para_open)
    return;
  /* ODF collapses runs of white space to one space and drops it at the
   * start of a paragraph, across the spans and marks in between; text:s,
   * a tab and a break stand for what is not collapsed. */
  for (gsize i = 0; i < len; i++)
    {
      char c = text[i];

      if (c == '\n' || c == '\r' || c == '\t' || c == ' ')
        {
          if (o->after_space)
            continue;
          c = ' ';
          o->after_space = TRUE;
        }
      else
        o->after_space = FALSE;
      g_string_append_c (o->text, c);
    }
}

/* meta.xml: what the document says about itself. */
typedef struct {
  GString *text;
  char    *field;
  char    *keep[6];      /* title, subject, author, keywords, comments,
                          * and the last one to edit it */
  gboolean ours;         /* Word42 wrote it */
} OdtMeta;

static void
meta_start (GMarkupParseContext *ctx, const char *name, const char **an,
            const char **av, gpointer data, GError **error)
{
  OdtMeta *m = data;

  (void) ctx; (void) an; (void) av; (void) error;
  g_free (m->field);
  m->field = g_strdup (name);
  g_string_truncate (m->text, 0);
}

static void
meta_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  OdtMeta *m = data;

  (void) ctx; (void) error;
  if (m->field != NULL)
    g_string_append_len (m->text, text, len);
}

static void
meta_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  OdtMeta *m = data;
  const char *tag = local (name);
  char **slot = NULL;

  (void) ctx; (void) error;
  if (g_str_equal (tag, "title"))            slot = &m->keep[0];
  else if (g_str_equal (tag, "subject"))     slot = &m->keep[1];
  /* The author is the initial creator; dc:creator is whoever saved it
   * last, the author only when the file says nothing else. */
  else if (g_str_equal (tag, "initial-creator")) slot = &m->keep[2];
  else if (g_str_equal (tag, "creator"))     slot = &m->keep[5];
  else if (g_str_equal (tag, "keyword"))     slot = &m->keep[3];
  else if (g_str_equal (tag, "description")) slot = &m->keep[4];
  else if (g_str_equal (tag, "generator"))
    m->ours = g_str_has_prefix (m->text->str, "Word42");
  if (slot != NULL && m->text->len > 0 && *slot == NULL)
    *slot = g_strdup (m->text->str);
  g_clear_pointer (&m->field, g_free);
  g_string_truncate (m->text, 0);
}

/* The summary information; and whether Word42 wrote the file, which a
 * program that saves it again says otherwise. */
static gboolean
odt_read_meta (W42Zip *zip, W42PieceTable *pt)
{
  GBytes *xml = w42_zip_read (zip, "meta.xml");
  GMarkupParser parser = { meta_start, meta_end, meta_text, NULL, NULL };
  GMarkupParseContext *ctx;
  W42DocInfo info;
  OdtMeta m;

  if (xml == NULL)
    return FALSE;
  memset (&m, 0, sizeof m);
  m.text = g_string_new (NULL);
  ctx = g_markup_parse_context_new (&parser, 0, &m, NULL);
  g_markup_parse_context_parse (ctx, g_bytes_get_data (xml, NULL), g_bytes_get_size (xml), NULL);
  g_markup_parse_context_free (ctx);

  memset (&info, 0, sizeof info);
  info.title = m.keep[0];
  info.subject = m.keep[1];
  info.author = m.keep[2] != NULL ? m.keep[2] : m.keep[5];
  info.keywords = m.keep[3];
  info.comments = m.keep[4];
  w42_pt_set_info (pt, &info);

  for (guint i = 0; i < G_N_ELEMENTS (m.keep); i++)
    g_free (m.keep[i]);
  g_free (m.field);
  g_string_free (m.text, TRUE);
  g_bytes_unref (xml);
  return m.ours;
}

static gboolean
parse_part (Odt *o, GBytes *xml, GError **error)
{
  GMarkupParser parser = { odt_start, odt_end, odt_text, NULL, NULL };
  GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, o, NULL);
  gboolean ok;

  ok = g_markup_parse_context_parse (ctx, g_bytes_get_data (xml, NULL), g_bytes_get_size (xml), error) &&
       g_markup_parse_context_end_parse (ctx, error);
  g_markup_parse_context_free (ctx);
  return ok;
}

gboolean
w42_odt_load (W42PieceTable *pt, W42PageSetup *page, GFile *file, GError **error)
{
  W42Zip *zip;
  GBytes *styles, *content;
  Odt o;
  W42PageSetup local_page;
  gboolean ok = TRUE;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  zip = w42_zip_open (file, error);
  if (zip == NULL)
    return FALSE;
  content = w42_zip_read (zip, "content.xml");
  if (content == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "The file is not an OpenDocument text: it has no content.xml.");
      w42_zip_free (zip);
      return FALSE;
    }
  styles = w42_zip_read (zip, "styles.xml");

  if (page == NULL)
    {
      memset (&local_page, 0, sizeof local_page);
      page = &local_page;
    }
  if (page->width == 0)
    {
      page->width = 12240; page->height = 15840;
      page->margin_left = page->margin_right = page->margin_top = page->margin_bottom = 1440;
    }

  memset (&o, 0, sizeof o);
  o.ours = odt_read_meta (zip, pt);
  w42_builder_init (&o.b, pt);
  o.pt = pt;
  o.page = page;
  o.zip = zip;
  o.styles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, style_free);
  o.list_styles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  o.fonts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  o.graphic_wraps = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  o.graphics = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  o.shape_text = g_string_new (NULL);
  o.col_widths = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  o.row_heights = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  o.cell_sides = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  o.cell_fills = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  o.cell_lines = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  o.pending_cell_sides = -1;
  o.hf_text = g_string_new (NULL);
  o.text = g_string_new (NULL);
  o.span_stack = g_array_new (FALSE, FALSE, sizeof (W42CharFmt));
  o.bookmark_start = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  o.list_style_stack = g_ptr_array_new_with_free_func (g_free);
  o.table_widths = g_array_new (FALSE, FALSE, sizeof (int));
  o.annotation = g_string_new (NULL);
  o.annotation_start = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  o.field_text = g_string_new (NULL);
  o.named = g_ptr_array_new_with_free_func (g_free);
  o.pictures = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify) g_bytes_unref);
  o.section_cols = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  {
    W42Fmt def;
    w42_fmt_init_default (&def);
    o.para_ch = def.ch;
  }

  /* The named styles and the page, then the content with its own
   * automatic styles before the body. */
  if (styles != NULL)
    ok = parse_part (&o, styles, error);
  odt_register_styles (&o);
  if (ok)
    ok = parse_part (&o, content, error);

  /* The header's box, when there is a header, is part of Word's margin. */
  for (int k = 0; k < W42_PAGE_TEXT_KINDS; k++)
    {
      const W42PageText *h = w42_pt_get_header_kind (pt, (W42PageTextKind) k);
      const W42PageText *f = w42_pt_get_footer_kind (pt, (W42PageTextKind) k);

      if (h != NULL && h->text != NULL && *h->text != '\0' && o.hf_box[0] > 0)
        {
          page->margin_top += o.hf_box[0];
          o.hf_box[0] = 0;
        }
      if (f != NULL && f->text != NULL && *f->text != '\0' && o.hf_box[1] > 0)
        {
          page->margin_bottom += o.hf_box[1];
          o.hf_box[1] = 0;
        }
    }

  odt_flush (&o);
  w42_builder_finish (&o.b);
  w42_pt_clear_undo (pt);

  g_hash_table_destroy (o.styles);
  g_hash_table_destroy (o.list_styles);
  g_hash_table_destroy (o.fonts);
  g_hash_table_destroy (o.col_widths);
  g_string_free (o.hf_text, TRUE);
  g_string_free (o.text, TRUE);
  g_array_free (o.span_stack, TRUE);
  g_hash_table_destroy (o.bookmark_start);
  g_ptr_array_free (o.list_style_stack, TRUE);
  g_array_free (o.table_widths, TRUE);
  g_string_free (o.annotation, TRUE);
  g_free (o.annotation_name);
  g_hash_table_destroy (o.annotation_start);
  g_string_free (o.field_text, TRUE);
  g_free (o.frame_href);
  g_free (o.cur_graphic);
  g_hash_table_destroy (o.graphic_wraps);
  g_hash_table_destroy (o.graphics);
  g_string_free (o.shape_text, TRUE);
  g_free (o.cur_row_style);
  g_hash_table_destroy (o.row_heights);
  g_free (o.cur_cell_style);
  g_hash_table_destroy (o.cell_sides);
  g_hash_table_destroy (o.cell_fills);
  g_hash_table_destroy (o.cell_lines);
  g_free (o.cur_style_name);
  g_free (o.cur_col_style);
  g_free (o.index_term);
  g_free (o.pending_comment);
  g_ptr_array_free (o.named, TRUE);
  g_hash_table_destroy (o.pictures);
  g_hash_table_destroy (o.section_cols);
  g_free (o.cur_section);
  g_free (o.frame_first_href);
  if (o.note_outer_spans != NULL)
    g_array_unref (o.note_outer_spans);
  if (o.tb_saved_spans != NULL)
    g_array_unref (o.tb_saved_spans);
  if (styles != NULL)
    g_bytes_unref (styles);
  g_bytes_unref (content);
  w42_zip_free (zip);
  return ok;
}

/* ====================================================================== */
/* Writing                                                                 */
/* ====================================================================== */

#define ODT_NS \
  "xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" " \
  "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" " \
  "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" " \
  "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" " \
  "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" " \
  "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" " \
  "xmlns:xlink=\"http://www.w3.org/1999/xlink\" " \
  "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" " \
  "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" " \
  "office:version=\"1.2\""

typedef struct {
  GString   *auto_styles;      /* content.xml's automatic styles */
  GString   *body;
  GPtrArray *pa_keys;          /* W42ParaFmt copies: P1.. */
  GPtrArray *ch_keys;          /* W42CharFmt copies: T1.. */
  GPtrArray *pictures;         /* GBytes*, Pictures/imageN.<ext> */
  GString   *shape_styles;     /* the graphic styles the shapes referred to */
  guint      n_shapes;
  GPtrArray *picture_exts;     /* const char*, static: "png", "jpeg", ... */
  GPtrArray *picture_mimes;    /* const char*, static */
  int        n_tables;
  int        n_index_marks;    /* index entries written, for their ids */
  int        list_style_used[W42_LIST_KINDS];
  W42CharFmt base_ch;
  int        note_id;
  int        annotation_id;
  GHashTable *cell_styles;  /* the names of the table-cell styles written */
  W42StyleSheet *styles;
  int        n_sections;       /* text:sections written, for their names */
} OdtWriter;

/* The lists open where the text is being written -- the body, a cell, a
 * text box -- none of which a list may run on out of. */
typedef struct {
  int         depth;
  W42ListKind kind[9];
  gboolean    used[9];      /* the item open at that depth has its paragraph */
  gboolean    resume;       /* the lists were closed for a section, not
                             * ended: the next goes on with their count */
} OdtLists;

/* The style name a paragraph's own properties get, one per distinct set. */
static int
para_style_index (OdtWriter *w, const W42ParaFmt *pa)
{
  for (guint i = 0; i < w->pa_keys->len; i++)
    if (memcmp (g_ptr_array_index (w->pa_keys, i), pa, sizeof *pa) == 0)
      return (int) i + 1;
  g_ptr_array_add (w->pa_keys, g_memdup2 (pa, sizeof *pa));
  return (int) w->pa_keys->len;
}

static int
text_style_index (OdtWriter *w, const W42CharFmt *ch)
{
  for (guint i = 0; i < w->ch_keys->len; i++)
    if (memcmp (g_ptr_array_index (w->ch_keys, i), ch, sizeof *ch) == 0)
      return (int) i + 1;
  g_ptr_array_add (w->ch_keys, g_memdup2 (ch, sizeof *ch));
  return (int) w->ch_keys->len;
}

/* A style's name as an XML name: its letters and digits as they are,
 * every other byte in hex between underscores -- "Heading 1" is
 * "Heading_20_1", as LibreOffice spells it.  The underscore is spelt so
 * too, so no two names come out the same: dropping what was not a letter
 * made "Body-Text" and "BodyText" one style, and a name in Cyrillic no
 * name at all.  Normal is OpenDocument's "Standard", which a style of
 * that name must then not be. */
static char *
style_id_for (const char *name)
{
  GString *id;

  if (name == NULL || g_ascii_strcasecmp (name, "Normal") == 0)
    return g_strdup ("Standard");
  id = g_string_new (NULL);
  for (const char *p = name; *p != '\0'; p++)
    {
      guchar c = (guchar) *p;

      /* A name may not begin with a digit. */
      if (g_ascii_isalpha (c) || (g_ascii_isdigit (c) && id->len > 0))
        g_string_append_c (id, (char) c);
      else
        g_string_append_printf (id, "_%02x_", c);
    }
  if (g_str_equal (id->str, "Standard"))
    g_string_append_c (id, '_');        /* what no other name spells */
  return g_string_free (id, FALSE);
}

static void
write_para_props_xml (GString *s, const W42ParaFmt *pa, const W42ParaFmt *base)
{
  g_string_append (s, "<style:paragraph-properties");
  if (base == NULL || pa->align != base->align)
    g_string_append_printf (s, " fo:text-align=\"%s\"",
                            pa->align == W42_ALIGN_CENTER ? "center" : pa->align == W42_ALIGN_RIGHT ? "end"
                            : pa->align == W42_ALIGN_JUSTIFY ? "justify" : "start");
  /* A length is written where it differs from the style the paragraph
   * is based on, nought included: a paragraph indented 0 in a style
   * indented an inch would otherwise inherit the inch. */
#define DIFFERS(field) (base == NULL ? pa->field != 0 : pa->field != base->field)
  if (DIFFERS (indent_left))  { g_string_append (s, " fo:margin-left=\""); twips_out (s, pa->indent_left); g_string_append_c (s, '"'); }
  if (DIFFERS (indent_right)) { g_string_append (s, " fo:margin-right=\""); twips_out (s, pa->indent_right); g_string_append_c (s, '"'); }
  if (DIFFERS (indent_first)) { g_string_append (s, " fo:text-indent=\""); twips_out (s, pa->indent_first); g_string_append_c (s, '"'); }
  if (DIFFERS (space_before)) { g_string_append (s, " fo:margin-top=\""); twips_out (s, pa->space_before); g_string_append_c (s, '"'); }
  if (DIFFERS (space_after))  { g_string_append (s, " fo:margin-bottom=\""); twips_out (s, pa->space_after); g_string_append_c (s, '"'); }
#undef DIFFERS
  if (pa->line_spacing_pct > 0 && pa->line_spacing_pct != 100)
    g_string_append_printf (s, " fo:line-height=\"%d%%\"", pa->line_spacing_pct);
  else if (pa->line_spacing > 0)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      g_ascii_formatd (buf, sizeof buf, "%.2f", pa->line_spacing / 20.0);
      g_string_append_printf (s, " fo:line-height=\"%spt\"", buf);
    }
  else if (base != NULL && ((base->line_spacing_pct > 0 && base->line_spacing_pct != 100) ||
                            base->line_spacing > 0))
    g_string_append (s, " fo:line-height=\"100%\"");
  /* The flags likewise: what the style turns on, a paragraph that has
   * it off turns off. */
  if (pa->page_break_before) g_string_append (s, " fo:break-before=\"page\"");
  else if (base != NULL && base->page_break_before) g_string_append (s, " fo:break-before=\"auto\"");
  if (pa->keep_next)     g_string_append (s, " fo:keep-with-next=\"always\"");
  else if (base != NULL && base->keep_next) g_string_append (s, " fo:keep-with-next=\"auto\"");
  if (pa->keep_together) g_string_append (s, " fo:keep-together=\"always\"");
  else if (base != NULL && base->keep_together) g_string_append (s, " fo:keep-together=\"auto\"");
  if (pa->rtl)           g_string_append (s, " style:writing-mode=\"rl-tb\"");
  else if (base != NULL && base->rtl) g_string_append (s, " style:writing-mode=\"lr-tb\"");
  /* Widow control is on unless said otherwise, as Word 97 had it; in
   * OpenDocument it is off unless said, so a style of its own says it. */
  if (!pa->widow_control)
    g_string_append (s, " fo:widows=\"0\" fo:orphans=\"0\"");
  else if (base == NULL || !base->widow_control)
    g_string_append (s, " fo:widows=\"2\" fo:orphans=\"2\"");
  if (pa->border != 0)
    {
      static const char *names[4] = { "fo:border-top", "fo:border-bottom", "fo:border-left", "fo:border-right" };
      static const int bits[4] = { W42_BORDER_TOP, W42_BORDER_BOTTOM, W42_BORDER_LEFT, W42_BORDER_RIGHT };
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      for (int i = 0; i < 4; i++)
        if (pa->border & bits[i])
          g_string_append_printf (s, " %s=\"%spt %s #%06x\"", names[i],
                                  g_ascii_formatd (buf, sizeof buf, "%.2f", W42_EDGE_WIDTH (&pa->edge[i]) / 20.0),
                                  w42_border_style_css (pa->edge[i].style),
                                  pa->edge[i].color & 0xFFFFFF);
      g_string_append (s, " fo:padding=\"0.02in\"");
    }
  else if (base != NULL && base->border != 0)
    g_string_append (s, " fo:border=\"none\"");
  if (pa->has_shading_color)
    g_string_append_printf (s, " fo:background-color=\"#%06x\"",
                            pa->shading_color & 0xFFFFFF);
  else if (pa->shading > 0)
    {
      int grey = 255 - pa->shading * 255 / 100;

      g_string_append_printf (s, " fo:background-color=\"#%02x%02x%02x\"", grey, grey, grey);
    }
  else if (base != NULL && (base->has_shading_color || base->shading > 0))
    g_string_append (s, " fo:background-color=\"transparent\"");
  if (pa->n_tabs > 0 || pa->drop_cap > 0 || (base != NULL && base->n_tabs > 0))
    {
      g_string_append_c (s, '>');
      if (pa->n_tabs == 0)
        {
          if (base != NULL && base->n_tabs > 0)
            g_string_append (s, "<style:tab-stops/>");   /* none, where the base has some */
        }
      else
        {
          g_string_append (s, "<style:tab-stops>");
          for (int i = 0; i < pa->n_tabs; i++)
            {
              g_string_append (s, "<style:tab-stop style:position=\"");
              twips_out (s, pa->tab_pos[i]);
              g_string_append_printf (s, "\"%s",
                                      W42_TAB_KIND (pa->tab_kind[i]) == W42_TAB_CENTER ? " style:type=\"center\""
                                      : W42_TAB_KIND (pa->tab_kind[i]) == W42_TAB_RIGHT ? " style:type=\"right\""
                                      : W42_TAB_KIND (pa->tab_kind[i]) == W42_TAB_DECIMAL ? " style:type=\"char\" style:char=\".\"" : "");
              switch (W42_TAB_LEADER (pa->tab_kind[i]))
                {
                case W42_TAB_LEAD_DOT:
                  g_string_append (s, " style:leader-style=\"dotted\" style:leader-text=\".\"");
                  break;
                case W42_TAB_LEAD_DASH:
                  g_string_append (s, " style:leader-style=\"dash\" style:leader-text=\"-\"");
                  break;
                case W42_TAB_LEAD_LINE:
                  g_string_append (s, " style:leader-style=\"solid\" style:leader-text=\"_\"");
                  break;
                default:
                  break;
                }
              g_string_append (s, "/>");
            }
          g_string_append (s, "</style:tab-stops>");
        }
      if (pa->drop_cap > 0)
        g_string_append_printf (s, "<style:drop-cap style:length=\"1\" style:lines=\"%d\" style:distance=\"0.05in\"/>", pa->drop_cap);
      g_string_append (s, "</style:paragraph-properties>");
    }
  else
    g_string_append (s, "/>");
}

static void
write_text_props_xml (GString *s, const W42CharFmt *ch, const W42CharFmt *base)
{
  g_string_append (s, "<style:text-properties");
  if (ch->family != NULL && (base == NULL || ch->family != base->family))
    {
      g_string_append (s, " fo:font-family=\"");
      xml_escape (s, ch->family, strlen (ch->family));
      g_string_append_c (s, '"');
    }
  if (base == NULL || ch->size != base->size)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      /* Half-points: 19 of them is 9.5pt, not 9. */
      g_ascii_formatd (buf, sizeof buf, "%.1f", ch->size / 2.0);
      g_string_append_printf (s, " fo:font-size=\"%spt\"", buf);
    }
  if (base == NULL || ch->bold != base->bold)
    g_string_append_printf (s, " fo:font-weight=\"%s\"", ch->bold ? "bold" : "normal");
  if (base == NULL || ch->italic != base->italic)
    g_string_append_printf (s, " fo:font-style=\"%s\"", ch->italic ? "italic" : "normal");
  if (ch->underline)
    {
      static const char *const STYLES[] = {
        "none", "solid", "solid", "solid", "dotted", "dash", "solid", "wave"
      };
      guint kind = MIN (ch->underline, G_N_ELEMENTS (STYLES) - 1);

      g_string_append_printf (s, " style:text-underline-style=\"%s\""
                                 " style:text-underline-width=\"%s\""
                                 " style:text-underline-color=\"font-color\"",
                              STYLES[kind],
                              ch->underline == W42_UNDERLINE_THICK ? "bold" : "auto");
      if (ch->underline == W42_UNDERLINE_DOUBLE)
        g_string_append (s, " style:text-underline-type=\"double\"");
      if (ch->underline == W42_UNDERLINE_WORDS)
        g_string_append (s, " style:text-underline-mode=\"skip-white-space\"");
    }
  /* Without a base, every property is said, the "off" ones included: a
   * span's style is applied over its paragraph's, which may underline
   * or colour, and what a style leaves unsaid it inherits. */
  else if (base == NULL)
    g_string_append (s, " style:text-underline-style=\"none\"");
  /* The line's type says "none" where there is none: LibreOffice takes a
   * type of "single" as a line through the text, whatever the style
   * beside it says. */
  if (ch->strikeout || ch->dstrike) g_string_append (s, " style:text-line-through-style=\"solid\"");
  else if (base == NULL || base->strikeout || base->dstrike)
    g_string_append (s, " style:text-line-through-style=\"none\"");
  if (ch->dstrike) g_string_append (s, " style:text-line-through-type=\"double\"");
  else if (ch->strikeout)
    {
      if (base == NULL || base->dstrike)
        g_string_append (s, " style:text-line-through-type=\"single\"");
    }
  else if (base == NULL || base->strikeout || base->dstrike)
    g_string_append (s, " style:text-line-through-type=\"none\"");
  /* The shadow's offset is what LibreOffice writes for its own. */
  if (ch->shadow) g_string_append (s, " fo:text-shadow=\"1pt 1pt\"");
  else if (base == NULL || base->shadow) g_string_append (s, " fo:text-shadow=\"none\"");
  if (ch->outline) g_string_append (s, " style:text-outline=\"true\"");
  else if (base == NULL || base->outline) g_string_append (s, " style:text-outline=\"false\"");
  if (ch->emboss) g_string_append (s, " style:font-relief=\"embossed\"");
  else if (ch->engrave) g_string_append (s, " style:font-relief=\"engraved\"");
  else if (base == NULL || base->emboss || base->engrave) g_string_append (s, " style:font-relief=\"none\"");
  if (ch->overline)  g_string_append (s, " style:text-overline-style=\"solid\"");
  else if (base == NULL) g_string_append (s, " style:text-overline-style=\"none\"");
  if (ch->color != 0 || base == NULL) g_string_append_printf (s, " fo:color=\"#%06x\"", ch->color & 0xFFFFFF);
  if (ch->highlight != 0) g_string_append_printf (s, " fo:background-color=\"#%06x\"", w42_highlight_rgb (ch->highlight));
  else if (base == NULL) g_string_append (s, " fo:background-color=\"transparent\"");
  if (ch->script > 0) g_string_append (s, " style:text-position=\"super 58%\"");
  else if (ch->script < 0) g_string_append (s, " style:text-position=\"sub 58%\"");
  else if (base == NULL) g_string_append (s, " style:text-position=\"0% 100%\"");
  /* LibreOffice keeps small capitals and capitals as one property, so
   * the "off" of either is left unsaid when the other is on: it would
   * cancel it. */
  if (ch->smallcaps) g_string_append (s, " fo:font-variant=\"small-caps\"");
  else if ((base == NULL || base->smallcaps) && !ch->allcaps)
    g_string_append (s, " fo:font-variant=\"normal\"");
  if (ch->allcaps)   g_string_append (s, " fo:text-transform=\"uppercase\"");
  else if ((base == NULL || base->allcaps) && !ch->smallcaps)
    g_string_append (s, " fo:text-transform=\"none\"");
  if (ch->spacing)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      g_ascii_formatd (buf, sizeof buf, "%.2f", ch->spacing / 20.0);
      g_string_append_printf (s, " fo:letter-spacing=\"%spt\"", buf);
    }
  else if (base == NULL || base->spacing != 0)
    g_string_append (s, " fo:letter-spacing=\"0pt\"");
  if (ch->lang != NULL)
    {
      /* OpenDocument keeps the language, the script and the country
       * apart, says a run that is not language at all with "zxx" and
       * "none", and has a tag they cannot say -- a variant -- whole in
       * rfc-language-tag.  "zh-Hans-CN" had come out as the country
       * "Hans-CN". */
      char **parts = g_strsplit (ch->lang, "-", -1);
      const char *script = NULL, *country = NULL;
      gboolean more = FALSE;

      for (int i = 1; parts[i] != NULL; i++)
        {
          gsize n = strlen (parts[i]);

          if (i == 1 && n == 4 && g_ascii_isalpha (parts[i][0]))
            script = parts[i];
          else if (country == NULL && (n == 2 || (n == 3 && g_ascii_isdigit (parts[i][0]))))
            country = parts[i];
          else
            more = TRUE;
        }
      g_string_append_printf (s, " fo:language=\"%s\"", parts[0]);
      if (script != NULL)
        g_string_append_printf (s, " fo:script=\"%s\"", script);
      g_string_append_printf (s, " fo:country=\"%s\"", country != NULL ? country : "none");
      if (more)
        {
          g_string_append (s, " style:rfc-language-tag=\"");
          xml_escape (s, ch->lang, strlen (ch->lang));
          g_string_append_c (s, '"');
        }
      g_strfreev (parts);
    }
  g_string_append (s, "/>");
}

/* Text with ODF's white-space rules: tabs and line breaks as their
 * elements, and as text:s every space a reader would collapse -- one of
 * several, and one where a paragraph starts or a space went before.
 * `after_space` carries that across a paragraph's runs: "a " and " b" in
 * runs of their own are two spaces, which LibreOffice took for one. */
static void
write_odt_text (GString *out, const char *text, gsize len, gboolean *after_space)
{
  gsize i = 0;

  while (i < len)
    {
      if (text[i] == '\t')
        {
          g_string_append (out, "<text:tab/>");
          *after_space = FALSE;
          i++;
        }
      else if (text[i] == ' ' && (*after_space || (i + 1 < len && text[i + 1] == ' ')))
        {
          gsize n = 0;

          while (i < len && text[i] == ' ')
            {
              n++;
              i++;
            }
          if (*after_space)
            g_string_append_printf (out, "<text:s text:c=\"%u\"/>", (unsigned) n);
          else
            g_string_append_printf (out, " <text:s text:c=\"%u\"/>", (unsigned) (n - 1));
          *after_space = FALSE;       /* a space after text:s is kept */
        }
      else if ((guchar) text[i] == 0xE2 && i + 2 < len && (guchar) text[i + 1] == 0x80 && (guchar) text[i + 2] == 0xA8)
        {
          g_string_append (out, "<text:line-break/>");
          *after_space = FALSE;
          i += 3;
        }
      else if ((guchar) text[i] == 0xC2 && i + 1 < len && (guchar) text[i + 1] == 0xAD)
        {
          g_string_append (out, "<text:soft-hyphen/>");
          *after_space = FALSE;
          i += 2;
        }
      else
        {
          gsize start = i;

          while (i < len && text[i] != '\t' && !(text[i] == ' ' && i + 1 < len && text[i + 1] == ' ') &&
                 !((guchar) text[i] == 0xE2 && i + 2 < len && (guchar) text[i + 1] == 0x80 && (guchar) text[i + 2] == 0xA8) &&
                 !((guchar) text[i] == 0xC2 && i + 1 < len && (guchar) text[i + 1] == 0xAD))
            i++;
          xml_escape (out, text + start, i - start);
          *after_space = text[i - 1] == ' ';
        }
    }
}

/* What stays open from one paragraph into the next: a bookmark or an
 * annotation over several paragraphs is one, its start in the first and
 * its end in the last, as OpenDocument lets it be. */
typedef struct {
  const char *bookmark;
  const char *comment;
  int         comment_id;
} OdtMarks;

static void write_runs (OdtWriter *w, W42PieceTable *pt, W42ApTable *aps, GPtrArray *blocks,
                        const W42Block *block, const W42CharFmt *para_ch,
                        OdtMarks *marks, const W42Block *next);

/* The extension and the media type a picture kept as it came is filed
 * under: its own extension, when that is a plain one, since it goes into
 * a part's name as it stands. */
static const char *
odt_original_kind (const char *format, const char **mime)
{
  static const struct { const char *ext, *mime; } known[] = {
    { "emf",  "image/x-emf" },
    { "wmf",  "image/x-wmf" },
    { "pict", "image/x-pict" },
    { "eps",  "application/postscript" },
    { "tif",  "image/tiff" },
    { "tiff", "image/tiff" },
    { "svg",  "image/svg+xml" },
  };

  for (guint i = 0; i < G_N_ELEMENTS (known); i++)
    if (g_ascii_strcasecmp (format, known[i].ext) == 0)
      {
        *mime = known[i].mime;
        return known[i].ext;
      }
  *mime = "application/octet-stream";
  for (const char *q = format; *q != '\0'; q++)
    if (!g_ascii_isalnum (*q) || q - format >= 8)
      return "bin";
  return *format != '\0' ? format : "bin";
}

/* The graphic style a wrapped object refers to: where the text goes. */
static const char *
graphic_style_name (const W42Object *object)
{
  switch (object->wrap)
    {
    case W42_WRAP_LEFT:       return "frL";
    case W42_WRAP_RIGHT:      return "frR";
    case W42_WRAP_TOP_BOTTOM: return "frTB";
    case W42_WRAP_BEHIND:     return "frBehind";
    default:                  return "frFront";
    }
}

/* svg:x and svg:y for an object placed at a spot of its own. */
static void
write_odt_position (GString *out, const W42Object *object)
{
  if (!object->positioned)
    return;
  g_string_append (out, " svg:x=\"");
  twips_out (out, object->pos_x);
  g_string_append (out, "\" svg:y=\"");
  twips_out (out, object->pos_y);
  g_string_append (out, "\"");
}

/* A shape as OpenDocument says one: a custom shape with its geometry,
 * or a line, in a graphic style of its own for its fill and outline. */
static void
write_odt_shape (OdtWriter *w, const W42Object *object)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  guint n = ++w->n_shapes;
  const char *parent = object->wrap == W42_WRAP_INLINE ? "" : graphic_style_name (object);

  /* The style: its fill and outline, over the wrapping style's placing. */
  g_string_append_printf (w->shape_styles,
    "<style:style style:name=\"Shape%u\" style:family=\"graphic\"%s%s%s><style:graphic-properties",
    n, *parent != '\0' ? " style:parent-style-name=\"" : "", parent, *parent != '\0' ? "\"" : "");
  if (object->wrap != W42_WRAP_INLINE)
    {
      const char *wrap = object->wrap == W42_WRAP_LEFT ? "right" : object->wrap == W42_WRAP_RIGHT ? "left"
                       : object->wrap == W42_WRAP_TOP_BOTTOM ? "none" : "run-through";

      g_string_append_printf (w->shape_styles, " style:wrap=\"%s\" style:horizontal-pos=\"%s\" style:horizontal-rel=\"paragraph\" "
                              "style:vertical-pos=\"%s\" style:vertical-rel=\"paragraph\"",
                              wrap, object->positioned ? "from-left" : object->wrap == W42_WRAP_RIGHT ? "right" : "left",
                              object->positioned ? "from-top" : "top");
      if (object->wrap == W42_WRAP_BEHIND)
        g_string_append (w->shape_styles, " style:run-through=\"background\"");
      else if (object->wrap == W42_WRAP_FRONT)
        g_string_append (w->shape_styles, " style:run-through=\"foreground\"");
    }
  if (object->filled)
    g_string_append_printf (w->shape_styles, " draw:fill=\"solid\" draw:fill-color=\"#%06x\"", object->fill_rgb);
  else
    g_string_append (w->shape_styles, " draw:fill=\"none\"");
  if (object->line_pt > 0.0)
    g_string_append_printf (w->shape_styles, " draw:stroke=\"solid\" svg:stroke-width=\"%spt\" svg:stroke-color=\"#%06x\"",
                            g_ascii_formatd (buf, sizeof buf, "%.2f", object->line_pt), object->line_rgb);
  else
    g_string_append (w->shape_styles, " draw:stroke=\"none\"");
  if (object->shape == W42_SHAPE_ARROW)
    g_string_append (w->shape_styles, " draw:marker-end=\"Arrow\" draw:marker-end-width=\"0.15in\"");
  g_string_append (w->shape_styles, " draw:textarea-vertical-align=\"middle\" draw:textarea-horizontal-align=\"center\" draw:auto-grow-height=\"false\" draw:auto-grow-width=\"false\" fo:padding=\"0.05in\"/></style:style>");

  if (object->shape == W42_SHAPE_LINE || object->shape == W42_SHAPE_ARROW)
    {
      int x = object->positioned ? object->pos_x : 0, y = object->positioned ? object->pos_y : 0;

      g_string_append_printf (w->body, "<draw:line draw:style-name=\"Shape%u\" text:anchor-type=\"%s\" svg:x1=\"",
                              n, object->wrap == W42_WRAP_INLINE ? "as-char" : "paragraph");
      twips_out (w->body, x);
      g_string_append (w->body, "\" svg:y1=\"");
      twips_out (w->body, y);
      g_string_append (w->body, "\" svg:x2=\"");
      twips_out (w->body, x + object->width);
      g_string_append (w->body, "\" svg:y2=\"");
      twips_out (w->body, y + object->height);
      g_string_append (w->body, "\"/>");
      return;
    }

  g_string_append_printf (w->body, "<draw:custom-shape draw:style-name=\"Shape%u\" text:anchor-type=\"%s\"",
                          n, object->wrap == W42_WRAP_INLINE ? "as-char" : "paragraph");
  write_odt_position (w->body, object);
  g_string_append (w->body, " svg:width=\"");
  twips_out (w->body, object->width);
  g_string_append (w->body, "\" svg:height=\"");
  twips_out (w->body, object->height);
  g_string_append (w->body, "\">");
  if (object->text != NULL)
    {
      char **lines = g_strsplit (object->text, "\n", -1);

      for (int i = 0; lines[i] != NULL; i++)
        {
          g_string_append (w->body, "<text:p>");
          xml_escape (w->body, lines[i], strlen (lines[i]));
          g_string_append (w->body, "</text:p>");
        }
      g_strfreev (lines);
    }
  g_string_append_printf (w->body, "<draw:enhanced-geometry svg:viewBox=\"0 0 21600 21600\" draw:type=\"%s\" draw:enhanced-path=\"%s\"/></draw:custom-shape>",
                          object->shape == W42_SHAPE_ELLIPSE ? "ellipse"
                          : object->shape == W42_SHAPE_ROUNDED_RECTANGLE ? "round-rectangle" : "rectangle",
                          object->shape == W42_SHAPE_ELLIPSE ? "U 10800 10800 10800 10800 0 360 Z N"
                          : object->shape == W42_SHAPE_ROUNDED_RECTANGLE
                            ? "M 3600 0 L 18000 0 X 21600 3600 L 21600 18000 Y 18000 21600 L 3600 21600 X 0 18000 L 0 3600 Y 3600 0 Z N"
                            : "M 0 0 L 21600 0 21600 21600 0 21600 Z N");
}

/* Whether `b` goes on where `a` leaves off: the same cell, note or box,
 * or the body, where a bookmark or an annotation may run on into it. */
static gboolean
same_flow (W42ApTable *aps, const W42Block *a, const W42Block *b)
{
  return b != NULL && a->note == b->note && a->table == b->table &&
         (a->table < 0 || (a->row == b->row && a->col == b->col)) &&
         w42_ap_table_get (aps, a->ap)->pa.frame_side == w42_ap_table_get (aps, b->ap)->pa.frame_side;
}

static void
write_paragraph (OdtWriter *w, W42PieceTable *pt, W42ApTable *aps, GPtrArray *blocks,
                 const W42Block *block, OdtMarks *marks, const W42Block *next)
{
  const W42Fmt *fmt = w42_ap_table_get (aps, block->ap);
  const W42ParaFmt *pa = &fmt->pa;
  const W42Style *style = pa->style != NULL ? w42_stylesheet_find (w->styles, pa->style) : NULL;
  int outline = style != NULL ? style->outline : 0;
  int pidx = para_style_index (w, pa);
  const W42CharFmt *para_ch = style != NULL ? &style->ch : &w->base_ch;

  if (outline > 0)
    g_string_append_printf (w->body, "<text:h text:style-name=\"P%d\" text:outline-level=\"%d\">", pidx, outline);
  else
    g_string_append_printf (w->body, "<text:p text:style-name=\"P%d\">", pidx);
  write_runs (w, pt, aps, blocks, block, para_ch, marks, same_flow (aps, block, next) ? next : NULL);
  g_string_append (w->body, outline > 0 ? "</text:h>" : "</text:p>");
}

/* A run's text, in a span of its own where its formatting is not the
 * paragraph's. */
static void
write_run_text (OdtWriter *w, const W42CharFmt *ch, const W42CharFmt *para_ch,
                const char *text, gsize len, gboolean *after_space)
{
  W42CharFmt plain = *ch, base = *para_ch;

  plain.link = plain.bookmark = plain.comment = plain.field = NULL;
  plain.revision = 0;
  base.link = base.bookmark = base.comment = base.field = NULL;
  base.revision = 0;
  if (memcmp (&plain, &base, sizeof plain) != 0)
    {
      g_string_append_printf (w->body, "<text:span text:style-name=\"T%d\">", text_style_index (w, &plain));
      write_odt_text (w->body, text, len, after_space);
      g_string_append (w->body, "</text:span>");
    }
  else
    write_odt_text (w->body, text, len, after_space);
}

static void
close_bookmark (OdtWriter *w, OdtMarks *marks)
{
  g_string_append (w->body, "<text:bookmark-end text:name=\"");
  xml_escape (w->body, marks->bookmark, strlen (marks->bookmark));
  g_string_append (w->body, "\"/>");
  marks->bookmark = NULL;
}

static void
close_comment (OdtWriter *w, OdtMarks *marks)
{
  g_string_append_printf (w->body, "<office:annotation-end office:name=\"w42c%d\"/>", marks->comment_id);
  marks->comment = NULL;
}

/* A paragraph's runs.  `marks` is what the paragraphs before left open,
 * and `next` the paragraph that may go on with it, or NULL. */
static void
write_runs (OdtWriter *w, W42PieceTable *pt, W42ApTable *aps, GPtrArray *blocks,
            const W42Block *block, const W42CharFmt *para_ch,
            OdtMarks *marks, const W42Block *next)
{
  const char *open_link = NULL;
  gboolean after_space = TRUE;         /* at the paragraph's start */

  for (guint i = 0; i < block->runs->len; i++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, i);
      const W42CharFmt *ch = &w42_ap_table_get (aps, run->ap)->ch;

      /* Neither the insertions nor the deletions of a tracked change are
       * written as such, so the file has them all accepted: a deletion
       * saved as text came back as words nobody had kept. */
      if (ch->revision == 2)
        continue;

      if (open_link != NULL && ch->link != open_link)
        {
          g_string_append (w->body, "</text:a>");
          open_link = NULL;
        }
      if (marks->bookmark != NULL && ch->bookmark != marks->bookmark)
        close_bookmark (w, marks);
      if (marks->comment != NULL && ch->comment != marks->comment)
        close_comment (w, marks);
      if (ch->bookmark != NULL && marks->bookmark == NULL)
        {
          g_string_append (w->body, "<text:bookmark-start text:name=\"");
          xml_escape (w->body, ch->bookmark, strlen (ch->bookmark));
          g_string_append (w->body, "\"/>");
          marks->bookmark = ch->bookmark;
        }
      if (ch->comment != NULL && marks->comment == NULL)
        {
          marks->comment_id = ++w->annotation_id;
          g_string_append_printf (w->body, "<office:annotation office:name=\"w42c%d\"><dc:creator>", marks->comment_id);
          {
            const char *who = w42_pt_get_author (pt);

            xml_escape (w->body, who != NULL ? who : "Word42", strlen (who != NULL ? who : "Word42"));
          }
          g_string_append (w->body, "</dc:creator><text:p>");
          xml_escape (w->body, ch->comment, strlen (ch->comment));
          g_string_append (w->body, "</text:p></office:annotation>");
          marks->comment = ch->comment;
        }
      if (ch->link != NULL && open_link == NULL)
        {
          g_string_append (w->body, "<text:a xlink:type=\"simple\" xlink:href=\"");
          xml_escape (w->body, ch->link, strlen (ch->link));
          g_string_append (w->body, "\">");
          open_link = ch->link;
        }

      if (run->object != W42_OBJECT_NONE)
        {
          const W42Object *object = w42_object_table_get (w42_pt_object_table (pt), run->object);
          const char *ext = "png", *mime = "image/png";
          GBytes *png = NULL;
          guint fallback = 0;         /* the placeholder's picture, or 0 */

          if (object != NULL && object->original != NULL && object->original_format != NULL)
            {
              /* A picture Word42 could not draw goes out as it came: a
               * metafile is one LibreOffice draws.  Of a kind it may not
               * draw, the placeholder follows it in the frame, which an
               * OpenDocument reader takes when it cannot read the first. */
              png = g_bytes_ref (object->original);
              ext = odt_original_kind (object->original_format, &mime);
              if (!g_str_equal (ext, "emf") && !g_str_equal (ext, "wmf"))
                {
                  g_ptr_array_add (w->pictures, g_bytes_ref (object->data));
                  g_ptr_array_add (w->picture_exts, (gpointer) "png");
                  g_ptr_array_add (w->picture_mimes, (gpointer) "image/png");
                  fallback = w->pictures->len;
                }
            }
          else if (object != NULL && object->shape == W42_SHAPE_PICTURE)
            png = w42_image_for_container (object->data, &ext, &mime);

          if (png == NULL && object != NULL && object->shape != W42_SHAPE_PICTURE)
            write_odt_shape (w, object);
          else if (png != NULL)
            {
              W42CharFmt bare = *ch, base_ch = *para_ch;
              gboolean own;

              g_ptr_array_add (w->pictures, png);
              g_ptr_array_add (w->picture_exts, (gpointer) ext);
              g_ptr_array_add (w->picture_mimes, (gpointer) mime);

              /* The picture's run has a font and a size like any other, and
               * the line it sits on is as tall as they make it. */
              bare.link = bare.bookmark = bare.comment = bare.field = NULL;
              bare.revision = 0;
              base_ch.link = base_ch.bookmark = base_ch.comment = base_ch.field = NULL;
              base_ch.revision = 0;
              own = memcmp (&bare, &base_ch, sizeof bare) != 0;
              if (own)
                g_string_append_printf (w->body, "<text:span text:style-name=\"T%d\">",
                                        text_style_index (w, &bare));
              if (object->wrap == W42_WRAP_INLINE)
                g_string_append_printf (w->body, "<draw:frame draw:name=\"Picture %u\" text:anchor-type=\"as-char\" svg:width=\"",
                                        w->pictures->len);
              else
                {
                  g_string_append_printf (w->body, "<draw:frame draw:name=\"Picture %u\" draw:style-name=\"%s\" text:anchor-type=\"paragraph\"",
                                          w->pictures->len, graphic_style_name (object));
                  write_odt_position (w->body, object);
                  g_string_append (w->body, " svg:width=\"");
                }
              twips_out (w->body, object->width);
              g_string_append (w->body, "\" svg:height=\"");
              twips_out (w->body, object->height);
              g_string_append_printf (w->body, "\"><draw:image xlink:href=\"Pictures/image%u.%s\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"/>",
                                      w->pictures->len, ext);
              if (fallback > 0)
                g_string_append_printf (w->body,
                                        "<draw:image xlink:href=\"Pictures/image%u.png\" xlink:type=\"simple\""
                                        " xlink:show=\"embed\" xlink:actuate=\"onLoad\"/>",
                                        fallback);
              g_string_append (w->body, "</draw:frame>");
              if (own)
                g_string_append (w->body, "</text:span>");
            }
          after_space = FALSE;
          continue;
        }
      if (run->footnote > 0)
        {
          int id = ++w->note_id;
          OdtMarks note_marks = { NULL, NULL, 0 };
          W42CharFmt plain = *ch, base = *para_ch;
          gboolean own;

          /* The mark in a span of its own where it is not in the
           * paragraph's text formatting. */
          plain.link = plain.bookmark = plain.comment = plain.field = NULL;
          plain.revision = 0;
          base.link = base.bookmark = base.comment = base.field = NULL;
          base.revision = 0;
          own = memcmp (&plain, &base, sizeof plain) != 0;
          if (own)
            g_string_append_printf (w->body, "<text:span text:style-name=\"T%d\">", text_style_index (w, &plain));
          g_string_append_printf (w->body, "<text:note text:id=\"w42n%d\" text:note-class=\"%s\"><text:note-citation>%d</text:note-citation><text:note-body>",
                                  id, run->endnote ? "endnote" : "footnote", run->footnote);
          for (guint b = 0; b < blocks->len; b++)
            {
              const W42Block *nb = g_ptr_array_index (blocks, b);
              const W42Block *nnext = b + 1 < blocks->len ? g_ptr_array_index (blocks, b + 1) : NULL;
              const W42ParaFmt *npa;
              const W42Style *nst;

              if (nb->note != run->footnote_id)
                continue;
              /* The note's paragraphs are set against their own style,
               * as the reader takes them, not the text's around the mark. */
              npa = &w42_ap_table_get (aps, nb->ap)->pa;
              nst = npa->style != NULL ? w42_stylesheet_find (w->styles, npa->style) : NULL;
              g_string_append_printf (w->body, "<text:p text:style-name=\"P%d\">",
                                      para_style_index (w, npa));
              write_runs (w, pt, aps, blocks, nb, nst != NULL ? &nst->ch : &w->base_ch,
                          &note_marks, same_flow (aps, nb, nnext) ? nnext : NULL);
              g_string_append (w->body, "</text:p>");
            }
          g_string_append (w->body, "</text:note-body></text:note>");
          if (own)
            g_string_append (w->body, "</text:span>");
          after_space = FALSE;
          continue;
        }

      /* An index entry is not a field in OpenDocument but a pair of
       * marks round the words, with the term in the first of them. */
      if (ch->field != NULL && g_str_has_prefix (ch->field, "XE"))
        {
          const char *colon = strchr (ch->field, ':');
          char *term = colon != NULL && colon[1] != '\0'
                         ? g_strdup (colon + 1)
                         : g_strndup (block->text->str + run->byte_offset, run->n_bytes);
          int id = ++w->n_index_marks;

          g_string_append_printf (w->body,
            "<text:alphabetical-index-mark-start text:id=\"IMark%d\" text:string-value=\"", id);
          xml_escape (w->body, term, strlen (term));
          g_string_append (w->body, "\"/>");
          write_run_text (w, ch, para_ch, block->text->str + run->byte_offset, run->n_bytes, &after_space);
          g_string_append_printf (w->body,
            "<text:alphabetical-index-mark-end text:id=\"IMark%d\"/>", id);
          g_free (term);
          continue;
        }

      /* The field elements carry their result as content.  A field
       * OpenDocument has no element for -- a table's formula -- keeps its
       * result as text: written as a word count, it counted words. */
      if (ch->field != NULL)
        {
          const char *el = g_str_equal (ch->field, "PAGE") ? "page-number"
                         : g_str_equal (ch->field, "NUMPAGES") ? "page-count"
                         : g_str_equal (ch->field, "DATE") ? "date"
                         : g_str_equal (ch->field, "TIME") ? "time"
                         : g_str_equal (ch->field, "FILENAME") ? "file-name"
                         : g_str_equal (ch->field, "NUMWORDS") ? "word-count" : NULL;

          if (el != NULL)
            {
              W42CharFmt plain = *ch, base = *para_ch;
              gboolean own;

              plain.link = plain.bookmark = plain.comment = plain.field = NULL;
              plain.revision = 0;
              base.link = base.bookmark = base.comment = base.field = NULL;
              base.revision = 0;
              own = memcmp (&plain, &base, sizeof plain) != 0;
              if (own)
                g_string_append_printf (w->body, "<text:span text:style-name=\"T%d\">", text_style_index (w, &plain));
              g_string_append_printf (w->body, "<text:%s>", el);
              xml_escape (w->body, block->text->str + run->byte_offset, run->n_bytes);
              g_string_append_printf (w->body, "</text:%s>", el);
              if (own)
                g_string_append (w->body, "</text:span>");
              after_space = FALSE;
              continue;
            }
        }

      /* A span only where the run differs from the paragraph's text. */
      write_run_text (w, ch, para_ch, block->text->str + run->byte_offset, run->n_bytes, &after_space);
    }
  if (open_link != NULL)
    g_string_append (w->body, "</text:a>");
  {
    /* A mark the next paragraph goes on with stays open over the break:
     * closed and opened again, one bookmark became two and one comment
     * several. */
    const W42CharFmt *go_on = NULL;

    if (next != NULL && next->runs->len > 0)
      go_on = &w42_ap_table_get (aps, g_array_index (next->runs, W42Run, 0).ap)->ch;
    if (marks->bookmark != NULL && (go_on == NULL || go_on->bookmark != marks->bookmark))
      close_bookmark (w, marks);
    if (marks->comment != NULL && (go_on == NULL || go_on->comment != marks->comment))
      close_comment (w, marks);
  }
}

/* A header's or a footer's text, its fields as their elements. */
static void
write_page_text (GString *s, const W42PageText *text, int style_idx)
{
  const char *p = text->text != NULL ? text->text : "";
  gboolean after_space = TRUE;

  g_string_append_printf (s, "<text:p text:style-name=\"MP%d\">", style_idx);
  while (*p)
    {
      const char *brace = strchr (p, '{');
      const char *close = brace != NULL ? strchr (brace, '}') : NULL;

      if (brace == NULL || close == NULL)
        {
          write_odt_text (s, p, strlen (p), &after_space);
          break;
        }
      write_odt_text (s, p, brace - p, &after_space);
      if (g_ascii_strncasecmp (brace, "{PAGE}", 6) == 0)
        g_string_append (s, "<text:page-number text:select-page=\"current\">1</text:page-number>");
      else if (g_ascii_strncasecmp (brace, "{NUMPAGES}", 10) == 0)
        g_string_append (s, "<text:page-count>1</text:page-count>");
      else if (g_ascii_strncasecmp (brace, "{DATE}", 6) == 0)
        g_string_append (s, "<text:date/>");
      else
        {
          write_odt_text (s, brace, close - brace + 1, &after_space);
          p = close + 1;
          continue;
        }
      after_space = FALSE;
      p = close + 1;
    }
  g_string_append (s, "</text:p>");
}

static void
list_item_open (OdtWriter *w, const W42ParaFmt *pa, gboolean own)
{
  /* Numbering begun again is said on the item it begins at. */
  if (own && pa->list_start > 0 && w42_list_is_numbered ((W42ListKind) pa->list))
    g_string_append_printf (w->body, "<text:list-item text:start-value=\"%d\">", pa->list_start);
  else
    g_string_append (w->body, "<text:list-item>");
}

/* Opens and closes lists so that the paragraph with `pa` goes in at its
 * level, or outside them all when `in_list` is FALSE. */
static void
lists_to (OdtWriter *w, OdtLists *ls, const W42ParaFmt *pa, gboolean in_list)
{
  int want = in_list && pa->list != W42_LIST_NONE ? MIN (pa->list_level, 8) + 1 : 0;

  while (ls->depth > want ||
         (ls->depth > 0 && ls->depth == want && ls->kind[ls->depth - 1] != pa->list))
    {
      ls->depth--;
      g_string_append (w->body, "</text:list-item></text:list>");
    }
  if (want > 0 && ls->depth == want && ls->used[want - 1])
    {
      /* Another item at this level: every paragraph is an item of its
       * own, numbered, as Word42 has it.  One put in after a sub-list, in
       * the item before, was a paragraph with no number to LibreOffice. */
      g_string_append (w->body, "</text:list-item>");
      list_item_open (w, pa, TRUE);
    }
  if (want == 0)
    ls->resume = FALSE;
  while (ls->depth < want)
    {
      int kind = pa->list < W42_LIST_KINDS ? pa->list : W42_LIST_NUMBER;

      /* The nested lists name their style too: one that did not took its
       * parent's, and a bullet under a number came back numbered.  One
       * the last section left off goes on counting in this one. */
      g_string_append_printf (w->body, "<text:list text:style-name=\"L%d\"%s>", kind,
                              ls->depth == 0 && ls->resume ? " text:continue-numbering=\"true\"" : "");
      ls->resume = FALSE;
      w->list_style_used[kind] = 1;
      list_item_open (w, pa, ls->depth + 1 == want);
      ls->kind[ls->depth] = pa->list;
      /* A level passed through on the way down counts as had: what comes
       * next at it is an item of its own. */
      ls->used[ls->depth] = ls->depth + 1 < want;
      ls->depth++;
    }
  if (want > 0)
    ls->used[want - 1] = TRUE;
}

static void
lists_close (OdtWriter *w, OdtLists *ls)
{
  while (ls->depth > 0)
    {
      ls->depth--;
      g_string_append (w->body, "</text:list-item></text:list>");
    }
}

/* Whether a section can begin at block `b`: before a table, but not
 * inside one; before a text box, but not between the paragraphs of one;
 * anywhere else in the body. */
static gboolean
section_opens_here (W42ApTable *aps, GPtrArray *blocks, guint b, int table_open)
{
  const W42Block *block = g_ptr_array_index (blocks, b);
  const W42Block *prev = b > 0 ? g_ptr_array_index (blocks, b - 1) : NULL;
  guint8 side = w42_ap_table_get (aps, block->ap)->pa.frame_side;

  if (block->table >= 0)
    return block->table != table_open;
  return side == W42_FRAME_NONE || prev == NULL || prev->table >= 0 || prev->note >= 0 ||
         w42_ap_table_get (aps, prev->ap)->pa.frame_side != side;
}

/* The automatic style of a header's or a footer's paragraph: where it
 * sits. */
static void
write_page_text_style (GString *s, int idx, const W42PageText *text)
{
  g_string_append_printf (s, "<style:style style:name=\"MP%d\" style:family=\"paragraph\">"
                             "<style:paragraph-properties fo:text-align=\"%s\"/></style:style>",
                          idx, text != NULL && text->align == W42_ALIGN_CENTER ? "center"
                             : text != NULL && text->align == W42_ALIGN_RIGHT ? "end" : "start");
}

gboolean
w42_odt_save (W42PieceTable *pt, const W42PageSetup *page, GFile *file, GError **error)
{
  GPtrArray *blocks;
  W42ApTable *aps;
  W42StyleSheet *styles;
  OdtWriter w;
  W42PageSetup pg;
  const W42PageText *header, *footer;
  OdtLists body_lists = { 0 }, cell_lists = { 0 }, box_lists = { 0 };
  OdtMarks marks = { NULL, NULL, 0 };
  int table_open = -1, row_open = -1;
  gboolean cell_covered = FALSE;      /* the open cell is one a merge swallowed */
  gboolean section_open = FALSE;      /* a text:section is open */
  W42ZipWriter *zip;
  GString *content, *stylesxml, *manifest;
  gboolean ok;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (page != NULL)
    pg = *page;
  else
    {
      memset (&pg, 0, sizeof pg);
      pg.width = 12240; pg.height = 15840;
      pg.margin_left = pg.margin_right = pg.margin_top = pg.margin_bottom = 1440;
    }

  blocks = w42_pt_snapshot_blocks (pt);
  aps = w42_pt_ap_table (pt);
  styles = w42_pt_stylesheet (pt);
  memset (&w, 0, sizeof w);
  w.auto_styles = g_string_new (NULL);
  w.body = g_string_new (NULL);
  w.pa_keys = g_ptr_array_new_with_free_func (g_free);
  w.ch_keys = g_ptr_array_new_with_free_func (g_free);
  w.pictures = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  w.shape_styles = g_string_new (NULL);
  w.cell_styles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  w.picture_exts = g_ptr_array_new ();
  w.picture_mimes = g_ptr_array_new ();
  w.styles = styles;
  {
    const W42Style *normal = w42_stylesheet_find (styles, "Normal");
    W42Fmt def;

    w42_fmt_init_default (&def);
    w.base_ch = normal != NULL ? normal->ch : def.ch;
  }

  /* The body. */
  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42ParaFmt *pa = &w42_ap_table_get (aps, block->ap)->pa;
      const W42Block *next = b + 1 < blocks->len ? g_ptr_array_index (blocks, b + 1) : NULL;

      if (block->note >= 0)
        continue;

      /* A section break: a text:section of its own from here, in a
       * section style saying its columns, as LibreOffice keeps a section
       * with columns.  Its page break is the new page its first paragraph
       * starts, said below with the page style.  It can open only between
       * what a section may hold -- not inside a table, nor between the
       * paragraphs of one text box -- and the lists close before it. */
      if (pa->section_break && section_opens_here (aps, blocks, b, table_open))
        {
          int n = ++w.n_sections;
          int cols = MAX (pa->columns, 1);

          if (body_lists.depth > 0)
            {
              lists_close (&w, &body_lists);
              body_lists.resume = TRUE;
            }
          if (section_open)
            g_string_append (w.body, "</text:section>");
          g_string_append_printf (w.auto_styles,
                                  "<style:style style:name=\"Sect%d\" style:family=\"section\">"
                                  "<style:section-properties text:dont-balance-text-columns=\"false\""
                                  " style:editable=\"false\">"
                                  "<style:columns fo:column-count=\"%d\" fo:column-gap=\"", n, cols);
          /* A gap of nought is Word42's half inch, which is what another
           * program is told when there is more than one column. */
          twips_out (w.auto_styles, cols > 1 && pa->column_gap <= 0 ? 720 : MAX (pa->column_gap, 0));
          g_string_append (w.auto_styles, "\"/></style:section-properties></style:style>");
          g_string_append_printf (w.body, "<text:section text:style-name=\"Sect%d\""
                                          " text:name=\"Section%d\">", n, n);
          section_open = TRUE;
        }

      /* Lists nest by level, in the body.  A table's cell and a text box
       * have lists of their own, and the body's close before either: a
       * list left open over a text box's frame was no XML at all. */
      lists_to (&w, &body_lists, pa, block->table < 0 && pa->frame_side == W42_FRAME_NONE);

      /* Tables. */
      if (block->table >= 0 && block->table != table_open)
        {
          const W42TableProps *tp = w42_pt_table_props (pt, block->table);
          int n_cols = tp != NULL ? tp->n_cols : 1;
          int t = ++w.n_tables;

          g_string_append_printf (w.auto_styles, "<style:style style:name=\"Table%d\" style:family=\"table\"><style:table-properties style:width=\"", t);
          twips_out (w.auto_styles, pg.width - pg.margin_left - pg.margin_right);
          g_string_append_printf (w.auto_styles, "\" table:align=\"left\"%s/></style:style>",
                                  pa->section_break ? " fo:break-before=\"page\"" : "");
          for (int c = 0; c < n_cols; c++)
            {
              int width = tp != NULL && c < (int) tp->widths->len ? g_array_index (tp->widths, int, c) : 0;

              if (width <= 0)
                width = (pg.width - pg.margin_left - pg.margin_right) / n_cols;
              g_string_append_printf (w.auto_styles, "<style:style style:name=\"Table%d.C%d\" style:family=\"table-column\"><style:table-column-properties style:column-width=\"", t, c);
              twips_out (w.auto_styles, width);
              g_string_append (w.auto_styles, "\"/></style:style>");
            }
          g_string_append_printf (w.auto_styles, "<style:style style:name=\"Table%d.Cell\" style:family=\"table-cell\"><style:table-cell-properties fo:padding=\"0.03in\" fo:border=\"%s\"/></style:style>",
                                  t, tp != NULL && !tp->borders ? "none" : "0.5pt solid #000000");
          g_string_append_printf (w.body, "<table:table table:name=\"Table%d\" table:style-name=\"Table%d\">", t, t);
          for (int c = 0; c < n_cols; c++)
            g_string_append_printf (w.body, "<table:table-column table:style-name=\"Table%d.C%d\"/>", t, c);
          table_open = block->table;
          row_open = -1;
        }
      if (block->table >= 0)
        {
          const W42Block *prev = b > 0 ? g_ptr_array_index (blocks, b - 1) : NULL;
          gboolean cell_start = prev == NULL || prev->table != block->table ||
                                prev->row != block->row || prev->col != block->col;
          gboolean cell_end = next == NULL || next->table != block->table ||
                              next->row != block->row || next->col != block->col;

          if (block->row != row_open)
            {
              const W42TableProps *tp = w42_pt_table_props (pt, block->table);
              int least = w42_pt_table_get_row_height (pt, block->table, block->row);
              int n_header = tp != NULL ? tp->header_rows : 0;

              if (row_open >= 0)
                g_string_append (w.body, "</table:table-row>");
              if (row_open >= 0 && row_open + 1 == n_header)
                g_string_append (w.body, "</table:table-header-rows>");
              if (block->row == 0 && n_header > 0)
                g_string_append (w.body, "<table:table-header-rows>");
              if (least > 0)
                {
                  g_string_append_printf (w.auto_styles, "<style:style style:name=\"Table%d.R%d\" style:family=\"table-row\"><style:table-row-properties style:min-row-height=\"",
                                          w.n_tables, block->row);
                  twips_out (w.auto_styles, least);
                  g_string_append (w.auto_styles, "\"/></style:style>");
                  g_string_append_printf (w.body, "<table:table-row table:style-name=\"Table%d.R%d\">", w.n_tables, block->row);
                }
              else
                g_string_append (w.body, "<table:table-row>");
              row_open = block->row;
            }
          if (cell_start)
            cell_covered = w42_ap_table_get (aps, block->cell_ap)->pa.cell_vspan == W42_CELL_COVERED;
          /* A cell a merge has swallowed keeps whatever is in it — it is
           * simply not shown — so it is written whole, under the name
           * OpenDocument gives such a cell. */
          if (cell_start && cell_covered)
            g_string_append (w.body, "<table:covered-table-cell office:value-type=\"string\">");
          if (cell_start && !cell_covered)
            {
              {
                const W42ParaFmt *cpa = &w42_ap_table_get (aps, block->cell_ap)->pa;
                const W42TableProps *props = w42_pt_table_props (pt, block->table);
                gboolean plain_table = props == NULL ||
                                       memcmp (props->edge, &(W42BorderEdge[W42_N_EDGES]) { { 0, 0, 0 } },
                                               sizeof props->edge) == 0;

                if ((cpa->border & W42_BORDER_CELL_SET) || cpa->has_shading_color ||
                    cpa->shading > 0 || cpa->cell_valign != W42_CELL_VALIGN_TOP || !plain_table)
                  {
                    /* A style of the cell's own: its sides and their
                     * lines -- the table's, where the cell has none of its
                     * own -- its background, and where its text sits.
                     * One style per distinct look. */
                    static const char *names[4] = { "fo:border-top", "fo:border-bottom", "fo:border-left", "fo:border-right" };
                    int sides = (cpa->border & W42_BORDER_CELL_SET)
                                  ? (cpa->border & W42_BORDER_BOX)
                                  : (props == NULL || props->borders) ? W42_BORDER_BOX : 0;
                    W42BorderEdge edges[4];
                    GString *look = g_string_new (NULL);
                    char *name;
                    gboolean first_row = block->row == 0, last_row = TRUE;
                    int n_cols = props != NULL ? props->n_cols : 1;

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
                    for (int k = 0; k < 4; k++)
                      {
                        gboolean outer = (k == W42_EDGE_TOP && first_row) || (k == W42_EDGE_BOTTOM && last_row) ||
                                         (k == W42_EDGE_LEFT && block->col == 0) ||
                                         (k == W42_EDGE_RIGHT && block->col + block->span >= n_cols);
                        const W42BorderEdge *e = &cpa->edge[k];

                        if ((cpa->border & W42_BORDER_CELL_SET) && (e->width != 0 || e->style != 0 || e->color != 0))
                          edges[k] = *e;
                        else if (props != NULL)
                          edges[k] = props->edge[outer ? k : (k <= W42_EDGE_BOTTOM ? W42_EDGE_INSIDE_H : W42_EDGE_INSIDE_V)];
                        else
                          edges[k] = (W42BorderEdge) { 0, 0, 0 };
                        if (edges[k].style == W42_BORDER_NONE)
                          sides &= ~(1 << k);
                      }
                    for (int k = 0; k < 4; k++)
                      if (sides & (1 << k))
                        {
                          char buf[G_ASCII_DTOSTR_BUF_SIZE];
                          int width = W42_EDGE_WIDTH (&edges[k]);

                          /* A double line is three widths across, as
                           * OpenDocument measures it. */
                          if (edges[k].style == W42_BORDER_DOUBLE)
                            width *= 3;
                          g_string_append_printf (look, " %s=\"%spt %s #%06x\"", names[k],
                                                  g_ascii_formatd (buf, sizeof buf, "%.2f", width / 20.0),
                                                  w42_border_style_css (edges[k].style),
                                                  edges[k].color & 0xFFFFFF);
                          if (edges[k].style == W42_BORDER_DOUBLE)
                            g_string_append_printf (look, " %s-%s=\"%spt %spt %spt\"",
                                                    "style:border-line-width", names[k] + 10,
                                                    g_ascii_formatd (buf, sizeof buf, "%.2f", W42_EDGE_WIDTH (&edges[k]) / 20.0),
                                                    g_ascii_formatd (buf, sizeof buf, "%.2f", W42_EDGE_WIDTH (&edges[k]) / 20.0),
                                                    g_ascii_formatd (buf, sizeof buf, "%.2f", W42_EDGE_WIDTH (&edges[k]) / 20.0));
                        }
                      else
                        g_string_append_printf (look, " %s=\"none\"", names[k]);
                    if (cpa->has_shading_color)
                      g_string_append_printf (look, " fo:background-color=\"#%06x\"",
                                              cpa->shading_color & 0xFFFFFF);
                    else if (cpa->shading > 0)
                      {
                        int grey = 255 - (int) cpa->shading * 255 / 100;

                        g_string_append_printf (look, " fo:background-color=\"#%02x%02x%02x\"", grey, grey, grey);
                      }
                    if (cpa->cell_valign == W42_CELL_VALIGN_CENTER)
                      g_string_append (look, " style:vertical-align=\"middle\"");
                    else if (cpa->cell_valign == W42_CELL_VALIGN_BOTTOM)
                      g_string_append (look, " style:vertical-align=\"bottom\"");

                    name = g_hash_table_lookup (w.cell_styles, look->str);
                    if (name == NULL)
                      {
                        name = g_strdup_printf ("Table%d.Cell%u", w.n_tables,
                                                g_hash_table_size (w.cell_styles) + 1);
                        g_hash_table_insert (w.cell_styles, g_strdup (look->str), name);
                        g_string_append_printf (w.auto_styles, "<style:style style:name=\"%s\" style:family=\"table-cell\"><style:table-cell-properties fo:padding=\"0.03in\"%s/></style:style>",
                                                name, look->str);
                      }
                    g_string_append_printf (w.body, "<table:table-cell table:style-name=\"%s\" office:value-type=\"string\"", name);
                    g_string_free (look, TRUE);
                  }
                else
                  g_string_append_printf (w.body, "<table:table-cell table:style-name=\"Table%d.Cell\" office:value-type=\"string\"", w.n_tables);
              }
              if (block->span > 1)
                g_string_append_printf (w.body, " table:number-columns-spanned=\"%d\"", block->span);
              {
                int vspan = w42_ap_table_get (aps, block->cell_ap)->pa.cell_vspan;

                if (vspan > 1 && vspan != W42_CELL_COVERED)
                  g_string_append_printf (w.body, " table:number-rows-spanned=\"%d\"", vspan);
              }
              g_string_append (w.body, ">");
            }
          lists_to (&w, &cell_lists, pa, TRUE);
          write_paragraph (&w, pt, aps, blocks, block, &marks, next);
          if (cell_end)
            {
              lists_close (&w, &cell_lists);
              g_string_append (w.body, cell_covered ? "</table:covered-table-cell>"
                                                    : "</table:table-cell>");
              for (int k = 1; k < block->span; k++)
                g_string_append (w.body, "<table:covered-table-cell/>");
            }
          if (next == NULL || next->table != block->table)
            {
              const W42TableProps *tp = w42_pt_table_props (pt, block->table);

              g_string_append (w.body, "</table:table-row>");
              if (tp != NULL && tp->header_rows > 0 && block->row + 1 <= tp->header_rows)
                g_string_append (w.body, "</table:table-header-rows>");
              g_string_append (w.body, "</table:table>");
              table_open = -1;
              row_open = -1;
            }
          continue;
        }

      {
        const W42ParaFmt *fpa = &w42_ap_table_get (aps, block->ap)->pa;
        const W42Block *fprev = b > 0 ? g_ptr_array_index (blocks, b - 1) : NULL;
        const W42Block *fnext = b + 1 < blocks->len ? g_ptr_array_index (blocks, b + 1) : NULL;
        gboolean framed = fpa->frame_side != W42_FRAME_NONE;
        gboolean prev_same = fprev != NULL && fprev->table < 0 && fprev->note < 0 &&
                             w42_ap_table_get (aps, fprev->ap)->pa.frame_side == fpa->frame_side;
        gboolean next_same = fnext != NULL && fnext->table < 0 && fnext->note < 0 &&
                             w42_ap_table_get (aps, fnext->ap)->pa.frame_side == fpa->frame_side;

        if (framed && !prev_same)
          {
            /* The box hangs on an empty paragraph of its own. */
            g_string_append_printf (w.body, "<text:p><draw:frame draw:style-name=\"%s\" text:anchor-type=\"paragraph\" svg:width=\"",
                                    fpa->frame_side == W42_FRAME_LEFT ? "frL" : "frR");
            twips_out (w.body, fpa->frame_width > 0 ? fpa->frame_width : 3120);
            g_string_append (w.body, "\"><draw:text-box>");
          }
        if (framed)
          lists_to (&w, &box_lists, pa, TRUE);
        write_paragraph (&w, pt, aps, blocks, block, &marks, next);
        if (framed && !next_same)
          {
            lists_close (&w, &box_lists);
            g_string_append (w.body, "</draw:text-box></draw:frame></text:p>");
          }
      }
    }
  lists_close (&w, &body_lists);
  if (section_open)
    g_string_append (w.body, "</text:section>");

  /* content.xml: the automatic styles that the body referred to. */
  content = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content " ODT_NS ">");
  g_string_append (content, "<office:automatic-styles>");
  for (guint i = 0; i < w.pa_keys->len; i++)
    {
      const W42ParaFmt *pa = g_ptr_array_index (w.pa_keys, i);
      const W42Style *style = pa->style != NULL ? w42_stylesheet_find (styles, pa->style) : NULL;
      char *parent = style_id_for (style != NULL ? style->name : NULL);

      g_string_append_printf (content, "<style:style style:name=\"P%u\" style:family=\"paragraph\" style:parent-style-name=\"%s\"",
                              i + 1, parent);
      g_free (parent);
      /* A section starts a page: its first paragraph starts one in the
       * document's page style, which is how LibreOffice breaks a page for
       * a section.  Said so, rather than with fo:break-before, the break
       * is the section's and not the paragraph's own, which a paragraph
       * may have as well. */
      if (pa->section_break)
        g_string_append (content, " style:master-page-name=\"Standard\"");
      if (pa->list != W42_LIST_NONE && pa->list < W42_LIST_KINDS)
        {
          g_string_append_printf (content, " style:list-style-name=\"L%d\"", (int) pa->list);
          w.list_style_used[pa->list] = 1;     /* named here, so declared below */
        }
      g_string_append (content, ">");
      write_para_props_xml (content, pa, style != NULL ? &style->pa : NULL);
      g_string_append (content, "</style:style>");
    }
  for (guint i = 0; i < w.ch_keys->len; i++)
    {
      g_string_append_printf (content, "<style:style style:name=\"T%u\" style:family=\"text\">", i + 1);
      /* In full, with nothing left to the document's default: a span is
       * applied over its paragraph's style, and that style may say
       * something quite different from the default.  A heading's run whose
       * font happened to match the default would otherwise come back
       * wearing the heading style's font instead of its own. */
      write_text_props_xml (content, g_ptr_array_index (w.ch_keys, i), NULL);
      g_string_append (content, "</style:style>");
    }
  for (int k = 1; k < W42_LIST_KINDS; k++)
    {
      if (!w.list_style_used[k])
        continue;
      g_string_append_printf (content, "<text:list-style style:name=\"L%d\">", k);
      for (int lv = 1; lv <= 9; lv++)
        {
          char marker[16];
          char tab[G_ASCII_DTOSTR_BUF_SIZE], left[G_ASCII_DTOSTR_BUF_SIZE];

          w42_list_marker ((W42ListKind) k, 1, marker, sizeof marker);
          if (w42_list_is_bullet ((W42ListKind) k))
            g_string_append_printf (content, "<text:list-level-style-bullet text:level=\"%d\" text:bullet-char=\"%s\">", lv, marker);
          else
            g_string_append_printf (content, "<text:list-level-style-number text:level=\"%d\" style:num-suffix=\".\" style:num-format=\"%s\">", lv,
                                    k == W42_LIST_LOWER_LETTER ? "a" : k == W42_LIST_UPPER_LETTER ? "A"
                                    : k == W42_LIST_LOWER_ROMAN ? "i" : k == W42_LIST_UPPER_ROMAN ? "I" : "1");
          /* The C locale's full stop: "0,50in" is no length in ODF. */
          g_string_append_printf (content, "<style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\">"
                                  "<style:list-level-label-alignment text:label-followed-by=\"listtab\" text:list-tab-stop-position=\"%sin\" fo:text-indent=\"-0.25in\" fo:margin-left=\"%sin\"/>"
                                  "</style:list-level-properties>%s",
                                  g_ascii_formatd (tab, sizeof tab, "%.2f", 0.25 * lv),
                                  g_ascii_formatd (left, sizeof left, "%.2f", 0.25 * lv),
                                  w42_list_is_bullet ((W42ListKind) k) ? "</text:list-level-style-bullet>" : "</text:list-level-style-number>");
        }
      g_string_append (content, "</text:list-style>");
    }
  g_string_append (content, w.auto_styles->str);
  g_string_append (content,
    "<style:style style:name=\"frL\" style:family=\"graphic\"><style:graphic-properties "
    "style:wrap=\"right\" style:horizontal-pos=\"left\" style:horizontal-rel=\"paragraph\" "
    "style:vertical-pos=\"top\" style:vertical-rel=\"paragraph\" fo:margin-right=\"0.125in\" fo:margin-bottom=\"0.125in\"/></style:style>"
    "<style:style style:name=\"frR\" style:family=\"graphic\"><style:graphic-properties "
    "style:wrap=\"left\" style:horizontal-pos=\"right\" style:horizontal-rel=\"paragraph\" "
    "style:vertical-pos=\"top\" style:vertical-rel=\"paragraph\" fo:margin-left=\"0.125in\" fo:margin-bottom=\"0.125in\"/></style:style>"
    /* Word 97's other wrapping styles: the text above and below, and
     * running on over or under the object. */
    "<style:style style:name=\"frTB\" style:family=\"graphic\"><style:graphic-properties "
    "style:wrap=\"none\" style:horizontal-pos=\"from-left\" style:horizontal-rel=\"paragraph\" "
    "style:vertical-pos=\"from-top\" style:vertical-rel=\"paragraph\" fo:margin-bottom=\"0.125in\"/></style:style>"
    "<style:style style:name=\"frFront\" style:family=\"graphic\"><style:graphic-properties "
    "style:wrap=\"run-through\" style:run-through=\"foreground\" style:horizontal-pos=\"from-left\" style:horizontal-rel=\"paragraph\" "
    "style:vertical-pos=\"from-top\" style:vertical-rel=\"paragraph\"/></style:style>"
    "<style:style style:name=\"frBehind\" style:family=\"graphic\"><style:graphic-properties "
    "style:wrap=\"run-through\" style:run-through=\"background\" style:horizontal-pos=\"from-left\" style:horizontal-rel=\"paragraph\" "
    "style:vertical-pos=\"from-top\" style:vertical-rel=\"paragraph\"/></style:style>");
  g_string_append (content, w.shape_styles->str);
  g_string_append (content, "</office:automatic-styles><office:body><office:text>");
  g_string_append (content, w.body->str);
  g_string_append (content, "</office:text></office:body></office:document-content>");

  /* styles.xml: the named styles, the page, the header and footer. */
  header = w42_pt_get_header (pt);
  footer = w42_pt_get_footer (pt);
  stylesxml = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-styles " ODT_NS ">");
  g_string_append (stylesxml, "<office:styles>");
  g_string_append (stylesxml, "<style:default-style style:family=\"paragraph\">");
  write_para_props_xml (stylesxml, &((const W42Style *) w42_stylesheet_get (styles, 0))->pa, NULL);
  /* Normal's text, its language included: that is the document's
   * language, and the default style is where LibreOffice looks for it.
   * The headings, the header's and the footer's paragraphs and every
   * style of no parent take after this one; without the language here
   * they were in whatever language the reader's own program is. */
  write_text_props_xml (stylesxml, &w.base_ch, NULL);
  g_string_append (stylesxml, "</style:default-style>");
  for (guint i = 0; i < w42_stylesheet_size (styles); i++)
    {
      const W42Style *s = w42_stylesheet_get (styles, i);
      gboolean is_normal = g_ascii_strcasecmp (s->name, "Normal") == 0;
      /* The style it is based on, of its own family: a character style
       * cannot take after a paragraph style.  One based on none takes
       * after the default style, which is Normal's, written above. */
      const W42Style *based = s->based_on != NULL ? w42_stylesheet_find (styles, s->based_on) : NULL;
      char *id = style_id_for (s->name);

      if (based != NULL && (based == s || based->character != s->character))
        based = NULL;
      g_string_append_printf (stylesxml, "<style:style style:name=\"%s\" style:display-name=\"", id);
      g_free (id);
      /* Normal is OpenDocument's "Standard", shown so: LibreOffice takes
       * a "Standard" shown as anything else for a style of its own beside
       * its default one. */
      xml_escape (stylesxml, is_normal ? "Standard" : s->name, strlen (is_normal ? "Standard" : s->name));
      g_string_append_printf (stylesxml, "\" style:family=\"%s\"", s->character ? "text" : "paragraph");
      if (based != NULL)
        {
          char *parent_id = style_id_for (based->name);

          g_string_append_printf (stylesxml, " style:parent-style-name=\"%s\"", parent_id);
          g_free (parent_id);
        }
      if (!is_normal && !s->character)
        g_string_append (stylesxml, " style:next-style-name=\"Standard\"");
      /* The level said, none included: a heading style taken out of the
       * outline is otherwise Word42's Heading 1 again, at level 1. */
      if (s->outline > 0 && !s->character)
        g_string_append_printf (stylesxml, " style:default-outline-level=\"%d\"", s->outline);
      else if (!s->character)
        g_string_append (stylesxml, " style:default-outline-level=\"\"");
      g_string_append (stylesxml, ">");
      if (!s->character)
        {
          /* A style takes from its parent what it does not say, so what
           * it has as nought -- no space after a heading whose Normal has
           * some -- is said, against the parent named above. */
          const W42Style *parent = based;

          if (parent == NULL && !is_normal)
            parent = w42_stylesheet_find (styles, "Normal");
          write_para_props_xml (stylesxml, &s->pa,
                                parent != NULL && parent != s ? &parent->pa : NULL);
        }
      write_text_props_xml (stylesxml, &s->ch, NULL);
      g_string_append (stylesxml, "</style:style>");
    }
  g_string_append (stylesxml, "</office:styles><office:automatic-styles>");
  /* OpenDocument's margin runs to the header, and the header's box --
   * its height and its gap to the text -- makes up the rest of Word's.
   * The header stands half an inch from the edge, where Word puts it and
   * the .docx says it is. */
  const W42PageText *heads[3], *feet[3];
  gboolean has_header, has_footer;

  /* The page's own, the left (even) pages' and the first page's, each
   * where the document has it: the choice of the even pages' and the
   * title page's is kept apart from their text, which may be blank. */
  heads[0] = header;
  feet[0] = footer;
  heads[1] = w42_pt_get_facing_pages (pt) ? w42_pt_get_header_kind (pt, W42_PAGE_TEXT_EVEN) : NULL;
  feet[1] = w42_pt_get_facing_pages (pt) ? w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_EVEN) : NULL;
  heads[2] = w42_pt_get_title_page (pt) ? w42_pt_get_header_kind (pt, W42_PAGE_TEXT_FIRST) : NULL;
  feet[2] = w42_pt_get_title_page (pt) ? w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_FIRST) : NULL;
  has_header = has_footer = FALSE;
  for (int k = 0; k < 3; k++)
    {
      has_header |= heads[k] != NULL && heads[k]->text != NULL && *heads[k]->text;
      has_footer |= feet[k] != NULL && feet[k]->text != NULL && *feet[k]->text;
    }
  int top = has_header ? MIN (720, pg.margin_top / 2) : pg.margin_top;
  int bottom = has_footer ? MIN (720, pg.margin_bottom / 2) : pg.margin_bottom;

  g_string_append (stylesxml, "<style:page-layout style:name=\"Mpm1\"><style:page-layout-properties fo:page-width=\"");
  twips_out (stylesxml, pg.width);
  g_string_append (stylesxml, "\" fo:page-height=\"");
  twips_out (stylesxml, pg.height);
  g_string_append_printf (stylesxml, "\" style:print-orientation=\"%s\"", pg.width > pg.height ? "landscape" : "portrait");
  if (pg.has_border)
    {
      /* OpenDocument's page border stands inside the margins, the
       * padding between it and the text: Word's distance from the edge
       * becomes the margin, and the rest of the margin the padding. */
      int width = pg.border_width > 0 ? pg.border_width : W42_BORDER_HAIRLINE;
      int space = CLAMP (pg.border_space, 0, MIN (pg.width, pg.height) / 4);
      int pad_t = MAX (top - space - width, 0);
      int pad_b = MAX (bottom - space - width, 0);
      int pad_l = MAX (pg.margin_left - space - width, 0);
      int pad_r = MAX (pg.margin_right - space - width, 0);
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      g_string_append (stylesxml, " fo:margin-top=\"");  twips_out (stylesxml, space);
      g_string_append (stylesxml, "\" fo:margin-bottom=\""); twips_out (stylesxml, space);
      g_string_append (stylesxml, "\" fo:margin-left=\""); twips_out (stylesxml, space);
      g_string_append (stylesxml, "\" fo:margin-right=\""); twips_out (stylesxml, space);
      g_string_append (stylesxml, "\" fo:padding-top=\""); twips_out (stylesxml, pad_t);
      g_string_append (stylesxml, "\" fo:padding-bottom=\""); twips_out (stylesxml, pad_b);
      g_string_append (stylesxml, "\" fo:padding-left=\""); twips_out (stylesxml, pad_l);
      g_string_append (stylesxml, "\" fo:padding-right=\""); twips_out (stylesxml, pad_r);
      g_string_append_printf (stylesxml, "\" fo:border=\"%spt %s #%06x\"",
                              g_ascii_formatd (buf, sizeof buf, "%.2f", width / 20.0),
                              w42_border_style_css ((W42BorderStyle) pg.border_style),
                              pg.border_color & 0xFFFFFF);
    }
  else
    {
      g_string_append (stylesxml, " fo:margin-top=\"");
      twips_out (stylesxml, top);
      g_string_append (stylesxml, "\" fo:margin-bottom=\"");
      twips_out (stylesxml, bottom);
      g_string_append (stylesxml, "\" fo:margin-left=\"");
      twips_out (stylesxml, pg.margin_left);
      g_string_append (stylesxml, "\" fo:margin-right=\"");
      twips_out (stylesxml, pg.margin_right);
      g_string_append (stylesxml, "\"");
    }
  if (pg.has_background)
    g_string_append_printf (stylesxml, " fo:background-color=\"#%06x\"",
                            pg.background & 0xFFFFFF);
  if (w42_page_columns (&pg) > 1)
    {
      g_string_append_printf (stylesxml, "><style:columns fo:column-count=\"%d\" fo:column-gap=\"", w42_page_columns (&pg));
      twips_out (stylesxml, w42_page_column_gap (&pg));
      g_string_append (stylesxml, "\"/></style:page-layout-properties>");
    }
  else
    g_string_append (stylesxml, "/>");
  if (has_header)
    {
      g_string_append (stylesxml, "<style:header-style><style:header-footer-properties fo:min-height=\"");
      twips_out (stylesxml, pg.margin_top - top);
      g_string_append (stylesxml, "\" fo:margin-bottom=\"0in\"/></style:header-style>");
    }
  if (has_footer)
    {
      g_string_append (stylesxml, "<style:footer-style><style:header-footer-properties fo:min-height=\"");
      twips_out (stylesxml, pg.margin_bottom - bottom);
      g_string_append (stylesxml, "\" fo:margin-top=\"0in\"/></style:footer-style>");
    }
  g_string_append (stylesxml, "</style:page-layout>");
  /* Each its own paragraph style, for where it sits: MP1, 3 and 5 the
   * headers, MP2, 4 and 6 the footers.  They shared two, and the first
   * page's right-aligned header came back at the left. */
  for (int k = 0; k < 3; k++)
    {
      if (has_header && (k == 0 || heads[k] != NULL))
        write_page_text_style (stylesxml, 2 * k + 1, heads[k]);
      if (has_footer && (k == 0 || feet[k] != NULL))
        write_page_text_style (stylesxml, 2 * k + 2, feet[k]);
    }
  g_string_append (stylesxml, "</office:automatic-styles><office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"Mpm1\">");
  {
    /* In OpenDocument's order: the header, then the left pages' and the
     * first page's beside it, then the footers the same.  Those are only
     * there beside the plain one, which stands in empty when only they
     * have text: written alone, LibreOffice dropped the even pages' footer. */
    static const char *const suffix[3] = { "", "-left", "-first" };

    for (int part = 0; part < 2; part++)
      {
        const W42PageText *const *texts = part == 0 ? heads : feet;
        const char *what = part == 0 ? "header" : "footer";

        if (!(part == 0 ? has_header : has_footer))
          continue;
        for (int k = 0; k < 3; k++)
          {
            W42PageText empty = { NULL, W42_ALIGN_LEFT };

            if (k > 0 && texts[k] == NULL)
              continue;
            g_string_append_printf (stylesxml, "<style:%s%s>", what, suffix[k]);
            write_page_text (stylesxml, texts[k] != NULL ? texts[k] : &empty, 2 * k + 1 + part);
            g_string_append_printf (stylesxml, "</style:%s%s>", what, suffix[k]);
          }
      }
  }
  g_string_append (stylesxml, "</style:master-page></office:master-styles></office:document-styles>");

  /* The zip: mimetype first and stored, then the rest. */
  manifest = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                           "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.2\">"
                           "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.2\" manifest:media-type=\"" ODT_MIME "\"/>"
                           "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
                           "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>");
  for (guint i = 0; i < w.pictures->len; i++)
    g_string_append_printf (manifest, "<manifest:file-entry manifest:full-path=\"Pictures/image%u.%s\" manifest:media-type=\"%s\"/>",
                            i + 1, (const char *) g_ptr_array_index (w.picture_exts, i),
                            (const char *) g_ptr_array_index (w.picture_mimes, i));
  g_string_append (manifest, "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>");
  g_string_append (manifest, "</manifest:manifest>");

  zip = w42_zip_writer_new ();
  w42_zip_writer_add_stored (zip, "mimetype", ODT_MIME, strlen (ODT_MIME));
  w42_zip_writer_add (zip, "content.xml", content->str, content->len);
  w42_zip_writer_add (zip, "styles.xml", stylesxml->str, stylesxml->len);
  {
    /* What the document says about itself. */
    const W42DocInfo *info = w42_pt_get_info (pt);
    GString *meta = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
      "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.2\"><office:meta>"
      "<meta:generator>Word42</meta:generator>");
    static const struct { const char *tag; gsize offset; } fields[] = {
      { "dc:title",       G_STRUCT_OFFSET (W42DocInfo, title) },
      { "dc:subject",     G_STRUCT_OFFSET (W42DocInfo, subject) },
      /* The author is the one who made it, and -- as far as this file
       * knows -- the one who last saved it too. */
      { "meta:initial-creator", G_STRUCT_OFFSET (W42DocInfo, author) },
      { "dc:creator",     G_STRUCT_OFFSET (W42DocInfo, author) },
      { "meta:keyword",   G_STRUCT_OFFSET (W42DocInfo, keywords) },
      { "dc:description", G_STRUCT_OFFSET (W42DocInfo, comments) },
    };

    for (guint i = 0; i < G_N_ELEMENTS (fields); i++)
      {
        const char *value = G_STRUCT_MEMBER (const char *, info, fields[i].offset);

        if (value == NULL)
          continue;
        g_string_append_printf (meta, "<%s>", fields[i].tag);
        xml_escape (meta, value, strlen (value));
        g_string_append_printf (meta, "</%s>", fields[i].tag);
      }
    g_string_append (meta, "</office:meta></office:document-meta>");
    w42_zip_writer_add (zip, "meta.xml", meta->str, meta->len);
    g_string_free (meta, TRUE);
  }
  for (guint i = 0; i < w.pictures->len; i++)
    {
      char *name = g_strdup_printf ("Pictures/image%u.%s", i + 1,
                                    (const char *) g_ptr_array_index (w.picture_exts, i));
      GBytes *png = g_ptr_array_index (w.pictures, i);

      w42_zip_writer_add (zip, name, g_bytes_get_data (png, NULL), g_bytes_get_size (png));
      g_free (name);
    }
  w42_zip_writer_add (zip, "META-INF/manifest.xml", manifest->str, manifest->len);
  ok = w42_zip_writer_save (zip, file, error);
  w42_zip_writer_free (zip);

  g_string_free (content, TRUE);
  g_string_free (stylesxml, TRUE);
  g_string_free (manifest, TRUE);
  g_string_free (w.auto_styles, TRUE);
  g_string_free (w.body, TRUE);
  g_ptr_array_free (w.pa_keys, TRUE);
  g_ptr_array_free (w.ch_keys, TRUE);
  g_ptr_array_free (w.pictures, TRUE);
  g_string_free (w.shape_styles, TRUE);
  g_hash_table_destroy (w.cell_styles);
  g_ptr_array_free (w.picture_exts, TRUE);
  g_ptr_array_free (w.picture_mimes, TRUE);
  g_ptr_array_free (blocks, TRUE);
  return ok;
}
