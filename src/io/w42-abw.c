/* w42-abw.c - see w42-abw.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-abw.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <glib/gi18n.h>

#include "w42-build.h"
#include "w42-image.h"
#include "w42-lang.h"

/* ---------------------------------------------------------------------- */
/* props="a:b; c:d" -- AbiWord's CSS-like attribute                        */
/* ---------------------------------------------------------------------- */

typedef void (*PropFn) (const char *key, const char *value, gpointer data);

static void
each_prop (const char *props, PropFn fn, gpointer data)
{
  char **items;

  if (props == NULL)
    return;
  items = g_strsplit (props, ";", -1);
  for (int i = 0; items[i] != NULL; i++)
    {
      char *colon = strchr (items[i], ':');

      if (colon == NULL)
        continue;
      *colon = '\0';
      fn (g_strstrip (items[i]), g_strstrip (colon + 1), data);
    }
  g_strfreev (items);
}

/* Which edge a "top-style", "bot-thickness" or "left-color" key names. */
static int
abw_edge (const char *key)
{
  return g_str_has_prefix (key, "top") ? W42_EDGE_TOP : g_str_has_prefix (key, "bot") ? W42_EDGE_BOTTOM
       : g_str_has_prefix (key, "left") ? W42_EDGE_LEFT : W42_EDGE_RIGHT;
}

/* "1.5in", "12pt", "0.5cm", "2mm" -> twips. */
static int
length_twips (const char *value)
{
  double v = g_ascii_strtod (value, NULL);
  double per;

  if      (strstr (value, "pt") != NULL) per = 20.0;
  else if (strstr (value, "cm") != NULL) per = 1440.0 / 2.54;
  else if (strstr (value, "mm") != NULL) per = 1440.0 / 25.4;
  else if (strstr (value, "px") != NULL) per = 15.0;
  else                                   per = 1440.0;   /* inches, AbiWord's default */

  /* To the nearest twip.  Truncating loses one on nearly every round trip:
   * a 1 cm margin written as 0.3937in would come back as 566.  Clamped
   * first, because a cast that does not fit an int is undefined. */
  v *= per;
  if (isnan (v))
    return 0;
  v = CLAMP (v, -1000000.0, 1000000.0);
  return (int) (v < 0 ? v - 0.5 : v + 0.5);
}

static const char *
attr (const char **names, const char **values, const char *want)
{
  for (int i = 0; names != NULL && names[i] != NULL; i++)
    if (g_str_equal (names[i], want))
      return values[i];
  return NULL;
}

/* ====================================================================== */
/* Reading                                                                 */
/* ====================================================================== */

typedef struct {
  int         id;
  W42ListKind kind;
  int         start;
  int         seen;             /* its items so far: AbiWord counts a list on
                                 * through whatever comes between them */
} AbwList;

/* A style as the file gives it.  What it says is said over its parent,
 * which may come after it -- AbiWord writes the styles in the order the
 * document uses them -- so each is resolved once all of them are read. */
typedef struct {
  char       *name;
  char       *based;
  char       *props;
  gboolean    character;
  int         state;            /* 0 not yet, 1 under way, 2 resolved */
  W42ParaFmt  pa;
  W42CharFmt  ch;
} AbwStyle;

/* A column that a merged cell above still covers: to which row, and how
 * many columns wide the merge is. */
typedef struct {
  int until;
  int span;
} AbwCover;

/* What a <frame> or an <image> says about itself. */
typedef struct {
  gboolean    textbox, image;
  int         width, height;
  int         x, y;
  gboolean    at_block;         /* xpos and ypos are from its paragraph */
  const char *wrap_mode;
  W42ShapeKind shape;           /* a shape Word42 drew, the picture its likeness */
  double      line_pt;
  guint32     line_rgb, fill_rgb;
  gboolean    filled;
  const char *shape_text;
} AbwFrame;

typedef struct {
  W42Builder   b;
  W42PieceTable *pt;
  W42PageSetup *page;

  GArray      *lists;            /* AbwList */
  GHashTable  *list_index;       /* list id -> its index in lists, plus one */
  int          level_n[9];       /* the numbers Word42 will count, by level */
  W42ListKind  level_kind[9];
  GHashTable  *images;           /* data name -> GBytes, from the first pass */
  GArray      *table_cols;       /* each <table>'s columns, in file order */
  int          tables_seen;
  GArray      *cover;            /* AbwCover, per column of the open table */
  GHashTable  *bookmarks;        /* name -> where it starts */
  GPtrArray   *styles;           /* AbwStyle, until the last is read */

  GString     *text;
  W42CharFmt   para_ch;          /* the paragraph's base (from <p props>) */
  const char  *link;
  GPtrArray   *comments;         /* the <ann>s open: each its text, or NULL */
  int          section_n;        /* body sections seen */
  gboolean     in_hf, hf_footer;
  W42PageTextKind hf_kind;
  GString     *hf_text;
  W42Align     hf_align;
  int          skip_depth;
  int          annotate_depth;   /* inside <annotate>: the comment's text */
  GString     *annotate_text;

  gboolean     in_foot;
  gboolean     foot_first_para;
  W42ParaFmt   foot_outer_pa;    /* the paragraph the note sits in */
  W42CharFmt   foot_outer_para_ch, foot_outer_ch;
  const char  *foot_outer_link;
  guint        foot_comments;    /* the <ann>s open round the note */
  gboolean     mark_pending;     /* a footnote_ref's formatting, for the mark */
  W42CharFmt   mark_ch;
  gboolean     para_open;
  gboolean     pbr_pending;      /* a page break waiting for its paragraph */
  gsize        anchor_pos;       /* the mark after the last body paragraph */
  gboolean     anchor_ok;
  int          textbox_depth;    /* inside a text-box <frame> */
  int          tb_side, tb_width;
  gpointer     meta_field;      /* slot + 1 of the <m key="..."> being read */
  GString     *meta_text;
  char        *meta[5];         /* title, subject, author, keywords, comments */
} Abw;

/* AbiWord 2 names the shape of a list in the paragraph's own properties
 * -- "list-style:Bullet List" -- rather than in an <l> element, so the
 * name has to be understood as well as the number. */
static W42ListKind
list_style_kind (const char *name)
{
  static const struct { const char *name; W42ListKind kind; } NAMES[] = {
    { "Numbered List",    W42_LIST_NUMBER },
    { "Lower Case List",  W42_LIST_LOWER_LETTER },
    { "Upper Case List",  W42_LIST_UPPER_LETTER },
    { "Lower Roman List", W42_LIST_LOWER_ROMAN },
    { "Upper Roman List", W42_LIST_UPPER_ROMAN },
    { "Dashed List",      W42_LIST_BULLET_DASH },
    { "Square List",      W42_LIST_BULLET_SQUARE },
    { "Bullet List",      W42_LIST_BULLET },
  };

  for (guint i = 0; i < G_N_ELEMENTS (NAMES); i++)
    if (g_ascii_strcasecmp (name, NAMES[i].name) == 0)
      return NAMES[i].kind;

  /* Anything else that calls itself a list is a bulleted one, except the
   * headings, which are numbered. */
  if (strstr (name, "Heading") != NULL || strstr (name, "Numbered") != NULL)
    return W42_LIST_NUMBER;
  return W42_LIST_BULLET;
}

/* A paragraph's props carry character properties too -- the font its text
 * starts in -- but a bgcolor among them is the paragraph's background, not
 * a highlight on its text.  Reading it as both put a yellow highlight on
 * every run of a shaded paragraph. */
static void char_prop (const char *key, const char *value, gpointer data);

static void
para_char_prop (const char *key, const char *value, gpointer data)
{
  if (g_str_equal (key, "bgcolor") || g_str_equal (key, "background-color"))
    return;
  char_prop (key, value, data);
}

static void
para_prop (const char *key, const char *value, gpointer data)
{
  W42ParaFmt *pa = data;

  if (g_str_equal (key, "text-align"))
    {
      if (g_str_equal (value, "center")) pa->align = W42_ALIGN_CENTER;
      else if (g_str_equal (value, "right")) pa->align = W42_ALIGN_RIGHT;
      else if (g_str_equal (value, "justify")) pa->align = W42_ALIGN_JUSTIFY;
      else pa->align = W42_ALIGN_LEFT;
    }
  else if (g_str_equal (key, "margin-left"))   pa->indent_left = length_twips (value);
  else if (g_str_equal (key, "margin-right"))  pa->indent_right = length_twips (value);
  else if (g_str_equal (key, "text-indent"))   pa->indent_first = length_twips (value);
  else if (g_str_equal (key, "margin-top"))    pa->space_before = length_twips (value);
  else if (g_str_equal (key, "margin-bottom")) pa->space_after = length_twips (value);
  else if (g_str_equal (key, "line-height") && *value != '\0')
    {
      /* Either kind replaces the other, which a style may have set; and
       * a multiple of one is single spacing, which the model says as 0. */
      pa->line_spacing = 0;
      pa->line_spacing_pct = 0;
      if (strchr (value, '+') != NULL)          /* "14pt+": at least */
        pa->line_spacing = length_twips (value);
      else if (g_ascii_isdigit (value[strlen (value) - 1]))
        pa->line_spacing_pct = (int) (CLAMP (g_ascii_strtod (value, NULL), 0.0, 100.0) * 100.0 + 0.5);
      else
        pa->line_spacing = length_twips (value);
      if (pa->line_spacing_pct == 100)
        pa->line_spacing_pct = 0;
    }
  else if (g_str_equal (key, "list-style") && *value != '\0')
    pa->list = (guint8) (g_ascii_strcasecmp (value, "None") == 0
                         ? W42_LIST_NONE : list_style_kind (value));
  else if (g_str_equal (key, "keep-with-next")) pa->keep_next = g_str_equal (value, "yes");
  else if (g_str_equal (key, "keep-together"))  pa->keep_together = g_str_equal (value, "yes");
  else if (g_str_equal (key, "widows"))         pa->widow_control = atoi (value) > 0;
  else if (g_str_equal (key, "dom-dir"))        pa->rtl = g_str_equal (value, "rtl");
  else if (g_str_equal (key, "page-break-before")) pa->page_break_before = g_str_equal (value, "yes");
  else if (g_str_equal (key, "tabstops"))
    {
      /* "1.0in/L0,2.5in/R0": a position, a kind and a leader each.  The
       * list is the paragraph's stops, not more of its style's: nothing
       * said is none. */
      char **stops = g_strsplit (value, ",", -1);

      pa->n_tabs = 0;
      for (int i = 0; stops[i] != NULL; i++)
        {
          char *slash = strchr (stops[i], '/');
          W42TabKind kind = W42_TAB_LEFT;
          W42TabLeader leader = W42_TAB_LEAD_NONE;

          if (slash != NULL)
            {
              *slash = '\0';
              kind = slash[1] == 'C' ? W42_TAB_CENTER : slash[1] == 'R' ? W42_TAB_RIGHT
                   : slash[1] == 'D' ? W42_TAB_DECIMAL : W42_TAB_LEFT;
              if (slash[1] != '\0' && slash[2] >= '1' && slash[2] <= '3')
                leader = (W42TabLeader) (slash[2] - '0');
            }
          if (*g_strstrip (stops[i]) != '\0')
            w42_para_fmt_set_tab_leader (pa, length_twips (stops[i]), kind, leader);
        }
      g_strfreev (stops);
    }
  else if (g_str_has_suffix (key, "-style") && (g_str_has_prefix (key, "top") || g_str_has_prefix (key, "bot") ||
                                                 g_str_has_prefix (key, "left") || g_str_has_prefix (key, "right")))
    {
      /* AbiWord's line styles: 1 solid, 2 dotted, 3 dashed, 4 double, or
       * those by name; 0 and "none" take away a side a style had. */
      int e = abw_edge (key);

      if (g_str_equal (value, "0") || g_str_equal (value, "none"))
        pa->border &= (guint8) ~(1 << e);
      else
        {
          pa->border |= (guint8) (1 << e);
          pa->edge[e].style = g_str_equal (value, "2") || g_str_equal (value, "dotted") ? W42_BORDER_DOTTED
                            : g_str_equal (value, "3") || g_str_equal (value, "dashed") ? W42_BORDER_DASHED
                            : g_str_equal (value, "4") || g_str_equal (value, "double") ? W42_BORDER_DOUBLE
                            : W42_BORDER_SINGLE;
        }
    }
  else if (g_str_has_suffix (key, "-thickness") && (g_str_has_prefix (key, "top") || g_str_has_prefix (key, "bot") ||
                                                    g_str_has_prefix (key, "left") || g_str_has_prefix (key, "right")))
    pa->edge[abw_edge (key)].width = (guint8) CLAMP (length_twips (value), 5, 120);
  else if (g_str_has_suffix (key, "-color") && (g_str_has_prefix (key, "top") || g_str_has_prefix (key, "bot") ||
                                                g_str_has_prefix (key, "left") || g_str_has_prefix (key, "right")))
    {
      if (strlen (value) >= 6)
        pa->edge[abw_edge (key)].color = (guint32) strtoul (value + (value[0] == '#'), NULL, 16) & 0xFFFFFF;
    }
  else if (g_str_equal (key, "bgcolor") || g_str_equal (key, "background-color"))
    {
      /* "transparent" takes away a shading the style had. */
      if (g_str_equal (value, "transparent"))
        {
          pa->has_shading_color = 0;
          pa->shading_color = 0;
          pa->shading = 0;
        }
      else if (strlen (value) >= 6)
        {
          pa->shading_color = (guint32) strtoul (value + (value[0] == '#'), NULL, 16) & 0xFFFFFF;
          pa->has_shading_color = 1;
          pa->shading = 0;
        }
    }
}

static void
char_prop (const char *key, const char *value, gpointer data)
{
  W42CharFmt *ch = data;

  if (g_str_equal (key, "font-weight"))     ch->bold = g_str_equal (value, "bold");
  else if (g_str_equal (key, "font-style")) ch->italic = g_str_equal (value, "italic");
  else if (g_str_equal (key, "text-decoration"))
    {
      ch->underline = strstr (value, "underline") != NULL
                        ? (strstr (value, "double") != NULL ? W42_UNDERLINE_DOUBLE
                                                            : W42_UNDERLINE_SINGLE)
                        : W42_UNDERLINE_NONE;
      ch->strikeout = strstr (value, "line-through") != NULL;
      ch->overline = strstr (value, "overline") != NULL;
    }
  else if (g_str_equal (key, "text-spacing"))
    ch->spacing = (gint16) CLAMP (length_twips (value), -720, 720);
  else if (g_str_equal (key, "font-size"))
    {
      /* Half-points, as the model counts them, from however many decimals
       * the file gave: 9.5pt is 19 of them, not 18.  Range-checked before
       * the cast, which is undefined for a size that does not fit. */
      double pt = g_ascii_strtod (value, NULL);

      if (pt > 0.0 && pt < 1700.0)
        ch->size = CLAMP ((int) (pt * 2.0 + 0.5), 2, 3276);
    }
  else if (g_str_equal (key, "font-family")) ch->family = g_intern_string (value);
  else if (g_str_equal (key, "color"))
    {
      if (strlen (value) >= 6)
        ch->color = (guint32) strtoul (value + (value[0] == '#'), NULL, 16) & 0xFFFFFF;
    }
  else if (g_str_equal (key, "bgcolor"))
    {
      /* The nearest of Word's sixteen, which is what a highlight is: taken
       * all as yellow, a green one came back yellow. */
      if (g_str_equal (value, "transparent"))
        ch->highlight = 0;
      else if (strlen (value) >= 6)
        ch->highlight = (guint8) w42_highlight_nearest (
          (guint32) strtoul (value + (value[0] == '#'), NULL, 16) & 0xFFFFFF);
      else
        ch->highlight = 7;
    }
  else if (g_str_equal (key, "text-position"))
    ch->script = g_str_equal (value, "superscript") ? 1 : g_str_equal (value, "subscript") ? -1 : 0;
  else if (g_str_equal (key, "font-variant")) ch->smallcaps = g_str_equal (value, "small-caps");
  else if (g_str_equal (key, "text-transform")) ch->allcaps = g_str_equal (value, "uppercase");
  else if (g_str_equal (key, "lang"))
    {
      /* AbiWord's own property for the language of a run. */
      const char *known = w42_lang_normalise (value);

      if (known != NULL)
        ch->lang = known;
    }
}

static void
section_prop (const char *key, const char *value, gpointer data)
{
  Abw *a = data;

  if (g_str_equal (key, "page-margin-left"))        a->page->margin_left = length_twips (value);
  else if (g_str_equal (key, "page-margin-right"))  a->page->margin_right = length_twips (value);
  else if (g_str_equal (key, "page-margin-top"))    a->page->margin_top = length_twips (value);
  else if (g_str_equal (key, "page-margin-bottom")) a->page->margin_bottom = length_twips (value);
  else if (g_str_equal (key, "columns"))
    {
      int n = atoi (value);

      if (a->section_n <= 1)
        a->page->columns = n;
      else
        a->b.pa.columns = (guint8) CLAMP (n, 1, 9);
    }
  else if (g_str_equal (key, "column-gap"))
    {
      if (a->section_n <= 1)
        a->page->column_gap = length_twips (value);
      else
        a->b.pa.column_gap = length_twips (value);
    }
}

static W42ListKind
abw_list_kind (int type)
{
  switch (type)
    {
    case 0: return W42_LIST_NUMBER;
    case 1: return W42_LIST_LOWER_LETTER;
    case 2: return W42_LIST_UPPER_LETTER;
    case 3: return W42_LIST_LOWER_ROMAN;
    case 4: return W42_LIST_UPPER_ROMAN;
    case 6: return W42_LIST_BULLET_DASH;
    case 7: case 13: return W42_LIST_BULLET_SQUARE;
    default: return W42_LIST_BULLET;
    }
}

/* The width of the page's column, which a frame's place is measured in:
 * the writer puts a frame with no place of its own at its side's edge,
 * and the reader knows it by that. */
static int
abw_column_width (const W42PageSetup *page)
{
  int cols = w42_page_columns (page);
  int w = page->width - page->margin_left - page->margin_right;

  if (cols > 1)
    w = (w - (cols - 1) * w42_page_column_gap (page)) / cols;
  return MAX (w, 1440);
}

/* AbiWord draws no shapes: Word42 writes a shape as the picture of it,
 * and what the shape is in props of its own, which AbiWord keeps and the
 * reader turns back into the shape. */
static const char *const SHAPE_NAMES[W42_SHAPE_KINDS] = {
  NULL, "line", "arrow", "rectangle", "rounded-rectangle", "ellipse"
};

static gboolean
shape_prop (const char *key, const char *value, AbwFrame *f)
{
  if (g_str_equal (key, "w42-shape"))
    {
      for (int k = 1; k < W42_SHAPE_KINDS; k++)
        if (g_str_equal (value, SHAPE_NAMES[k]))
          f->shape = (W42ShapeKind) k;
    }
  else if (g_str_equal (key, "w42-line"))       f->line_pt = CLAMP (length_twips (value), 0, 2000) / 20.0;
  else if (g_str_equal (key, "w42-line-color")) f->line_rgb = (guint32) strtoul (value, NULL, 16) & 0xFFFFFF;
  else if (g_str_equal (key, "w42-fill"))
    {
      f->filled = TRUE;
      f->fill_rgb = (guint32) strtoul (value, NULL, 16) & 0xFFFFFF;
    }
  else if (g_str_equal (key, "w42-shape-text")) f->shape_text = g_intern_string (value);
  else
    return FALSE;
  return TRUE;
}

static void
frame_prop (const char *key, const char *value, gpointer data)
{
  AbwFrame *f = data;

  if (shape_prop (key, value, f))
    return;
  if (g_str_equal (key, "frame-type"))
    {
      f->textbox = g_str_equal (value, "textbox");
      f->image = g_str_equal (value, "image");
    }
  else if (g_str_equal (key, "frame-width"))  f->width = CLAMP (length_twips (value), 0, 31680);
  else if (g_str_equal (key, "frame-height")) f->height = CLAMP (length_twips (value), 0, 31680);
  else if (g_str_equal (key, "xpos"))         f->x = CLAMP (length_twips (value), -31680, 31680);
  else if (g_str_equal (key, "ypos"))         f->y = CLAMP (length_twips (value), -31680, 31680);
  else if (g_str_equal (key, "position-to"))  f->at_block = g_str_equal (value, "block-above-text");
  else if (g_str_equal (key, "wrap-mode"))    f->wrap_mode = g_intern_string (value);
}

/* An inline <image>'s size. */
static void
image_prop (const char *key, const char *value, gpointer data)
{
  AbwFrame *f = data;

  if (shape_prop (key, value, f))
    return;
  if (g_str_equal (key, "width"))       f->width = CLAMP (length_twips (value), 0, 31680);
  else if (g_str_equal (key, "height")) f->height = CLAMP (length_twips (value), 0, 31680);
}

/* How the text goes round a frame.  AbiWord names the side the text is
 * on; Word42, the side the picture is on. */
static W42Wrap
abw_frame_wrap (const Abw *a, const AbwFrame *f)
{
  const char *m = f->wrap_mode;

  if (m == NULL)
    return W42_WRAP_INLINE;
  if (g_str_equal (m, "wrapped-to-right")) return W42_WRAP_LEFT;
  if (g_str_equal (m, "wrapped-to-left"))  return W42_WRAP_RIGHT;
  if (g_str_equal (m, "wrapped-topbot"))   return W42_WRAP_TOP_BOTTOM;
  if (g_str_equal (m, "above-text"))       return W42_WRAP_FRONT;
  if (g_str_equal (m, "below-text"))       return W42_WRAP_BEHIND;
  /* Text on both sides: Word42 puts it on the side the frame leaves
   * the more room. */
  return f->x + f->width / 2 > abw_column_width (a->page) / 2 ? W42_WRAP_RIGHT : W42_WRAP_LEFT;
}

typedef struct {
  int left, right, top, bot;
} AbwAttach;

/* Where a <cell> is: the columns and rows it runs between. */
static void
attach_prop (const char *key, const char *value, gpointer data)
{
  AbwAttach *at = data;

  if (g_str_equal (key, "left-attach"))       at->left = CLAMP (atoi (value), 0, 1023);
  else if (g_str_equal (key, "right-attach")) at->right = CLAMP (atoi (value), 0, 1024);
  else if (g_str_equal (key, "top-attach"))   at->top = CLAMP (atoi (value), 0, 4096);
  else if (g_str_equal (key, "bot-attach"))   at->bot = CLAMP (atoi (value), 0, 4097);
}

static void
abw_style_free (gpointer data)
{
  AbwStyle *s = data;

  g_free (s->name);
  g_free (s->based);
  g_free (s->props);
  g_free (s);
}

static AbwStyle *
abw_style_lookup (Abw *a, const char *name)
{
  for (guint i = 0; i < a->styles->len; i++)
    {
      AbwStyle *s = g_ptr_array_index (a->styles, i);

      if (g_ascii_strcasecmp (s->name, name) == 0)
        return s;
    }
  return NULL;
}

/* A style's formatting: its parent's, resolved first, with its own props
 * over them.  A parent the file does not give is the one the sheet has;
 * a loop of parents, or a chain too long to be meant, stops at nothing. */
static void
abw_resolve_style (Abw *a, AbwStyle *s, int depth)
{
  AbwStyle *p;
  W42Fmt def;

  if (s->state != 0)
    return;
  s->state = 1;
  w42_fmt_init_default (&def);
  s->pa = def.pa;
  s->ch = def.ch;
  p = s->based != NULL && depth < 16 ? abw_style_lookup (a, s->based) : NULL;
  if (p != NULL)
    {
      abw_resolve_style (a, p, depth + 1);
      if (p->state == 2)
        {
          s->pa = p->pa;
          s->ch = p->ch;
        }
    }
  else if (s->based != NULL)
    {
      const W42Style *bs = w42_stylesheet_find (w42_pt_stylesheet (a->pt), s->based);

      if (bs != NULL)
        {
          s->pa = bs->pa;
          s->ch = bs->ch;
        }
    }
  each_prop (s->props, para_prop, &s->pa);
  each_prop (s->props, para_char_prop, &s->ch);
  s->state = 2;
}

/* The file's styles into the sheet, each resolved, before the text that
 * names them. */
static void
abw_register_styles (Abw *a)
{
  W42StyleSheet *sheet = w42_pt_stylesheet (a->pt);

  for (guint i = 0; i < a->styles->len; i++)
    abw_resolve_style (a, g_ptr_array_index (a->styles, i), 0);
  for (guint i = 0; i < a->styles->len; i++)
    {
      const AbwStyle *s = g_ptr_array_index (a->styles, i);
      const W42Style *have = w42_stylesheet_find (sheet, s->name);
      W42Style st;

      /* As many as a document means to have, as the .odt reader takes:
       * every one more makes each after it slower to add. */
      if (have == NULL && w42_stylesheet_size (sheet) >= 128)
        continue;
      memset (&st, 0, sizeof st);
      st.name = g_intern_string (s->name);
      st.character = s->character;
      st.pa = s->pa;
      st.ch = s->ch;
      st.pa.style = st.name;
      st.pa_own = W42_STYLE_PA_ALL;
      st.ch_own = W42_STYLE_CH_ALL;
      /* AbiWord has no outline levels: a heading keeps the one Word42
       * gives it, and a "Heading 4" of the file's own is one too. */
      if (have != NULL)
        st.outline = have->outline;
      else if (g_ascii_strncasecmp (s->name, "Heading ", 8) == 0 &&
               s->name[8] >= '1' && s->name[8] <= '9' && s->name[9] == '\0')
        st.outline = s->name[8] - '0';
      if (s->based != NULL)
        {
          const W42Style *b = w42_stylesheet_find (sheet, s->based);

          st.based_on = b != NULL ? b->name : g_intern_string (s->based);
        }
      w42_stylesheet_set (sheet, &st);
    }
  g_ptr_array_set_size (a->styles, 0);
}

static void
abw_flush (Abw *a)
{
  const char *comment = NULL;

  /* The innermost <ann> that has its text says what the run is about --
   * in a note, one in the note: the note is not in the text round it. */
  for (guint i = a->comments->len; i > (a->in_foot ? a->foot_comments : 0) && comment == NULL; i--)
    comment = g_ptr_array_index (a->comments, i - 1);
  a->b.ch.link = a->link;
  a->b.ch.comment = comment;
  if (a->text->len == 0)
    return;
  if (a->in_hf)
    g_string_append (a->hf_text, a->text->str);
  else
    w42_builder_text (&a->b, a->text->str);
  g_string_truncate (a->text, 0);
}

/* The cells of the row being built up to column `col`: covered ones where
 * a merge from a row above runs on, as AbiWord writes no cell there, and
 * empty ones for any other it leaves out. */
static void
abw_fill_row (Abw *a, int col)
{
  col = MIN (col, a->b.n_cols);
  while (a->b.col < col)
    {
      int before = a->b.col;
      const AbwCover *c = before < (int) a->cover->len
                            ? &g_array_index (a->cover, AbwCover, before) : NULL;
      gboolean covered = c != NULL && c->until > a->b.row;

      w42_builder_begin_cell (&a->b, covered ? MAX (c->span, 1) : 1);
      if (covered && a->b.cell_pos != (gsize) -1)
        w42_pt_set_cell_vspan (a->pt, a->b.cell_pos, W42_CELL_COVERED);
      w42_builder_end_cell (&a->b);
      if (a->b.col == before)
        break;
    }
}

static void
abw_end_row (Abw *a)
{
  w42_builder_end_cell (&a->b);
  abw_fill_row (a, a->b.n_cols);
  w42_builder_end_row (&a->b);
}

static gboolean
abw_row_covered (Abw *a)
{
  for (guint c = 0; c < a->cover->len; c++)
    if (g_array_index (a->cover, AbwCover, c).until > a->b.row)
      return TRUE;
  return FALSE;
}

/* A picture in a frame of its own.  AbiWord puts the frame after the
 * paragraph it is anchored to, so it goes in at that paragraph's end --
 * straight away, where the paragraph still is.  Anywhere else (before
 * any paragraph, in a note) it starts the paragraph that follows. */
static void
abw_frame_object (Abw *a, GBytes *bytes, const char *format, int pw, int ph,
                  const AbwFrame *f, W42Wrap wrap, gboolean positioned)
{
  W42ObjectTable *objs = w42_pt_object_table (a->pt);
  W42ApTable *aps = w42_pt_ap_table (a->pt);
  gboolean after_body = a->anchor_ok && a->b.pos == a->anchor_pos + 1 && !a->b.in_para &&
                        a->b.table < 0 && a->b.note_return == (gsize) -1;
  /* In a cell the paragraph that ended is still open to more: the next
   * one's mark waits for its text. */
  gboolean after_cell = a->b.table >= 0 && a->b.in_cell && a->b.cell_break_pending &&
                        a->b.note_return == (gsize) -1;
  W42ObjectIdx idx;

  if (after_body || after_cell)
    {
      gsize at = after_body ? a->anchor_pos : a->b.pos;
      W42Fmt fmt = *w42_ap_table_get (aps, w42_pt_ap_at (a->pt, at > 0 ? at - 1 : 0));

      /* In the paragraph's own text formatting, as the writer, which says
       * none for a frame, left it. */
      fmt.ch = a->para_ch;
      idx = w42_object_table_add (objs, bytes, format, pw, ph,
                                  f->width > 0 ? f->width : pw * 15,
                                  f->height > 0 ? f->height : ph * 15);
      w42_pt_insert_object (a->pt, at, idx, w42_ap_table_intern (aps, &fmt));
      if (after_body)
        a->anchor_pos++;
      a->b.pos++;
    }
  else
    {
      w42_builder_object (&a->b, bytes, format, pw, ph, f->width, f->height);
      idx = a->b.last_object;
    }
  if (idx == W42_OBJECT_NONE)
    return;
  if (f->shape != W42_SHAPE_PICTURE)
    w42_object_table_set_shape (objs, idx, f->shape, f->line_pt, f->line_rgb,
                                f->filled, f->fill_rgb, f->shape_text);
  w42_object_table_set_wrap (objs, idx, wrap);
  if (positioned)
    w42_object_table_set_position (objs, idx, TRUE, f->x, f->y);
}

static void
abw_start (GMarkupParseContext *ctx, const char *name, const char **an,
           const char **av, gpointer data, GError **error)
{
  Abw *a = data;
  int table_index = -1;

  (void) ctx; (void) error;

  /* Counted wherever they are, as the first pass counted them. */
  if (g_str_equal (name, "table"))
    table_index = a->tables_seen++;

  if (a->skip_depth > 0)
    {
      a->skip_depth++;
      return;
    }

  if (a->annotate_depth > 0)
    {
      /* A comment's own paragraphs: its text, and nothing else. */
      a->annotate_depth++;
      if (g_str_equal (name, "br"))
        g_string_append_c (a->annotate_text, '\n');
      return;
    }

  if (g_str_equal (name, "m"))
    {
      /* The document's metadata, which is what File > Summary Info
       * shows: AbiWord keeps it in Dublin Core's names. */
      static const struct { const char *key; int slot; } KEYS[] = {
        { "dc.title", 0 }, { "dc.subject", 1 }, { "dc.creator", 2 },
        { "abiword.keywords", 3 }, { "dc.description", 4 },
      };
      const char *key = attr (an, av, "key");

      a->meta_field = NULL;
      if (key != NULL)
        for (guint i = 0; i < G_N_ELEMENTS (KEYS); i++)
          if (g_str_equal (key, KEYS[i].key))
            a->meta_field = GINT_TO_POINTER (KEYS[i].slot + 1);
      g_string_truncate (a->meta_text, 0);
      return;
    }

  if (g_str_equal (name, "section"))
    {
      const char *type = attr (an, av, "type");

      abw_flush (a);
      abw_register_styles (a);
      if (type != NULL && (g_str_has_prefix (type, "header") || g_str_has_prefix (type, "footer")))
        {
          /* "header", "header-first", "header-even", and the footers
           * likewise; Word42 has no last-page one. */
          const char *variant = type + 6;

          if (*variant == '\0')
            a->hf_kind = W42_PAGE_TEXT_DEFAULT;
          else if (g_str_equal (variant, "-first"))
            a->hf_kind = W42_PAGE_TEXT_FIRST;
          else if (g_str_equal (variant, "-even"))
            a->hf_kind = W42_PAGE_TEXT_EVEN;
          else
            {
              a->skip_depth = 1;
              return;
            }
          a->in_hf = TRUE;
          a->hf_footer = type[0] == 'f';
          g_string_truncate (a->hf_text, 0);
          a->hf_align = W42_ALIGN_LEFT;
        }
      else if (type != NULL)
        a->skip_depth = 1;          /* footnote sections and the like */
      else
        {
          a->section_n++;
          w42_builder_reset_para (&a->b);
          if (a->section_n > 1)
            {
              /* The columns go on the section's first paragraph, which
               * <p> below will start with this pa. */
              a->b.pa.section_break = 1;
              a->b.pa.columns = 1;
            }
          each_prop (attr (an, av, "props"), section_prop, a);
        }
    }
  else if (g_str_equal (name, "pagesize"))
    {
      const char *w = attr (an, av, "width"), *h = attr (an, av, "height");
      const char *units = attr (an, av, "units");
      const char *orientation = attr (an, av, "orientation");
      double scale = units != NULL && g_str_equal (units, "cm") ? 1440.0 / 2.54
                   : units != NULL && g_str_equal (units, "mm") ? 1440.0 / 25.4 : 1440.0;

      if (w != NULL && h != NULL)
        {
          double dw = g_ascii_strtod (w, NULL) * scale;
          double dh = g_ascii_strtod (h, NULL) * scale;
          int pw = (int) (isnan (dw) ? 0.0 : CLAMP (dw, 0.0, 31680.0));
          int ph = (int) (isnan (dh) ? 0.0 : CLAMP (dh, 0.0, 31680.0));

          if (orientation != NULL && g_str_equal (orientation, "landscape") && pw < ph)
            {
              int t = pw; pw = ph; ph = t;
            }
          a->page->width = pw;
          a->page->height = ph;
        }
    }
  else if (g_str_equal (name, "l"))
    {
      AbwList l;
      const char *type = attr (an, av, "type");
      const char *start = attr (an, av, "start-value");
      const char *id = attr (an, av, "id");

      l.id = id != NULL ? atoi (id) : 0;
      l.kind = abw_list_kind (type != NULL ? atoi (type) : 5);
      l.start = start != NULL ? CLAMP (atoi (start), 0, 100000) : 1;
      l.seen = 0;
      if (!g_hash_table_contains (a->list_index, GINT_TO_POINTER (l.id)))
        {
          g_array_append_val (a->lists, l);
          g_hash_table_insert (a->list_index, GINT_TO_POINTER (l.id), GINT_TO_POINTER (a->lists->len));
        }
    }
  else if (g_str_equal (name, "p"))
    {
      const char *style = attr (an, av, "style");
      const char *list = attr (an, av, "list");
      const char *level = attr (an, av, "level");
      const char *props = attr (an, av, "props");
      const W42Style *s = style != NULL ? w42_stylesheet_find (w42_pt_stylesheet (a->pt), style) : NULL;
      gboolean body = !a->in_hf && !a->in_foot;
      W42ParaFmt keep_section;
      W42Fmt def;

      abw_flush (a);
      /* A note's paragraphs end as the next one starts, so that its last
       * is not followed by an empty one; pa is still the last one's. */
      if (a->in_foot)
        {
          if (!a->foot_first_para)
            w42_builder_end_paragraph (&a->b);
          a->foot_first_para = FALSE;
        }
      keep_section = a->b.pa;
      w42_builder_reset_para (&a->b);
      w42_fmt_init_default (&def);
      a->para_ch = def.ch;
      if (s != NULL)
        {
          a->b.pa = s->pa;
          a->para_ch = s->ch;
        }
      /* The section's columns were parked in pa: they go on its first
       * paragraph, and nothing after. */
      a->b.pa.section_break = body ? keep_section.section_break : 0;
      a->b.pa.columns = body ? keep_section.columns : 0;
      a->b.pa.column_gap = body ? keep_section.column_gap : 0;
      each_prop (props, para_prop, &a->b.pa);
      each_prop (props, para_char_prop, &a->para_ch);
      a->b.ch = a->para_ch;
      if (a->pbr_pending && body)
        {
          a->b.pa.page_break_before = 1;
          a->pbr_pending = FALSE;
        }
      if (a->textbox_depth > 0 && body && a->b.table < 0)
        {
          a->b.pa.frame_side = (guint8) a->tb_side;
          a->b.pa.frame_width = a->tb_width;
        }
      if (list != NULL)
        {
          int id = atoi (list);
          gpointer v = g_hash_table_lookup (a->list_index, GINT_TO_POINTER (id));

          if (level != NULL)
            a->b.pa.list_level = (guint8) CLAMP (atoi (level) - 1, 0, 8);
          if (v != NULL)
            {
              AbwList *l = &g_array_index (a->lists, AbwList, GPOINTER_TO_INT (v) - 1);
              int lv = MIN (a->b.pa.list_level, 8);

              a->b.pa.list = (guint8) l->kind;
              /* An item that says nothing of its indents hangs, as
               * AbiWord's own always say they do. */
              if (a->b.pa.indent_first >= 0 && (props == NULL || strstr (props, "text-indent") == NULL))
                {
                  a->b.pa.indent_left = MAX (a->b.pa.indent_left, 360);
                  a->b.pa.indent_first = -360;
                }
              if (body && w42_list_is_numbered (l->kind))
                {
                  /* AbiWord numbers a list's items on from its start,
                   * whatever comes between them; Word42 counts on while
                   * the kind at a level stays the same.  Where the two
                   * differ -- a list restarted, or one begun at 5 -- the
                   * item says its number. */
                  int n = l->start + l->seen;
                  int expect = a->level_kind[lv] == l->kind ? a->level_n[lv] + 1 : 1;

                  if (n != expect)
                    a->b.pa.list_start = (guint8) CLAMP (n, 1, 255);
                  a->level_n[lv] = a->b.pa.list_start > 0 ? a->b.pa.list_start : expect;
                  a->level_kind[lv] = l->kind;
                  for (int deeper = lv + 1; deeper < 9; deeper++)
                    {
                      a->level_n[deeper] = 0;
                      a->level_kind[deeper] = W42_LIST_NONE;
                    }
                }
              if (body)
                l->seen++;
            }
        }
      else if (body && a->b.pa.list == W42_LIST_NONE)
        {
          /* A plain paragraph ends Word42's count. */
          memset (a->level_n, 0, sizeof a->level_n);
          for (int lv = 0; lv < 9; lv++)
            a->level_kind[lv] = W42_LIST_NONE;
        }
      if (a->in_hf)
        {
          a->hf_align = a->b.pa.align;
          if (a->hf_text->len > 0)
            g_string_append_c (a->hf_text, ' ');
        }
      a->para_open = TRUE;
    }
  else if (g_str_equal (name, "c"))
    {
      abw_flush (a);
      a->b.ch = a->para_ch;
      each_prop (attr (an, av, "props"), char_prop, &a->b.ch);
    }
  else if (g_str_equal (name, "a"))
    {
      const char *href = attr (an, av, "xlink:href");

      abw_flush (a);
      a->link = href != NULL ? g_intern_string (href) : NULL;
    }
  else if (g_str_equal (name, "ann"))
    {
      /* An annotation: <annotate> in it holds the comment, and the rest
       * of it is the text the comment is on. */
      abw_flush (a);
      g_ptr_array_add (a->comments, NULL);
    }
  else if (g_str_equal (name, "annotate"))
    {
      abw_flush (a);
      if (a->in_hf || a->comments->len == 0)
        a->skip_depth = 1;
      else
        {
          a->annotate_depth = 1;
          g_string_truncate (a->annotate_text, 0);
        }
    }
  else if (g_str_equal (name, "bookmark"))
    {
      const char *type = attr (an, av, "type");
      const char *bname = attr (an, av, "name");

      abw_flush (a);
      if (type != NULL && bname != NULL && !a->in_hf)
        {
          if (g_str_equal (type, "start"))
            g_hash_table_insert (a->bookmarks, g_strdup (bname), GSIZE_TO_POINTER (a->b.pos));
          else
            {
              gpointer start = g_hash_table_lookup (a->bookmarks, bname);

              if (start != NULL && a->b.pos > GPOINTER_TO_SIZE (start))
                {
                  W42CharFmt want;

                  memset (&want, 0, sizeof want);
                  want.bookmark = g_intern_string (bname);
                  w42_pt_apply_char_fmt (a->pt, GPOINTER_TO_SIZE (start),
                                         a->b.pos - GPOINTER_TO_SIZE (start),
                                         W42_CHAR_BOOKMARK, &want);
                }
            }
        }
    }
  else if (g_str_equal (name, "br"))
    g_string_append (a->text, "\342\200\250");
  else if (g_str_equal (name, "s"))
    {
      /* A style of the file's own: paragraph (P) or character (C).  Kept
       * as said until </styles>, when its parent is sure to be read. */
      const char *sname = attr (an, av, "name");
      const char *type = attr (an, av, "type");
      const char *based = attr (an, av, "basedon");

      if (sname != NULL && *sname != '\0' && strlen (sname) < 64 && a->styles->len < 1024 &&
          (type == NULL || g_str_equal (type, "P") || g_str_equal (type, "C")))
        {
          AbwStyle *st = g_new0 (AbwStyle, 1);

          st->name = g_strdup (sname);
          st->based = based != NULL && *based != '\0' ? g_strdup (based) : NULL;
          st->props = g_strdup (attr (an, av, "props"));
          st->character = type != NULL && g_str_equal (type, "C");
          g_ptr_array_add (a->styles, st);
        }
    }
  else if (g_str_equal (name, "pbr"))
    {
      /* A page break in the text: what follows it starts a page, in a
       * paragraph of its own -- this one's formatting, and the break.
       * Nothing following it in this paragraph, the next one takes it. */
      abw_flush (a);
      if (!a->in_hf && !a->in_foot)
        {
          if (a->b.in_para)
            {
              W42ParaFmt keep = a->b.pa;

              w42_builder_end_paragraph (&a->b);
              if (a->b.table < 0)
                {
                  a->anchor_pos = a->b.pos - 1;
                  a->anchor_ok = TRUE;
                }
              a->b.pa = keep;
              a->b.pa.section_break = 0;
              a->b.pa.columns = 0;
              a->b.pa.column_gap = 0;
            }
          a->b.pa.page_break_before = 1;
          a->pbr_pending = TRUE;
        }
    }
  else if (g_str_equal (name, "image"))
    {
      const char *dataid = attr (an, av, "dataid");
      AbwFrame f;

      memset (&f, 0, sizeof f);
      each_prop (attr (an, av, "props"), image_prop, &f);
      abw_flush (a);
      if (dataid != NULL && !a->in_hf)
        {
          /* The first pass read the data, so the picture goes in where it
           * is met: kept as a place to fill in later, it was a position
           * that a note or a frame moved on meanwhile. */
          GBytes *bytes = g_hash_table_lookup (a->images, dataid);
          int pw = 0, ph = 0;
          const char *format = NULL;

          if (bytes != NULL && w42_image_probe (bytes, &pw, &ph, &format))
            {
              w42_builder_object (&a->b, bytes, format, pw, ph, f.width, f.height);
              if (a->b.last_object != W42_OBJECT_NONE && f.shape != W42_SHAPE_PICTURE)
                w42_object_table_set_shape (w42_pt_object_table (a->pt), a->b.last_object, f.shape,
                                            f.line_pt, f.line_rgb, f.filled, f.fill_rgb, f.shape_text);
            }
        }
    }
  else if (g_str_equal (name, "field"))
    {
      const char *type = attr (an, av, "type");

      abw_flush (a);
      if (type != NULL && a->in_hf)
        {
          if (g_str_equal (type, "page_number")) g_string_append (a->text, "{PAGE}");
          else if (g_str_equal (type, "page_count")) g_string_append (a->text, "{NUMPAGES}");
          else if (g_str_has_prefix (type, "date")) g_string_append (a->text, "{DATE}");
        }
      else if (type != NULL && (g_str_equal (type, "footnote_ref") || g_str_equal (type, "endnote_ref")))
        {
          /* The mark the note that follows hangs from, in the reference's
           * formatting. */
          a->mark_ch = a->b.ch;
          a->mark_pending = TRUE;
        }
      else if (type != NULL && a->para_open)
        {
          /* In the body a field is a run of its own; AbiWord keeps no
           * result, so a placeholder stands until Update Fields. */
          const char *code = g_str_equal (type, "page_number") ? "PAGE"
                           : g_str_equal (type, "page_count") ? "NUMPAGES"
                           : g_str_has_prefix (type, "date") ? "DATE"
                           : g_str_has_prefix (type, "time") ? "TIME"
                           : g_str_equal (type, "file_name") ? "FILENAME"
                           : g_str_equal (type, "word_count") ? "NUMWORDS" : NULL;

          if (code != NULL)
            {
              W42CharFmt keep = a->b.ch;

              a->b.ch.field = g_intern_static_string (code);
              w42_builder_text (&a->b, g_str_equal (code, "PAGE") ? "1" : "?");
              a->b.ch = keep;
            }
        }
    }
  else if (g_str_equal (name, "foot") || g_str_equal (name, "endnote"))
    {
      abw_flush (a);
      if (!a->in_foot && !a->in_hf && !w42_builder_in_table (&a->b))
        {
          a->foot_outer_pa = a->b.pa;
          a->foot_outer_para_ch = a->para_ch;
          a->foot_outer_ch = a->b.ch;
          a->foot_outer_link = a->link;
          a->foot_comments = a->comments->len;
          if (a->mark_pending)
            a->b.ch = a->mark_ch;
          a->mark_pending = FALSE;
          w42_builder_begin_note (&a->b, g_str_equal (name, "endnote"));
          a->b.ch = a->foot_outer_ch;
          a->in_foot = TRUE;
          a->foot_first_para = TRUE;
          /* Its text is in its paragraphs: the line end before the first
           * is not.  Nor is it in a link round the reference. */
          a->para_open = FALSE;
          a->link = NULL;
        }
      else
        a->skip_depth = 1;
    }
  else if (g_str_equal (name, "table"))
    {
      const char *props = attr (an, av, "props");
      GArray *widths = g_array_new (FALSE, FALSE, sizeof (int));
      const char *cols = props != NULL ? strstr (props, "table-column-props:") : NULL;
      int n_cols = table_index >= 0 && table_index < (int) a->table_cols->len
                     ? g_array_index (a->table_cols, int, table_index) : 0;

      abw_flush (a);
      if (cols != NULL)
        {
          char **parts = g_strsplit (cols + strlen ("table-column-props:"), "/", -1);

          for (int i = 0; parts[i] != NULL; i++)
            {
              char *v = g_strstrip (parts[i]);
              char *semi = strchr (v, ';');

              if (semi != NULL) *semi = '\0';
              if (*v != '\0')
                {
                  int w = length_twips (v);
                  g_array_append_val (widths, w);
                }
            }
          g_strfreev (parts);
        }
      if (w42_builder_in_table (&a->b) || a->in_hf)
        a->skip_depth = 1;            /* nested, or in a header: not read */
      else
        {
          /* As many columns as its cells reach, which the first pass
           * counted: a table that gives no widths is not one column. */
          n_cols = MAX (n_cols, (int) widths->len);
          w42_builder_begin_table (&a->b, MAX (n_cols, 1),
                                   (int) widths->len == n_cols && n_cols > 0
                                     ? (const int *) widths->data : NULL);
          /* AbiWord keeps every rule in the cells, so the table's own
           * "ruled" flag is off and each cell says what it wants. */
          w42_pt_table_set_borders (a->pt, a->b.table, FALSE);
          g_array_set_size (a->cover, 0);
        }
      g_array_free (widths, TRUE);
    }
  else if (g_str_equal (name, "cell"))
    {
      const char *props = attr (an, av, "props");
      AbwAttach at = { 0, 1, 0, 0 };
      W42ParaFmt cell_pa;
      W42Fmt def;

      w42_fmt_init_default (&def);
      cell_pa = def.pa;
      each_prop (props, attach_prop, &at);
      each_prop (props, para_prop, &cell_pa);
      abw_flush (a);
      if (w42_builder_in_table (&a->b))
        {
          int col;

          /* A cell that teleports thousands of empty rows down is broken
           * input, and every row it skips is a full row of cells to make. */
          if (at.top - a->b.row > 256)
            at.top = a->b.row;
          while (a->b.row < at.top)
            abw_end_row (a);
          abw_fill_row (a, at.left);
          col = a->b.col;
          w42_builder_begin_cell (&a->b, MAX (at.right - at.left, 1));
          if (a->b.cell_pos != (gsize) -1)
            {
              w42_pt_cell_set_borders_at (a->pt, a->b.cell_pos,
                                          cell_pa.border | W42_BORDER_CELL_SET);
              w42_pt_cell_set_edges_at (a->pt, a->b.cell_pos, cell_pa.edge);
              if (cell_pa.has_shading_color)
                w42_pt_cell_set_fill_at (a->pt, a->b.cell_pos, TRUE,
                                         cell_pa.shading_color);
              if (at.bot > a->b.row + 1)
                {
                  /* Merged down: the rows below have no cell here, and
                   * get covered ones as they are built. */
                  int rows = MIN (at.bot - a->b.row, 254);
                  AbwCover *c;

                  w42_pt_set_cell_vspan (a->pt, a->b.cell_pos, rows);
                  if ((int) a->cover->len < a->b.n_cols)
                    g_array_set_size (a->cover, a->b.n_cols);
                  c = &g_array_index (a->cover, AbwCover, col);
                  c->until = a->b.row + rows;
                  c->span = MIN (MAX (at.right - at.left, 1), a->b.n_cols - col);
                }
            }
        }
    }
  else if (g_str_equal (name, "frame"))
    {
      const char *dataid = attr (an, av, "strux-image-dataid");
      AbwFrame f;

      memset (&f, 0, sizeof f);
      each_prop (attr (an, av, "props"), frame_prop, &f);
      abw_flush (a);
      if (f.textbox && !a->in_hf)
        {
          /* A text box: its paragraphs are Word42's framed ones, at the
           * side the frame is on.  They are read, not skipped. */
          if (a->textbox_depth++ == 0)
            {
              W42Wrap wrap = abw_frame_wrap (a, &f);
              int third = abw_column_width (a->page) / 3;

              if (wrap != W42_WRAP_LEFT && wrap != W42_WRAP_RIGHT)
                wrap = f.x + f.width / 2 > abw_column_width (a->page) / 2 ? W42_WRAP_RIGHT : W42_WRAP_LEFT;
              a->tb_side = wrap == W42_WRAP_RIGHT ? W42_FRAME_RIGHT : W42_FRAME_LEFT;
              /* A third of the column is what a frame of no width of its
               * own is written as. */
              a->tb_width = ABS (f.width - third) <= 2 ? 0 : f.width;
            }
          return;
        }
      if (dataid != NULL && !a->in_hf)
        {
          GBytes *bytes = g_hash_table_lookup (a->images, dataid);
          int pw = 0, ph = 0;
          const char *format = NULL;

          if (bytes != NULL && w42_image_probe (bytes, &pw, &ph, &format) && pw > 0 && ph > 0)
            {
              W42Wrap wrap = abw_frame_wrap (a, &f);
              gboolean positioned = wrap != W42_WRAP_INLINE && f.at_block && (f.x != 0 || f.y != 0);
              int width = f.width > 0 ? f.width : pw * 15;

              /* One at its column's right edge is where Word42 puts a
               * picture on the right with no place of its own. */
              if (positioned && wrap == W42_WRAP_RIGHT && f.y == 0 &&
                  ABS (f.x - (abw_column_width (a->page) - width)) <= 2)
                positioned = FALSE;
              abw_frame_object (a, bytes, format, pw, ph, &f, wrap, positioned);
            }
        }
      a->skip_depth = 1;
    }
  else if (g_str_equal (name, "data") || g_str_equal (name, "d") ||
           g_str_equal (name, "history") || g_str_equal (name, "revisions") ||
           g_str_equal (name, "ignoredwords") || g_str_equal (name, "authors"))
    a->skip_depth = 1;                /* the first pass read the data */
  /* <metadata> is read: its <m> elements are the summary information. */
}

static void
abw_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  Abw *a = data;

  (void) ctx; (void) error;

  if (a->skip_depth > 0)
    {
      a->skip_depth--;
      return;
    }

  if (a->annotate_depth > 0)
    {
      if (--a->annotate_depth == 0)
        {
          /* </annotate>: the comment is on the text the <ann> goes on
           * round. */
          char *clean = g_strstrip (g_strdup (a->annotate_text->str));

          if (a->comments->len > 0 && *clean != '\0')
            g_ptr_array_index (a->comments, a->comments->len - 1) = (gpointer) g_intern_string (clean);
          g_free (clean);
        }
      else if (g_str_equal (name, "p"))
        g_string_append_c (a->annotate_text, ' ');
      return;
    }

  if (g_str_equal (name, "m"))
    {
      int slot = GPOINTER_TO_INT (a->meta_field) - 1;

      if (slot >= 0 && slot < 5 && a->meta_text->len > 0 && a->meta[slot] == NULL)
        a->meta[slot] = g_strdup (g_strstrip (a->meta_text->str));
      a->meta_field = NULL;
      g_string_truncate (a->meta_text, 0);
      return;
    }

  if (g_str_equal (name, "styles"))
    abw_register_styles (a);
  else if (g_str_equal (name, "p"))
    {
      abw_flush (a);
      a->mark_pending = FALSE;
      if (a->in_hf || a->in_foot)
        ;                             /* a note's paragraphs end at the next, or at </foot> */
      else if (a->pbr_pending && !a->b.in_para)
        ;                             /* nothing after its page break: the next paragraph takes it */
      else
        {
          a->pbr_pending = FALSE;
          w42_builder_end_paragraph (&a->b);
          /* The section break belongs to its first paragraph only. */
          w42_builder_reset_para (&a->b);
          if (a->b.table < 0)
            {
              a->anchor_pos = a->b.pos - 1;
              a->anchor_ok = TRUE;
            }
        }
      a->para_open = FALSE;
    }
  else if (g_str_equal (name, "c"))
    {
      abw_flush (a);
      a->b.ch = a->para_ch;
    }
  else if (g_str_equal (name, "a"))
    {
      abw_flush (a);
      a->link = NULL;
    }
  else if (g_str_equal (name, "ann"))
    {
      abw_flush (a);
      if (a->comments->len > 0)
        g_ptr_array_set_size (a->comments, a->comments->len - 1);
    }
  else if (g_str_equal (name, "foot") || g_str_equal (name, "endnote"))
    {
      abw_flush (a);
      if (a->in_foot)
        {
          w42_builder_end_note (&a->b);
          a->in_foot = FALSE;
          /* Back in the paragraph the note hung from, in the run it was
           * in. */
          a->b.pa = a->foot_outer_pa;
          a->para_ch = a->foot_outer_para_ch;
          a->b.ch = a->foot_outer_ch;
          a->link = a->foot_outer_link;
          a->para_open = TRUE;
        }
    }
  else if (g_str_equal (name, "cell"))
    {
      abw_flush (a);
      w42_builder_end_cell (&a->b);
    }
  else if (g_str_equal (name, "table"))
    {
      abw_flush (a);
      if (w42_builder_in_table (&a->b))
        {
          int table = a->b.table;

          if (a->b.in_cell || a->b.col > 0)
            abw_end_row (a);
          /* The rows a merge runs on into, when no cell of their own
           * follows to make them. */
          for (int guard = 0; guard < 256 && abw_row_covered (a); guard++)
            abw_end_row (a);
          w42_builder_end_table (&a->b);
          w42_pt_resolve_vmerges (a->pt, table);
          g_array_set_size (a->cover, 0);
        }
    }
  else if (g_str_equal (name, "frame"))
    {
      /* Only a text box's end comes here; a picture's was skipped. */
      if (a->textbox_depth > 0)
        a->textbox_depth--;
    }
  else if (g_str_equal (name, "section"))
    {
      abw_flush (a);
      if (a->in_hf)
        {
          if (a->hf_footer)
            w42_pt_set_footer_kind (a->pt, a->hf_kind, a->hf_text->str, a->hf_align);
          else
            w42_pt_set_header_kind (a->pt, a->hf_kind, a->hf_text->str, a->hf_align);
          /* A file that has one says so by having it, an empty one
           * included: a title page with no header is a title page. */
          if (a->hf_kind == W42_PAGE_TEXT_FIRST)
            w42_pt_set_title_page (a->pt, TRUE);
          else if (a->hf_kind == W42_PAGE_TEXT_EVEN)
            w42_pt_set_facing_pages (a->pt, TRUE);
          a->in_hf = FALSE;
        }
    }
}

static void
abw_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  Abw *a = data;
  GString *to;

  (void) ctx; (void) error;
  if (a->skip_depth > 0)
    return;
  if (a->meta_field != NULL)
    {
      g_string_append_len (a->meta_text, text, len);
      return;
    }
  to = a->annotate_depth > 0 ? a->annotate_text : a->para_open ? a->text : NULL;
  if (to == NULL)
    return;
  /* A line end in the text is a space, as AbiWord reads it: a line break
   * is <br/>. */
  for (gsize i = 0; i < len; i++)
    g_string_append_c (to, text[i] == '\n' || text[i] == '\r' ? ' ' : text[i]);
}

/* ---- the first pass: pictures and table widths --------------------------- */

typedef struct {
  GHashTable *images;         /* data name -> GBytes */
  GArray     *cols;           /* int, per <table> in file order */
  GArray     *open;           /* the index in cols of each table open */
  gboolean    in_data;
  GString    *data;
  char       *name;
  gboolean    b64;
} AbwScan;

static void
scan_start (GMarkupParseContext *ctx, const char *name, const char **an,
            const char **av, gpointer data, GError **error)
{
  AbwScan *s = data;

  (void) ctx; (void) error;
  if (g_str_equal (name, "d"))
    {
      const char *b64 = attr (an, av, "base64");

      s->in_data = TRUE;
      g_free (s->name);
      s->name = g_strdup (attr (an, av, "name"));
      s->b64 = b64 == NULL || g_str_equal (b64, "yes");
      g_string_truncate (s->data, 0);
    }
  else if (g_str_equal (name, "table"))
    {
      int zero = 0, index = (int) s->cols->len;

      g_array_append_val (s->cols, zero);
      g_array_append_val (s->open, index);
    }
  else if (g_str_equal (name, "cell") && s->open->len > 0)
    {
      AbwAttach at = { 0, 1, 0, 0 };
      int *n = &g_array_index (s->cols, int, g_array_index (s->open, int, s->open->len - 1));

      each_prop (attr (an, av, "props"), attach_prop, &at);
      *n = MAX (*n, at.right);
    }
}

static void
scan_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  AbwScan *s = data;

  (void) ctx; (void) error;
  if (g_str_equal (name, "d"))
    {
      if (s->in_data && s->name != NULL && s->b64)
        {
          gsize len = 0;
          /* Whitespace inside base64 is fine for g_base64_decode. */
          guchar *bytes = g_base64_decode (s->data->str, &len);

          if (bytes != NULL && len > 0)
            g_hash_table_insert (s->images, g_strdup (s->name), g_bytes_new_take (bytes, len));
          else
            g_free (bytes);
        }
      s->in_data = FALSE;
      g_string_truncate (s->data, 0);
    }
  else if (g_str_equal (name, "table") && s->open->len > 0)
    g_array_set_size (s->open, s->open->len - 1);
}

static void
scan_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  AbwScan *s = data;

  (void) ctx; (void) error;
  if (s->in_data)
    g_string_append_len (s->data, text, len);
}

/* .zabw is the same, gzipped: one gzip member, or several one after the
 * other, as gzip reads them.  A stream that does not end as it should is
 * an error, not a shorter document. */
static gboolean
abw_gunzip (char **contents, gsize *length, GError **error)
{
  GZlibDecompressor *dec = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
  GByteArray *out = g_byte_array_new ();
  guint8 *buf = g_malloc (65536);
  gsize in_pos = 0;
  gboolean ok = FALSE;

  for (;;)
    {
      gsize read = 0, written = 0;
      GConverterResult res;
      GError *err = NULL;

      res = g_converter_convert (G_CONVERTER (dec), *contents + in_pos, *length - in_pos,
                                 buf, 65536, G_CONVERTER_INPUT_AT_END, &read, &written, &err);
      in_pos += read;
      g_byte_array_append (out, buf, written);
      if (res == G_CONVERTER_ERROR)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                       /* Translators: %s says what the fault is. */
                       _("The compressed AbiWord file is damaged: %s"), err->message);
          g_error_free (err);
          break;
        }
      if (out->len > (256u << 20))
        {
          /* A small file that would unpack without end. */
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               _("The compressed AbiWord file unpacks to more than 256 MB."));
          break;
        }
      if (res == G_CONVERTER_FINISHED)
        {
          if (*length - in_pos >= 2 && (guchar) (*contents)[in_pos] == 0x1f &&
              (guchar) (*contents)[in_pos + 1] == 0x8b)
            {
              g_converter_reset (G_CONVERTER (dec));
              continue;
            }
          ok = TRUE;
          break;
        }
      if (read == 0 && written == 0)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                               _("The compressed AbiWord file is cut short."));
          break;
        }
    }
  g_free (buf);
  g_object_unref (dec);
  g_free (*contents);
  *length = out->len;
  g_byte_array_append (out, (const guint8 *) "", 1);
  *contents = (char *) g_byte_array_free (out, FALSE);
  return ok;
}

gboolean
w42_abw_load (W42PieceTable *pt, W42PageSetup *page, GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  Abw a;
  GMarkupParser parser = { abw_start, abw_end, abw_text, NULL, NULL };
  GMarkupParseContext *ctx;
  W42PageSetup local_page;
  gboolean ok;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return FALSE;

  if (length >= 2 && (guchar) contents[0] == 0x1f && (guchar) contents[1] == 0x8b &&
      !abw_gunzip (&contents, &length, error))
    {
      g_free (contents);
      return FALSE;
    }

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

  memset (&a, 0, sizeof a);
  w42_builder_init (&a.b, pt);
  a.pt = pt;
  a.page = page;
  a.lists = g_array_new (FALSE, FALSE, sizeof (AbwList));
  a.list_index = g_hash_table_new (g_direct_hash, g_direct_equal);
  a.images = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
  a.table_cols = g_array_new (FALSE, FALSE, sizeof (int));
  a.cover = g_array_new (FALSE, TRUE, sizeof (AbwCover));
  a.bookmarks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  a.styles = g_ptr_array_new_with_free_func (abw_style_free);
  a.comments = g_ptr_array_new ();
  a.text = g_string_new (NULL);
  a.hf_text = g_string_new (NULL);
  a.annotate_text = g_string_new (NULL);
  a.meta_text = g_string_new (NULL);
  {
    W42Fmt def;
    w42_fmt_init_default (&def);
    a.para_ch = def.ch;
  }

  /* The first pass: the pictures, which AbiWord keeps at the end of the
   * file, and how many columns each table's cells reach.  A file that
   * does not parse says so in the second. */
  {
    AbwScan scan;
    GMarkupParser scanner = { scan_start, scan_end, scan_text, NULL, NULL };

    memset (&scan, 0, sizeof scan);
    scan.images = a.images;
    scan.cols = a.table_cols;
    scan.open = g_array_new (FALSE, FALSE, sizeof (int));
    scan.data = g_string_new (NULL);
    ctx = g_markup_parse_context_new (&scanner, 0, &scan, NULL);
    if (g_markup_parse_context_parse (ctx, contents, length, NULL))
      g_markup_parse_context_end_parse (ctx, NULL);
    g_markup_parse_context_free (ctx);
    g_array_free (scan.open, TRUE);
    g_string_free (scan.data, TRUE);
    g_free (scan.name);
  }

  ctx = g_markup_parse_context_new (&parser, 0, &a, NULL);
  ok = g_markup_parse_context_parse (ctx, contents, length, error) &&
       g_markup_parse_context_end_parse (ctx, error);
  g_markup_parse_context_free (ctx);

  abw_flush (&a);
  abw_register_styles (&a);
  w42_builder_finish (&a.b);

  /* What the metadata said, as the document's summary information. */
  if (a.meta[0] != NULL || a.meta[1] != NULL || a.meta[2] != NULL ||
      a.meta[3] != NULL || a.meta[4] != NULL)
    {
      W42DocInfo info;

      memset (&info, 0, sizeof info);
      info.title    = a.meta[0];
      info.subject  = a.meta[1];
      info.author   = a.meta[2];
      info.keywords = a.meta[3];
      info.comments = a.meta[4];
      w42_pt_set_info (pt, &info);
    }

  w42_pt_clear_undo (pt);
  g_string_free (a.text, TRUE);
  g_string_free (a.hf_text, TRUE);
  g_string_free (a.annotate_text, TRUE);
  g_string_free (a.meta_text, TRUE);
  for (guint i = 0; i < G_N_ELEMENTS (a.meta); i++)
    g_free (a.meta[i]);
  g_array_free (a.lists, TRUE);
  g_hash_table_destroy (a.list_index);
  g_hash_table_destroy (a.images);
  g_array_free (a.table_cols, TRUE);
  g_array_free (a.cover, TRUE);
  g_hash_table_destroy (a.bookmarks);
  g_ptr_array_free (a.styles, TRUE);
  g_ptr_array_free (a.comments, TRUE);
  g_free (contents);
  return ok;
}

/* ====================================================================== */
/* Writing                                                                 */
/* ====================================================================== */

/* The text as XML: the markup characters escaped, and nothing that is
 * not a character in XML at all -- a control, a byte that is not UTF-8,
 * a non-character -- since one of those makes a conformant parser,
 * AbiWord's among them, refuse the whole file. */
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
          if (c == 0xFFFE || c == 0xFFFF)
            g_string_append (out, "\357\277\275");
          else if (c >= 0x20 || c == '\t')
            g_string_append_len (out, text + i, (gssize) n);
        }
      i += n;
    }
}

/* Text in a paragraph: the same, with a line break as AbiWord's <br/>. */
static void
xml_text (GString *out, const char *text, gsize len)
{
  gsize start = 0;

  for (gsize i = 0; i < len; i++)
    {
      gsize n = text[i] == '\n' ? 1
              : (guchar) text[i] == 0xE2 && i + 2 < len && (guchar) text[i + 1] == 0x80 &&
                (guchar) text[i + 2] == 0xA8 ? 3 : 0;

      if (n == 0)
        continue;
      xml_escape (out, text + start, i - start);
      g_string_append (out, "<br/>");
      i += n - 1;
      start = i + 1;
    }
  xml_escape (out, text + start, len - start);
}

/* A value in props, where a ';' would end it and begin a property of its
 * own: a font called "X; font-size:99pt" set the size. */
static void
prop_escape (GString *out, const char *value)
{
  char *safe = g_strdelimit (g_strdup (value), ";", ',');

  xml_escape (out, safe, strlen (safe));
  g_free (safe);
}

static void
append_twips (GString *s, const char *key, int twips)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_string_append_printf (s, "%s%s:%sin", s->len > 0 ? "; " : "", key,
                          g_ascii_formatd (buf, sizeof buf, "%.5f", twips / 1440.0));
}

static gboolean
same_tabs (const W42ParaFmt *a, const W42ParaFmt *b)
{
  if (a->n_tabs != b->n_tabs)
    return FALSE;
  for (int i = 0; i < a->n_tabs; i++)
    if (a->tab_pos[i] != b->tab_pos[i] || a->tab_kind[i] != b->tab_kind[i])
      return FALSE;
  return TRUE;
}

static void
para_props (GString *s, const W42ParaFmt *pa, const W42ParaFmt *base)
{
  static const char *sides[4] = { "top", "bot", "left", "right" };
  static const int bits[4] = { W42_BORDER_TOP, W42_BORDER_BOTTOM, W42_BORDER_LEFT, W42_BORDER_RIGHT };
  const char *align = pa->align == W42_ALIGN_CENTER ? "center" : pa->align == W42_ALIGN_RIGHT ? "right"
                    : pa->align == W42_ALIGN_JUSTIFY ? "justify" : "left";

  if (base == NULL || pa->align != base->align)
    g_string_append_printf (s, "%stext-align:%s", s->len > 0 ? "; " : "", align);
  /* A paragraph's props go over its style's, so what differs from the
   * style is said, nought included: a paragraph indented 0 in a style
   * indented an inch would otherwise take the inch. */
#define DIFFERS(field) (base == NULL ? pa->field != 0 : pa->field != base->field)
  if (DIFFERS (indent_left))  append_twips (s, "margin-left", pa->indent_left);
  if (DIFFERS (indent_right)) append_twips (s, "margin-right", pa->indent_right);
  if (DIFFERS (indent_first)) append_twips (s, "text-indent", pa->indent_first);
  if (DIFFERS (space_before)) append_twips (s, "margin-top", pa->space_before);
  if (DIFFERS (space_after))  append_twips (s, "margin-bottom", pa->space_after);
#undef DIFFERS
  if (pa->line_spacing_pct > 0 && pa->line_spacing_pct != 100)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      g_string_append_printf (s, "%sline-height:%s", s->len > 0 ? "; " : "",
                              g_ascii_formatd (buf, sizeof buf, "%.2f", pa->line_spacing_pct / 100.0));
    }
  else if (pa->line_spacing > 0)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      g_string_append_printf (s, "%sline-height:%spt", s->len > 0 ? "; " : "",
                              g_ascii_formatd (buf, sizeof buf, "%.2f", pa->line_spacing / 20.0));
    }
  else if (base != NULL && ((base->line_spacing_pct > 0 && base->line_spacing_pct != 100) ||
                            base->line_spacing > 0))
    g_string_append_printf (s, "%sline-height:1.0", s->len > 0 ? "; " : "");
  /* The stops are the paragraph's own, not more of its style's: said
   * where they differ, and said empty where it has none and the style
   * has some. */
  if (base == NULL ? pa->n_tabs > 0 : !same_tabs (pa, base))
    {
      g_string_append_printf (s, "%stabstops:", s->len > 0 ? "; " : "");
      for (int i = 0; i < pa->n_tabs; i++)
        {
          char buf[G_ASCII_DTOSTR_BUF_SIZE];

          /* AbiWord writes the stop as position/kind and a leader digit:
           * 0 none, 1 dots, 2 hyphens, 3 underline. */
          g_string_append_printf (s, "%s%sin/%c%d", i > 0 ? "," : "",
                                  g_ascii_formatd (buf, sizeof buf, "%.4f", pa->tab_pos[i] / 1440.0),
                                  W42_TAB_KIND (pa->tab_kind[i]) == W42_TAB_CENTER ? 'C'
                                  : W42_TAB_KIND (pa->tab_kind[i]) == W42_TAB_RIGHT ? 'R'
                                  : W42_TAB_KIND (pa->tab_kind[i]) == W42_TAB_DECIMAL ? 'D' : 'L',
                                  (int) W42_TAB_LEADER (pa->tab_kind[i]));
        }
    }
  /* Each side the paragraph has where it is not the base's, and "none"
   * for a side the base has and the paragraph does not. */
  for (int i = 0; i < 4; i++)
    {
      gboolean has = (pa->border & bits[i]) != 0;
      gboolean base_has = base != NULL && (base->border & bits[i]) != 0;

      if (has && (!base_has || memcmp (&pa->edge[i], &base->edge[i], sizeof pa->edge[i]) != 0))
        {
          char bw[G_ASCII_DTOSTR_BUF_SIZE];
          const W42BorderEdge *edge = &pa->edge[i];
          int style = edge->style == W42_BORDER_DOTTED ? 2 : edge->style == W42_BORDER_DASHED ? 3
                    : edge->style == W42_BORDER_DOUBLE ? 4 : 1;

          g_string_append_printf (s, "%s%s-style:%d; %s-thickness:%spt; %s-color:%06x", s->len > 0 ? "; " : "",
                                  sides[i], style, sides[i],
                                  g_ascii_formatd (bw, sizeof bw, "%.2f", W42_EDGE_WIDTH (edge) / 20.0),
                                  sides[i], edge->color & 0xFFFFFF);
        }
      else if (!has && base_has)
        g_string_append_printf (s, "%s%s-style:none", s->len > 0 ? "; " : "", sides[i]);
    }
  if (pa->has_shading_color)
    g_string_append_printf (s, "%sbgcolor:%06x", s->len > 0 ? "; " : "",
                            pa->shading_color & 0xFFFFFF);
  else if (pa->shading > 0)
    {
      int grey = 255 - pa->shading * 255 / 100;

      g_string_append_printf (s, "%sbgcolor:%02x%02x%02x", s->len > 0 ? "; " : "", grey, grey, grey);
    }
  else if (base != NULL && (base->has_shading_color || base->shading > 0))
    g_string_append_printf (s, "%sbgcolor:transparent", s->len > 0 ? "; " : "");
  if (pa->keep_next)     g_string_append_printf (s, "%skeep-with-next:yes", s->len > 0 ? "; " : "");
  else if (base != NULL && base->keep_next)
    g_string_append_printf (s, "%skeep-with-next:no", s->len > 0 ? "; " : "");
  if (pa->keep_together) g_string_append_printf (s, "%skeep-together:yes", s->len > 0 ? "; " : "");
  else if (base != NULL && base->keep_together)
    g_string_append_printf (s, "%skeep-together:no", s->len > 0 ? "; " : "");
  if (pa->page_break_before)
    g_string_append_printf (s, "%spage-break-before:yes", s->len > 0 ? "; " : "");
  else if (base != NULL && base->page_break_before)
    g_string_append_printf (s, "%spage-break-before:no", s->len > 0 ? "; " : "");
  if (pa->rtl)           g_string_append_printf (s, "%sdom-dir:rtl", s->len > 0 ? "; " : "");
  else if (base != NULL && base->rtl)
    g_string_append_printf (s, "%sdom-dir:ltr", s->len > 0 ? "; " : "");
  /* Widow control is on unless said otherwise; AbiWord counts it in
   * lines, 2 being its own default. */
  if (!pa->widow_control)
    g_string_append_printf (s, "%swidows:0; orphans:0", s->len > 0 ? "; " : "");
  else if (base != NULL && !base->widow_control)
    g_string_append_printf (s, "%swidows:2; orphans:2", s->len > 0 ? "; " : "");
}

static void
char_props (GString *s, const W42CharFmt *ch, const W42CharFmt *base)
{
  if (ch->family != NULL && (base == NULL || ch->family != base->family))
    {
      g_string_append_printf (s, "%sfont-family:", s->len > 0 ? "; " : "");
      prop_escape (s, ch->family);
    }
  if (base == NULL || ch->size != base->size)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      /* Half-points: 19 of them is 9.5pt, not 9. */
      g_string_append_printf (s, "%sfont-size:%spt", s->len > 0 ? "; " : "",
                              g_ascii_formatd (buf, sizeof buf, "%.1f", ch->size / 2.0));
    }
  if (base == NULL || ch->bold != base->bold)
    g_string_append_printf (s, "%sfont-weight:%s", s->len > 0 ? "; " : "", ch->bold ? "bold" : "normal");
  if (base == NULL || ch->italic != base->italic)
    g_string_append_printf (s, "%sfont-style:%s", s->len > 0 ? "; " : "", ch->italic ? "italic" : "normal");
  /* The rest likewise say "none" and "normal" where the base has them
   * on and the run does not. */
  if (ch->underline || ch->strikeout || ch->overline)
    g_string_append_printf (s, "%stext-decoration:%s%s%s", s->len > 0 ? "; " : "",
                            ch->underline ? "underline " : "", ch->strikeout ? "line-through " : "",
                            ch->overline ? "overline" : "");
  else if (base != NULL && (base->underline || base->strikeout || base->overline))
    g_string_append_printf (s, "%stext-decoration:none", s->len > 0 ? "; " : "");
  if (ch->color != 0 || (base != NULL && base->color != 0))
    g_string_append_printf (s, "%scolor:%06x", s->len > 0 ? "; " : "", ch->color & 0xFFFFFF);
  if (ch->highlight != 0)
    g_string_append_printf (s, "%sbgcolor:%06x", s->len > 0 ? "; " : "", w42_highlight_rgb (ch->highlight));
  else if (base != NULL && base->highlight != 0)
    g_string_append_printf (s, "%sbgcolor:transparent", s->len > 0 ? "; " : "");
  if (ch->script != 0)
    g_string_append_printf (s, "%stext-position:%s", s->len > 0 ? "; " : "", ch->script > 0 ? "superscript" : "subscript");
  else if (base != NULL && base->script != 0)
    g_string_append_printf (s, "%stext-position:normal", s->len > 0 ? "; " : "");
  if (ch->smallcaps)
    g_string_append_printf (s, "%sfont-variant:small-caps", s->len > 0 ? "; " : "");
  else if (base != NULL && base->smallcaps)
    g_string_append_printf (s, "%sfont-variant:normal", s->len > 0 ? "; " : "");
  if (ch->allcaps)
    g_string_append_printf (s, "%stext-transform:uppercase", s->len > 0 ? "; " : "");
  else if (base != NULL && base->allcaps)
    g_string_append_printf (s, "%stext-transform:none", s->len > 0 ? "; " : "");
  if (ch->spacing != 0 || (base != NULL && base->spacing != 0))
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      g_string_append_printf (s, "%stext-spacing:%spt", s->len > 0 ? "; " : "",
                              g_ascii_formatd (buf, sizeof buf, "%.2f", ch->spacing / 20.0));
    }
  if (ch->lang != NULL && (base == NULL || ch->lang != base->lang))
    g_string_append_printf (s, "%slang:%s", s->len > 0 ? "; " : "", ch->lang);
}

static int
abw_list_type (W42ListKind kind)
{
  switch (kind)
    {
    case W42_LIST_NUMBER: return 0;
    case W42_LIST_LOWER_LETTER: return 1;
    case W42_LIST_UPPER_LETTER: return 2;
    case W42_LIST_LOWER_ROMAN: return 3;
    case W42_LIST_UPPER_ROMAN: return 4;
    case W42_LIST_BULLET_DASH: return 6;
    case W42_LIST_BULLET_SQUARE: return 7;
    default: return 5;
    }
}

typedef struct {
  GString       *out;
  GString       *data;          /* the <data> section */
  int            n_images;
  W42StyleSheet *styles;
  const char    *author;
  int            colw;          /* the column's width, which frames sit in */
  GString       *lists;         /* the <lists> table */
  int            next_list_id;
  W42ListKind    level_kind[9]; /* Word42's count as it stands, by level */
  int            level_id[9];
  W42ListKind    bullet_kind[9];
  int            bullet_id[9];
  int            level_last[9]; /* the last list at each level: a parent */
  int            note_id;
  int            annotation_id;
  const char    *bookmark;      /* open over a paragraph break */
  GString       *frames;        /* wrapped pictures, written after their paragraph */
} AbwWriter;

static int
abw_new_list (AbwWriter *w, const W42ParaFmt *pa, int lv)
{
  int id = w->next_list_id++;

  g_string_append_printf (w->lists,
    "<l id=\"%d\" parentid=\"%d\" type=\"%d\" start-value=\"%d\" list-delim=\"%s\" list-decimal=\".\"/>\n",
    id, lv > 0 ? w->level_last[lv - 1] : 0, abw_list_type (pa->list),
    pa->list_start > 0 ? pa->list_start : 1, w42_list_is_bullet (pa->list) ? "%L" : "%L.");
  return id;
}

/* The <l> a list paragraph is in.  AbiWord numbers a list's items on from
 * its start whatever comes between them; Word42 counts on while the kind
 * at a level stays the same, and starts again at another kind, a plain
 * paragraph or a restart.  So a new list begins wherever Word42's count
 * does: AbiWord numbers the items as Word42 does, and the reader knows a
 * restart by it. */
static int
abw_list_id (AbwWriter *w, const W42ParaFmt *pa)
{
  int lv = MIN (pa->list_level, 8);
  int id;

  if (w42_list_is_numbered (pa->list))
    {
      if (pa->list_start == 0 && w->level_kind[lv] == pa->list && w->level_id[lv] != 0)
        id = w->level_id[lv];
      else
        id = abw_new_list (w, pa, lv);
      w->level_kind[lv] = pa->list;
      w->level_id[lv] = id;
      /* An item at this level starts the deeper ones over. */
      for (int deeper = lv + 1; deeper < 9; deeper++)
        {
          w->level_kind[deeper] = w->bullet_kind[deeper] = W42_LIST_NONE;
          w->level_id[deeper] = w->bullet_id[deeper] = w->level_last[deeper] = 0;
        }
    }
  else
    {
      if (w->bullet_kind[lv] == pa->list && w->bullet_id[lv] != 0)
        id = w->bullet_id[lv];
      else
        id = abw_new_list (w, pa, lv);
      w->bullet_kind[lv] = pa->list;
      w->bullet_id[lv] = id;
    }
  w->level_last[lv] = id;
  return id;
}

/* A plain paragraph: Word42's count is over, and so are the lists. */
static void
abw_lists_end (AbwWriter *w)
{
  for (int lv = 0; lv < 9; lv++)
    {
      w->level_kind[lv] = w->bullet_kind[lv] = W42_LIST_NONE;
      w->level_id[lv] = w->bullet_id[lv] = w->level_last[lv] = 0;
    }
}

/* What a shape is, after the props of its picture. */
static void
shape_props (GString *s, const W42Object *object)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  if (object->shape <= W42_SHAPE_PICTURE || object->shape >= W42_SHAPE_KINDS)
    return;
  g_string_append_printf (s, "; w42-shape:%s; w42-line:%spt; w42-line-color:%06x",
                          SHAPE_NAMES[object->shape],
                          g_ascii_formatd (buf, sizeof buf, "%.2f", object->line_pt),
                          object->line_rgb & 0xFFFFFF);
  if (object->filled)
    g_string_append_printf (s, "; w42-fill:%06x", object->fill_rgb & 0xFFFFFF);
  if (object->text != NULL)
    {
      g_string_append (s, "; w42-shape-text:");
      prop_escape (s, object->text);
    }
}

/* A wrapped picture: AbiWord's frame, which follows the paragraph it is
 * anchored to, placed from that paragraph's top.  One with no place of its
 * own goes at the edge of the column on its side, which the reader takes
 * for no place. */
static void
write_frame_object (AbwWriter *w, const W42Object *object)
{
  char xb[G_ASCII_DTOSTR_BUF_SIZE], yb[G_ASCII_DTOSTR_BUF_SIZE];
  char wb[G_ASCII_DTOSTR_BUF_SIZE], hb[G_ASCII_DTOSTR_BUF_SIZE];
  const char *mode = object->wrap == W42_WRAP_LEFT ? "wrapped-to-right"
                   : object->wrap == W42_WRAP_RIGHT ? "wrapped-to-left"
                   : object->wrap == W42_WRAP_TOP_BOTTOM ? "wrapped-topbot"
                   : object->wrap == W42_WRAP_BEHIND ? "below-text" : "above-text";
  int x = object->pos_x, y = object->pos_y;

  if (!object->positioned)
    {
      x = object->wrap == W42_WRAP_RIGHT ? MAX (w->colw - object->width, 0) : 0;
      y = 0;
    }
  g_string_append_printf (w->frames,
                          "<frame props=\"frame-type:image; wrap-mode:%s; position-to:block-above-text; "
                          "xpos:%sin; ypos:%sin; frame-width:%sin; frame-height:%sin",
                          mode, g_ascii_formatd (xb, sizeof xb, "%.4f", x / 1440.0),
                          g_ascii_formatd (yb, sizeof yb, "%.4f", y / 1440.0),
                          g_ascii_formatd (wb, sizeof wb, "%.4f", object->width / 1440.0),
                          g_ascii_formatd (hb, sizeof hb, "%.4f", object->height / 1440.0));
  shape_props (w->frames, object);
  g_string_append_printf (w->frames, "\" strux-image-dataid=\"image%d\"/>\n", w->n_images);
}

static void
close_bookmark (AbwWriter *w)
{
  g_string_append (w->out, "<bookmark type=\"end\" name=\"");
  xml_escape (w->out, w->bookmark, strlen (w->bookmark));
  g_string_append (w->out, "\"/>");
  w->bookmark = NULL;
}

static void write_paragraph (AbwWriter *w, W42PieceTable *pt, W42ApTable *aps, GPtrArray *blocks,
                             const W42Block *block, const W42Block *next, gboolean counted,
                             gboolean note_anchor, const W42Run *note_run);

/* Whether `b` goes on where `a` leaves off -- the same cell or note, or
 * the body -- so that a bookmark may run on into it. */
static gboolean
same_flow (const W42Block *a, const W42Block *b)
{
  return b != NULL && a->note == b->note && a->table == b->table &&
         (a->table < 0 || (a->row == b->row && a->col == b->col));
}

/* A paragraph's runs.  `next` is the paragraph that may go on with a
 * bookmark this one leaves open, or NULL. */
static void
write_block_runs (AbwWriter *w, W42PieceTable *pt, W42ApTable *aps, GPtrArray *blocks,
                  const W42Block *block, const W42CharFmt *para_ch, const W42Block *next)
{
  const char *open_link = NULL;
  const char *open_comment = NULL;

  for (guint i = 0; i < block->runs->len; i++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, i);
      const W42CharFmt *ch = &w42_ap_table_get (aps, run->ap)->ch;
      GString *props;

      /* Neither the insertions nor the deletions of a tracked change are
       * written as such, so the file has them all accepted: a deletion
       * saved as text came back as words nobody had kept. */
      if (ch->revision == 2)
        continue;

      /* <ann> and <a> nest: a link the comment's end cuts through is
       * closed with it, and opened again after. */
      if (open_comment != NULL && ch->comment != open_comment)
        {
          if (open_link != NULL)
            {
              g_string_append (w->out, "</a>");
              open_link = NULL;
            }
          g_string_append (w->out, "</ann>");
          open_comment = NULL;
        }
      if (open_link != NULL && ch->link != open_link)
        {
          g_string_append (w->out, "</a>");
          open_link = NULL;
        }
      if (w->bookmark != NULL && ch->bookmark != w->bookmark)
        close_bookmark (w);
      if (ch->bookmark != NULL && w->bookmark == NULL)
        {
          g_string_append (w->out, "<bookmark type=\"start\" name=\"");
          xml_escape (w->out, ch->bookmark, strlen (ch->bookmark));
          g_string_append (w->out, "\"/>");
          w->bookmark = ch->bookmark;
        }
      if (ch->comment != NULL && open_comment == NULL)
        {
          /* The annotation: the comment's text first, in <annotate>, and
           * the text it is on after it, to </ann>. */
          int id = ++w->annotation_id;

          if (open_link != NULL)
            {
              g_string_append (w->out, "</a>");
              open_link = NULL;
            }
          g_string_append_printf (w->out, "<ann annotation=\"%d\"><annotate annotation-id=\"%d\"", id, id);
          if (w->author != NULL)
            {
              g_string_append (w->out, " props=\"annotation-author:");
              prop_escape (w->out, w->author);
              g_string_append (w->out, "\"");
            }
          g_string_append (w->out, "><p>");
          xml_text (w->out, ch->comment, strlen (ch->comment));
          g_string_append (w->out, "</p></annotate>");
          open_comment = ch->comment;
        }
      if (ch->link != NULL && open_link == NULL)
        {
          g_string_append (w->out, "<a xlink:href=\"");
          xml_escape (w->out, ch->link, strlen (ch->link));
          g_string_append (w->out, "\">");
          open_link = ch->link;
        }

      if (run->object != W42_OBJECT_NONE)
        {
          const W42Object *object = w42_object_table_get (w42_pt_object_table (pt), run->object);
          const char *mime = "image/png";
          GString *iprops = g_string_new (NULL);
          GBytes *png = object != NULL
                          ? w42_image_for_container (object->data, NULL, &mime) : NULL;

          if (png != NULL)
            {
              char *b64 = g_base64_encode (g_bytes_get_data (png, NULL), g_bytes_get_size (png));
              char wbuf[G_ASCII_DTOSTR_BUF_SIZE], hbuf[G_ASCII_DTOSTR_BUF_SIZE];

              w->n_images++;
              /* A frame follows its paragraph, which a note's cannot:
               * there the picture stays in the line. */
              if (object->wrap != W42_WRAP_INLINE && block->note < 0)
                write_frame_object (w, object);
              else
                {
                  /* The picture's run has a font and a size like any other,
                   * and the line it sits on is as tall as they make it. */
                  char_props (iprops, ch, NULL);
                  g_string_append_printf (w->out, "<c props=\"%s\"><image dataid=\"image%d\" props=\"width:%sin; height:%sin",
                                          iprops->str, w->n_images,
                                          g_ascii_formatd (wbuf, sizeof wbuf, "%.4f", object->width / 1440.0),
                                          g_ascii_formatd (hbuf, sizeof hbuf, "%.4f", object->height / 1440.0));
                  shape_props (w->out, object);
                  g_string_append (w->out, "\"/></c>");
                }
              g_string_append_printf (w->data, "<d name=\"image%d\" mime-type=\"%s\" base64=\"yes\">\n%s\n</d>\n",
                                      w->n_images, mime, b64);
              g_free (b64);
              g_bytes_unref (png);
            }
          g_string_free (iprops, TRUE);
          continue;
        }
      if (run->footnote > 0)
        {
          /* The reference in the mark's own formatting, then the note's
           * paragraphs inline, as AbiWord has them.  A link or a comment
           * round the mark stops short of the note, or the note's text
           * would be in it too. */
          const char *kind = run->endnote ? "endnote" : "footnote";
          const char *outer_bookmark = w->bookmark;
          const W42Block *first = NULL;
          int id = w->note_id++;

          props = g_string_new (NULL);
          char_props (props, ch, para_ch);
          if (props->len > 0)
            g_string_append_printf (w->out, "<c props=\"%s\">", props->str);
          g_string_append_printf (w->out, "<field type=\"%s_ref\" %s-id=\"%d\"/>", kind, kind, id);
          if (props->len > 0)
            g_string_append (w->out, "</c>");
          g_string_free (props, TRUE);
          if (open_link != NULL)
            {
              g_string_append (w->out, "</a>");
              open_link = NULL;
            }
          if (open_comment != NULL)
            {
              g_string_append (w->out, "</ann>");
              open_comment = NULL;
            }
          g_string_append_printf (w->out, "<%s %s-id=\"%d\">", run->endnote ? "endnote" : "foot", kind, id);
          w->bookmark = NULL;
          for (guint b = 0; b < blocks->len; b++)
            {
              const W42Block *nb = g_ptr_array_index (blocks, b);
              const W42Block *nnext = b + 1 < blocks->len ? g_ptr_array_index (blocks, b + 1) : NULL;

              if (nb->note != run->footnote_id)
                continue;
              write_paragraph (w, pt, aps, blocks, nb, same_flow (nb, nnext) ? nnext : NULL,
                               FALSE, first == NULL, run);
              if (first == NULL)
                first = nb;
            }
          if (w->bookmark != NULL)
            close_bookmark (w);
          w->bookmark = outer_bookmark;
          g_string_append_printf (w->out, "</%s>", run->endnote ? "endnote" : "foot");
          continue;
        }

      if (ch->field != NULL)
        {
          /* AbiWord keeps the field, not its result.  A field it has no
           * type for -- a table's formula, an index entry -- keeps its
           * text instead: written as a word count, it counted words, and
           * the words an index entry marked were gone. */
          const char *type = g_str_equal (ch->field, "PAGE") ? "page_number"
                           : g_str_equal (ch->field, "NUMPAGES") ? "page_count"
                           : g_str_equal (ch->field, "DATE") ? "date"
                           : g_str_equal (ch->field, "TIME") ? "time"
                           : g_str_equal (ch->field, "FILENAME") ? "file_name"
                           : g_str_equal (ch->field, "NUMWORDS") ? "word_count" : NULL;

          if (type != NULL)
            {
              props = g_string_new (NULL);
              char_props (props, ch, para_ch);
              if (props->len > 0)
                g_string_append_printf (w->out, "<c props=\"%s\"><field type=\"%s\"/></c>", props->str, type);
              else
                g_string_append_printf (w->out, "<field type=\"%s\"/>", type);
              g_string_free (props, TRUE);
              continue;
            }
        }

      props = g_string_new (NULL);
      char_props (props, ch, para_ch);
      if (props->len > 0)
        {
          g_string_append_printf (w->out, "<c props=\"%s\">", props->str);
          xml_text (w->out, block->text->str + run->byte_offset, run->n_bytes);
          g_string_append (w->out, "</c>");
        }
      else
        xml_text (w->out, block->text->str + run->byte_offset, run->n_bytes);
      g_string_free (props, TRUE);
    }
  if (open_link != NULL)
    g_string_append (w->out, "</a>");
  if (open_comment != NULL)
    g_string_append (w->out, "</ann>");
  /* A bookmark the next paragraph goes on with stays open over the
   * break: closed and opened again, one became two of the same name. */
  if (w->bookmark != NULL)
    {
      const W42CharFmt *go_on = NULL;

      if (next != NULL && next->runs->len > 0)
        go_on = &w42_ap_table_get (aps, g_array_index (next->runs, W42Run, 0).ap)->ch;
      if (go_on == NULL || go_on->bookmark != w->bookmark)
        close_bookmark (w);
    }
}

/* A paragraph, in its style, with what it says over that style.
 * `counted` is for the body's paragraphs, which Word42 numbers its lists
 * through; a note's paragraphs are not.  `note_run` is the reference of
 * the note whose first paragraph this is when `note_anchor` is set. */
static void
write_paragraph (AbwWriter *w, W42PieceTable *pt, W42ApTable *aps, GPtrArray *blocks,
                 const W42Block *block, const W42Block *next, gboolean counted,
                 gboolean note_anchor, const W42Run *note_run)
{
  const W42ParaFmt *pa = &w42_ap_table_get (aps, block->ap)->pa;
  const W42Style *style = pa->style != NULL ? w42_stylesheet_find (w->styles, pa->style) : NULL;
  GString *props = g_string_new (NULL);
  int list_id = 0;

  if (pa->list != W42_LIST_NONE)
    list_id = counted ? abw_list_id (w, pa) : abw_new_list (w, pa, MIN (pa->list_level, 8));
  else if (counted)
    abw_lists_end (w);
  para_props (props, pa, style != NULL ? &style->pa : NULL);
  if (pa->list != W42_LIST_NONE)
    {
      /* An item's indents are always said, as AbiWord says them: one
       * that says none is read as wanting the usual hanging indent. */
      if (strstr (props->str, "margin-left:") == NULL)
        append_twips (props, "margin-left", pa->indent_left);
      if (strstr (props->str, "text-indent:") == NULL)
        append_twips (props, "text-indent", pa->indent_first);
    }
  g_string_append (w->out, "<p");
  if (pa->style != NULL)
    {
      g_string_append (w->out, " style=\"");
      xml_escape (w->out, pa->style, strlen (pa->style));
      g_string_append (w->out, "\"");
    }
  if (pa->list != W42_LIST_NONE)
    g_string_append_printf (w->out, " list=\"%d\" level=\"%d\"", list_id, MIN (pa->list_level, 8) + 1);
  if (props->len > 0)
    g_string_append_printf (w->out, " props=\"%s\"", props->str);
  g_string_append (w->out, ">");
  g_string_free (props, TRUE);
  if (note_anchor && note_run != NULL)
    {
      const char *kind = note_run->endnote ? "endnote" : "footnote";

      g_string_append_printf (w->out, "<c props=\"text-position:superscript\"><field type=\"%s_anchor\" %s-id=\"%d\"/></c>",
                              kind, kind, w->note_id - 1);
    }
  write_block_runs (w, pt, aps, blocks, block, style != NULL ? &style->ch : NULL, next);
  /* A note's paragraphs are inside its reference's, where a line end
   * between them would be text. */
  g_string_append (w->out, counted ? "</p>\n" : "</p>");
}

/* A text box's height, which AbiWord wants said: its paragraphs' lines at
 * their size, near enough -- Word42's frame grows with its text. */
static int
textbox_height (W42ApTable *aps, GPtrArray *blocks, guint first, int side, int width)
{
  int h = 0;

  for (guint k = first; k < blocks->len; k++)
    {
      const W42Block *bk = g_ptr_array_index (blocks, k);
      const W42Fmt *f = w42_ap_table_get (aps, bk->ap);
      int size = MAX (f->ch.size, 2);                       /* half-points */
      int per_line = MAX (width / (size * 5), 1);           /* half an em a letter */
      int chars = (int) g_utf8_strlen (bk->text->str, (gssize) bk->text->len);

      if (bk->table >= 0 || bk->note >= 0 || f->pa.frame_side != side)
        break;
      h += MAX ((chars + per_line - 1) / per_line, 1) * size * 12      /* 1.2 em a line */
           + f->pa.space_before + f->pa.space_after;
    }
  return CLAMP (h, 360, 31680);
}

/* A header's or a footer's section, its fields as AbiWord's. */
static void
write_page_text (GString *body, const W42PageText *text, const char *type, int id)
{
  const char *p = text != NULL && text->text != NULL ? text->text : "";
  const char *brace;

  g_string_append_printf (body, "<section id=\"%d\" type=\"%s\"><p", id, type);
  if (text != NULL && text->align == W42_ALIGN_CENTER) g_string_append (body, " props=\"text-align:center\"");
  if (text != NULL && text->align == W42_ALIGN_RIGHT) g_string_append (body, " props=\"text-align:right\"");
  g_string_append (body, ">");
  while ((brace = strchr (p, '{')) != NULL && strchr (brace, '}') != NULL)
    {
      const char *close = strchr (brace, '}');

      xml_text (body, p, brace - p);
      if (g_ascii_strncasecmp (brace, "{PAGE}", 6) == 0) g_string_append (body, "<field type=\"page_number\"/>");
      else if (g_ascii_strncasecmp (brace, "{NUMPAGES}", 10) == 0) g_string_append (body, "<field type=\"page_count\"/>");
      else if (g_ascii_strncasecmp (brace, "{DATE}", 6) == 0) g_string_append (body, "<field type=\"date\"/>");
      else xml_text (body, brace, close - brace + 1);
      p = close + 1;
    }
  xml_text (body, p, strlen (p));
  g_string_append (body, "</p></section>\n");
}

gboolean
w42_abw_save (W42PieceTable *pt, const W42PageSetup *page, GFile *file, GError **error)
{
  static const char *const hf_suffix[3] = { "", "-even", "-first" };
  GPtrArray *blocks;
  W42ApTable *aps;
  W42StyleSheet *styles;
  AbwWriter w;
  GString *body = g_string_new (NULL);
  GString *out;
  W42PageSetup pg;
  const W42PageText *heads[3], *feet[3];
  gboolean has_header = FALSE, has_footer = FALSE;
  int table_open = -1;
  int sect_cols, sect_gap;
  gboolean ok;
  char b1[G_ASCII_DTOSTR_BUF_SIZE], b2[G_ASCII_DTOSTR_BUF_SIZE];

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
  w.out = body;
  w.data = g_string_new (NULL);
  w.frames = g_string_new (NULL);
  w.lists = g_string_new (NULL);
  w.styles = styles;
  w.author = w42_pt_get_author (pt);
  w.colw = abw_column_width (&pg);
  w.next_list_id = 1;
  w.note_id = 0;

  /* The page's own header and footer, the even pages' and the first
   * page's, each where the document has it: the choice of the even pages'
   * and the title page's is kept apart from their text, which may be
   * blank.  Those are only there beside the plain one, which stands in
   * empty when only they have text. */
  heads[0] = w42_pt_get_header (pt);
  feet[0] = w42_pt_get_footer (pt);
  heads[1] = w42_pt_get_facing_pages (pt) ? w42_pt_get_header_kind (pt, W42_PAGE_TEXT_EVEN) : NULL;
  feet[1] = w42_pt_get_facing_pages (pt) ? w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_EVEN) : NULL;
  heads[2] = w42_pt_get_title_page (pt) ? w42_pt_get_header_kind (pt, W42_PAGE_TEXT_FIRST) : NULL;
  feet[2] = w42_pt_get_title_page (pt) ? w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_FIRST) : NULL;
  for (int k = 0; k < 3; k++)
    {
      has_header |= heads[k] != NULL && heads[k]->text != NULL && *heads[k]->text;
      has_footer |= feet[k] != NULL && feet[k]->text != NULL && *feet[k]->text;
    }

  /* The first section opens with the page's margins and columns. */
  sect_cols = w42_page_columns (&pg);
  sect_gap = w42_page_column_gap (&pg);
  {
    GString *sp = g_string_new (NULL);

    append_twips (sp, "page-margin-left", pg.margin_left);
    append_twips (sp, "page-margin-right", pg.margin_right);
    append_twips (sp, "page-margin-top", pg.margin_top);
    append_twips (sp, "page-margin-bottom", pg.margin_bottom);
    if (sect_cols > 1)
      {
        g_string_append_printf (sp, "; columns:%d", sect_cols);
        append_twips (sp, "column-gap", sect_gap);
      }
    g_string_append (body, "<section id=\"1\"");
    for (int k = 0; k < 3; k++)
      {
        if (has_header && (k == 0 || heads[k] != NULL))
          g_string_append_printf (body, " header%s=\"%d\"", hf_suffix[k], 100 + k);
        if (has_footer && (k == 0 || feet[k] != NULL))
          g_string_append_printf (body, " footer%s=\"%d\"", hf_suffix[k], 103 + k);
      }
    g_string_append_printf (body, " props=\"%s\">\n", sp->str);
    g_string_free (sp, TRUE);
  }

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42Fmt *fmt = w42_ap_table_get (aps, block->ap);
      const W42ParaFmt *pa = &fmt->pa;
      const W42Block *prev = b > 0 ? g_ptr_array_index (blocks, b - 1) : NULL;
      const W42Block *next = b + 1 < blocks->len ? g_ptr_array_index (blocks, b + 1) : NULL;
      gboolean framed, prev_same, next_same;

      if (block->note >= 0)
        continue;                         /* written inline at their references */

      if (pa->section_break && b > 0 && block->table < 0)
        {
          GString *sp = g_string_new (NULL);

          if (table_open >= 0)
            {
              g_string_append (body, "</table>\n");
              table_open = -1;
            }
          sect_cols = MAX (pa->columns, 1);
          sect_gap = pa->column_gap > 0 ? pa->column_gap : 720;
          append_twips (sp, "page-margin-left", pg.margin_left);
          append_twips (sp, "page-margin-right", pg.margin_right);
          append_twips (sp, "page-margin-top", pg.margin_top);
          append_twips (sp, "page-margin-bottom", pg.margin_bottom);
          if (sect_cols > 1)
            {
              g_string_append_printf (sp, "; columns:%d", sect_cols);
              append_twips (sp, "column-gap", sect_gap);
            }
          g_string_append_printf (body, "</section>\n<section props=\"%s\">\n", sp->str);
          g_string_free (sp, TRUE);
        }

      /* Tables. */
      if (block->table >= 0 && block->table != table_open)
        {
          const W42TableProps *tp = w42_pt_table_props (pt, block->table);
          GString *cols = g_string_new (NULL);

          if (tp != NULL)
            for (int c = 0; c < tp->n_cols; c++)
              g_string_append_printf (cols, "%sin/", g_ascii_formatd (b1, sizeof b1, "%.4f",
                                      g_array_index (tp->widths, int, c) / 1440.0));
          g_string_append_printf (body, "<table props=\"table-column-props:%s\">\n", cols->str);
          g_string_free (cols, TRUE);
          table_open = block->table;
        }
      if (block->table >= 0)
        {
          const W42ParaFmt *cpa = &w42_ap_table_get (aps, block->cell_ap)->pa;
          gboolean cell_start = prev == NULL || prev->table != block->table ||
                                prev->row != block->row || prev->col != block->col;

          if (cpa->cell_vspan == W42_CELL_COVERED)
            {
              /* A cell a merge above covers: AbiWord has no cell there. */
              if (next == NULL || next->table != block->table)
                {
                  g_string_append (body, "</table>\n");
                  table_open = -1;
                }
              continue;
            }
          if (cell_start)
            {
              GString *cell_props = g_string_new (NULL);
              int rows = cpa->cell_vspan > 1 ? cpa->cell_vspan : 1;

              /* The cell's own rules and background, in the same words a
               * paragraph's are written in. */
              {
                const W42TableProps *tp = w42_pt_table_props (pt, block->table);
                W42ParaFmt shown = *cpa;
                gboolean own = (cpa->border & W42_BORDER_CELL_SET) != 0;
                gboolean first_row = block->row == 0, last_row = TRUE;
                int n_cols = tp != NULL ? tp->n_cols : 1;

                for (guint k = b + 1; k < blocks->len; k++)
                  {
                    const W42Block *rb = g_ptr_array_index (blocks, k);

                    if (rb->table != block->table)
                      break;
                    if (rb->row >= block->row + rows)
                      {
                        last_row = FALSE;
                        break;
                      }
                  }
                /* Each side's line: the cell's own, else the table's --
                 * its outer line round the outside, its inside rule
                 * between the cells -- so that a reader that looks only
                 * at the cells sees the table whole. */
                shown.border = own ? (cpa->border & W42_BORDER_BOX)
                             : (tp == NULL || tp->borders) ? W42_BORDER_BOX : 0;
                for (int k = 0; k < 4; k++)
                  {
                    gboolean outer = (k == W42_EDGE_TOP && first_row) || (k == W42_EDGE_BOTTOM && last_row) ||
                                     (k == W42_EDGE_LEFT && block->col == 0) ||
                                     (k == W42_EDGE_RIGHT && block->col + block->span >= n_cols);
                    const W42BorderEdge *e = &cpa->edge[k];

                    if (own && (e->width != 0 || e->style != 0 || e->color != 0))
                      continue;
                    shown.edge[k] = tp != NULL
                      ? tp->edge[outer ? k : (k <= W42_EDGE_BOTTOM ? W42_EDGE_INSIDE_H : W42_EDGE_INSIDE_V)]
                      : (W42BorderEdge) { 0, 0, 0 };
                    if (shown.edge[k].style == W42_BORDER_NONE)
                      {
                        shown.border &= (guint8) ~(1 << k);
                        shown.edge[k].style = W42_BORDER_SINGLE;
                      }
                  }
                if (!cpa->has_shading_color && cpa->shading > 0)
                  {
                    int grey = 255 - (int) cpa->shading * 255 / 100;

                    shown.has_shading_color = 1;
                    shown.shading_color = (guint32) ((grey << 16) | (grey << 8) | grey);
                    shown.shading = 0;
                  }
                if (shown.border != 0 || shown.has_shading_color || own)
                  para_props (cell_props, &shown, NULL);
              }
              /* A cell merged down runs to the row its merge ends at. */
              g_string_append_printf (body, "<cell props=\"left-attach:%d; right-attach:%d; top-attach:%d; bot-attach:%d%s%s\">\n",
                                      block->col, block->col + MAX (block->span, 1), block->row, block->row + rows,
                                      cell_props->len > 0 ? "; " : "", cell_props->str);
              g_string_free (cell_props, TRUE);
            }
        }

      /* Paragraphs in a frame: AbiWord's text box, after the paragraph
       * before them and at the side of the column they are set at. */
      framed = pa->frame_side != W42_FRAME_NONE && block->table < 0;
      prev_same = prev != NULL && prev->table < 0 && prev->note < 0 &&
                  w42_ap_table_get (aps, prev->ap)->pa.frame_side == pa->frame_side;
      next_same = next != NULL && next->table < 0 && next->note < 0 &&
                  w42_ap_table_get (aps, next->ap)->pa.frame_side == pa->frame_side;
      if (framed && !prev_same)
        {
          int width = pa->frame_width > 0 ? pa->frame_width : w.colw / 3;
          int x = pa->frame_side == W42_FRAME_RIGHT ? MAX (w.colw - width, 0) : 0;
          char xb[G_ASCII_DTOSTR_BUF_SIZE], wb[G_ASCII_DTOSTR_BUF_SIZE], hb[G_ASCII_DTOSTR_BUF_SIZE];

          g_string_append_printf (body, "<frame props=\"frame-type:textbox; wrap-mode:%s; position-to:block-above-text; "
                                  "xpos:%sin; ypos:0.0000in; frame-width:%sin; frame-height:%sin\">\n",
                                  pa->frame_side == W42_FRAME_RIGHT ? "wrapped-to-left" : "wrapped-to-right",
                                  g_ascii_formatd (xb, sizeof xb, "%.4f", x / 1440.0),
                                  g_ascii_formatd (wb, sizeof wb, "%.4f", width / 1440.0),
                                  g_ascii_formatd (hb, sizeof hb, "%.4f",
                                                   textbox_height (aps, blocks, b, pa->frame_side, width) / 1440.0));
        }

      write_paragraph (&w, pt, aps, blocks, block, same_flow (block, next) ? next : NULL, TRUE, FALSE, NULL);
      if (w.frames->len > 0)
        {
          g_string_append (body, w.frames->str);
          g_string_truncate (w.frames, 0);
        }
      if (framed && !next_same)
        g_string_append (body, "</frame>\n");

      if (block->table >= 0)
        {
          gboolean cell_end = next == NULL || next->table != block->table ||
                              next->row != block->row || next->col != block->col;

          if (cell_end)
            g_string_append (body, "</cell>\n");
          if (next == NULL || next->table != block->table)
            {
              g_string_append (body, "</table>\n");
              table_open = -1;
            }
        }
    }
  g_string_append (body, "</section>\n");

  /* Header and footer sections, fields for the page numbers. */
  for (int k = 0; k < 3; k++)
    if (has_header && (k == 0 || heads[k] != NULL))
      {
        char *type = g_strconcat ("header", hf_suffix[k], NULL);

        write_page_text (body, heads[k], type, 100 + k);
        g_free (type);
      }
  for (int k = 0; k < 3; k++)
    if (has_footer && (k == 0 || feet[k] != NULL))
      {
        char *type = g_strconcat ("footer", hf_suffix[k], NULL);

        write_page_text (body, feet[k], type, 103 + k);
        g_free (type);
      }

  /* The file. */
  out = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<abiword template=\"false\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
                      "xmlns=\"http://www.abisource.com/awml.dtd\" xmlns:awml=\"http://www.abisource.com/awml.dtd\" "
                      "version=\"3.0.5\" fileformat=\"1.1\" styles=\"unlocked\">\n"
                      "<metadata><m key=\"dc.format\">application/x-abiword</m><m key=\"abiword.generator\">Word42</m>");
  {
    /* File > Summary Info, under the names AbiWord gives it and the
     * reader looks for. */
    const W42DocInfo *info = w42_pt_get_info (pt);
    static const struct { const char *key; gsize offset; } fields[] = {
      { "dc.title",         G_STRUCT_OFFSET (W42DocInfo, title) },
      { "dc.subject",       G_STRUCT_OFFSET (W42DocInfo, subject) },
      { "dc.creator",       G_STRUCT_OFFSET (W42DocInfo, author) },
      { "abiword.keywords", G_STRUCT_OFFSET (W42DocInfo, keywords) },
      { "dc.description",   G_STRUCT_OFFSET (W42DocInfo, comments) },
    };

    for (guint i = 0; info != NULL && i < G_N_ELEMENTS (fields); i++)
      {
        const char *value = G_STRUCT_MEMBER (const char *, info, fields[i].offset);

        if (value == NULL || *value == '\0')
          continue;
        g_string_append_printf (out, "<m key=\"%s\">", fields[i].key);
        xml_escape (out, value, strlen (value));
        g_string_append (out, "</m>");
      }
  }
  g_string_append (out, "</metadata>\n");
  g_string_append (out, "<styles>\n");
  for (guint i = 0; i < w42_stylesheet_size (styles); i++)
    {
      const W42Style *s = w42_stylesheet_get (styles, i);
      const W42Style *parent = s->based_on != NULL ? w42_stylesheet_find (styles, s->based_on) : NULL;
      GString *props = g_string_new (NULL);

      /* Said over its parent, as the reader reads it: what the style
       * shares with its parent is left to the parent, and a nought where
       * the parent has more is said.  Written whole, a heading with no
       * space after under a Normal with some came back with Normal's. */
      if (parent == s)
        parent = NULL;
      para_props (props, &s->pa, parent != NULL ? &parent->pa : NULL);
      char_props (props, &s->ch, parent != NULL ? &parent->ch : NULL);
      g_string_append_printf (out, "<s type=\"%s\" name=\"", s->character ? "C" : "P");
      xml_escape (out, s->name, strlen (s->name));
      g_string_append (out, "\"");
      if (s->based_on != NULL)
        {
          g_string_append (out, " basedon=\"");
          xml_escape (out, s->based_on, strlen (s->based_on));
          g_string_append (out, "\"");
        }
      g_string_append_printf (out, " followedby=\"Normal\" props=\"%s\"/>\n", props->str);
      g_string_free (props, TRUE);
    }
  g_string_append (out, "</styles>\n");
  if (w.lists->len > 0)
    g_string_append_printf (out, "<lists>\n%s</lists>\n", w.lists->str);
  g_string_append_printf (out, "<pagesize pagetype=\"Custom\" orientation=\"%s\" width=\"%s\" height=\"%s\" units=\"in\" page-scale=\"1.000000\"/>\n",
                          pg.width > pg.height ? "landscape" : "portrait",
                          g_ascii_formatd (b1, sizeof b1, "%.4f", pg.width / 1440.0),
                          g_ascii_formatd (b2, sizeof b2, "%.4f", pg.height / 1440.0));
  g_string_append (out, body->str);
  if (w.data->len > 0)
    g_string_append_printf (out, "<data>\n%s</data>\n", w.data->str);
  g_string_append (out, "</abiword>\n");

  /* .zabw: the same, gzipped. */
  {
    char *basename = g_file_get_basename (file);
    gboolean gz = basename != NULL && g_str_has_suffix (basename, ".zabw");

    if (gz)
      {
        GZlibCompressor *comp = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, 6);
        GByteArray *packed = g_byte_array_new ();
        guint8 buf[65536];
        gsize in_pos = 0;
        GConverterResult res;
        GError *err = NULL;

        do
          {
            gsize read = 0, written = 0;

            res = g_converter_convert (G_CONVERTER (comp), out->str + in_pos, out->len - in_pos,
                                       buf, sizeof buf, G_CONVERTER_INPUT_AT_END, &read, &written, &err);
            in_pos += read;
            g_byte_array_append (packed, buf, written);
          }
        while (res == G_CONVERTER_CONVERTED);
        g_object_unref (comp);
        /* A stream that did not finish is not the document, and must not
         * take its place on the disk. */
        if (res != G_CONVERTER_FINISHED)
          {
            if (err != NULL)
              g_propagate_error (error, err);
            else
              g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   _("The document could not be compressed."));
            ok = FALSE;
          }
        else
          ok = g_file_replace_contents (file, (const char *) packed->data, packed->len, NULL, FALSE,
                                        G_FILE_CREATE_NONE, NULL, NULL, error);
        g_byte_array_free (packed, TRUE);
      }
    else
      ok = g_file_replace_contents (file, out->str, out->len, NULL, FALSE,
                                    G_FILE_CREATE_NONE, NULL, NULL, error);
    g_free (basename);
  }

  g_string_free (out, TRUE);
  g_string_free (body, TRUE);
  g_string_free (w.data, TRUE);
  g_string_free (w.frames, TRUE);
  g_string_free (w.lists, TRUE);
  g_ptr_array_free (blocks, TRUE);
  return ok;
}
