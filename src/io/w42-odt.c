/* w42-odt.c - see w42-odt.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-odt.h"

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
  double v;

  if (value == NULL)
    return 0;
  v = g_ascii_strtod (value, NULL);
  if (strstr (value, "pt") != NULL) return (int) (v * 20.0);
  if (strstr (value, "cm") != NULL) return (int) (v * 1440.0 / 2.54);
  if (strstr (value, "mm") != NULL) return (int) (v * 1440.0 / 25.4);
  if (strstr (value, "in") != NULL) return (int) (v * 1440.0);
  if (strstr (value, "px") != NULL) return (int) (v * 15.0);
  return (int) (v * 20.0);
}

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
        if ((guchar) text[i] >= 0x20 || text[i] == '\t' || text[i] == '\n')
          g_string_append_c (out, text[i]);
      }
}

static void
twips_out (GString *s, int twips)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];

  g_string_append (s, g_ascii_formatd (buf, sizeof buf, "%.4f", twips / 1440.0));
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
  gboolean   resolved;
  gboolean   has_pa, has_ch;
} OdtStyle;

typedef struct {
  W42ListKind kind[10];
} OdtListStyle;

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
  char          *cur_cell_style;
  int            pending_cell_sides;   /* for the cell about to begin; -1 none */
  char          *cur_row_style;
  int            table_row;     /* rows begun in the table being read */
  gboolean       in_header_rows;

  /* while a style is being read */
  OdtStyle      *cur_style;
  char          *cur_style_name;   /* and its name and family */
  gboolean       cur_style_text;
  gboolean       in_named_styles;  /* inside office:styles: the named ones */
  OdtListStyle  *cur_list;
  char          *cur_col_style;
  gboolean       in_page_layout, page_seen;
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
  GHashTable    *bookmark_start;
  int            list_depth;
  GPtrArray     *list_style_stack;   /* char*, one per open list */
  int            in_table;
  GArray        *table_widths;
  gboolean       table_started;
  gboolean       cell_pending;
  int            cell_span;
  int            cell_vspan;         /* rows the pending cell spans, or W42_CELL_COVERED */
  int            skip_covered;       /* covered cells the last sideways span accounts for */
  int            in_note;
  gboolean       note_first_para;
  W42ParaFmt     note_outer_pa;
  W42CharFmt     note_outer_ch;
  gboolean       in_annotation;
  GString       *annotation;
  gsize          annotation_pos;
  char          *annotation_name;
  GHashTable    *annotation_start;
  int            skip_depth;
  const char    *field;           /* inside a field element */
  char          *index_term;      /* between the marks of an index entry */
  gsize          index_start;
  GString       *field_text;
  gboolean       frame_pending;
  int            frame_w, frame_h;
  char          *frame_href;
  W42Wrap        frame_wrap;
  int            tb_depth;         /* inside a draw:text-box */
  int            tb_side, tb_width;
  W42ParaFmt     tb_saved_pa;      /* the anchoring paragraph, to go on with */
  W42CharFmt     tb_saved_ch;
  gboolean       tb_reopened;      /* it was reopened after the box, empty */
  GHashTable    *graphic_wraps;  /* graphic style name -> W42Wrap + 1 */
  char          *cur_graphic;    /* the graphic style being read */
} Odt;

static void
style_free (gpointer data)
{
  OdtStyle *s = data;

  g_free (s->parent);
  g_free (s->list_style);
  g_free (s->display);
  g_free (s);
}

/* ---- properties into formats -------------------------------------------- */

static void
para_props (Odt *o, W42ParaFmt *pa, const char **an, const char **av)
{
  for (int i = 0; an != NULL && an[i] != NULL; i++)
    {
      const char *k = an[i], *v = av[i];

      if (g_str_equal (k, "fo:text-align"))
        pa->align = g_str_equal (v, "center") ? W42_ALIGN_CENTER
                  : g_str_equal (v, "end") || g_str_equal (v, "right") ? W42_ALIGN_RIGHT
                  : g_str_equal (v, "justify") ? W42_ALIGN_JUSTIFY : W42_ALIGN_LEFT;
      else if (g_str_equal (k, "fo:margin-left"))   pa->indent_left = length_twips (v);
      else if (g_str_equal (k, "fo:margin-right"))  pa->indent_right = length_twips (v);
      else if (g_str_equal (k, "fo:text-indent"))   pa->indent_first = length_twips (v);
      else if (g_str_equal (k, "fo:margin-top"))    pa->space_before = length_twips (v);
      else if (g_str_equal (k, "fo:margin-bottom")) pa->space_after = length_twips (v);
      else if (g_str_equal (k, "fo:line-height"))
        {
          if (strchr (v, '%') != NULL)
            pa->line_spacing_pct = atoi (v);
          else if (!g_str_equal (v, "normal"))
            pa->line_spacing = length_twips (v);
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
          if (!g_str_equal (v, "none"))
            {
              int bit = g_str_equal (k, "fo:border") ? W42_BORDER_BOX
                      : g_str_equal (k, "fo:border-top") ? W42_BORDER_TOP
                      : g_str_equal (k, "fo:border-bottom") ? W42_BORDER_BOTTOM
                      : g_str_equal (k, "fo:border-left") ? W42_BORDER_LEFT : W42_BORDER_RIGHT;

              pa->border |= bit;
              pa->border_width = (guint8) CLAMP (length_twips (v), 5, 120);
            }
        }
      else if (g_str_equal (k, "fo:background-color"))
        {
          if (v[0] == '#' && strlen (v) >= 7)
            {
              guint32 rgb = (guint32) strtoul (v + 1, NULL, 16);
              int grey = ((rgb >> 16) & 0xff) * 30 / 100 + ((rgb >> 8) & 0xff) * 59 / 100 + (rgb & 0xff) * 11 / 100;

              pa->shading = (guint8) CLAMP (100 - grey * 100 / 255, 0, 100);
            }
        }
    }
  (void) o;
}

static void
text_props (Odt *o, W42CharFmt *ch, const char **an, const char **av)
{
  const char *lang = NULL, *country = NULL;

  for (int i = 0; an != NULL && an[i] != NULL; i++)
    {
      const char *k = an[i], *v = av[i];

      if (g_str_equal (k, "fo:language"))      lang = v;
      else if (g_str_equal (k, "fo:country"))  country = v;

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
      else if (g_str_equal (k, "style:text-overline-style")) ch->overline = !g_str_equal (v, "none");
      else if (g_str_equal (k, "fo:font-size"))
        {
          if (strchr (v, '%') == NULL)
            ch->size = CLAMP (length_twips (v) / 10, 2, 3276);
        }
      else if (g_str_equal (k, "fo:color"))
        {
          if (v[0] == '#' && strlen (v) >= 7)
            ch->color = (guint32) strtoul (v + 1, NULL, 16);
        }
      else if (g_str_equal (k, "fo:background-color"))
        ch->highlight = g_str_equal (v, "transparent") ? 0 : 7;
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
          if (!g_str_equal (v, "normal"))
            ch->spacing = (gint16) CLAMP (length_twips (v), -720, 720);
        }
    }

  /* The language and the country are two attributes and one tag. */
  if (lang != NULL && *lang != '\0')
    {
      if (g_str_equal (lang, W42_LANG_NONE))
        ch->lang = g_intern_static_string (W42_LANG_NONE);
      else
        {
          char *tag = country != NULL && *country != '\0' && !g_str_equal (country, "none")
                        ? g_strdup_printf ("%s-%s", lang, country)
                        : g_strdup (lang);

          const char *known = w42_lang_normalise (tag);

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
           * parent.  The properties were read on to defaults, so the
           * merge is: parent, then the style's own attributes again --
           * which is what re-reading would do.  Here the style's own
           * values were kept separately in has_pa/has_ch as whole
           * blocks; good enough for the styles files carry. */
          W42ParaFmt pa = parent->pa;
          W42CharFmt ch = parent->ch;

          if (s->has_pa)
            {
              /* keep the style's own paragraph values where they differ from the default */
              W42Fmt def;

              w42_fmt_init_default (&def);
              if (s->pa.align != def.pa.align) pa.align = s->pa.align;
              if (s->pa.indent_left != def.pa.indent_left) pa.indent_left = s->pa.indent_left;
              if (s->pa.indent_right != def.pa.indent_right) pa.indent_right = s->pa.indent_right;
              if (s->pa.indent_first != def.pa.indent_first) pa.indent_first = s->pa.indent_first;
              if (s->pa.space_before != def.pa.space_before) pa.space_before = s->pa.space_before;
              if (s->pa.space_after != def.pa.space_after) pa.space_after = s->pa.space_after;
              if (s->pa.line_spacing_pct != def.pa.line_spacing_pct) pa.line_spacing_pct = s->pa.line_spacing_pct;
              if (s->pa.line_spacing != def.pa.line_spacing) pa.line_spacing = s->pa.line_spacing;
              if (s->pa.page_break_before) pa.page_break_before = 1;
              if (s->pa.keep_next) pa.keep_next = 1;
              if (s->pa.keep_together) pa.keep_together = 1;
              if (s->pa.border) { pa.border = s->pa.border; pa.border_width = s->pa.border_width; }
              if (s->pa.shading) pa.shading = s->pa.shading;
              if (s->pa.rtl) pa.rtl = 1;
              if (s->pa.n_tabs) { pa.n_tabs = s->pa.n_tabs; memcpy (pa.tab_pos, s->pa.tab_pos, sizeof pa.tab_pos); memcpy (pa.tab_kind, s->pa.tab_kind, sizeof pa.tab_kind); }
              if (s->pa.drop_cap) pa.drop_cap = s->pa.drop_cap;
            }
          {
            /* A named style of the file's own keeps its name; an automatic
             * style takes its parent's. */
            W42Fmt def;

            w42_fmt_init_default (&def);
            if (s->pa.style != NULL && s->pa.style != def.pa.style)
              pa.style = s->pa.style;
          }
          if (s->has_ch)
            {
              W42Fmt def;

              w42_fmt_init_default (&def);
              if (s->ch.bold) ch.bold = 1;
              if (s->ch.italic) ch.italic = 1;
              if (s->ch.underline) ch.underline = s->ch.underline;
              if (s->ch.strikeout) ch.strikeout = 1;
              if (s->ch.overline) ch.overline = 1;
              if (s->ch.size != def.ch.size) ch.size = s->ch.size;
              if (s->ch.color != def.ch.color) ch.color = s->ch.color;
              if (s->ch.family != def.ch.family) ch.family = s->ch.family;
              if (s->ch.highlight) ch.highlight = s->ch.highlight;
              if (s->ch.script) ch.script = s->ch.script;
              if (s->ch.smallcaps) ch.smallcaps = 1;
              if (s->ch.allcaps) ch.allcaps = 1;
              if (s->ch.spacing) ch.spacing = s->ch.spacing;
              if (s->ch.lang != NULL) ch.lang = s->ch.lang;
            }
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

/* The word42 style a named style stands for, by its display name. */
static const char *
our_style_name (Odt *o, const char *display)
{
  W42StyleSheet *sheet = w42_pt_stylesheet (o->pt);

  if (display == NULL)
    return NULL;
  if (g_str_equal (display, "Standard") || g_str_equal (display, "Default Paragraph Style") ||
      g_str_equal (display, "Text body") || g_str_equal (display, "Default"))
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
      if (family == NULL || !(g_str_equal (family, "paragraph") || g_str_equal (family, "text")))
        return;
      if (name == NULL)
        name = g_str_equal (family, "paragraph") ? "@default-paragraph" : "@default-text";

      s = g_new0 (OdtStyle, 1);
      w42_fmt_init_default (&def);
      s->pa = def.pa;
      s->ch = def.ch;
      s->parent = g_strdup (attr (an, av, "style:parent-style-name"));
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
        const char *ours = our_style_name (o, s->display);

        if (lvl != NULL)
          s->outline = CLAMP (atoi (lvl), 0, 9);
        if (ours != NULL)
          s->pa.style = ours;
        else if (s->outline >= 1 && s->outline <= 3)
          {
            char *hn = g_strdup_printf ("Heading %d", s->outline);
            s->pa.style = g_intern_string (hn);
            g_free (hn);
          }
      }
      g_hash_table_insert (o->styles, g_strdup (name), s);
      o->cur_style = s;
      g_free (o->cur_style_name);
      o->cur_style_name = g_strdup (name);
      o->cur_style_text = g_str_equal (family, "text");
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

      if (wrap != NULL && !(g_str_equal (wrap, "none") || g_str_equal (wrap, "run-through")))
        {
          if (g_str_equal (wrap, "left"))
            w = W42_WRAP_RIGHT;
          else if (g_str_equal (wrap, "right"))
            w = W42_WRAP_LEFT;
          else
            w = (hpos != NULL && g_str_equal (hpos, "right")) ? W42_WRAP_RIGHT : W42_WRAP_LEFT;
        }
      g_hash_table_insert (o->graphic_wraps, g_strdup (o->cur_graphic), GINT_TO_POINTER ((int) w + 1));
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
      o->cur_style->has_pa = TRUE;
    }
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
      o->cur_style->has_ch = TRUE;
    }
  else if (g_str_equal (tag, "table-cell-properties") && o->cur_cell_style != NULL)
    {
      /* fo:border sets all four sides; fo:border-* one each; "none" clears. */
      int sides = -1;

      for (int i = 0; an != NULL && an[i] != NULL; i++)
        {
          const char *k = an[i], *v = av[i];
          int bit = g_str_equal (k, "fo:border") ? W42_BORDER_BOX
                  : g_str_equal (k, "fo:border-top") ? W42_BORDER_TOP
                  : g_str_equal (k, "fo:border-bottom") ? W42_BORDER_BOTTOM
                  : g_str_equal (k, "fo:border-left") ? W42_BORDER_LEFT
                  : g_str_equal (k, "fo:border-right") ? W42_BORDER_RIGHT : 0;

          if (bit == 0)
            continue;
          if (sides < 0)
            sides = 0;
          if (g_str_equal (v, "none"))
            sides &= ~bit;
          else
            sides |= bit;
        }
      if (sides >= 0)
        g_hash_table_insert (o->cell_sides, g_strdup (o->cur_cell_style),
                             GINT_TO_POINTER (W42_BORDER_CELL_SET | sides));
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
      if (attr (an, av, "fo:margin-top")) o->page->margin_top = length_twips (attr (an, av, "fo:margin-top"));
      if (attr (an, av, "fo:margin-bottom")) o->page->margin_bottom = length_twips (attr (an, av, "fo:margin-bottom"));
      if (attr (an, av, "fo:margin-left")) o->page->margin_left = length_twips (attr (an, av, "fo:margin-left"));
      if (attr (an, av, "fo:margin-right")) o->page->margin_right = length_twips (attr (an, av, "fo:margin-right"));
      {
        /* The colour behind the page. */
        const char *bg = attr (an, av, "fo:background-color");

        if (bg != NULL && *bg == '#' && strlen (bg) >= 7)
          {
            o->page->background = (guint32) strtoul (bg + 1, NULL, 16);
            o->page->has_background = 1;
          }
      }
      o->in_page_layout = TRUE;
    }
  else if (g_str_equal (tag, "columns") && o->in_page_layout)
    {
      const char *n = attr (an, av, "fo:column-count"), *gap = attr (an, av, "fo:column-gap");

      if (n != NULL) o->page->columns = CLAMP (atoi (n), 1, 9);
      if (gap != NULL) o->page->column_gap = length_twips (gap);
    }
  else if (g_str_equal (tag, "header") || g_str_equal (tag, "footer") ||
           g_str_equal (tag, "header-left") || g_str_equal (tag, "footer-left") ||
           g_str_equal (tag, "header-first") || g_str_equal (tag, "footer-first"))
    {
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

static void
styles_end (Odt *o, const char *tag)
{
  if (g_str_equal (tag, "styles"))
    o->in_named_styles = FALSE;
  if (g_str_equal (tag, "style") && o->cur_style != NULL && o->in_named_styles &&
      o->cur_style->display != NULL &&
      our_style_name (o, o->cur_style->display) == NULL &&
      !odt_internal_style (o->cur_style->display) && strlen (o->cur_style->display) < 64 &&
      w42_stylesheet_size (w42_pt_stylesheet (o->pt)) < 128)
    {
      /* A named style of the file's own joins the sheet, so that the
       * document keeps it and Format > Style shows it. */
      OdtStyle *s = o->cur_style;
      W42Style st;
      W42Fmt def;

      w42_fmt_init_default (&def);
      memset (&st, 0, sizeof st);
      st.name = g_intern_string (s->display);
      st.ch = s->has_ch ? s->ch : def.ch;
      st.pa = s->has_pa ? s->pa : def.pa;
      st.pa.style = st.name;
      st.outline = s->outline;
      st.character = o->cur_style_text ? 1 : 0;
      st.pa_own = W42_STYLE_PA_ALL;    /* read resolved: all its own */
      st.ch_own = W42_STYLE_CH_ALL;
      if (s->parent != NULL)
        {
          OdtStyle *ps = g_hash_table_lookup (o->styles, s->parent);
          const char *ours = our_style_name (o, ps != NULL ? ps->display : s->parent);

          st.based_on = ours != NULL ? ours : ps != NULL && ps->display != NULL ? g_intern_string (ps->display) : NULL;
        }
      w42_stylesheet_set (w42_pt_stylesheet (o->pt), &st);
      if (!st.character)
        s->pa.style = st.name;
    }
  if (g_str_equal (tag, "style") || g_str_equal (tag, "default-style"))
    {
      o->cur_style = NULL;
      g_free (o->cur_col_style);
      o->cur_col_style = NULL;
    }
  else if (g_str_equal (tag, "list-style"))
    o->cur_list = NULL;
  else if (g_str_equal (tag, "page-layout"))
    {
      if (o->in_page_layout)
        o->page_seen = TRUE;
      o->in_page_layout = FALSE;
    }
  else if (g_str_equal (tag, "header") || g_str_equal (tag, "footer") ||
           g_str_equal (tag, "header-left") || g_str_equal (tag, "footer-left") ||
           g_str_equal (tag, "header-first") || g_str_equal (tag, "footer-first"))
    {
      if (o->hf_text->len > 0)
        {
          if (tag[0] == 'h')
            w42_pt_set_header_kind (o->pt, o->hf_kind, o->hf_text->str, o->hf_align);
          else
            w42_pt_set_footer_kind (o->pt, o->hf_kind, o->hf_text->str, o->hf_align);

          /* A file that has one says so by having it. */
          if (o->hf_kind == W42_PAGE_TEXT_FIRST)
            w42_pt_set_title_page (o->pt, TRUE);
          else if (o->hf_kind == W42_PAGE_TEXT_EVEN)
            w42_pt_set_facing_pages (o->pt, TRUE);
        }
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
  w42_builder_text (&o->b, o->text->str);
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
  if (o->cell_vspan != 1)
    w42_pt_set_cell_vspan (o->pt, o->b.cell_pos, o->cell_vspan);
}

static void
body_start (Odt *o, const char *tag, const char **an, const char **av)
{
  if (o->in_annotation)
    return;                           /* its paragraphs are the note's text */
  if (g_str_equal (tag, "h") || g_str_equal (tag, "p"))
    {
      const char *sn = attr (an, av, "text:style-name");
      OdtStyle *s = resolve_style (o, sn, 0);
      W42Fmt def;

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
      if (g_str_equal (tag, "h"))
        {
          const char *lvl = attr (an, av, "text:outline-level");
          int level = lvl != NULL ? CLAMP (atoi (lvl), 1, 9) : (s != NULL && s->outline > 0 ? s->outline : 1);

          if (level <= 3)
            {
              char *hn = g_strdup_printf ("Heading %d", level);
              const W42Style *st = w42_stylesheet_find (w42_pt_stylesheet (o->pt), hn);

              if (st != NULL)
                {
                  o->b.pa.style = st->name;
                  if (s == NULL || !s->has_ch)
                    o->para_ch = st->ch;
                }
              g_free (hn);
            }
        }
      /* In a list: the kind from the list style, the level from the nesting. */
      if (o->list_depth > 0)
        {
          const char *ls_name = o->list_style_stack->len > 0 ? g_ptr_array_index (o->list_style_stack, o->list_style_stack->len - 1) : NULL;
          OdtListStyle *ls = ls_name != NULL ? g_hash_table_lookup (o->list_styles, ls_name) : NULL;
          int level = CLAMP (o->list_depth - 1, 0, 8);

          if (ls == NULL && s != NULL && s->list_style != NULL)
            ls = g_hash_table_lookup (o->list_styles, s->list_style);
          o->b.pa.list = (guint8) (ls != NULL ? ls->kind[level] : W42_LIST_NUMBER);
          o->b.pa.list_level = (guint8) level;
          if (o->b.pa.indent_left == 0)
            o->b.pa.indent_left = 360 * (level + 1);
          if (o->b.pa.indent_first == 0)
            o->b.pa.indent_first = -360;
        }
      g_array_set_size (o->span_stack, 0);
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
      if (s != NULL && s->has_ch)
        {
          /* A text style adds to what is there. */
          if (s->ch.bold) ch.bold = 1;
          if (s->ch.italic) ch.italic = 1;
          if (s->ch.underline) ch.underline = s->ch.underline;
          if (s->ch.strikeout) ch.strikeout = 1;
          if (s->ch.overline) ch.overline = 1;
          {
            W42Fmt def;

            w42_fmt_init_default (&def);
            if (s->ch.size != def.ch.size) ch.size = s->ch.size;
            if (s->ch.color != def.ch.color) ch.color = s->ch.color;
            if (s->ch.family != def.ch.family) ch.family = s->ch.family;
          }
          if (s->ch.highlight) ch.highlight = s->ch.highlight;
          if (s->ch.script) ch.script = s->ch.script;
          if (s->ch.smallcaps) ch.smallcaps = 1;
          if (s->ch.allcaps) ch.allcaps = 1;
          if (s->ch.spacing) ch.spacing = s->ch.spacing;
          if (s->ch.lang != NULL) ch.lang = s->ch.lang;
        }
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
    }
  else if (g_str_equal (tag, "tab"))
    g_string_append_c (o->text, '\t');
  else if (g_str_equal (tag, "line-break"))
    g_string_append (o->text, "\342\200\250");
  else if (g_str_equal (tag, "soft-hyphen"))
    g_string_append (o->text, "\302\255");
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
      o->list_depth++;
      if (sn == NULL && o->list_style_stack->len > 0)
        sn = g_ptr_array_index (o->list_style_stack, o->list_style_stack->len - 1);
      g_ptr_array_add (o->list_style_stack, g_strdup (sn != NULL ? sn : ""));
    }
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
      if (anchor != NULL && !g_str_equal (anchor, "as-char") && w != NULL)
        o->frame_wrap = (W42Wrap) (GPOINTER_TO_INT (w) - 1);
      g_free (o->frame_href);
      o->frame_href = NULL;
    }
  else if (g_str_equal (tag, "text-box") && o->frame_pending && o->tb_depth == 0)
    {
      /* A text box: its paragraphs are framed at the side the frame's
       * style puts it, and the paragraph it hangs on goes on after it. */
      o->tb_depth = 1;
      o->tb_side = o->frame_wrap == W42_WRAP_RIGHT ? W42_FRAME_RIGHT : W42_FRAME_LEFT;
      o->tb_width = o->frame_w;
      o->tb_saved_pa = o->b.pa;
      o->tb_saved_ch = o->para_ch;
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
      g_free (o->frame_href);
      o->frame_href = g_strdup (attr (an, av, "xlink:href"));
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

static void
body_end (Odt *o, const char *tag)
{
  if (o->in_annotation && !g_str_equal (tag, "annotation"))
    {
      if (g_str_equal (tag, "p"))
        g_string_append_c (o->annotation, ' ');
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
      o->para_open = TRUE;
      o->tb_reopened = TRUE;
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
        }
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
          o->para_open = TRUE;
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
          GBytes *bytes = w42_zip_read (o->zip, o->frame_href);
          int pw = 0, ph = 0;
          const char *format = NULL;

          odt_flush (o);
          if (bytes != NULL && w42_image_probe (bytes, &pw, &ph, &format))
            {
              o->b.ch = current_ch (o);
              w42_builder_object (&o->b, bytes, format, pw, ph, o->frame_w, o->frame_h);
              w42_builder_object_wrap (&o->b, o->frame_wrap);
            }
          if (bytes != NULL)
            g_bytes_unref (bytes);
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
      code = *o->index_term != '\0' ? g_strconcat ("XE:", o->index_term, NULL)
                                    : g_strdup ("XE");
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
      w42_builder_text (&o->b, o->field_text->str);
      memset (&want, 0, sizeof want);
      want.field = g_intern_string (code);
      w42_pt_apply_char_fmt (o->pt, start, o->b.pos - start, W42_CHAR_FIELD, &want);
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
  if (!o->in_body || !o->para_open)
    return;
  /* ODF collapses runs of white space to one space; text:s stands for more. */
  for (gsize i = 0; i < len; i++)
    {
      char c = text[i];

      if (c == '\n' || c == '\r' || c == '\t')
        c = ' ';
      if (c == ' ' && o->text->len > 0 && o->text->str[o->text->len - 1] == ' ')
        continue;
      g_string_append_c (o->text, c);
    }
}

/* meta.xml: what the document says about itself. */
typedef struct {
  GString *text;
  char    *field;
  char    *keep[5];
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
  else if (g_str_equal (tag, "creator") ||
           g_str_equal (tag, "initial-creator")) slot = &m->keep[2];
  else if (g_str_equal (tag, "keyword"))     slot = &m->keep[3];
  else if (g_str_equal (tag, "description")) slot = &m->keep[4];
  if (slot != NULL && m->text->len > 0 && *slot == NULL)
    *slot = g_strdup (m->text->str);
  g_clear_pointer (&m->field, g_free);
  g_string_truncate (m->text, 0);
}

static void
odt_read_meta (W42Zip *zip, W42PieceTable *pt)
{
  GBytes *xml = w42_zip_read (zip, "meta.xml");
  GMarkupParser parser = { meta_start, meta_end, meta_text, NULL, NULL };
  GMarkupParseContext *ctx;
  W42DocInfo info;
  OdtMeta m;

  if (xml == NULL)
    return;
  memset (&m, 0, sizeof m);
  m.text = g_string_new (NULL);
  ctx = g_markup_parse_context_new (&parser, 0, &m, NULL);
  g_markup_parse_context_parse (ctx, g_bytes_get_data (xml, NULL), g_bytes_get_size (xml), NULL);
  g_markup_parse_context_free (ctx);

  memset (&info, 0, sizeof info);
  info.title = m.keep[0];
  info.subject = m.keep[1];
  info.author = m.keep[2];
  info.keywords = m.keep[3];
  info.comments = m.keep[4];
  w42_pt_set_info (pt, &info);

  for (guint i = 0; i < G_N_ELEMENTS (m.keep); i++)
    g_free (m.keep[i]);
  g_free (m.field);
  g_string_free (m.text, TRUE);
  g_bytes_unref (xml);
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
  odt_read_meta (zip, pt);

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
  w42_builder_init (&o.b, pt);
  o.pt = pt;
  o.page = page;
  o.zip = zip;
  o.styles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, style_free);
  o.list_styles = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  o.fonts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  o.graphic_wraps = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  o.col_widths = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  o.row_heights = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  o.cell_sides = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
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
  {
    W42Fmt def;
    w42_fmt_init_default (&def);
    o.para_ch = def.ch;
  }

  /* The named styles and the page, then the content with its own
   * automatic styles before the body. */
  if (styles != NULL)
    ok = parse_part (&o, styles, error);
  if (ok)
    ok = parse_part (&o, content, error);

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
  g_free (o.cur_row_style);
  g_hash_table_destroy (o.row_heights);
  g_free (o.cur_cell_style);
  g_hash_table_destroy (o.cell_sides);
  g_free (o.cur_style_name);
  g_free (o.cur_col_style);
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
  GPtrArray *pictures;         /* GBytes*, Pictures/imageN.png */
  int        n_tables;
  int        n_index_marks;    /* index entries written, for their ids */
  int        list_style_used[W42_LIST_KINDS];
  W42CharFmt base_ch;
  int        note_id;
  int        annotation_id;
  guint cell_styles_made;   /* per table: a bit per set of sides written */
} OdtWriter;

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

static const char *
style_id_for (const char *name)
{
  /* "Heading 1" -> "Heading_20_1", as ODF spells a space. */
  static char buf[96];
  gsize n = 0;

  for (const char *p = name != NULL ? name : "Normal"; *p && n + 5 < sizeof buf; p++)
    {
      if (*p == ' ')
        {
          memcpy (buf + n, "_20_", 4);
          n += 4;
        }
      else if (g_ascii_isalnum (*p))
        buf[n++] = *p;
    }
  buf[n] = '\0';
  if (g_str_equal (buf, "Normal"))
    g_strlcpy (buf, "Standard", sizeof buf);
  return buf;
}

static void
write_para_props_xml (GString *s, const W42ParaFmt *pa, const W42ParaFmt *base)
{
  g_string_append (s, "<style:paragraph-properties");
  if (base == NULL || pa->align != base->align)
    g_string_append_printf (s, " fo:text-align=\"%s\"",
                            pa->align == W42_ALIGN_CENTER ? "center" : pa->align == W42_ALIGN_RIGHT ? "end"
                            : pa->align == W42_ALIGN_JUSTIFY ? "justify" : "start");
  if (pa->indent_left)  { g_string_append (s, " fo:margin-left=\""); twips_out (s, pa->indent_left); g_string_append_c (s, '"'); }
  if (pa->indent_right) { g_string_append (s, " fo:margin-right=\""); twips_out (s, pa->indent_right); g_string_append_c (s, '"'); }
  if (pa->indent_first) { g_string_append (s, " fo:text-indent=\""); twips_out (s, pa->indent_first); g_string_append_c (s, '"'); }
  if (pa->space_before) { g_string_append (s, " fo:margin-top=\""); twips_out (s, pa->space_before); g_string_append_c (s, '"'); }
  if (pa->space_after)  { g_string_append (s, " fo:margin-bottom=\""); twips_out (s, pa->space_after); g_string_append_c (s, '"'); }
  if (pa->line_spacing_pct > 0 && pa->line_spacing_pct != 100)
    g_string_append_printf (s, " fo:line-height=\"%d%%\"", pa->line_spacing_pct);
  else if (pa->line_spacing > 0)
    g_string_append_printf (s, " fo:line-height=\"%dpt\"", pa->line_spacing / 20);
  if (pa->page_break_before) g_string_append (s, " fo:break-before=\"page\"");
  if (pa->keep_next)     g_string_append (s, " fo:keep-with-next=\"always\"");
  if (pa->keep_together) g_string_append (s, " fo:keep-together=\"always\"");
  if (pa->rtl)           g_string_append (s, " style:writing-mode=\"rl-tb\"");
  if (pa->border != 0)
    {
      static const char *names[4] = { "fo:border-top", "fo:border-bottom", "fo:border-left", "fo:border-right" };
      static const int bits[4] = { W42_BORDER_TOP, W42_BORDER_BOTTOM, W42_BORDER_LEFT, W42_BORDER_RIGHT };
      char buf[G_ASCII_DTOSTR_BUF_SIZE];

      for (int i = 0; i < 4; i++)
        if (pa->border & bits[i])
          g_string_append_printf (s, " %s=\"%spt solid #000000\"", names[i],
                                  g_ascii_formatd (buf, sizeof buf, "%.2f", (pa->border_width > 0 ? pa->border_width : 15) / 20.0));
      g_string_append (s, " fo:padding=\"0.02in\"");
    }
  if (pa->shading > 0)
    {
      int grey = 255 - pa->shading * 255 / 100;

      g_string_append_printf (s, " fo:background-color=\"#%02x%02x%02x\"", grey, grey, grey);
    }
  if (pa->n_tabs > 0 || pa->drop_cap > 0)
    {
      g_string_append_c (s, '>');
      if (pa->n_tabs > 0)
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
    g_string_append_printf (s, " fo:font-size=\"%dpt\"", ch->size / 2);
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
  if (ch->strikeout) g_string_append (s, " style:text-line-through-style=\"solid\"");
  if (ch->overline)  g_string_append (s, " style:text-overline-style=\"solid\"");
  if (ch->color != 0) g_string_append_printf (s, " fo:color=\"#%06x\"", ch->color);
  if (ch->highlight != 0) g_string_append_printf (s, " fo:background-color=\"#%06x\"", w42_highlight_rgb (ch->highlight));
  if (ch->script > 0) g_string_append (s, " style:text-position=\"super 58%\"");
  if (ch->script < 0) g_string_append (s, " style:text-position=\"sub 58%\"");
  if (ch->smallcaps) g_string_append (s, " fo:font-variant=\"small-caps\"");
  if (ch->allcaps)   g_string_append (s, " fo:text-transform=\"uppercase\"");
  if (ch->spacing)   g_string_append_printf (s, " fo:letter-spacing=\"%dpt\"", ch->spacing / 20);
  if (ch->lang != NULL)
    {
      /* OpenDocument keeps the language and the country apart, and says
       * a run that is not language at all with "zxx" and "none". */
      char **parts = g_strsplit (ch->lang, "-", 2);

      g_string_append_printf (s, " fo:language=\"%s\"", parts[0]);
      g_string_append_printf (s, " fo:country=\"%s\"", parts[1] != NULL ? parts[1] : "none");
      g_strfreev (parts);
    }
  g_string_append (s, "/>");
}

/* Text with ODF's white-space rules: runs of spaces as text:s, tabs and
 * line breaks as their elements. */
static void
write_odt_text (GString *out, const char *text, gsize len)
{
  gsize i = 0;

  while (i < len)
    {
      if (text[i] == '\t')
        {
          g_string_append (out, "<text:tab/>");
          i++;
        }
      else if (text[i] == ' ' && (i + 1 < len && text[i + 1] == ' '))
        {
          gsize n = 0;

          while (i < len && text[i] == ' ')
            {
              n++;
              i++;
            }
          g_string_append_printf (out, " <text:s text:c=\"%u\"/>", (unsigned) (n - 1));
        }
      else if ((guchar) text[i] == 0xE2 && i + 2 < len && (guchar) text[i + 1] == 0x80 && (guchar) text[i + 2] == 0xA8)
        {
          g_string_append (out, "<text:line-break/>");
          i += 3;
        }
      else if ((guchar) text[i] == 0xC2 && i + 1 < len && (guchar) text[i + 1] == 0xAD)
        {
          g_string_append (out, "<text:soft-hyphen/>");
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
        }
    }
}

static void write_runs (OdtWriter *w, W42PieceTable *pt, W42ApTable *aps, GPtrArray *blocks,
                        const W42Block *block, const W42CharFmt *para_ch);

static void
write_paragraph (OdtWriter *w, W42PieceTable *pt, W42ApTable *aps, GPtrArray *blocks,
                 const W42Block *block, W42StyleSheet *styles)
{
  const W42Fmt *fmt = w42_ap_table_get (aps, block->ap);
  const W42ParaFmt *pa = &fmt->pa;
  const W42Style *style = pa->style != NULL ? w42_stylesheet_find (styles, pa->style) : NULL;
  int outline = style != NULL ? style->outline : 0;
  int pidx = para_style_index (w, pa);
  const W42CharFmt *para_ch = style != NULL ? &style->ch : &w->base_ch;

  if (outline > 0)
    g_string_append_printf (w->body, "<text:h text:style-name=\"P%d\" text:outline-level=\"%d\">", pidx, outline);
  else
    g_string_append_printf (w->body, "<text:p text:style-name=\"P%d\">", pidx);
  write_runs (w, pt, aps, blocks, block, para_ch);
  g_string_append (w->body, outline > 0 ? "</text:h>" : "</text:p>");
}

static void
write_runs (OdtWriter *w, W42PieceTable *pt, W42ApTable *aps, GPtrArray *blocks,
            const W42Block *block, const W42CharFmt *para_ch)
{
  const char *open_link = NULL;
  const char *open_bookmark = NULL;
  const char *open_comment = NULL;
  int comment_id = 0;

  for (guint i = 0; i < block->runs->len; i++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, i);
      const W42CharFmt *ch = &w42_ap_table_get (aps, run->ap)->ch;
      W42CharFmt plain;

      if (open_link != NULL && ch->link != open_link)
        {
          g_string_append (w->body, "</text:a>");
          open_link = NULL;
        }
      if (open_bookmark != NULL && ch->bookmark != open_bookmark)
        {
          g_string_append (w->body, "<text:bookmark-end text:name=\"");
          xml_escape (w->body, open_bookmark, strlen (open_bookmark));
          g_string_append (w->body, "\"/>");
          open_bookmark = NULL;
        }
      if (open_comment != NULL && ch->comment != open_comment)
        {
          g_string_append_printf (w->body, "<office:annotation-end office:name=\"w42c%d\"/>", comment_id);
          open_comment = NULL;
        }
      if (ch->bookmark != NULL && open_bookmark == NULL)
        {
          g_string_append (w->body, "<text:bookmark-start text:name=\"");
          xml_escape (w->body, ch->bookmark, strlen (ch->bookmark));
          g_string_append (w->body, "\"/>");
          open_bookmark = ch->bookmark;
        }
      if (ch->comment != NULL && open_comment == NULL)
        {
          comment_id = ++w->annotation_id;
          g_string_append_printf (w->body, "<office:annotation office:name=\"w42c%d\"><dc:creator>", comment_id);
          {
            const char *who = w42_pt_get_author (pt);

            xml_escape (w->body, who != NULL ? who : "Word42", strlen (who != NULL ? who : "Word42"));
          }
          g_string_append (w->body, "</dc:creator><text:p>");
          xml_escape (w->body, ch->comment, strlen (ch->comment));
          g_string_append (w->body, "</text:p></office:annotation>");
          open_comment = ch->comment;
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
          GBytes *png = object != NULL ? w42_image_to_png (object->data) : NULL;

          if (png != NULL)
            {
              g_ptr_array_add (w->pictures, png);
              if (object->wrap == W42_WRAP_INLINE)
                g_string_append_printf (w->body, "<draw:frame draw:name=\"Picture %u\" text:anchor-type=\"as-char\" svg:width=\"",
                                        w->pictures->len);
              else
                g_string_append_printf (w->body, "<draw:frame draw:name=\"Picture %u\" draw:style-name=\"%s\" text:anchor-type=\"paragraph\" svg:width=\"",
                                        w->pictures->len, object->wrap == W42_WRAP_LEFT ? "frL" : "frR");
              twips_out (w->body, object->width);
              g_string_append (w->body, "\" svg:height=\"");
              twips_out (w->body, object->height);
              g_string_append_printf (w->body, "\"><draw:image xlink:href=\"Pictures/image%u.png\" xlink:type=\"simple\" xlink:show=\"embed\" xlink:actuate=\"onLoad\"/></draw:frame>",
                                      w->pictures->len);
            }
          continue;
        }
      if (run->footnote > 0)
        {
          int id = ++w->note_id;

          g_string_append_printf (w->body, "<text:note text:id=\"w42n%d\" text:note-class=\"%s\"><text:note-citation>%d</text:note-citation><text:note-body>",
                                  id, run->endnote ? "endnote" : "footnote", run->footnote);
          for (guint b = 0; b < blocks->len; b++)
            {
              const W42Block *nb = g_ptr_array_index (blocks, b);

              if (nb->note != run->footnote_id)
                continue;
              g_string_append_printf (w->body, "<text:p text:style-name=\"P%d\">",
                                      para_style_index (w, &w42_ap_table_get (aps, nb->ap)->pa));
              write_runs (w, pt, aps, blocks, nb, para_ch);
              g_string_append (w->body, "</text:p>");
            }
          g_string_append (w->body, "</text:note-body></text:note>");
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
          xml_escape (w->body, block->text->str + run->byte_offset, run->n_bytes);
          g_string_append_printf (w->body,
            "<text:alphabetical-index-mark-end text:id=\"IMark%d\"/>", id);
          g_free (term);
          continue;
        }

      /* The field elements carry their result as content. */
      if (ch->field != NULL)
        {
          const char *el = g_str_equal (ch->field, "PAGE") ? "page-number" : g_str_equal (ch->field, "NUMPAGES") ? "page-count"
                         : g_str_equal (ch->field, "DATE") ? "date" : g_str_equal (ch->field, "TIME") ? "time"
                         : g_str_equal (ch->field, "FILENAME") ? "file-name" : "word-count";

          g_string_append_printf (w->body, "<text:%s>", el);
          xml_escape (w->body, block->text->str + run->byte_offset, run->n_bytes);
          g_string_append_printf (w->body, "</text:%s>", el);
          continue;
        }

      /* A span only where the run differs from the paragraph's text. */
      plain = *ch;
      plain.link = NULL; plain.bookmark = NULL; plain.comment = NULL; plain.field = NULL;
      plain.revision = 0;
      {
        W42CharFmt base = *para_ch;

        base.link = NULL; base.bookmark = NULL; base.comment = NULL; base.field = NULL; base.revision = 0;
        if (memcmp (&plain, &base, sizeof plain) != 0)
          {
            g_string_append_printf (w->body, "<text:span text:style-name=\"T%d\">", text_style_index (w, &plain));
            write_odt_text (w->body, block->text->str + run->byte_offset, run->n_bytes);
            g_string_append (w->body, "</text:span>");
          }
        else
          write_odt_text (w->body, block->text->str + run->byte_offset, run->n_bytes);
      }
    }
  if (open_link != NULL)
    g_string_append (w->body, "</text:a>");
  if (open_bookmark != NULL)
    {
      g_string_append (w->body, "<text:bookmark-end text:name=\"");
      xml_escape (w->body, open_bookmark, strlen (open_bookmark));
      g_string_append (w->body, "\"/>");
    }
  if (open_comment != NULL)
    g_string_append_printf (w->body, "<office:annotation-end office:name=\"w42c%d\"/>", comment_id);
}

static void
write_page_text (GString *s, const W42PageText *text, int style_idx)
{
  const char *p = text->text;

  g_string_append_printf (s, "<text:p text:style-name=\"MP%d\">", style_idx);
  while (*p)
    {
      const char *brace = strchr (p, '{');
      const char *close = brace != NULL ? strchr (brace, '}') : NULL;

      if (brace == NULL || close == NULL)
        {
          write_odt_text (s, p, strlen (p));
          break;
        }
      write_odt_text (s, p, brace - p);
      if (g_ascii_strncasecmp (brace, "{PAGE}", 6) == 0)
        g_string_append (s, "<text:page-number text:select-page=\"current\">1</text:page-number>");
      else if (g_ascii_strncasecmp (brace, "{NUMPAGES}", 10) == 0)
        g_string_append (s, "<text:page-count>1</text:page-count>");
      else if (g_ascii_strncasecmp (brace, "{DATE}", 6) == 0)
        g_string_append (s, "<text:date/>");
      else
        write_odt_text (s, brace, close - brace + 1);
      p = close + 1;
    }
  g_string_append (s, "</text:p>");
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
  int list_depth = 0;
  W42ListKind list_stack[9];
  int table_open = -1, row_open = -1;
  gboolean cell_covered = FALSE;      /* the open cell is one a merge swallowed */
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
      int want_depth;

      if (block->note >= 0)
        continue;

      /* Lists nest by level. */
      want_depth = pa->list != W42_LIST_NONE && block->table < 0 ? MIN (pa->list_level, 8) + 1 : 0;
      while (list_depth > want_depth ||
             (list_depth > 0 && list_depth == want_depth && list_stack[list_depth - 1] != pa->list))
        {
          list_depth--;
          g_string_append (w.body, "</text:list-item></text:list>");
        }
      while (list_depth < want_depth)
        {
          if (list_depth == 0)
            {
              g_string_append_printf (w.body, "<text:list text:style-name=\"L%d\">", (int) pa->list);
              w.list_style_used[pa->list] = 1;
            }
          else
            g_string_append (w.body, "<text:list>");
          g_string_append (w.body, "<text:list-item>");
          list_stack[list_depth++] = pa->list;
        }
      if (want_depth > 0 && list_depth == want_depth && b > 0)
        {
          /* Another item at this level: the item before it closes. */
          const W42Block *prev = g_ptr_array_index (blocks, b - 1);
          const W42ParaFmt *ppa = &w42_ap_table_get (aps, prev->ap)->pa;

          if (ppa->list != W42_LIST_NONE && MIN (ppa->list_level, 8) + 1 == want_depth &&
              g_str_has_suffix (w.body->str, "</text:p>"))
            g_string_append (w.body, "</text:list-item><text:list-item>");
        }

      /* Tables. */
      if (block->table >= 0 && block->table != table_open)
        {
          const W42TableProps *tp = w42_pt_table_props (pt, block->table);
          int n_cols = tp != NULL ? tp->n_cols : 1;
          int t = ++w.n_tables;

          w.cell_styles_made = 0;

          g_string_append_printf (w.auto_styles, "<style:style style:name=\"Table%d\" style:family=\"table\"><style:table-properties style:width=\"", t);
          twips_out (w.auto_styles, pg.width - pg.margin_left - pg.margin_right);
          g_string_append (w.auto_styles, "\" table:align=\"left\"/></style:style>");
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

                if (cpa->border & W42_BORDER_CELL_SET)
                  {
                    /* A style of the cell's own, one per set of sides. */
                    static const char *names[4] = { "fo:border-top", "fo:border-bottom", "fo:border-left", "fo:border-right" };
                    static const int bits[4] = { W42_BORDER_TOP, W42_BORDER_BOTTOM, W42_BORDER_LEFT, W42_BORDER_RIGHT };
                    int sides = cpa->border & W42_BORDER_BOX;

                    if (!(w.cell_styles_made & (1 << sides)))
                      {
                        w.cell_styles_made |= (1 << sides);
                        g_string_append_printf (w.auto_styles, "<style:style style:name=\"Table%d.Cell%d\" style:family=\"table-cell\"><style:table-cell-properties fo:padding=\"0.03in\"",
                                                w.n_tables, sides);
                        for (int k = 0; k < 4; k++)
                          g_string_append_printf (w.auto_styles, " %s=\"%s\"", names[k], (sides & bits[k]) ? "0.5pt solid #000000" : "none");
                        g_string_append (w.auto_styles, "/></style:style>");
                      }
                    g_string_append_printf (w.body, "<table:table-cell table:style-name=\"Table%d.Cell%d\" office:value-type=\"string\"", w.n_tables, sides);
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
          write_paragraph (&w, pt, aps, blocks, block, styles);
          if (cell_end)
            {
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
        write_paragraph (&w, pt, aps, blocks, block, styles);
        if (framed && !next_same)
          g_string_append (w.body, "</draw:text-box></draw:frame></text:p>");
      }
    }
  while (list_depth > 0)
    {
      list_depth--;
      g_string_append (w.body, "</text:list-item></text:list>");
    }

  /* content.xml: the automatic styles that the body referred to. */
  content = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<office:document-content " ODT_NS ">");
  g_string_append (content, "<office:automatic-styles>");
  for (guint i = 0; i < w.pa_keys->len; i++)
    {
      const W42ParaFmt *pa = g_ptr_array_index (w.pa_keys, i);
      const W42Style *style = pa->style != NULL ? w42_stylesheet_find (styles, pa->style) : NULL;

      g_string_append_printf (content, "<style:style style:name=\"P%u\" style:family=\"paragraph\" style:parent-style-name=\"%s\"",
                              i + 1, style_id_for (pa->style));
      if (pa->list != W42_LIST_NONE)
        g_string_append_printf (content, " style:list-style-name=\"L%d\"", (int) pa->list);
      g_string_append (content, ">");
      write_para_props_xml (content, pa, style != NULL ? &style->pa : NULL);
      g_string_append (content, "</style:style>");
    }
  for (guint i = 0; i < w.ch_keys->len; i++)
    {
      g_string_append_printf (content, "<style:style style:name=\"T%u\" style:family=\"text\">", i + 1);
      write_text_props_xml (content, g_ptr_array_index (w.ch_keys, i), &w.base_ch);
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

          w42_list_marker ((W42ListKind) k, 1, marker, sizeof marker);
          if (w42_list_is_bullet ((W42ListKind) k))
            g_string_append_printf (content, "<text:list-level-style-bullet text:level=\"%d\" text:bullet-char=\"%s\">", lv, marker);
          else
            g_string_append_printf (content, "<text:list-level-style-number text:level=\"%d\" style:num-suffix=\".\" style:num-format=\"%s\">", lv,
                                    k == W42_LIST_LOWER_LETTER ? "a" : k == W42_LIST_UPPER_LETTER ? "A"
                                    : k == W42_LIST_LOWER_ROMAN ? "i" : k == W42_LIST_UPPER_ROMAN ? "I" : "1");
          g_string_append_printf (content, "<style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\">"
                                  "<style:list-level-label-alignment text:label-followed-by=\"listtab\" text:list-tab-stop-position=\"%.2fin\" fo:text-indent=\"-0.25in\" fo:margin-left=\"%.2fin\"/>"
                                  "</style:list-level-properties>%s",
                                  0.25 * lv, 0.25 * lv,
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
    "style:vertical-pos=\"top\" style:vertical-rel=\"paragraph\" fo:margin-left=\"0.125in\" fo:margin-bottom=\"0.125in\"/></style:style>");
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
  write_text_props_xml (stylesxml, &w.base_ch, NULL);
  g_string_append (stylesxml, "</style:default-style>");
  for (guint i = 0; i < w42_stylesheet_size (styles); i++)
    {
      const W42Style *s = w42_stylesheet_get (styles, i);
      gboolean is_normal = g_ascii_strcasecmp (s->name, "Normal") == 0;

      g_string_append_printf (stylesxml, "<style:style style:name=\"%s\" style:display-name=\"", style_id_for (s->name));
      xml_escape (stylesxml, is_normal ? "Standard" : s->name, strlen (is_normal ? "Standard" : s->name));
      g_string_append_printf (stylesxml, "\" style:family=\"%s\"", s->character ? "text" : "paragraph");
      if (s->based_on != NULL && w42_stylesheet_find (styles, s->based_on) != NULL)
        g_string_append_printf (stylesxml, " style:parent-style-name=\"%s\"",
                                g_ascii_strcasecmp (s->based_on, "Normal") == 0 ? "Standard" : style_id_for (s->based_on));
      else if (!is_normal && !s->character)
        g_string_append (stylesxml, " style:parent-style-name=\"Standard\"");
      if (!is_normal && !s->character)
        g_string_append (stylesxml, " style:next-style-name=\"Standard\"");
      if (s->outline > 0 && !s->character)
        g_string_append_printf (stylesxml, " style:default-outline-level=\"%d\"", s->outline);
      g_string_append (stylesxml, ">");
      if (!s->character)
        write_para_props_xml (stylesxml, &s->pa, NULL);
      write_text_props_xml (stylesxml, &s->ch, NULL);
      g_string_append (stylesxml, "</style:style>");
    }
  g_string_append (stylesxml, "</office:styles><office:automatic-styles>");
  g_string_append (stylesxml, "<style:page-layout style:name=\"Mpm1\"><style:page-layout-properties fo:page-width=\"");
  twips_out (stylesxml, pg.width);
  g_string_append (stylesxml, "\" fo:page-height=\"");
  twips_out (stylesxml, pg.height);
  g_string_append_printf (stylesxml, "\" style:print-orientation=\"%s\" fo:margin-top=\"", pg.width > pg.height ? "landscape" : "portrait");
  twips_out (stylesxml, pg.margin_top);
  g_string_append (stylesxml, "\" fo:margin-bottom=\"");
  twips_out (stylesxml, pg.margin_bottom);
  g_string_append (stylesxml, "\" fo:margin-left=\"");
  twips_out (stylesxml, pg.margin_left);
  g_string_append (stylesxml, "\" fo:margin-right=\"");
  twips_out (stylesxml, pg.margin_right);
  g_string_append (stylesxml, "\"");
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
  g_string_append (stylesxml, "</style:page-layout>");
  if (header != NULL && header->text != NULL && *header->text)
    {
      g_string_append (stylesxml, "<style:style style:name=\"MP1\" style:family=\"paragraph\">");
      g_string_append_printf (stylesxml, "<style:paragraph-properties fo:text-align=\"%s\"/></style:style>",
                              header->align == W42_ALIGN_CENTER ? "center" : header->align == W42_ALIGN_RIGHT ? "end" : "start");
    }
  if (footer != NULL && footer->text != NULL && *footer->text)
    {
      g_string_append (stylesxml, "<style:style style:name=\"MP2\" style:family=\"paragraph\">");
      g_string_append_printf (stylesxml, "<style:paragraph-properties fo:text-align=\"%s\"/></style:style>",
                              footer->align == W42_ALIGN_CENTER ? "center" : footer->align == W42_ALIGN_RIGHT ? "end" : "start");
    }
  g_string_append (stylesxml, "</office:automatic-styles><office:master-styles><style:master-page style:name=\"Standard\" style:page-layout-name=\"Mpm1\">");
  if (header != NULL && header->text != NULL && *header->text)
    {
      g_string_append (stylesxml, "<style:header>");
      write_page_text (stylesxml, header, 1);
      g_string_append (stylesxml, "</style:header>");
    }
  if (footer != NULL && footer->text != NULL && *footer->text)
    {
      g_string_append (stylesxml, "<style:footer>");
      write_page_text (stylesxml, footer, 2);
      g_string_append (stylesxml, "</style:footer>");
    }
  {
    /* Even pages and a title page may have their own; OpenDocument calls
     * the even ones "left" and takes them from the master page. */
    const W42PageText *even_h = w42_pt_get_facing_pages (pt) ? w42_pt_get_header_kind (pt, W42_PAGE_TEXT_EVEN) : NULL;
    const W42PageText *even_f = w42_pt_get_facing_pages (pt) ? w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_EVEN) : NULL;
    const W42PageText *first_h = w42_pt_get_title_page (pt) ? w42_pt_get_header_kind (pt, W42_PAGE_TEXT_FIRST) : NULL;
    const W42PageText *first_f = w42_pt_get_title_page (pt) ? w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_FIRST) : NULL;

    if (even_h != NULL && even_h->text != NULL && *even_h->text)
      {
        g_string_append (stylesxml, "<style:header-left>");
        write_page_text (stylesxml, even_h, 1);
        g_string_append (stylesxml, "</style:header-left>");
      }
    if (even_f != NULL && even_f->text != NULL && *even_f->text)
      {
        g_string_append (stylesxml, "<style:footer-left>");
        write_page_text (stylesxml, even_f, 2);
        g_string_append (stylesxml, "</style:footer-left>");
      }
    if (first_h != NULL && first_h->text != NULL && *first_h->text)
      {
        g_string_append (stylesxml, "<style:header-first>");
        write_page_text (stylesxml, first_h, 1);
        g_string_append (stylesxml, "</style:header-first>");
      }
    if (first_f != NULL && first_f->text != NULL && *first_f->text)
      {
        g_string_append (stylesxml, "<style:footer-first>");
        write_page_text (stylesxml, first_f, 2);
        g_string_append (stylesxml, "</style:footer-first>");
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
    g_string_append_printf (manifest, "<manifest:file-entry manifest:full-path=\"Pictures/image%u.png\" manifest:media-type=\"image/png\"/>", i + 1);
  g_string_append (manifest, "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>");
  g_string_append (manifest, "</manifest:manifest>");

  zip = w42_zip_writer_new ();
  w42_zip_writer_add (zip, "mimetype", ODT_MIME, strlen (ODT_MIME));
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
      char *name = g_strdup_printf ("Pictures/image%u.png", i + 1);
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
  g_ptr_array_free (blocks, TRUE);
  return ok;
}
