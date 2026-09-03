/* w42-abw.c - see w42-abw.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-abw.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

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
} AbwList;

typedef struct {
  W42Builder   b;
  W42PieceTable *pt;
  W42PageSetup *page;

  GArray      *lists;            /* AbwList */
  GHashTable  *images;           /* data name -> GBytes (base64 decoded) */
  GHashTable  *image_uses;       /* data name -> GArray of gsize positions+sizes? */

  GString     *text;
  int          c_depth;          /* inside <c>: text is formatting-bearing */
  W42CharFmt   para_ch;          /* the paragraph's base (from <p props>) */
  const char  *link;
  int          section_n;        /* body sections seen */
  gboolean     in_header, in_footer, in_ignored;
  GString     *hf_text;
  W42Align     hf_align;
  int          skip_depth;
  gboolean     in_data;
  GString     *data_b64;
  char        *data_name;
  gboolean     data_is_b64;

  GPtrArray   *pending_images;   /* PendingImage: placed after data is read */
  gboolean     in_foot;
  gboolean     foot_first_para;
  W42ParaFmt   foot_outer_pa;      /* the paragraph the note sits in */
  W42CharFmt   foot_outer_ch;
  gboolean     para_open;
  gboolean     list_para;
  int          list_last_id;
  gpointer     meta_field;      /* slot + 1 of the <m key="..."> being read */
  GString     *meta_text;
  char        *meta[5];         /* title, subject, author, keywords, comments */
} Abw;

typedef struct {
  char  *name;
  gsize  pos;
  int    width, height;         /* twips; 0 means the picture's own */
  W42Wrap wrap;
} PendingImage;

static void
image_free (gpointer data)
{
  PendingImage *pi = data;

  g_free (pi->name);
  g_free (pi);
}

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
      if (strchr (value, '+') != NULL)          /* "14pt+": at least */
        pa->line_spacing = length_twips (value);
      else if (g_ascii_isdigit (value[strlen (value) - 1]))
        pa->line_spacing_pct = (int) (CLAMP (g_ascii_strtod (value, NULL), 0.0, 100.0) * 100.0 + 0.5);
      else
        pa->line_spacing = length_twips (value);
    }
  else if (g_str_equal (key, "list-style") && *value != '\0' &&
           g_ascii_strcasecmp (value, "None") != 0)
    pa->list = (guint8) list_style_kind (value);
  else if (g_str_equal (key, "keep-with-next")) pa->keep_next = g_str_equal (value, "yes");
  else if (g_str_equal (key, "keep-together"))  pa->keep_together = g_str_equal (value, "yes");
  else if (g_str_equal (key, "widows"))         pa->widow_control = atoi (value) > 0;
  else if (g_str_equal (key, "dom-dir"))        pa->rtl = g_str_equal (value, "rtl");
  else if (g_str_equal (key, "page-break-before")) pa->page_break_before = g_str_equal (value, "yes");
  else if (g_str_equal (key, "tabstops"))
    {
      /* "1.0in/L0,2.5in/R0": a position, a kind and a leader each. */
      char **stops = g_strsplit (value, ",", -1);

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
      if (!g_str_equal (value, "0") && !g_str_equal (value, "none"))
        pa->border |= g_str_has_prefix (key, "top") ? W42_BORDER_TOP : g_str_has_prefix (key, "bot") ? W42_BORDER_BOTTOM
                    : g_str_has_prefix (key, "left") ? W42_BORDER_LEFT : W42_BORDER_RIGHT;
    }
  else if (g_str_has_suffix (key, "-thickness"))
    pa->border_width = (guint8) CLAMP (length_twips (value), 5, 120);
  else if (g_str_has_suffix (key, "-color") && (g_str_has_prefix (key, "top") || g_str_has_prefix (key, "bot") ||
                                                g_str_has_prefix (key, "left") || g_str_has_prefix (key, "right")))
    {
      if (strlen (value) >= 6)
        pa->border_color = (guint32) strtoul (value + (value[0] == '#'), NULL, 16) & 0xFFFFFF;
    }
  else if (g_str_equal (key, "bgcolor") || g_str_equal (key, "background-color"))
    {
      if (strlen (value) >= 6 && !g_str_equal (value, "transparent"))
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
        ch->color = (guint32) strtoul (value + (value[0] == '#'), NULL, 16);
    }
  else if (g_str_equal (key, "bgcolor"))
    {
      if (g_str_equal (value, "transparent"))
        ch->highlight = 0;
      else
        ch->highlight = 7;         /* AbiWord's yellow, near enough */
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

static void
abw_flush (Abw *a)
{
  if (a->text->len == 0)
    return;
  if (a->in_header || a->in_footer)
    g_string_append (a->hf_text, a->text->str);
  else
    {
      a->b.ch.link = a->link;
      w42_builder_text (&a->b, a->text->str);
    }
  g_string_truncate (a->text, 0);
}

static void
abw_start (GMarkupParseContext *ctx, const char *name, const char **an,
           const char **av, gpointer data, GError **error)
{
  Abw *a = data;

  (void) ctx; (void) error;

  if (a->skip_depth > 0)
    {
      a->skip_depth++;
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

      if (type != NULL && g_str_equal (type, "header"))
        {
          a->in_header = TRUE;
          g_string_truncate (a->hf_text, 0);
          a->hf_align = W42_ALIGN_LEFT;
        }
      else if (type != NULL && g_str_equal (type, "footer"))
        {
          a->in_footer = TRUE;
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
              a->b.pa.section_break = 1;
              a->b.pa.columns = 1;
            }
          each_prop (attr (an, av, "props"), section_prop, a);
          if (a->section_n > 1)
            {
              /* The columns go on the section's first paragraph, which
               * <p> below will start with this pa. */
              a->list_para = FALSE;
            }
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
      l.start = start != NULL ? atoi (start) : 1;
      g_array_append_val (a->lists, l);
    }
  else if (g_str_equal (name, "p"))
    {
      const char *style = attr (an, av, "style");
      const char *list = attr (an, av, "list");
      const char *level = attr (an, av, "level");
      W42ParaFmt keep_section;
      W42Fmt def;

      /* The section's columns were parked in pa; keep them for this,
       * the first paragraph, and nothing after. */
      keep_section = a->b.pa;
      w42_builder_reset_para (&a->b);
      if (keep_section.section_break)
        {
          a->b.pa.section_break = 1;
          a->b.pa.columns = keep_section.columns;
          a->b.pa.column_gap = keep_section.column_gap;
        }
      w42_fmt_init_default (&def);
      a->para_ch = def.ch;
      if (style != NULL)
        {
          const W42Style *s = w42_stylesheet_find (w42_pt_stylesheet (a->pt), style);

          if (s != NULL)
            {
              a->b.pa = s->pa;
              a->b.pa.section_break = keep_section.section_break;
              a->b.pa.columns = keep_section.columns;
              a->b.pa.column_gap = keep_section.column_gap;
              a->para_ch = s->ch;
            }
        }
      each_prop (attr (an, av, "props"), para_prop, &a->b.pa);
      each_prop (attr (an, av, "props"), para_char_prop, &a->para_ch);
      a->b.ch = a->para_ch;
      if (list != NULL)
        {
          int id = atoi (list);

          for (guint i = 0; i < a->lists->len; i++)
            {
              const AbwList *l = &g_array_index (a->lists, AbwList, i);

              if (l->id == id)
                {
                  a->b.pa.list = (guint8) l->kind;
                  if (id != a->list_last_id && l->start != 1 && w42_list_is_numbered (l->kind))
                    a->b.pa.list_start = (guint8) CLAMP (l->start, 1, 255);
                  if (a->b.pa.indent_first >= 0)
                    {
                      a->b.pa.indent_left = MAX (a->b.pa.indent_left, 360);
                      a->b.pa.indent_first = -360;
                    }
                }
            }
          if (level != NULL)
            a->b.pa.list_level = (guint8) CLAMP (atoi (level) - 1, 0, 8);
          a->list_last_id = id;
        }
      else
        a->list_last_id = -1;
      if (a->in_foot)
        {
          if (!a->foot_first_para)
            w42_builder_end_paragraph (&a->b);
          a->foot_first_para = FALSE;
        }
      if (a->in_header || a->in_footer)
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
      a->c_depth++;
    }
  else if (g_str_equal (name, "a"))
    {
      const char *href = attr (an, av, "xlink:href");

      abw_flush (a);
      a->link = href != NULL ? g_intern_string (href) : NULL;
    }
  else if (g_str_equal (name, "bookmark"))
    {
      const char *type = attr (an, av, "type");
      const char *bname = attr (an, av, "name");

      abw_flush (a);
      if (type != NULL && bname != NULL)
        {
          if (g_str_equal (type, "start"))
            g_hash_table_insert (a->image_uses, g_strconcat ("bm:", bname, NULL), GSIZE_TO_POINTER (a->b.pos));
          else
            {
              char *key = g_strconcat ("bm:", bname, NULL);
              gpointer start = g_hash_table_lookup (a->image_uses, key);

              if (start != NULL && a->b.pos > GPOINTER_TO_SIZE (start))
                {
                  W42CharFmt want;

                  memset (&want, 0, sizeof want);
                  want.bookmark = g_intern_string (bname);
                  w42_pt_apply_char_fmt (a->pt, GPOINTER_TO_SIZE (start),
                                         a->b.pos - GPOINTER_TO_SIZE (start),
                                         W42_CHAR_BOOKMARK, &want);
                }
              g_free (key);
            }
        }
    }
  else if (g_str_equal (name, "br"))
    g_string_append (a->text, "\342\200\250");
  else if (g_str_equal (name, "s"))
    {
      /* A style of the file's own: paragraph (P) or character (C). */
      const char *sname = attr (an, av, "name");
      const char *type = attr (an, av, "type");
      const char *based = attr (an, av, "basedon");
      const char *props = attr (an, av, "props");
      W42StyleSheet *sheet = w42_pt_stylesheet (a->pt);

      if (sname != NULL && *sname != '\0' && strlen (sname) < 64 &&
          (type == NULL || g_str_equal (type, "P") || g_str_equal (type, "C")))
        {
          W42ParaFmt keep = a->b.pa;
          W42Fmt def;
          W42Style st;

          w42_fmt_init_default (&def);
          memset (&st, 0, sizeof st);
          st.name = g_intern_string (sname);
          st.character = type != NULL && g_str_equal (type, "C");
          a->b.pa = def.pa;
          st.ch = def.ch;
          if (based != NULL && *based != '\0')
            {
              /* The base's formatting first; the entry's own on top. */
              const W42Style *bs = w42_stylesheet_find (sheet, based);

              if (bs != NULL)
                {
                  a->b.pa = bs->pa;
                  st.ch = bs->ch;
                }
            }
          each_prop (props, para_prop, &a->b.pa);
          each_prop (props, para_char_prop, &st.ch);
          st.pa = a->b.pa;
          st.pa.style = st.name;
          st.pa_own = W42_STYLE_PA_ALL;
          st.ch_own = W42_STYLE_CH_ALL;
          a->b.pa = keep;
          if (based != NULL && *based != '\0')
            {
              const W42Style *b = w42_stylesheet_find (sheet, based);

              st.based_on = b != NULL ? b->name : g_intern_string (based);
            }
          w42_stylesheet_set (sheet, &st);
        }
    }
  else if (g_str_equal (name, "pbr"))
    {
      abw_flush (a);
      w42_builder_end_paragraph (&a->b);
      a->b.pa.page_break_before = 1;
    }
  else if (g_str_equal (name, "image"))
    {
      const char *dataid = attr (an, av, "dataid");
      PendingImage *pi;
      int width = 0, height = 0;
      char **items = g_strsplit (attr (an, av, "props") != NULL ? attr (an, av, "props") : "", ";", -1);

      for (int i = 0; items[i] != NULL; i++)
        {
          char *colon = strchr (items[i], ':');

          if (colon == NULL) continue;
          *colon = '\0';
          if (g_str_equal (g_strstrip (items[i]), "width")) width = length_twips (g_strstrip (colon + 1));
          if (g_str_equal (g_strstrip (items[i]), "height")) height = length_twips (g_strstrip (colon + 1));
        }
      g_strfreev (items);

      abw_flush (a);
      if (dataid != NULL && !(a->in_header || a->in_footer))
        {
          /* A placeholder now; the bytes come at the end of the file. */
          pi = g_new0 (PendingImage, 1);
          pi->name = g_strdup (dataid);
          pi->pos = a->b.pos;
          pi->width = width;
          pi->height = height;
          g_ptr_array_add (a->pending_images, pi);
          w42_builder_text (&a->b, "\357\277\274");    /* U+FFFC, replaced later */
        }
    }
  else if (g_str_equal (name, "field"))
    {
      const char *type = attr (an, av, "type");

      if (type != NULL && (a->in_header || a->in_footer))
        {
          if (g_str_equal (type, "page_number")) g_string_append (a->text, "{PAGE}");
          else if (g_str_equal (type, "page_count")) g_string_append (a->text, "{NUMPAGES}");
          else if (g_str_has_prefix (type, "date")) g_string_append (a->text, "{DATE}");
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

              abw_flush (a);
              a->b.ch.field = g_intern_static_string (code);
              w42_builder_text (&a->b, g_str_equal (code, "PAGE") ? "1" : "?");
              a->b.ch = keep;
            }
        }
    }
  else if (g_str_equal (name, "foot") || g_str_equal (name, "endnote"))
    {
      abw_flush (a);
      if (!a->in_foot && !w42_builder_in_table (&a->b))
        {
          a->foot_outer_pa = a->b.pa;
          a->foot_outer_ch = a->para_ch;
          w42_builder_begin_note (&a->b, g_str_equal (name, "endnote"));
          a->in_foot = TRUE;
          a->foot_first_para = TRUE;
        }
      else
        a->skip_depth = 1;
    }
  else if (g_str_equal (name, "table"))
    {
      const char *props = attr (an, av, "props");
      GArray *widths = g_array_new (FALSE, FALSE, sizeof (int));
      const char *cols = props != NULL ? strstr (props, "table-column-props:") : NULL;

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
      if (w42_builder_in_table (&a->b))
        a->skip_depth = 1;            /* nested: not read */
      else
        {
          w42_builder_begin_table (&a->b, widths->len > 0 ? (int) widths->len : 1,
                                   widths->len > 0 ? (const int *) widths->data : NULL);
          /* AbiWord keeps every rule in the cells, so the table's own
           * "ruled" flag is off and each cell says what it wants. */
          w42_pt_table_set_borders (a->pt, a->b.table, FALSE);
        }
      g_array_free (widths, TRUE);
    }
  else if (g_str_equal (name, "cell"))
    {
      int left = 0, right = 1, top = 0;
      W42ParaFmt cell_pa;
      char **items = g_strsplit (attr (an, av, "props") != NULL ? attr (an, av, "props") : "", ";", -1);

      {
        W42Fmt def;

        w42_fmt_init_default (&def);
        cell_pa = def.pa;
      }

      for (int i = 0; items[i] != NULL; i++)
        {
          char *colon = strchr (items[i], ':');

          if (colon == NULL) continue;
          *colon = '\0';
          if (g_str_equal (g_strstrip (items[i]), "left-attach")) left = CLAMP (atoi (colon + 1), 0, 1023);
          if (g_str_equal (g_strstrip (items[i]), "right-attach")) right = CLAMP (atoi (colon + 1), 0, 1024);
          if (g_str_equal (g_strstrip (items[i]), "top-attach")) top = CLAMP (atoi (colon + 1), 0, 4096);
          para_prop (g_strstrip (items[i]), g_strstrip (colon + 1), &cell_pa);
        }
      g_strfreev (items);
      abw_flush (a);
      if (w42_builder_in_table (&a->b))
        {
          /* A cell that teleports thousands of empty rows down is broken
           * input, and every row it skips is a full row of cells to make. */
          if (top - a->b.row > 256)
            top = a->b.row;
          while (a->b.row < top)
            w42_builder_end_row (&a->b);
          while (a->b.col < left && a->b.col < a->b.n_cols)
            {
              int before = a->b.col;

              w42_builder_begin_cell (&a->b, 1);
              w42_builder_end_cell (&a->b);
              if (a->b.col == before)
                break;
            }
          w42_builder_begin_cell (&a->b, MAX (right - left, 1));
          if (a->b.cell_pos != (gsize) -1)
            {
              w42_pt_cell_set_borders_at (a->pt, a->b.cell_pos,
                                          cell_pa.border | W42_BORDER_CELL_SET);
              if (cell_pa.has_shading_color)
                w42_pt_cell_set_fill_at (a->pt, a->b.cell_pos, TRUE,
                                         cell_pa.shading_color);
            }
        }
    }
  else if (g_str_equal (name, "d"))
    {
      const char *b64 = attr (an, av, "base64");

      a->in_data = TRUE;
      g_free (a->data_name);
      a->data_name = g_strdup (attr (an, av, "name"));
      a->data_is_b64 = b64 == NULL || g_str_equal (b64, "yes");
      g_string_truncate (a->data_b64, 0);
    }
  else if (g_str_equal (name, "frame"))
    {
      /* A positioned picture: AbiWord puts the frame after the paragraph
       * it is anchored to, so it goes in before that paragraph's mark. */
      const char *dataid = attr (an, av, "strux-image-dataid");
      const char *props = attr (an, av, "props");
      char **items = g_strsplit (props != NULL ? props : "", ";", -1);
      gboolean image = FALSE, wrapped = FALSE;
      int width = 0, height = 0, xpos = 0;
      W42Wrap wrap = W42_WRAP_INLINE;

      for (int i = 0; items[i] != NULL; i++)
        {
          char *colon = strchr (items[i], ':');
          const char *key, *value;

          if (colon == NULL) continue;
          *colon = '\0';
          key = g_strstrip (items[i]);
          value = g_strstrip (colon + 1);
          if (g_str_equal (key, "frame-type")) image = g_str_equal (value, "image");
          else if (g_str_equal (key, "frame-width")) width = length_twips (value);
          else if (g_str_equal (key, "frame-height")) height = length_twips (value);
          else if (g_str_equal (key, "xpos")) xpos = length_twips (value);
          else if (g_str_equal (key, "wrap-mode"))
            {
              wrapped = g_str_has_prefix (value, "wrapped");
              if (g_str_equal (value, "wrapped-to-right")) wrap = W42_WRAP_LEFT;
              else if (g_str_equal (value, "wrapped-to-left")) wrap = W42_WRAP_RIGHT;
            }
        }
      g_strfreev (items);
      if (wrapped && wrap == W42_WRAP_INLINE)
        wrap = xpos > 720 ? W42_WRAP_RIGHT : W42_WRAP_LEFT;

      abw_flush (a);
      if (image && dataid != NULL && !(a->in_header || a->in_footer) && a->b.pos > 0)
        {
          PendingImage *pi = g_new0 (PendingImage, 1);

          pi->name = g_strdup (dataid);
          pi->pos = a->b.pos - 1;
          pi->width = width;
          pi->height = height;
          pi->wrap = wrap;
          g_ptr_array_add (a->pending_images, pi);
          w42_pt_insert_text (a->b.pt, pi->pos, "\357\277\274",
                              w42_pt_ap_at (a->b.pt, pi->pos));
          a->b.pos++;
        }
      a->skip_depth = 1;
    }
  else if (g_str_equal (name, "history") ||
           g_str_equal (name, "revisions") || g_str_equal (name, "ignoredwords") ||
           g_str_equal (name, "authors"))
    a->skip_depth = 1;
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

  if (g_str_equal (name, "m"))
    {
      int slot = GPOINTER_TO_INT (a->meta_field) - 1;

      if (slot >= 0 && slot < 5 && a->meta_text->len > 0 && a->meta[slot] == NULL)
        a->meta[slot] = g_strdup (g_strstrip (a->meta_text->str));
      a->meta_field = NULL;
      g_string_truncate (a->meta_text, 0);
      return;
    }

  if (g_str_equal (name, "p"))
    {
      abw_flush (a);
      if (a->in_header || a->in_footer)
        ;
      else if (a->in_foot)
        ;                             /* the note's paragraphs end at <foot>'s close */
      else
        {
          w42_builder_end_paragraph (&a->b);
          /* The section break belongs to its first paragraph only. */
          w42_builder_reset_para (&a->b);
        }
      a->para_open = FALSE;
    }
  else if (g_str_equal (name, "c"))
    {
      abw_flush (a);
      if (a->c_depth > 0)
        a->c_depth--;
      a->b.ch = a->para_ch;
    }
  else if (g_str_equal (name, "a"))
    {
      abw_flush (a);
      a->link = NULL;
    }
  else if (g_str_equal (name, "foot") || g_str_equal (name, "endnote"))
    {
      abw_flush (a);
      if (a->in_foot)
        {
          w42_builder_end_note (&a->b);
          a->in_foot = FALSE;
          /* Back in the paragraph the note hung from. */
          a->b.pa = a->foot_outer_pa;
          a->para_ch = a->foot_outer_ch;
          a->b.ch = a->para_ch;
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
      w42_builder_end_table (&a->b);
    }
  else if (g_str_equal (name, "section"))
    {
      abw_flush (a);
      if (a->in_header)
        {
          w42_pt_set_header (a->pt, a->hf_text->str, a->hf_align);
          a->in_header = FALSE;
        }
      else if (a->in_footer)
        {
          w42_pt_set_footer (a->pt, a->hf_text->str, a->hf_align);
          a->in_footer = FALSE;
        }
    }
  else if (g_str_equal (name, "d"))
    {
      if (a->data_name != NULL && a->data_is_b64)
        {
          gsize len = 0;
          guchar *bytes;
          /* Whitespace inside base64 is fine for g_base64_decode. */
          bytes = g_base64_decode (a->data_b64->str, &len);
          if (bytes != NULL && len > 0)
            g_hash_table_insert (a->images, g_strdup (a->data_name), g_bytes_new_take (bytes, len));
          else
            g_free (bytes);
        }
      a->in_data = FALSE;
    }
}

static void
abw_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  Abw *a = data;

  (void) ctx; (void) error;
  if (a->skip_depth > 0)
    return;
  if (a->meta_field != NULL)
    g_string_append_len (a->meta_text, text, len);
  else if (a->in_data)
    g_string_append_len (a->data_b64, text, len);
  else if (a->para_open)
    g_string_append_len (a->text, text, len);
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

  /* .zabw is the same, gzipped. */
  if (length >= 2 && (guchar) contents[0] == 0x1f && (guchar) contents[1] == 0x8b)
    {
      GZlibDecompressor *dec = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
      GByteArray *out = g_byte_array_new ();
      guint8 buf[65536];
      gsize in_pos = 0;
      GConverterResult res;

      do
        {
          gsize read = 0, written = 0;

          res = g_converter_convert (G_CONVERTER (dec), contents + in_pos, length - in_pos,
                                     buf, sizeof buf, G_CONVERTER_INPUT_AT_END,
                                     &read, &written, NULL);
          in_pos += read;
          g_byte_array_append (out, buf, written);
          if (out->len > (256u << 20))
            {
              res = G_CONVERTER_ERROR;   /* a small file that would unpack without end */
              break;
            }
          if (read == 0 && written == 0 && in_pos >= length)
            break;                       /* nothing more can happen */
        }
      while (res == G_CONVERTER_CONVERTED);
      g_object_unref (dec);
      g_free (contents);
      length = out->len;
      g_byte_array_append (out, (const guint8 *) "", 1);
      contents = (char *) g_byte_array_free (out, FALSE);
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
  a.images = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_bytes_unref);
  a.image_uses = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  a.text = g_string_new (NULL);
  a.hf_text = g_string_new (NULL);
  a.data_b64 = g_string_new (NULL);
  a.meta_text = g_string_new (NULL);
  a.pending_images = g_ptr_array_new_with_free_func (image_free);
  a.list_last_id = -1;
  {
    W42Fmt def;
    w42_fmt_init_default (&def);
    a.para_ch = def.ch;
  }

  ctx = g_markup_parse_context_new (&parser, 0, &a, NULL);
  ok = g_markup_parse_context_parse (ctx, contents, length, error) &&
       g_markup_parse_context_end_parse (ctx, error);
  g_markup_parse_context_free (ctx);

  abw_flush (&a);
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

  /* The pictures: each placeholder becomes its object, back to front so
   * the positions stay right (an object is one position, like the
   * placeholder, so they do anyway). */
  for (guint i = a.pending_images->len; i > 0; i--)
    {
      PendingImage *pi = g_ptr_array_index (a.pending_images, i - 1);
      GBytes *bytes = g_hash_table_lookup (a.images, pi->name);
      int pw = 0, ph = 0;
      const char *format = NULL;

      if (bytes != NULL && w42_image_probe (bytes, &pw, &ph, &format))
        {
          W42ObjectIdx idx = w42_object_table_add (w42_pt_object_table (pt), bytes, format, pw, ph,
                                                   pi->width > 0 ? pi->width : pw * 15,
                                                   pi->height > 0 ? pi->height : ph * 15);
          W42ApIdx ap = w42_pt_ap_at (pt, pi->pos + 1);

          w42_object_table_set_wrap (w42_pt_object_table (pt), idx, pi->wrap);
          w42_pt_delete (pt, pi->pos, 1);
          w42_pt_insert_object (pt, pi->pos, idx, ap);
        }
      else
        w42_pt_delete (pt, pi->pos, 1);
    }

  w42_pt_clear_undo (pt);
  g_ptr_array_free (a.pending_images, TRUE);
  g_string_free (a.text, TRUE);
  g_string_free (a.hf_text, TRUE);
  g_string_free (a.data_b64, TRUE);
  g_string_free (a.meta_text, TRUE);
  for (guint i = 0; i < G_N_ELEMENTS (a.meta); i++)
    g_free (a.meta[i]);
  g_free (a.data_name);
  g_array_free (a.lists, TRUE);
  g_hash_table_destroy (a.images);
  g_hash_table_destroy (a.image_uses);
  g_free (contents);
  return ok;
}

/* ====================================================================== */
/* Writing                                                                 */
/* ====================================================================== */

static void
xml_escape (GString *out, const char *text, gsize len)
{
  for (gsize i = 0; i < len; i++)
    switch (text[i])
      {
      case '<': g_string_append (out, "&lt;"); break;
      case '>': g_string_append (out, "&gt;"); break;
      case '&': g_string_append (out, "&amp;"); break;
      case '"': g_string_append (out, "&quot;"); break;
      default:
        if ((guchar) text[i] == 0xE2 && i + 2 < len && (guchar) text[i + 1] == 0x80 && (guchar) text[i + 2] == 0xA8)
          {
            g_string_append (out, "<br/>");
            i += 2;
          }
        else if ((guchar) text[i] >= 0x20 || text[i] == '\t')
          g_string_append_c (out, text[i]);
      }
}

static void
append_twips (GString *s, const char *key, int twips)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_string_append_printf (s, "%s%s:%sin", s->len > 0 ? "; " : "", key,
                          g_ascii_formatd (buf, sizeof buf, "%.5f", twips / 1440.0));
}

static void
para_props (GString *s, const W42ParaFmt *pa, const W42ParaFmt *base)
{
  const char *align = pa->align == W42_ALIGN_CENTER ? "center" : pa->align == W42_ALIGN_RIGHT ? "right"
                    : pa->align == W42_ALIGN_JUSTIFY ? "justify" : "left";

  if (base == NULL || pa->align != base->align)
    g_string_append_printf (s, "%stext-align:%s", s->len > 0 ? "; " : "", align);
  if (pa->indent_left)  append_twips (s, "margin-left", pa->indent_left);
  if (pa->indent_right) append_twips (s, "margin-right", pa->indent_right);
  if (pa->indent_first) append_twips (s, "text-indent", pa->indent_first);
  if (pa->space_before) append_twips (s, "margin-top", pa->space_before);
  if (pa->space_after)  append_twips (s, "margin-bottom", pa->space_after);
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
  if (pa->n_tabs > 0)
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
  if (pa->border != 0)
    {
      static const char *sides[4] = { "top", "bot", "left", "right" };
      static const int bits[4] = { W42_BORDER_TOP, W42_BORDER_BOTTOM, W42_BORDER_LEFT, W42_BORDER_RIGHT };

      for (int i = 0; i < 4; i++)
        if (pa->border & bits[i])
          {
            char bw[G_ASCII_DTOSTR_BUF_SIZE];

            g_string_append_printf (s, "%s%s-style:1; %s-thickness:%spt; %s-color:%06x", s->len > 0 ? "; " : "",
                                    sides[i], sides[i],
                                    g_ascii_formatd (bw, sizeof bw, "%.2f",
                                                     (pa->border_width > 0 ? pa->border_width : 15) / 20.0),
                                    sides[i], pa->border_color & 0xFFFFFF);
          }
    }
  if (pa->has_shading_color)
    g_string_append_printf (s, "%sbgcolor:%06x", s->len > 0 ? "; " : "",
                            pa->shading_color & 0xFFFFFF);
  else if (pa->shading > 0)
    {
      int grey = 255 - pa->shading * 255 / 100;

      g_string_append_printf (s, "%sbgcolor:%02x%02x%02x", s->len > 0 ? "; " : "", grey, grey, grey);
    }
  if (pa->keep_next)     g_string_append_printf (s, "%skeep-with-next:yes", s->len > 0 ? "; " : "");
  if (pa->keep_together) g_string_append_printf (s, "%skeep-together:yes", s->len > 0 ? "; " : "");
  if (pa->rtl)           g_string_append_printf (s, "%sdom-dir:rtl", s->len > 0 ? "; " : "");
}

static void
char_props (GString *s, const W42CharFmt *ch, const W42CharFmt *base)
{
  if (ch->family != NULL && (base == NULL || ch->family != base->family))
    {
      g_string_append_printf (s, "%sfont-family:", s->len > 0 ? "; " : "");
      xml_escape (s, ch->family, strlen (ch->family));
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
  if (ch->underline || ch->strikeout || ch->overline)
    g_string_append_printf (s, "%stext-decoration:%s%s%s", s->len > 0 ? "; " : "",
                            ch->underline ? "underline " : "", ch->strikeout ? "line-through " : "",
                            ch->overline ? "overline" : "");
  if (ch->color != 0)
    g_string_append_printf (s, "%scolor:%06x", s->len > 0 ? "; " : "", ch->color);
  if (ch->highlight != 0)
    g_string_append_printf (s, "%sbgcolor:%06x", s->len > 0 ? "; " : "", w42_highlight_rgb (ch->highlight));
  if (ch->script != 0)
    g_string_append_printf (s, "%stext-position:%s", s->len > 0 ? "; " : "", ch->script > 0 ? "superscript" : "subscript");
  if (ch->smallcaps)
    g_string_append_printf (s, "%sfont-variant:small-caps", s->len > 0 ? "; " : "");
  if (ch->allcaps)
    g_string_append_printf (s, "%stext-transform:uppercase", s->len > 0 ? "; " : "");
  if (ch->spacing != 0)
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      g_string_append_printf (s, "%stext-spacing:%spt", s->len > 0 ? "; " : "",
                              g_ascii_formatd (buf, sizeof buf, "%.2f", ch->spacing / 20.0));
    }
  if (ch->lang != NULL)
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
  GString   *out;
  GString   *data;          /* the <data> section */
  int        n_images;
  int        next_list_id;
  GString   *lists;         /* the <lists> table */
  int        note_id;
  GString *frames;    /* wrapped pictures, written after their paragraph */
} AbwWriter;

static void
write_block_runs (AbwWriter *w, W42PieceTable *pt, W42ApTable *aps, GPtrArray *blocks,
                  const W42Block *block, const W42CharFmt *para_ch)
{
  const char *open_link = NULL;
  const char *open_bookmark = NULL;

  for (guint i = 0; i < block->runs->len; i++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, i);
      const W42CharFmt *ch = &w42_ap_table_get (aps, run->ap)->ch;
      GString *props;

      if (open_link != NULL && ch->link != open_link)
        {
          g_string_append (w->out, "</a>");
          open_link = NULL;
        }
      if (open_bookmark != NULL && ch->bookmark != open_bookmark)
        {
          g_string_append (w->out, "<bookmark type=\"end\" name=\"");
          xml_escape (w->out, open_bookmark, strlen (open_bookmark));
          g_string_append (w->out, "\"/>");
          open_bookmark = NULL;
        }
      if (ch->bookmark != NULL && open_bookmark == NULL)
        {
          g_string_append (w->out, "<bookmark type=\"start\" name=\"");
          xml_escape (w->out, ch->bookmark, strlen (ch->bookmark));
          g_string_append (w->out, "\"/>");
          open_bookmark = ch->bookmark;
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
              if (object->wrap != W42_WRAP_INLINE)
                /* A positioned picture: AbiWord's frame, which follows the
                 * paragraph it is anchored to. */
                g_string_append_printf (w->frames,
                                        "<frame props=\"frame-type:image; wrap-mode:%s; position-to:column-above-text; "
                                        "xpos:0.0000in; ypos:0.0000in; frame-width:%sin; frame-height:%sin; "
                                        "frame-col-xpos:0.0000in; frame-col-ypos:0.0000in\" strux-image-dataid=\"image%d\"/>\n",
                                        object->wrap == W42_WRAP_LEFT ? "wrapped-to-right" : "wrapped-to-left",
                                        g_ascii_formatd (wbuf, sizeof wbuf, "%.4f", object->width / 1440.0),
                                        g_ascii_formatd (hbuf, sizeof hbuf, "%.4f", object->height / 1440.0),
                                        w->n_images);
              else
                {
                  /* The picture's run has a font and a size like any other,
                   * and the line it sits on is as tall as they make it. */
                  char_props (iprops, ch, NULL);
                  g_string_append_printf (w->out, "<c props=\"%s\"><image dataid=\"image%d\" props=\"width:%sin; height:%sin\"/></c>",
                                          iprops->str, w->n_images,
                                          g_ascii_formatd (wbuf, sizeof wbuf, "%.4f", object->width / 1440.0),
                                          g_ascii_formatd (hbuf, sizeof hbuf, "%.4f", object->height / 1440.0));
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
          /* The reference, then the note's paragraphs inline, as AbiWord
           * has them. */
          int id = w->note_id++;

          g_string_append_printf (w->out, "<c props=\"text-position:superscript\"><field type=\"%s_ref\" %s-id=\"%d\"/></c>",
                                  run->endnote ? "endnote" : "footnote", run->endnote ? "endnote" : "footnote", id);
          g_string_append_printf (w->out, "<%s %s-id=\"%d\">", run->endnote ? "endnote" : "foot",
                                  run->endnote ? "endnote" : "footnote", id);
          for (guint b = 0; b < blocks->len; b++)
            {
              const W42Block *nb = g_ptr_array_index (blocks, b);
              const W42Fmt *nfmt;
              GString *np;

              if (nb->note != run->footnote_id)
                continue;
              nfmt = w42_ap_table_get (aps, nb->ap);
              np = g_string_new (NULL);
              para_props (np, &nfmt->pa, NULL);
              g_string_append_printf (w->out, "<p style=\"Normal\" props=\"%s\">", np->str);
              g_string_free (np, TRUE);
              g_string_append_printf (w->out, "<c props=\"text-position:superscript\"><field type=\"%s_anchor\" %s-id=\"%d\"/></c>",
                                      run->endnote ? "endnote" : "footnote", run->endnote ? "endnote" : "footnote", id);
              write_block_runs (w, pt, aps, blocks, nb, &nfmt->ch);
              g_string_append (w->out, "</p>");
            }
          g_string_append_printf (w->out, "</%s>", run->endnote ? "endnote" : "foot");
          continue;
        }

      if (ch->field != NULL)
        {
          /* AbiWord keeps the field, not its result. */
          const char *type = g_str_equal (ch->field, "PAGE") ? "page_number"
                           : g_str_equal (ch->field, "NUMPAGES") ? "page_count"
                           : g_str_equal (ch->field, "DATE") ? "date"
                           : g_str_equal (ch->field, "TIME") ? "time"
                           : g_str_equal (ch->field, "FILENAME") ? "file_name" : "word_count";

          g_string_append_printf (w->out, "<field type=\"%s\"/>", type);
          continue;
        }

      props = g_string_new (NULL);
      char_props (props, ch, para_ch);
      if (props->len > 0)
        {
          g_string_append_printf (w->out, "<c props=\"%s\">", props->str);
          xml_escape (w->out, block->text->str + run->byte_offset, run->n_bytes);
          g_string_append (w->out, "</c>");
        }
      else
        xml_escape (w->out, block->text->str + run->byte_offset, run->n_bytes);
      g_string_free (props, TRUE);
    }
  if (open_link != NULL)
    g_string_append (w->out, "</a>");
  if (open_bookmark != NULL)
    {
      g_string_append (w->out, "<bookmark type=\"end\" name=\"");
      xml_escape (w->out, open_bookmark, strlen (open_bookmark));
      g_string_append (w->out, "\"/>");
    }
}

gboolean
w42_abw_save (W42PieceTable *pt, const W42PageSetup *page, GFile *file, GError **error)
{
  GPtrArray *blocks;
  W42ApTable *aps;
  W42StyleSheet *styles;
  AbwWriter w;
  GString *body = g_string_new (NULL);
  GString *out;
  W42PageSetup pg;
  const W42PageText *header, *footer;
  int table_open = -1;
  int list_id = 0;
  W42ListKind list_kind = W42_LIST_NONE;
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
  w.next_list_id = 1;
  w.note_id = 0;
  header = w42_pt_get_header (pt);
  footer = w42_pt_get_footer (pt);

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
    g_string_append_printf (body, "<section id=\"1\"%s%s props=\"%s\">\n",
                            header != NULL && header->text != NULL && *header->text ? " header=\"100\"" : "",
                            footer != NULL && footer->text != NULL && *footer->text ? " footer=\"101\"" : "",
                            sp->str);
    g_string_free (sp, TRUE);
  }

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42Fmt *fmt = w42_ap_table_get (aps, block->ap);
      const W42ParaFmt *pa = &fmt->pa;
      const W42Block *next = b + 1 < blocks->len ? g_ptr_array_index (blocks, b + 1) : NULL;
      const W42Style *style = pa->style != NULL ? w42_stylesheet_find (styles, pa->style) : NULL;
      GString *props;

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
          const W42Block *prev = b > 0 ? g_ptr_array_index (blocks, b - 1) : NULL;
          gboolean cell_start = prev == NULL || prev->table != block->table ||
                                prev->row != block->row || prev->col != block->col;

          if (cell_start)
            {
              const W42ParaFmt *cpa = &w42_ap_table_get (aps, block->cell_ap)->pa;
              GString *cell_props = g_string_new (NULL);

              /* The cell's own rules and background, in the same words a
               * paragraph's are written in. */
              if (cpa->border & W42_BORDER_CELL_SET || cpa->has_shading_color)
                {
                  W42ParaFmt shown = *cpa;

                  shown.border = cpa->border & W42_BORDER_BOX;
                  para_props (cell_props, &shown, NULL);
                }
              g_string_append_printf (body, "<cell props=\"left-attach:%d; right-attach:%d; top-attach:%d; bot-attach:%d%s%s\">\n",
                                      block->col, block->col + MAX (block->span, 1), block->row, block->row + 1,
                                      cell_props->len > 0 ? "; " : "", cell_props->str);
              g_string_free (cell_props, TRUE);
            }
        }

      /* Lists: one <l> per run of items of a kind. */
      if (pa->list != W42_LIST_NONE)
        {
          if (pa->list != list_kind || list_id == 0 || pa->list_start > 0)
            {
              list_id = w.next_list_id++;
              list_kind = pa->list;
              g_string_append_printf (w.lists,
                "<l id=\"%d\" parentid=\"0\" type=\"%d\" start-value=\"%d\" list-delim=\"%s\" list-decimal=\".\"/>\n",
                list_id, abw_list_type (pa->list), pa->list_start > 0 ? pa->list_start : 1,
                w42_list_is_bullet (pa->list) ? "%L" : "%L.");
            }
        }
      else
        {
          list_kind = W42_LIST_NONE;
          list_id = 0;
        }

      props = g_string_new (NULL);
      para_props (props, pa, style != NULL ? &style->pa : NULL);
      if (pa->page_break_before)
        g_string_append (props, props->len > 0 ? "; page-break-before:yes" : "page-break-before:yes");
      g_string_append (body, "<p");
      if (pa->style != NULL)
        {
          g_string_append (body, " style=\"");
          xml_escape (body, pa->style, strlen (pa->style));
          g_string_append (body, "\"");
        }
      if (pa->list != W42_LIST_NONE)
        g_string_append_printf (body, " list=\"%d\" level=\"%d\"", list_id, MIN (pa->list_level, 8) + 1);
      if (props->len > 0)
        g_string_append_printf (body, " props=\"%s\"", props->str);
      g_string_append (body, ">");
      g_string_free (props, TRUE);

      write_block_runs (&w, pt, aps, blocks, block, style != NULL ? &style->ch : NULL);
      g_string_append (body, "</p>\n");
      if (w.frames->len > 0)
        {
          g_string_append (body, w.frames->str);
          g_string_truncate (w.frames, 0);
        }

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
  if (header != NULL && header->text != NULL && *header->text)
    {
      g_string_append (body, "<section id=\"100\" type=\"header\"><p");
      if (header->align == W42_ALIGN_CENTER) g_string_append (body, " props=\"text-align:center\"");
      if (header->align == W42_ALIGN_RIGHT) g_string_append (body, " props=\"text-align:right\"");
      g_string_append (body, ">");
      {
        const char *p = header->text;
        const char *brace;

        while ((brace = strchr (p, '{')) != NULL && strchr (brace, '}') != NULL)
          {
            const char *close = strchr (brace, '}');
            xml_escape (body, p, brace - p);
            if (g_ascii_strncasecmp (brace, "{PAGE}", 6) == 0) g_string_append (body, "<field type=\"page_number\"/>");
            else if (g_ascii_strncasecmp (brace, "{NUMPAGES}", 10) == 0) g_string_append (body, "<field type=\"page_count\"/>");
            else if (g_ascii_strncasecmp (brace, "{DATE}", 6) == 0) g_string_append (body, "<field type=\"date\"/>");
            else xml_escape (body, brace, close - brace + 1);
            p = close + 1;
          }
        xml_escape (body, p, strlen (p));
      }
      g_string_append (body, "</p></section>\n");
    }
  if (footer != NULL && footer->text != NULL && *footer->text)
    {
      g_string_append (body, "<section id=\"101\" type=\"footer\"><p");
      if (footer->align == W42_ALIGN_CENTER) g_string_append (body, " props=\"text-align:center\"");
      if (footer->align == W42_ALIGN_RIGHT) g_string_append (body, " props=\"text-align:right\"");
      g_string_append (body, ">");
      {
        const char *p = footer->text;
        const char *brace;

        while ((brace = strchr (p, '{')) != NULL && strchr (brace, '}') != NULL)
          {
            const char *close = strchr (brace, '}');
            xml_escape (body, p, brace - p);
            if (g_ascii_strncasecmp (brace, "{PAGE}", 6) == 0) g_string_append (body, "<field type=\"page_number\"/>");
            else if (g_ascii_strncasecmp (brace, "{NUMPAGES}", 10) == 0) g_string_append (body, "<field type=\"page_count\"/>");
            else if (g_ascii_strncasecmp (brace, "{DATE}", 6) == 0) g_string_append (body, "<field type=\"date\"/>");
            else xml_escape (body, brace, close - brace + 1);
            p = close + 1;
          }
        xml_escape (body, p, strlen (p));
      }
      g_string_append (body, "</p></section>\n");
    }

  /* The file. */
  out = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<abiword template=\"false\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
                      "xmlns=\"http://www.abisource.com/awml.dtd\" xmlns:awml=\"http://www.abisource.com/awml.dtd\" "
                      "version=\"3.0.5\" fileformat=\"1.1\" styles=\"unlocked\">\n"
                      "<metadata><m key=\"dc.format\">application/x-abiword</m><m key=\"abiword.generator\">Word42</m></metadata>\n");
  g_string_append (out, "<styles>\n");
  for (guint i = 0; i < w42_stylesheet_size (styles); i++)
    {
      const W42Style *s = w42_stylesheet_get (styles, i);
      GString *props = g_string_new (NULL);

      if (!s->character)
        para_props (props, &s->pa, NULL);
      char_props (props, &s->ch, NULL);
      g_string_append_printf (out, "<s type=\"%s\" name=\"", s->character ? "C" : "P");
      xml_escape (out, s->name, strlen (s->name));
      g_string_append (out, "\" basedon=\"");
      if (s->based_on != NULL)
        xml_escape (out, s->based_on, strlen (s->based_on));
      else if (g_ascii_strcasecmp (s->name, "Normal") != 0)
        g_string_append (out, "Normal");
      g_string_append_printf (out, "\" followedby=\"Normal\" props=\"%s\"/>\n", props->str);
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

        do
          {
            gsize read = 0, written = 0;

            res = g_converter_convert (G_CONVERTER (comp), out->str + in_pos, out->len - in_pos,
                                       buf, sizeof buf, G_CONVERTER_INPUT_AT_END, &read, &written, NULL);
            in_pos += read;
            g_byte_array_append (packed, buf, written);
          }
        while (res == G_CONVERTER_CONVERTED);
        g_object_unref (comp);
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
