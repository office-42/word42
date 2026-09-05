/* w42-docx.c - see w42-docx.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-docx.h"

#include <string.h>
#include <stdlib.h>

#include "w42-build.h"
#include "w42-image.h"
#include "w42-lang.h"
#include "w42-zip.h"

#define EMU_PER_TWIP 635

/* Word's highlight names, by the index RTF and .doc use. */
static const char * const HIGHLIGHT_NAMES[17] = {
  NULL, "black", "blue", "cyan", "green", "magenta", "red", "yellow", "white",
  "darkBlue", "darkCyan", "darkGreen", "darkMagenta", "darkRed", "darkYellow",
  "darkGray", "lightGray"
};

/* Which of Word's sixteen highlights a colour is nearest to: LibreOffice
 * writes a run's highlight as a shading colour, and this gives it a name.
 * White, and no colour at all, mean no highlight. */
static int
nearest_highlight (const char *hex)
{
  guint32 want;
  int best = 0;
  long best_away = 0;

  if (hex == NULL || strlen (hex) < 6)
    return 0;
  want = (guint32) g_ascii_strtoull (hex[0] == '#' ? hex + 1 : hex, NULL, 16);
  if (want == 0xFFFFFF)
    return 0;

  for (int i = 1; i <= 16; i++)
    {
      guint32 c = w42_highlight_rgb (i);
      long dr = (long) ((c >> 16) & 0xFF) - (long) ((want >> 16) & 0xFF);
      long dg = (long) ((c >> 8) & 0xFF) - (long) ((want >> 8) & 0xFF);
      long db = (long) (c & 0xFF) - (long) (want & 0xFF);
      long away = dr * dr + dg * dg + db * db;

      if (best == 0 || away < best_away)
        {
          best = i;
          best_away = away;
        }
    }
  return best;
}

/* Word stores a paragraph's alignment relative to its direction: in a
 * right-to-left paragraph "left" means the start of the line, which is
 * the right-hand margin.  Word42 keeps alignment as what the eye sees,
 * so the two are turned round on the way in and on the way out. */
static W42Align
mirror_align (W42Align align, gboolean rtl)
{
  if (!rtl)
    return align;
  if (align == W42_ALIGN_LEFT)
    return W42_ALIGN_RIGHT;
  if (align == W42_ALIGN_RIGHT)
    return W42_ALIGN_LEFT;
  return align;
}

/* Word's names for the kinds of underline, and ours. */
static guint
underline_from_val (const char *v)
{
  if (v == NULL || g_str_equal (v, "none"))            return W42_UNDERLINE_NONE;
  if (g_str_equal (v, "double"))                       return W42_UNDERLINE_DOUBLE;
  if (g_str_equal (v, "words"))                        return W42_UNDERLINE_WORDS;
  if (g_str_has_prefix (v, "dotted"))                  return W42_UNDERLINE_DOTTED;
  if (g_str_has_prefix (v, "dash") || g_str_has_prefix (v, "dotDash"))
                                                       return W42_UNDERLINE_DASHED;
  if (g_str_has_prefix (v, "thick") || g_str_has_prefix (v, "wavyHeavy"))
                                                       return W42_UNDERLINE_THICK;
  if (g_str_has_prefix (v, "wave"))                    return W42_UNDERLINE_WAVE;
  return W42_UNDERLINE_SINGLE;
}

static const char *
underline_val (guint kind)
{
  switch (kind)
    {
    case W42_UNDERLINE_NONE:   return NULL;
    case W42_UNDERLINE_DOUBLE: return "double";
    case W42_UNDERLINE_WORDS:  return "words";
    case W42_UNDERLINE_DOTTED: return "dotted";
    case W42_UNDERLINE_DASHED: return "dash";
    case W42_UNDERLINE_THICK:  return "thick";
    case W42_UNDERLINE_WAVE:   return "wave";
    default:                   return "single";
    }
}

static int
highlight_index (const char *name)
{
  for (int i = 1; i < 17; i++)
    if (g_ascii_strcasecmp (HIGHLIGHT_NAMES[i], name) == 0)
      return i;
  return 7;
}

/* The tag without its namespace prefix: "w:p" -> "p". */
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
    if (g_str_equal (local (names[i]), want))
      return values[i];
  return NULL;
}

static int attr_int (const char **names, const char **values, const char *want, int fallback);

/* A w:top, w:bottom, w:insideH... border element: its val, sz and
 * color into `edge`.  TRUE for a line, FALSE for nil or none. */
static gboolean
border_element (const char **an, const char **av, W42BorderEdge *edge)
{
  const char *val = attr (an, av, "val");
  const char *colour = attr (an, av, "color");
  int sz = attr_int (an, av, "sz", 4);

  if (val == NULL || g_str_equal (val, "nil") || g_str_equal (val, "none"))
    {
      edge->style = W42_BORDER_NONE;
      edge->width = 0;
      edge->color = 0;
      return FALSE;
    }
  edge->style = strstr (val, "ouble") != NULL || g_str_equal (val, "triple") ||
                g_str_has_prefix (val, "thinThick") || g_str_has_prefix (val, "thickThin")
                  ? W42_BORDER_DOUBLE
              : g_str_has_prefix (val, "dash") || g_str_has_prefix (val, "dotDash") ||
                g_str_has_prefix (val, "dotDotDash") ? W42_BORDER_DASHED
              : g_str_equal (val, "dotted") ? W42_BORDER_DOTTED : W42_BORDER_SINGLE;
  edge->width = (guint8) CLAMP (sz * 20 / 8, 5, 255);   /* eighths of a point */
  edge->color = (colour != NULL && !g_str_equal (colour, "auto"))
                  ? (guint32) strtoul (colour, NULL, 16) & 0xFFFFFF : 0;
  return TRUE;
}

/* The edge a border element's tag names, by W42_EDGE_*; -1 for none. */
static int
border_edge_index (const char *tag)
{
  if (g_str_equal (tag, "top"))     return W42_EDGE_TOP;
  if (g_str_equal (tag, "bottom"))  return W42_EDGE_BOTTOM;
  if (g_str_equal (tag, "left") || g_str_equal (tag, "start")) return W42_EDGE_LEFT;
  if (g_str_equal (tag, "right") || g_str_equal (tag, "end"))  return W42_EDGE_RIGHT;
  if (g_str_equal (tag, "insideH")) return W42_EDGE_INSIDE_H;
  if (g_str_equal (tag, "insideV")) return W42_EDGE_INSIDE_V;
  return -1;
}

/* One border element -- w:top, w:insideH... -- for `edge`, or a nil one
 * when the side is off.  `space` is the gap to the text in points. */
static void
write_border_element (GString *out, int which, const W42BorderEdge *edge, gboolean on, int space)
{
  static const char *names[W42_N_EDGES] = { "top", "bottom", "left", "right", "insideH", "insideV" };
  const char *val;
  int sz;

  if (!on || edge == NULL || edge->style == W42_BORDER_NONE)
    {
      g_string_append_printf (out, "<w:%s w:val=\"nil\"/>", names[which]);
      return;
    }
  val = edge->style == W42_BORDER_DOUBLE ? "double"
      : edge->style == W42_BORDER_DASHED ? "dashed"
      : edge->style == W42_BORDER_DOTTED ? "dotted" : "single";
  sz = MAX (W42_EDGE_WIDTH (edge) * 8 / 20, 2);
  g_string_append_printf (out, "<w:%s w:val=\"%s\" w:sz=\"%d\" w:space=\"%d\" w:color=\"%06X\"/>",
                          names[which], val, sz, space, edge->color & 0xFFFFFF);
}

static int
attr_int (const char **names, const char **values, const char *want, int fallback)
{
  const char *v = attr (names, values, want);

  return v != NULL ? atoi (v) : fallback;
}

/* w:b, w:i and the like are on unless they say w:val="0" or "false". */
static gboolean
toggle_on (const char **names, const char **values)
{
  const char *v = attr (names, values, "val");

  return v == NULL || !(g_str_equal (v, "0") || g_str_equal (v, "false") ||
                        g_str_equal (v, "off") || g_str_equal (v, "none"));
}

/* ====================================================================== */
/* Reading                                                                 */
/* ====================================================================== */

/* ---- small parsers for the side parts ---------------------------------- */

typedef struct {
  GHashTable *map;     /* rId -> target */
} Rels;

static void
rels_start (GMarkupParseContext *ctx, const char *name, const char **an,
            const char **av, gpointer data, GError **error)
{
  Rels *r = data;

  (void) ctx; (void) error;
  if (g_str_equal (local (name), "Relationship"))
    {
      const char *id = attr (an, av, "Id");
      const char *target = attr (an, av, "Target");

      if (id != NULL && target != NULL)
        g_hash_table_insert (r->map, g_strdup (id), g_strdup (target));
    }
}

static GHashTable *
read_rels (W42Zip *zip, const char *part)
{
  GHashTable *map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  GBytes *xml = w42_zip_read (zip, part);

  if (xml != NULL)
    {
      GMarkupParser parser = { rels_start, NULL, NULL, NULL, NULL };
      Rels r = { map };
      GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &r, NULL);

      g_markup_parse_context_parse (ctx, g_bytes_get_data (xml, NULL), g_bytes_get_size (xml), NULL);
      g_markup_parse_context_free (ctx);
      g_bytes_unref (xml);
    }
  return map;
}

/* styles.xml: styleId -> the word42 style it stands for (interned name),
 * by the style's display name. */
typedef struct {
  GHashTable    *map;
  W42StyleSheet *sheet;
  char          *current_id;
  /* A style of the file's own, being read: it joins the sheet when its
   * element closes. */
  W42Style       cur;
  gboolean       cur_new;        /* not one of ours: to be added */
  gboolean       cur_paragraph, cur_character;
  char          *cur_based;      /* the basedOn id */
  GHashTable    *display;        /* every style id -> its name */
  GPtrArray     *pending_based;  /* char*: "name\tbased-id", resolved at the end */
} Styles;

static void
styles_start (GMarkupParseContext *ctx, const char *name, const char **an,
              const char **av, gpointer data, GError **error)
{
  Styles *s = data;
  const char *tag = local (name);

  (void) ctx; (void) error;
  if (g_str_equal (tag, "style"))
    {
      const char *type = attr (an, av, "type");
      W42Fmt def;

      g_free (s->current_id);
      s->current_id = g_strdup (attr (an, av, "styleId"));
      w42_fmt_init_default (&def);
      memset (&s->cur, 0, sizeof s->cur);
      s->cur.ch = def.ch;
      s->cur.pa = def.pa;
      s->cur_new = FALSE;
      s->cur_paragraph = type == NULL || g_str_equal (type, "paragraph");
      s->cur_character = type != NULL && g_str_equal (type, "character");
      g_free (s->cur_based);
      s->cur_based = NULL;
    }
  else if (g_str_equal (tag, "name") && s->current_id != NULL)
    {
      const char *val = attr (an, av, "val");
      const char *ours = NULL;

      if (val != NULL)
        {
          for (guint i = 0; i < w42_stylesheet_size (s->sheet); i++)
            {
              const W42Style *style = w42_stylesheet_get (s->sheet, i);

              if (g_ascii_strcasecmp (style->name, val) == 0)
                ours = style->name;
            }
          g_hash_table_insert (s->display, g_strdup (s->current_id), g_strdup (val));
        }
      if (ours != NULL)
        g_hash_table_insert (s->map, g_strdup (s->current_id), (gpointer) ours);
      else if (val != NULL && *val != '\0' && (s->cur_paragraph || s->cur_character) &&
               !g_str_has_prefix (val, "toc ") && !g_str_has_prefix (val, "TOC ") &&
               strlen (val) < 64 &&
               /* Every added style is looked up by a scan of all of them,
                * so a file of millions would cost their square. */
               w42_stylesheet_size (s->sheet) < 512)
        {
          /* The file's own style: read on, and add it when it closes. */
          s->cur.name = g_intern_string (val);
          s->cur.pa.style = s->cur.name;
          s->cur.character = s->cur_character ? 1 : 0;
          s->cur_new = TRUE;
          g_hash_table_insert (s->map, g_strdup (s->current_id), (gpointer) s->cur.name);
        }
    }
  else if (s->cur_new && g_str_equal (tag, "basedOn"))
    {
      g_free (s->cur_based);
      s->cur_based = g_strdup (attr (an, av, "val"));
    }
  else if (s->cur_new && g_str_equal (tag, "rFonts"))
    {
      const char *f = attr (an, av, "ascii");

      if (f == NULL)
        f = attr (an, av, "hAnsi");
      if (f != NULL && *f != '\0')
        {
          s->cur.ch.family = g_intern_string (f);
          s->cur.ch_own |= W42_STYLE_CH_FAMILY;
        }
    }
  else if (s->cur_new && g_str_equal (tag, "sz"))
    {
      s->cur.ch.size = CLAMP (attr_int (an, av, "val", s->cur.ch.size), 2, 3276);
      s->cur.ch_own |= W42_STYLE_CH_SIZE;
    }
  else if (s->cur_new && g_str_equal (tag, "b"))
    {
      s->cur.ch.bold = toggle_on (an, av);
      s->cur.ch_own |= W42_STYLE_CH_BOLD;
    }
  else if (s->cur_new && g_str_equal (tag, "i"))
    {
      s->cur.ch.italic = toggle_on (an, av);
      s->cur.ch_own |= W42_STYLE_CH_ITALIC;
    }
  else if (s->cur_new && g_str_equal (tag, "u"))
    {
      const char *v = attr (an, av, "val");

      s->cur.ch.underline = v == NULL || !g_str_equal (v, "none");
      s->cur.ch_own |= W42_STYLE_CH_UNDERLINE;
    }
  else if (s->cur_new && g_str_equal (tag, "color"))
    {
      const char *v = attr (an, av, "val");

      if (v != NULL && strlen (v) == 6 && !g_str_equal (v, "auto"))
        {
          s->cur.ch.color = (guint32) g_ascii_strtoull (v, NULL, 16);
          s->cur.ch_own |= W42_STYLE_CH_COLOR;
        }
    }
  else if (s->cur_new && g_str_equal (tag, "jc"))
    {
      const char *v = attr (an, av, "val");

      if (v == NULL) ;
      else if (g_str_equal (v, "center")) s->cur.pa.align = W42_ALIGN_CENTER;
      else if (g_str_equal (v, "right") || g_str_equal (v, "end")) s->cur.pa.align = W42_ALIGN_RIGHT;
      else if (g_str_equal (v, "both")) s->cur.pa.align = W42_ALIGN_JUSTIFY;
      if (v != NULL)
        s->cur.pa_own |= W42_STYLE_PA_ALIGN;
    }
  else if (s->cur_new && g_str_equal (tag, "spacing"))
    {
      s->cur.pa.space_before = CLAMP (attr_int (an, av, "before", s->cur.pa.space_before), 0, 31680);
      s->cur.pa.space_after = CLAMP (attr_int (an, av, "after", s->cur.pa.space_after), 0, 31680);
      s->cur.pa_own |= W42_STYLE_PA_SPACE_BEFORE | W42_STYLE_PA_SPACE_AFTER;
    }
  else if (s->cur_new && g_str_equal (tag, "outlineLvl"))
    s->cur.outline = CLAMP (attr_int (an, av, "val", 0) + 1, 1, 9);
  else if (s->cur_new && g_str_equal (tag, "ind"))
    {
      const char *hanging = attr (an, av, "hanging");

      s->cur.pa.indent_left = CLAMP (attr_int (an, av, "left", s->cur.pa.indent_left), -31680, 31680);
      s->cur.pa.indent_right = CLAMP (attr_int (an, av, "right", s->cur.pa.indent_right), -31680, 31680);
      s->cur.pa_own |= W42_STYLE_PA_INDENT_LEFT | W42_STYLE_PA_INDENT_RIGHT | W42_STYLE_PA_INDENT_FIRST;
      if (hanging != NULL)
        s->cur.pa.indent_first = -CLAMP (atoi (hanging), 0, 31680);
      else
        s->cur.pa.indent_first = CLAMP (attr_int (an, av, "firstLine", s->cur.pa.indent_first), -31680, 31680);
    }
}

static void
styles_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  Styles *s = data;

  (void) ctx; (void) error;
  if (g_str_equal (local (name), "style") && s->cur_new)
    {
      w42_stylesheet_set (s->sheet, &s->cur);
      if (s->cur_based != NULL)
        g_ptr_array_add (s->pending_based, g_strdup_printf ("%s\t%s", s->cur.name, s->cur_based));
      s->cur_new = FALSE;
    }
}

/* docProps/core.xml: what the document says about itself. */
typedef struct {
  W42DocInfo info;
  GString   *text;
  char      *field;      /* the tag being read */
  char      *keep[5];    /* the strings until they are interned */
} CoreProps;

static void
core_start (GMarkupParseContext *ctx, const char *name, const char **an,
            const char **av, gpointer data, GError **error)
{
  CoreProps *c = data;

  (void) ctx; (void) an; (void) av; (void) error;
  g_free (c->field);
  c->field = g_strdup (name);
  g_string_truncate (c->text, 0);
}

static void
core_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  CoreProps *c = data;

  (void) ctx; (void) error;
  if (c->field != NULL)
    g_string_append_len (c->text, text, len);
}

static void
core_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  CoreProps *c = data;
  const char *tag = local (name);
  char **slot = NULL;

  (void) ctx; (void) error;
  if (g_str_equal (tag, "title"))            slot = &c->keep[0];
  else if (g_str_equal (tag, "subject"))     slot = &c->keep[1];
  else if (g_str_equal (tag, "creator"))     slot = &c->keep[2];
  else if (g_str_equal (tag, "keywords"))    slot = &c->keep[3];
  else if (g_str_equal (tag, "description")) slot = &c->keep[4];
  if (slot != NULL && c->text->len > 0)
    {
      g_free (*slot);
      *slot = g_strdup (c->text->str);
    }
  g_clear_pointer (&c->field, g_free);
  g_string_truncate (c->text, 0);
}

static void
read_core_props (W42Zip *zip, W42PieceTable *pt)
{
  GBytes *xml = w42_zip_read (zip, "docProps/core.xml");
  GMarkupParser parser = { core_start, core_end, core_text, NULL, NULL };
  CoreProps c;
  GMarkupParseContext *ctx;

  if (xml == NULL)
    return;
  memset (&c, 0, sizeof c);
  c.text = g_string_new (NULL);
  ctx = g_markup_parse_context_new (&parser, 0, &c, NULL);
  g_markup_parse_context_parse (ctx, g_bytes_get_data (xml, NULL), g_bytes_get_size (xml), NULL);
  g_markup_parse_context_free (ctx);

  c.info.title = c.keep[0];
  c.info.subject = c.keep[1];
  c.info.author = c.keep[2];
  c.info.keywords = c.keep[3];
  c.info.comments = c.keep[4];
  w42_pt_set_info (pt, &c.info);

  for (guint i = 0; i < G_N_ELEMENTS (c.keep); i++)
    g_free (c.keep[i]);
  g_free (c.field);
  g_string_free (c.text, TRUE);
  g_bytes_unref (xml);
}

static GHashTable *
read_styles (W42Zip *zip, W42StyleSheet *sheet)
{
  GHashTable *map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GBytes *xml = w42_zip_read (zip, "word/styles.xml");

  if (xml != NULL)
    {
      GMarkupParser parser = { styles_start, styles_end, NULL, NULL, NULL };
      Styles s = { .map = map, .sheet = sheet };
      GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &s, NULL);

      s.display = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
      s.pending_based = g_ptr_array_new_with_free_func (g_free);
      g_markup_parse_context_parse (ctx, g_bytes_get_data (xml, NULL), g_bytes_get_size (xml), NULL);
      g_markup_parse_context_free (ctx);

      /* basedOn names an id, which may belong to a style read later. */
      for (guint i = 0; i < s.pending_based->len; i++)
        {
          char *entry = g_ptr_array_index (s.pending_based, i);
          char *tab = strchr (entry, '\t');
          const W42Style *st;

          if (tab == NULL)
            continue;
          *tab = '\0';
          st = w42_stylesheet_find (sheet, entry);
          if (st != NULL)
            {
              const char *ours = g_hash_table_lookup (map, tab + 1);
              const char *shown = g_hash_table_lookup (s.display, tab + 1);
              W42Style copy = *st;

              copy.based_on = ours != NULL ? ours : shown != NULL ? g_intern_string (shown) : NULL;
              w42_stylesheet_set (sheet, &copy);
            }
        }
      /* Now that every base is known, each style takes what it does not
       * set from its base, as Word does. */
      for (guint i = 0; i < w42_stylesheet_size (sheet); i++)
        {
          const W42Style *st = w42_stylesheet_get (sheet, i);

          if (st->based_on != NULL)
            w42_stylesheet_follow (sheet, st->name);
        }
      g_ptr_array_free (s.pending_based, TRUE);
      g_hash_table_destroy (s.display);
      g_free (s.cur_based);
      g_free (s.current_id);
      g_bytes_unref (xml);
    }
  return map;
}

/* numbering.xml: numId -> list kind, through the abstract numbering's
 * first level. */
typedef struct {
  GHashTable *abstract;   /* abstractNumId -> kind */
  GHashTable *nums;       /* numId -> kind */
  char       *current_abstract;
  gboolean    in_level0;
  gboolean    level0_done;
  char       *current_num;
  char       *bullet_text;
} Numbering;

static void
numbering_start (GMarkupParseContext *ctx, const char *name, const char **an,
                 const char **av, gpointer data, GError **error)
{
  Numbering *n = data;
  const char *tag = local (name);

  (void) ctx; (void) error;
  if (g_str_equal (tag, "abstractNum"))
    {
      g_free (n->current_abstract);
      n->current_abstract = g_strdup (attr (an, av, "abstractNumId"));
      n->level0_done = FALSE;
      if (n->current_abstract != NULL)
        g_hash_table_insert (n->abstract, g_strdup (n->current_abstract),
                             GINT_TO_POINTER (W42_LIST_NUMBER));
    }
  else if (g_str_equal (tag, "lvl"))
    n->in_level0 = n->current_abstract != NULL && attr_int (an, av, "ilvl", 0) == 0 && !n->level0_done;
  else if (g_str_equal (tag, "lvlText") && n->in_level0)
    {
      g_free (n->bullet_text);
      n->bullet_text = g_strdup (attr (an, av, "val"));
    }
  else if (g_str_equal (tag, "numFmt") && n->in_level0 && n->current_abstract != NULL)
    {
      const char *fmt = attr (an, av, "val");
      W42ListKind kind = W42_LIST_NUMBER;

      if (fmt != NULL)
        {
          if (g_str_equal (fmt, "bullet"))          kind = W42_LIST_BULLET;
          else if (g_str_equal (fmt, "lowerLetter")) kind = W42_LIST_LOWER_LETTER;
          else if (g_str_equal (fmt, "upperLetter")) kind = W42_LIST_UPPER_LETTER;
          else if (g_str_equal (fmt, "lowerRoman"))  kind = W42_LIST_LOWER_ROMAN;
          else if (g_str_equal (fmt, "upperRoman"))  kind = W42_LIST_UPPER_ROMAN;
        }
      g_hash_table_insert (n->abstract, g_strdup (n->current_abstract), GINT_TO_POINTER (kind));
    }
  else if (g_str_equal (tag, "num"))
    {
      g_free (n->current_num);
      n->current_num = g_strdup (attr (an, av, "numId"));
    }
  else if (g_str_equal (tag, "abstractNumId") && n->current_num != NULL)
    {
      const char *val = attr (an, av, "val");
      gpointer kind = val != NULL ? g_hash_table_lookup (n->abstract, val) : NULL;

      g_hash_table_insert (n->nums, g_strdup (n->current_num),
                           kind != NULL ? kind : GINT_TO_POINTER (W42_LIST_NUMBER));
    }
}

static void
numbering_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  Numbering *n = data;
  const char *tag = local (name);

  (void) ctx; (void) error;
  if (g_str_equal (tag, "lvl") && n->in_level0)
    {
      /* A bullet's character says which bullet. */
      gpointer kind = g_hash_table_lookup (n->abstract, n->current_abstract);

      if (GPOINTER_TO_INT (kind) == W42_LIST_BULLET && n->bullet_text != NULL)
        {
          gunichar c = g_utf8_get_char (n->bullet_text);
          W42ListKind k = c == 'o' || c == 0x25E6 ? W42_LIST_BULLET_CIRCLE
                        : c == 0x25AA || c == 0x25A0 || c == 0xA7 ? W42_LIST_BULLET_SQUARE
                        : c == '-' || c == 0x2013 ? W42_LIST_BULLET_DASH
                        : W42_LIST_BULLET;
          g_hash_table_insert (n->abstract, g_strdup (n->current_abstract), GINT_TO_POINTER (k));
        }
      n->in_level0 = FALSE;
      n->level0_done = TRUE;
      g_free (n->bullet_text);
      n->bullet_text = NULL;
    }
}

static GHashTable *
read_numbering (W42Zip *zip)
{
  GHashTable *nums = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GBytes *xml = w42_zip_read (zip, "word/numbering.xml");

  if (xml != NULL)
    {
      GMarkupParser parser = { numbering_start, numbering_end, NULL, NULL, NULL };
      Numbering n = { g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL), nums, NULL, FALSE, FALSE, NULL, NULL };
      GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &n, NULL);

      g_markup_parse_context_parse (ctx, g_bytes_get_data (xml, NULL), g_bytes_get_size (xml), NULL);
      g_markup_parse_context_free (ctx);
      g_hash_table_destroy (n.abstract);
      g_free (n.current_abstract);
      g_free (n.current_num);
      g_free (n.bullet_text);
      g_bytes_unref (xml);
    }
  return nums;
}

/* The text of a part -- a header, footer or note -- paragraph by
 * paragraph, joined by newlines; and the first paragraph's alignment. */
typedef struct {
  GString  *text;
  gboolean  in_t;
  gboolean  started;
  W42Align  align;
  gboolean  align_set;
  GHashTable *notes;       /* for footnotes.xml: id -> text */
  char     *note_id;
  gboolean  in_instr;      /* inside w:instrText: a field's code */
  gboolean  in_result;     /* between a field's separate and end: skipped */
  GString  *instr;
} PartText;

/* A field code becomes word42's own {PAGE}, {NUMPAGES} or {DATE}. */
static void
part_field (PartText *p)
{
  char *code = g_strstrip (g_strdup (p->instr->str));

  if (g_ascii_strncasecmp (code, "PAGE", 4) == 0 && g_ascii_strncasecmp (code, "PAGEREF", 7) != 0)
    g_string_append (p->text, "{PAGE}");
  else if (g_ascii_strncasecmp (code, "NUMPAGES", 8) == 0 || g_ascii_strncasecmp (code, "SECTIONPAGES", 12) == 0)
    g_string_append (p->text, "{NUMPAGES}");
  else if (g_ascii_strncasecmp (code, "DATE", 4) == 0 || g_ascii_strncasecmp (code, "TIME", 4) == 0)
    g_string_append (p->text, "{DATE}");
  g_free (code);
  g_string_truncate (p->instr, 0);
}

static void
part_start (GMarkupParseContext *ctx, const char *name, const char **an,
            const char **av, gpointer data, GError **error)
{
  PartText *p = data;
  const char *tag = local (name);

  (void) ctx; (void) error;
  if (p->notes != NULL && (g_str_equal (tag, "footnote") || g_str_equal (tag, "endnote") ||
                           g_str_equal (tag, "comment")))
    {
      g_free (p->note_id);
      p->note_id = g_strdup (attr (an, av, "id"));
      g_string_truncate (p->text, 0);
      p->started = FALSE;
    }
  else if (g_str_equal (tag, "p"))
    {
      if (p->started)
        g_string_append_c (p->text, '\n');
      p->started = TRUE;
    }
  else if (g_str_equal (tag, "t") || g_str_equal (tag, "delText"))
    p->in_t = !p->in_result;
  else if (g_str_equal (tag, "instrText"))
    p->in_instr = TRUE;
  else if (g_str_equal (tag, "fldChar"))
    {
      const char *kind = attr (an, av, "fldCharType");

      if (kind != NULL && g_str_equal (kind, "separate"))
        {
          part_field (p);
          p->in_result = TRUE;
        }
      else if (kind != NULL && g_str_equal (kind, "end"))
        {
          if (!p->in_result)
            part_field (p);          /* a field with no cached result */
          p->in_result = FALSE;
        }
    }
  else if (g_str_equal (tag, "fldSimple"))
    {
      const char *instr = attr (an, av, "instr");

      if (instr != NULL)
        {
          g_string_assign (p->instr, instr);
          part_field (p);
          p->in_result = TRUE;
        }
    }
  else if (g_str_equal (tag, "tab"))
    g_string_append_c (p->text, '\t');
  else if (g_str_equal (tag, "jc") && !p->align_set)
    {
      const char *v = attr (an, av, "val");

      p->align_set = TRUE;
      if (v != NULL && (g_str_equal (v, "center")))
        p->align = W42_ALIGN_CENTER;
      else if (v != NULL && (g_str_equal (v, "right") || g_str_equal (v, "end")))
        p->align = W42_ALIGN_RIGHT;
      else if (v != NULL && g_str_equal (v, "both"))
        p->align = W42_ALIGN_JUSTIFY;
    }
}

static void
part_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  PartText *p = data;
  const char *tag = local (name);

  (void) ctx; (void) error;
  if (g_str_equal (tag, "t") || g_str_equal (tag, "delText"))
    p->in_t = FALSE;
  else if (g_str_equal (tag, "instrText"))
    p->in_instr = FALSE;
  else if (g_str_equal (tag, "fldSimple"))
    p->in_result = FALSE;
  else if (p->notes != NULL && (g_str_equal (tag, "footnote") || g_str_equal (tag, "endnote") ||
                                g_str_equal (tag, "comment")) &&
           p->note_id != NULL)
    {
      g_hash_table_insert (p->notes, g_strdup (p->note_id), g_strdup (p->text->str));
      g_string_truncate (p->text, 0);
    }
}

static void
part_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  PartText *p = data;

  (void) ctx; (void) error;
  if (p->in_instr)
    g_string_append_len (p->instr, text, len);
  else if (p->in_t)
    g_string_append_len (p->text, text, len);
}

static gboolean
parse_part (W42Zip *zip, const char *part, PartText *p)
{
  GBytes *xml = w42_zip_read (zip, part);
  GMarkupParser parser = { part_start, part_end, part_text, NULL, NULL };
  GMarkupParseContext *ctx;

  if (xml == NULL)
    return FALSE;
  p->instr = g_string_new (NULL);
  ctx = g_markup_parse_context_new (&parser, 0, p, NULL);
  g_markup_parse_context_parse (ctx, g_bytes_get_data (xml, NULL), g_bytes_get_size (xml), NULL);
  g_markup_parse_context_free (ctx);
  g_string_free (p->instr, TRUE);
  p->instr = NULL;
  g_bytes_unref (xml);
  return TRUE;
}

static GHashTable *
read_notes (W42Zip *zip, const char *part)
{
  PartText p = { .text = g_string_new (NULL), .align = W42_ALIGN_LEFT,
                 .notes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free) };

  parse_part (zip, part, &p);
  g_string_free (p.text, TRUE);
  g_free (p.note_id);
  return p.notes;
}

/* ---- the document ------------------------------------------------------- */

typedef struct {
  W42Builder  b;
  W42PieceTable *pt;
  W42PageSetup *page;
  W42Zip     *zip;
  GHashTable *rels, *styles, *numbering, *footnotes, *endnotes, *comments;
  GHashTable *comment_start;      /* id -> gsize position */
  gboolean    in_pbdr;

  GString    *text;
  gboolean    in_t;

  int         depth_tbl;          /* nested tables are read as paragraphs */
  gboolean    cell_pending;
  int         cell_span;
  int         cell_vmerge;   /* 0 none, 1 the merge starts here, 2 it carries on */
  GArray     *grid;               /* int widths of the table being read */
  gboolean    table_started;
  gboolean    in_tblborders;
  int         fld_state;          /* 0 none, 1 after begin (code), 2 after separate (result) */
  GString    *fld_instr;
  gsize       fld_start;
  gboolean    in_instr;
  int         skip_depth;         /* inside mc:Fallback: read nothing */
  gboolean    tbl_borders;     /* w:tblBorders, or a grid table style, said so */

  gboolean    in_ppr, in_rpr, in_sectpr_body;
  W42CharFmt  run_ch;             /* built from the run's rPr */
  gboolean    have_run;

  int         revision;           /* inside w:ins (1) or w:del (2) */
  const char *link;               /* inside w:hyperlink */
  GHashTable *bookmarks;          /* id -> name, and starts by name */
  GHashTable *bookmark_start;     /* name -> gsize position */

  gboolean    in_drawing;
  int         drop_pending;  /* framePr dropCap on this paragraph: its letter
                              * joins the next paragraph, which drops it */
  int         drop_join;
  int         txbx_depth;    /* inside w:txbxContent: a text box's paragraphs */
  gboolean    in_tcborders;  /* w:tcBorders: the cell's own sides */
  gboolean    cell_has_fill; /* w:tcPr/w:shd: the cell's own background */
  guint32     cell_fill;
  int         cell_sides;    /* -1 for none of its own */
  W42BorderEdge cell_edge[4];  /* the lines of the sides it names */
  gboolean    cell_edges_set;
  int         cell_shading;  /* w:shd val="pctNN": its grey, in percent */
  int         cell_valign;   /* W42CellVAlign */
  W42BorderEdge tbl_edge[W42_N_EDGES];  /* w:tblBorders, side by side */
  W42CharFmt  style_ch;      /* the paragraph style's character formatting */
  gboolean    have_style_ch;
  int         txbx_side, txbx_width;
  W42ParaFmt  txbx_saved_pa; /* the paragraph the box hangs on */
  gboolean    txbx_reopened;
  gboolean    in_align;      /* wp:align: the anchored picture's side */
  gboolean    anchored;      /* wp:anchor rather than wp:inline */
  W42Wrap     wrap;          /* the side the text keeps off */
  gint64      cx, cy;
  char       *blip;
  gboolean    in_pos_h, in_pos_v, in_offset;   /* wp:positionH/V and their posOffset */
  gboolean    pos_h_set, pos_v_set;            /* an offset was read, from the column
                                                * or the paragraph rather than the page */
  gint64      pos_x, pos_y;                    /* EMU */
  gboolean    behind;                          /* behindDoc="1" */
  gboolean    in_wsp;        /* wps:wsp: a shape */
  W42ShapeKind shape;
  gboolean    in_ln;         /* a:ln: the outline, whose fill is the line's colour */
  gboolean    in_wsp_style;  /* wps:style: theme references, not the shape's own colours */
  double      line_pt;
  guint32     line_rgb;
  gboolean    has_line, filled;
  guint32     fill_rgb;
  gboolean    shape_txbx;    /* the shape's text, gathered rather than laid out */
  GString    *shape_text;

  gboolean    section_pending;    /* the next paragraph starts a section */
  gsize       section_first;      /* the first paragraph of the current section */
  gboolean    first_para;
  int         page_cols, page_gap;

  gboolean    in_note_body;       /* text goes to a note */

  /* The notes parts are read with this same parser into a piece table of
   * their own, so that a note keeps its paragraphs and its formatting
   * rather than coming back as a line of plain text.  `note_spans` says
   * which stretch of that table each note is. */
  gboolean       reading_notes;   /* this parser is on footnotes.xml */
  W42PieceTable *note_pt[2];      /* the footnotes' table and the endnotes' */
  GHashTable    *note_spans;      /* "f1", "e1" -> W42NoteSpan */
  char          *note_kind;       /* "f" or "e", for the part being read */
  char          *note_open;       /* the id of the note being read */
  gsize          note_from;
  gboolean       note_skipped;    /* a separator, not a note */
  gboolean       note_lead;       /* drop the space Word puts after the mark */
} Docx;

typedef struct {
  gsize      start, end;
  W42ParaFmt pa;              /* the note's first paragraph, whose mark is
                               * not in the span: the note's body has one
                               * of its own for the text to go into. */
} W42NoteSpan;

static const char *
map_style (Docx *d, const char *id)
{
  const char *ours = id != NULL ? g_hash_table_lookup (d->styles, id) : NULL;

  return ours != NULL ? ours : g_intern_static_string ("Normal");
}

/* What a cell's w:tcPr said, onto the cell mark just made. */
static void
docx_apply_cell_props (Docx *d)
{
  if (d->b.cell_pos == (gsize) -1)
    return;
  if (d->cell_sides >= 0)
    w42_pt_cell_set_borders_at (d->pt, d->b.cell_pos, d->cell_sides);
  if (d->cell_edges_set)
    w42_pt_cell_set_edges_at (d->pt, d->b.cell_pos, d->cell_edge);
  if (d->cell_has_fill)
    w42_pt_cell_set_fill_at (d->pt, d->b.cell_pos, TRUE, d->cell_fill);
  else if (d->cell_shading > 0)
    w42_pt_cell_set_shading_at (d->pt, d->b.cell_pos, d->cell_shading);
  if (d->cell_valign != W42_CELL_VALIGN_TOP)
    w42_pt_cell_set_valign_at (d->pt, d->b.cell_pos, (W42CellVAlign) d->cell_valign);
}

static void
docx_flush_text (Docx *d)
{
  if (d->text->len == 0)
    return;
  /* Word puts its note's own number and a space in front of the note's
   * text; the number is a field word42 paints itself, and the space came
   * with it. */
  if (d->note_lead)
    {
      const char *p = d->text->str;

      while (*p == ' ' || *p == '\t')
        p++;
      if (*p == '\0')
        {
          g_string_truncate (d->text, 0);
          return;
        }
      g_string_erase (d->text, 0, (gssize) (p - d->text->str));
      d->note_lead = FALSE;
    }
  d->b.ch = d->run_ch;
  d->b.ch.link = d->link;
  d->b.ch.revision = (guint8) d->revision;
  w42_builder_text (&d->b, d->text->str);
  g_string_truncate (d->text, 0);
}

static void
docx_apply_section_columns (Docx *d, int cols, int gap)
{
  if (d->section_first == (gsize) -1)
    {
      d->page->columns = cols;
      d->page->column_gap = gap;
    }
  else
    {
      W42ParaFmt want;

      memset (&want, 0, sizeof want);
      want.section_break = 1;
      want.columns = (guint8) CLAMP (cols, 1, 9);
      want.column_gap = gap;
      w42_pt_apply_para_fmt (d->pt, d->section_first, 0, W42_PARA_SECTION, &want);
    }
}

/* The text since the field's separator is its cached result: mark it. */
static void
docx_apply_field (Docx *d)
{
  const char *code = w42_field_code (d->fld_instr->str);

  docx_flush_text (d);
  if (d->fld_state == 2 && code != NULL && d->b.pos > d->fld_start)
    {
      W42CharFmt want;

      memset (&want, 0, sizeof want);
      want.field = code;
      w42_pt_apply_char_fmt (d->pt, d->fld_start, d->b.pos - d->fld_start, W42_CHAR_FIELD, &want);
    }
}

static void
docx_start (GMarkupParseContext *ctx, const char *name, const char **an,
            const char **av, gpointer data, GError **error)
{
  Docx *d = data;
  const char *tag = local (name);

  (void) ctx; (void) error;

  if (d->skip_depth > 0)
    {
      d->skip_depth++;              /* a Fallback inside a Fallback counts too */
      return;
    }
  else if (g_str_equal (tag, "Fallback"))
    d->skip_depth = 1;
  else if (d->shape_txbx)
    {
      /* The text in a shape: its runs are gathered as the shape's label. */
      if (g_str_equal (tag, "t"))
        d->in_t = TRUE;
      else if (g_str_equal (tag, "br") || g_str_equal (tag, "tab"))
        g_string_append_c (d->shape_text, g_str_equal (tag, "br") ? '\n' : ' ');
    }
  else if (g_str_equal (tag, "p"))
    {
      if (d->cell_pending)
        {
          w42_builder_begin_cell (&d->b, d->cell_span);
          d->cell_pending = FALSE;
          docx_apply_cell_props (d);
          if (d->cell_vmerge != 0 && d->b.cell_pos != (gsize) -1)
            {
              /* The rows a merge covers are counted once the table is
               * read; for now the cell says which end of one it is. */
              w42_pt_set_cell_vspan (d->pt, d->b.cell_pos,
                                     d->cell_vmerge == 1 ? 2 : W42_CELL_COVERED);
            }
        }
      w42_builder_reset_para (&d->b);
      d->have_style_ch = FALSE;
      if (d->section_pending)
        {
          d->b.pa.section_break = 1;
          d->b.pa.columns = 1;
          d->section_pending = FALSE;
          d->section_first = d->b.pos;
        }
      else if (d->first_para)
        d->section_first = (gsize) -1;
      d->first_para = FALSE;
    }
  else if (g_str_equal (tag, "pPr"))
    d->in_ppr = TRUE;
  else if (g_str_equal (tag, "rPr"))
    d->in_rpr = TRUE;
  else if (g_str_equal (tag, "r"))
    {
      w42_builder_reset_char (&d->b);
      d->run_ch = d->have_style_ch ? d->style_ch : d->b.ch;
      d->have_run = TRUE;
    }
  else if (g_str_equal (tag, "t") || g_str_equal (tag, "delText"))
    d->in_t = TRUE;
  else if (g_str_equal (tag, "instrText"))
    d->in_instr = TRUE;
  else if (g_str_equal (tag, "fldChar"))
    {
      const char *kind = attr (an, av, "fldCharType");

      docx_flush_text (d);
      if (kind != NULL && g_str_equal (kind, "begin"))
        {
          d->fld_state = 1;
          g_string_truncate (d->fld_instr, 0);
        }
      else if (kind != NULL && g_str_equal (kind, "separate"))
        {
          d->fld_state = 2;
          d->fld_start = d->b.pos;
        }
      else if (kind != NULL && g_str_equal (kind, "end"))
        {
          docx_apply_field (d);
          d->fld_state = 0;
        }
    }
  else if (g_str_equal (tag, "fldSimple"))
    {
      const char *instr = attr (an, av, "instr");

      docx_flush_text (d);
      g_string_assign (d->fld_instr, instr != NULL ? instr : "");
      d->fld_state = 2;
      d->fld_start = d->b.pos;
    }
  else if (g_str_equal (tag, "tab") && !d->in_ppr)
    g_string_append_c (d->text, '\t');
  else if (g_str_equal (tag, "br"))
    {
      const char *type = attr (an, av, "type");

      docx_flush_text (d);
      if (type != NULL && g_str_equal (type, "page"))
        {
          w42_builder_end_paragraph (&d->b);
          d->b.pa.page_break_before = 1;
        }
      else
        w42_builder_text (&d->b, "\342\200\250");
    }
  else if (g_str_equal (tag, "softHyphen"))
    g_string_append (d->text, "\302\255");
  else if (g_str_equal (tag, "noBreakHyphen"))
    g_string_append (d->text, "\342\200\221");
  else if (g_str_equal (tag, "sym"))
    {
      const char *ch = attr (an, av, "char");

      if (ch != NULL)
        {
          gunichar c = (gunichar) strtoul (ch, NULL, 16);

          if (c >= 0xF000 && c <= 0xF0FF)
            c -= 0xF000;                 /* the symbol fonts' private range */
          if (g_unichar_validate (c) && c >= 0x20)
            g_string_append_unichar (d->text, c);
        }
    }

  /* Paragraph properties. */
  else if (d->in_ppr && !d->in_rpr)
    {
      W42ParaFmt *pa = &d->b.pa;

      if (g_str_equal (tag, "pStyle"))
        {
          const W42Style *st;

          pa->style = map_style (d, attr (an, av, "val"));
          st = w42_stylesheet_find (w42_pt_stylesheet (d->pt), pa->style);
          if (st != NULL && !st->character)
            {
              /* The style's formatting under the paragraph's own, which
               * follows in the pPr, and its font under each run's. */
              pa->align = st->pa.align;
              pa->indent_left = st->pa.indent_left;
              pa->indent_right = st->pa.indent_right;
              pa->indent_first = st->pa.indent_first;
              pa->space_before = st->pa.space_before;
              pa->space_after = st->pa.space_after;
              pa->line_spacing = st->pa.line_spacing;
              pa->line_spacing_pct = st->pa.line_spacing_pct;
              pa->keep_next = st->pa.keep_next;
              pa->keep_together = st->pa.keep_together;
              d->style_ch = st->ch;
              d->have_style_ch = TRUE;
            }
        }
      else if (g_str_equal (tag, "framePr"))
        {
          const char *drop = attr (an, av, "dropCap");
          const char *xa = attr (an, av, "xAlign");
          int w = attr_int (an, av, "w", 0);

          if (drop != NULL && !g_str_equal (drop, "none"))
            d->drop_pending = CLAMP (attr_int (an, av, "lines", 3), 1, 10);
          else if (w > 0 || xa != NULL)
            {
              /* A framed paragraph at the side of the column. */
              pa->frame_side = (xa != NULL && g_str_equal (xa, "right")) ? W42_FRAME_RIGHT : W42_FRAME_LEFT;
              pa->frame_width = CLAMP (w, 0, 31680);
            }
        }
      else if (g_str_equal (tag, "jc"))
        {
          const char *v = attr (an, av, "val");

          if (v == NULL) ;
          else if (g_str_equal (v, "center")) pa->align = W42_ALIGN_CENTER;
          else if (g_str_equal (v, "right") || g_str_equal (v, "end")) pa->align = W42_ALIGN_RIGHT;
          else if (g_str_equal (v, "both") || g_str_equal (v, "distribute")) pa->align = W42_ALIGN_JUSTIFY;
          else pa->align = W42_ALIGN_LEFT;
        }
      else if (g_str_equal (tag, "ind"))
        {
          const char *hanging = attr (an, av, "hanging");
          const char *first = attr (an, av, "firstLine");

          pa->indent_left = CLAMP (attr_int (an, av, "left", attr_int (an, av, "start", pa->indent_left)), -31680, 31680);
          pa->indent_right = CLAMP (attr_int (an, av, "right", attr_int (an, av, "end", pa->indent_right)), -31680, 31680);
          if (hanging != NULL)
            pa->indent_first = -CLAMP (atoi (hanging), 0, 31680);
          else if (first != NULL)
            pa->indent_first = CLAMP (atoi (first), -31680, 31680);
        }
      else if (g_str_equal (tag, "spacing"))
        {
          const char *line = attr (an, av, "line");
          const char *rule = attr (an, av, "lineRule");

          pa->space_before = attr_int (an, av, "before", pa->space_before);
          pa->space_after = attr_int (an, av, "after", pa->space_after);
          if (line != NULL)
            {
              int l = CLAMP (atoi (line), 0, 31680);

              if (rule == NULL || g_str_equal (rule, "auto"))
                pa->line_spacing_pct = l * 100 / 240;
              else
                pa->line_spacing = l;
            }
          pa->space_before = CLAMP (pa->space_before, 0, 31680);
          pa->space_after = CLAMP (pa->space_after, 0, 31680);
        }
      else if (g_str_equal (tag, "ilvl"))
        pa->list_level = (guint8) CLAMP (attr_int (an, av, "val", 0), 0, 8);
      else if (g_str_equal (tag, "numId"))
        {
          const char *v = attr (an, av, "val");
          gpointer kind = v != NULL ? g_hash_table_lookup (d->numbering, v) : NULL;

          if (v != NULL && !g_str_equal (v, "0"))
            {
              pa->list = (guint8) (kind != NULL ? GPOINTER_TO_INT (kind) : W42_LIST_NUMBER);
              if (pa->indent_left == 0)
                pa->indent_left = 360 * (pa->list_level + 1);
              if (pa->indent_first == 0)
                pa->indent_first = -360;
            }
        }
      else if (g_str_equal (tag, "tab"))
        {
          const char *val = attr (an, av, "val");
          const char *leader = attr (an, av, "leader");
          int pos = attr_int (an, av, "pos", -1);

          if (pos >= 0 && val != NULL && !g_str_equal (val, "clear"))
            w42_para_fmt_set_tab_leader (pa, pos,
                                  g_str_equal (val, "center") ? W42_TAB_CENTER
                                  : g_str_equal (val, "right") || g_str_equal (val, "end") ? W42_TAB_RIGHT
                                  : g_str_equal (val, "decimal") ? W42_TAB_DECIMAL : W42_TAB_LEFT,
                                  leader == NULL ? W42_TAB_LEAD_NONE
                                  : g_str_equal (leader, "dot") || g_str_equal (leader, "middleDot") ? W42_TAB_LEAD_DOT
                                  : g_str_equal (leader, "hyphen") ? W42_TAB_LEAD_DASH
                                  : g_str_equal (leader, "underscore") ||
                                    g_str_equal (leader, "heavy") ? W42_TAB_LEAD_LINE : W42_TAB_LEAD_NONE);
        }
      else if (g_str_equal (tag, "pBdr"))
        d->in_pbdr = TRUE;
      else if (d->in_pbdr && (g_str_equal (tag, "top") || g_str_equal (tag, "bottom") ||
                              g_str_equal (tag, "left") || g_str_equal (tag, "right")))
        {
          int e = border_edge_index (tag);
          W42BorderEdge edge;

          if (e >= 0 && e < 4 && border_element (an, av, &edge))
            {
              pa->border |= (guint8) (1 << e);
              pa->edge[e] = edge;
            }
        }
      else if (g_str_equal (tag, "shd"))
        {
          const char *fill = attr (an, av, "fill");

          if (fill != NULL && !g_str_equal (fill, "auto"))
            {
              pa->shading_color = (guint32) strtoul (fill, NULL, 16) & 0xFFFFFF;
              pa->has_shading_color = 1;
              pa->shading = 0;
            }
        }
      else if (g_str_equal (tag, "keepNext"))     pa->keep_next = toggle_on (an, av);
      else if (g_str_equal (tag, "keepLines"))    pa->keep_together = toggle_on (an, av);
      else if (g_str_equal (tag, "widowControl")) pa->widow_control = toggle_on (an, av);
      else if (g_str_equal (tag, "pageBreakBefore")) pa->page_break_before = toggle_on (an, av);
      else if (g_str_equal (tag, "bidi"))         pa->rtl = toggle_on (an, av);
      else if (g_str_equal (tag, "cols"))
        {
          /* A section ends with this paragraph; its columns belong to
           * the section that began at section_first. */
          docx_apply_section_columns (d, attr_int (an, av, "num", 1),
                                      attr_int (an, av, "space", 720));
          d->section_pending = TRUE;
        }
      else if (g_str_equal (tag, "sectPr"))
        d->section_pending = TRUE;    /* even without w:cols */
    }

  /* Run properties. */
  else if (d->in_rpr)
    {
      W42CharFmt *ch = &d->run_ch;

      if (g_str_equal (tag, "rStyle"))
        {
          /* A character style: its formatting under the run's own. */
          const char *id = attr (an, av, "val");
          const char *ours = id != NULL ? g_hash_table_lookup (d->styles, id) : NULL;
          const W42Style *st = ours != NULL ? w42_stylesheet_find (w42_pt_stylesheet (d->pt), ours) : NULL;

          if (st != NULL && st->character)
            {
              W42Fmt def;

              w42_fmt_init_default (&def);
              if (st->ch.family != def.ch.family) ch->family = st->ch.family;
              if (st->ch.size != def.ch.size) ch->size = st->ch.size;
              if (st->ch.bold) ch->bold = 1;
              if (st->ch.italic) ch->italic = 1;
              if (st->ch.underline) ch->underline = 1;
              if (st->ch.color != def.ch.color) ch->color = st->ch.color;
            }
        }

      if (g_str_equal (tag, "b"))          ch->bold = toggle_on (an, av);
      else if (g_str_equal (tag, "i"))     ch->italic = toggle_on (an, av);
      else if (g_str_equal (tag, "u"))
        ch->underline = underline_from_val (attr (an, av, "val"));
      else if (g_str_equal (tag, "strike") || g_str_equal (tag, "dstrike")) ch->strikeout = toggle_on (an, av);
      else if (g_str_equal (tag, "lang"))
        {
          const char *known = w42_lang_normalise (attr (an, av, "val"));

          if (known != NULL)
            ch->lang = known;
        }
      else if (g_str_equal (tag, "noProof"))
        ch->lang = toggle_on (an, av) ? g_intern_static_string (W42_LANG_NONE) : NULL;
      else if (g_str_equal (tag, "caps"))  ch->allcaps = toggle_on (an, av);
      else if (g_str_equal (tag, "smallCaps")) ch->smallcaps = toggle_on (an, av);
      else if (g_str_equal (tag, "sz"))
        {
          /* A dropped letter's paragraph sets it large; the letter goes
           * back to the size of the text it drops into. */
          if (d->drop_pending == 0)
            ch->size = CLAMP (attr_int (an, av, "val", ch->size), 2, 3276);
        }
      else if (g_str_equal (tag, "color"))
        {
          const char *v = attr (an, av, "val");

          if (v != NULL && !g_str_equal (v, "auto"))
            ch->color = (guint32) strtoul (v, NULL, 16);
        }
      else if (g_str_equal (tag, "rFonts"))
        {
          const char *f = attr (an, av, "ascii");

          if (f == NULL) f = attr (an, av, "hAnsi");
          if (f != NULL)
            ch->family = g_intern_string (f);
        }
      else if (g_str_equal (tag, "highlight"))
        {
          const char *v = attr (an, av, "val");

          ch->highlight = (v != NULL && !g_str_equal (v, "none")) ? (guint8) highlight_index (v) : 0;
        }
      else if (g_str_equal (tag, "shd"))
        {
          /* A run's shading is how LibreOffice writes a highlight: a
           * colour rather than one of Word's sixteen names. */
          const char *fill = attr (an, av, "fill");

          if (fill != NULL && !g_str_equal (fill, "auto"))
            ch->highlight = (guint8) nearest_highlight (fill);
        }
      else if (g_str_equal (tag, "vertAlign"))
        {
          const char *v = attr (an, av, "val");

          ch->script = v == NULL ? 0 : g_str_equal (v, "superscript") ? 1
                     : g_str_equal (v, "subscript") ? -1 : 0;
        }
      else if (g_str_equal (tag, "spacing"))
        ch->spacing = (gint16) CLAMP (attr_int (an, av, "val", 0), -720, 720);
    }

  /* Marks, links, notes. */
  else if (g_str_equal (tag, "hyperlink"))
    {
      const char *id = attr (an, av, "id");
      const char *anchor = attr (an, av, "anchor");
      const char *target = id != NULL ? g_hash_table_lookup (d->rels, id) : NULL;

      docx_flush_text (d);
      if (target != NULL)
        d->link = g_intern_string (target);
      else if (anchor != NULL)
        {
          char *hash = g_strconcat ("#", anchor, NULL);
          d->link = g_intern_string (hash);
          g_free (hash);
        }
    }
  else if (g_str_equal (tag, "bookmarkStart"))
    {
      const char *id = attr (an, av, "id");
      const char *bname = attr (an, av, "name");

      docx_flush_text (d);
      if (id != NULL && bname != NULL && !g_str_equal (bname, "_GoBack"))
        {
          g_hash_table_insert (d->bookmarks, g_strdup (id), g_strdup (bname));
          g_hash_table_insert (d->bookmark_start, g_strdup (bname), GSIZE_TO_POINTER (d->b.pos));
        }
    }
  else if (g_str_equal (tag, "bookmarkEnd"))
    {
      const char *id = attr (an, av, "id");
      const char *bname = id != NULL ? g_hash_table_lookup (d->bookmarks, id) : NULL;

      docx_flush_text (d);
      if (bname != NULL && g_hash_table_contains (d->bookmark_start, bname))
        {
          gsize start = GPOINTER_TO_SIZE (g_hash_table_lookup (d->bookmark_start, bname));
          W42CharFmt want;

          if (d->b.pos > start)
            {
              memset (&want, 0, sizeof want);
              want.bookmark = g_intern_string (bname);
              w42_pt_apply_char_fmt (d->pt, start, d->b.pos - start, W42_CHAR_BOOKMARK, &want);
            }
        }
    }
  else if (g_str_equal (tag, "commentRangeStart"))
    {
      const char *id = attr (an, av, "id");

      docx_flush_text (d);
      if (id != NULL)
        g_hash_table_insert (d->comment_start, g_strdup (id), GSIZE_TO_POINTER (d->b.pos));
    }
  else if (g_str_equal (tag, "commentRangeEnd"))
    {
      const char *id = attr (an, av, "id");
      const char *body = id != NULL ? g_hash_table_lookup (d->comments, id) : NULL;

      docx_flush_text (d);
      if (id != NULL && body != NULL && g_hash_table_contains (d->comment_start, id))
        {
          gsize start = GPOINTER_TO_SIZE (g_hash_table_lookup (d->comment_start, id));

          if (d->b.pos > start)
            {
              W42CharFmt want;
              char *clean = g_strstrip (g_strdup (body));

              memset (&want, 0, sizeof want);
              want.comment = g_intern_string (clean);
              w42_pt_apply_char_fmt (d->pt, start, d->b.pos - start, W42_CHAR_COMMENT, &want);
              g_free (clean);
            }
        }
    }
  else if (g_str_equal (tag, "ins"))
    d->revision = 1;
  else if (g_str_equal (tag, "del"))
    d->revision = 2;
  else if (d->reading_notes &&
           (g_str_equal (tag, "footnote") || g_str_equal (tag, "endnote")))
    {
      const char *id = attr (an, av, "id");
      const char *kind = attr (an, av, "type");

      docx_flush_text (d);
      if (d->b.in_para)
        w42_builder_end_paragraph (&d->b);
      g_free (d->note_open);
      d->note_open = id != NULL ? g_strdup (id) : NULL;
      d->note_from = d->b.pos;
      /* The separator rules Word keeps at the head of the part are not
       * notes and have no text of their own worth having. */
      d->note_skipped = kind != NULL && strstr (kind, "eparator") != NULL;
      d->note_lead = TRUE;
      w42_builder_reset_char (&d->b);
      w42_builder_reset_para (&d->b);
    }
  else if (g_str_equal (tag, "footnoteReference") || g_str_equal (tag, "endnoteReference"))
    {
      gboolean endnote = g_str_equal (tag, "endnoteReference");
      const char *id = attr (an, av, "id");
      const char *body = id != NULL ? g_hash_table_lookup (endnote ? d->endnotes : d->footnotes, id) : NULL;

      char *key = id != NULL ? g_strdup_printf ("%s%s", endnote ? "e" : "f", id) : NULL;
      const W42NoteSpan *span = key != NULL && d->note_spans != NULL
        ? g_hash_table_lookup (d->note_spans, key) : NULL;

      docx_flush_text (d);
      if (span != NULL && d->note_pt[endnote ? 1 : 0] != NULL &&
          !w42_builder_in_table (&d->b))
        {
          /* The note as it was written: its paragraphs, their formatting
           * and their runs, copied whole out of the notes part. */
          W42PieceTable *frag = w42_pt_extract (d->note_pt[endnote ? 1 : 0],
                                                span->start,
                                                span->end - span->start);
          W42ParaFmt keep = d->b.pa;

          w42_builder_begin_note (&d->b, endnote);
          w42_pt_apply_para_fmt (d->pt, d->b.pos > 0 ? d->b.pos - 1 : 0, 0,
                                 W42_PARA_ALL, &span->pa);
          d->b.pos += w42_pt_insert_fragment (d->pt, d->b.pos, frag);
          d->b.in_para = FALSE;
          w42_builder_end_note (&d->b);
          d->b.pa = keep;
          w42_pt_free (frag);
        }
      else if (body != NULL && !w42_builder_in_table (&d->b))
        {
          char **paras = g_strsplit (body, "\n", -1);
          W42ParaFmt keep = d->b.pa;

          w42_builder_begin_note (&d->b, endnote);
          w42_builder_reset_char (&d->b);
          w42_builder_reset_para (&d->b);
          for (int i = 0; paras[i] != NULL; i++)
            {
              if (i > 0)
                w42_builder_end_paragraph (&d->b);
              w42_builder_text (&d->b, g_strstrip (paras[i]));
            }
          w42_builder_end_note (&d->b);
          d->b.pa = keep;
          g_strfreev (paras);
        }
      g_free (key);
    }

  /* Pictures. */
  else if (g_str_equal (tag, "drawing"))
    {
      docx_flush_text (d);
      d->in_drawing = TRUE;
      d->anchored = FALSE;
      d->wrap = W42_WRAP_INLINE;
      d->cx = d->cy = 0;
      g_free (d->blip);
      d->blip = NULL;
    }
  else if (d->in_drawing && g_str_equal (tag, "anchor"))
    {
      /* Anchored: beside the text unless the wrap says otherwise. */
      const char *behind = attr (an, av, "behindDoc");

      d->anchored = TRUE;
      d->wrap = W42_WRAP_LEFT;
      d->behind = behind != NULL && (g_str_equal (behind, "1") || g_str_equal (behind, "true"));
      d->pos_h_set = d->pos_v_set = FALSE;
      d->pos_x = d->pos_y = 0;
    }
  else if (d->in_drawing && (g_str_equal (tag, "positionH") || g_str_equal (tag, "positionV")))
    {
      /* Where it sits: an offset from the column and the paragraph is a
       * place word42 can put it; from the page's edge it is not, and it
       * goes at the column's side instead. */
      const char *from = attr (an, av, "relativeFrom");
      gboolean usable = from == NULL || g_str_equal (from, "column") || g_str_equal (from, "paragraph") ||
                        g_str_equal (from, "margin") || g_str_equal (from, "character") ||
                        g_str_equal (from, "line");

      d->in_pos_h = g_str_equal (tag, "positionH") && usable;
      d->in_pos_v = g_str_equal (tag, "positionV") && usable;
    }
  else if (d->in_drawing && g_str_equal (tag, "posOffset") && (d->in_pos_h || d->in_pos_v))
    d->in_offset = TRUE;
  else if (d->in_drawing && g_str_equal (tag, "wsp"))
    {
      d->in_wsp = TRUE;
      d->in_wsp_style = FALSE;
      d->shape = W42_SHAPE_RECTANGLE;
      d->line_pt = 0.75;
      d->line_rgb = 0;
      d->has_line = TRUE;
      d->filled = FALSE;
      d->fill_rgb = 0xFFFFFF;
      d->in_ln = FALSE;
    }
  else if (d->in_wsp && g_str_equal (tag, "prstGeom"))
    {
      const char *prst = attr (an, av, "prst");

      d->shape = prst == NULL ? W42_SHAPE_RECTANGLE
               : g_str_equal (prst, "ellipse") ? W42_SHAPE_ELLIPSE
               : g_str_equal (prst, "roundRect") ? W42_SHAPE_ROUNDED_RECTANGLE
               : g_str_equal (prst, "line") || g_str_has_prefix (prst, "straightConnector") ? W42_SHAPE_LINE
               : W42_SHAPE_RECTANGLE;
    }
  else if (d->in_wsp && g_str_equal (tag, "style"))
    d->in_wsp_style = TRUE;
  else if (d->in_wsp && d->in_wsp_style)
    ;                             /* theme references: nothing the shape says itself */
  else if (d->in_wsp && g_str_equal (tag, "ln"))
    {
      const char *w = attr (an, av, "w");

      d->in_ln = TRUE;
      if (w != NULL)
        d->line_pt = g_ascii_strtoll (w, NULL, 10) / 12700.0;
    }
  else if (d->in_wsp && d->in_ln && g_str_equal (tag, "noFill"))
    d->has_line = FALSE;
  else if (d->in_wsp && !d->in_ln && g_str_equal (tag, "noFill"))
    d->filled = FALSE;
  else if (d->in_wsp && g_str_equal (tag, "solidFill"))
    {
      if (!d->in_ln)
        d->filled = TRUE;
    }
  else if (d->in_wsp && g_str_equal (tag, "srgbClr"))
    {
      const char *val = attr (an, av, "val");
      guint32 rgb = val != NULL ? (guint32) strtoul (val, NULL, 16) & 0xFFFFFF : 0;

      if (d->in_ln)
        d->line_rgb = rgb;
      else
        d->fill_rgb = rgb;
    }
  else if (d->in_wsp && g_str_equal (tag, "schemeClr"))
    {
      /* A theme colour: word42 has no theme, so Word's default accent,
       * a blue, stands for the fill and its darker shade for the line. */
      if (d->in_ln)
        d->line_rgb = 0x2F5597;
      else
        d->fill_rgb = 0x4472C4;
    }
  else if (d->in_wsp && (g_str_equal (tag, "tailEnd") || g_str_equal (tag, "headEnd")))
    {
      const char *type = attr (an, av, "type");

      if (type != NULL && !g_str_equal (type, "none") && d->shape == W42_SHAPE_LINE)
        d->shape = W42_SHAPE_ARROW;
    }
  else if (g_str_equal (tag, "txbxContent") && d->txbx_depth == 0 && d->in_wsp &&
           (d->filled || d->shape != W42_SHAPE_RECTANGLE))
    {
      /* Text in a filled shape, or in one that is not a plain box, is the
       * shape's label; a plain text box is paragraphs framed beside the
       * text, below. */
      d->shape_txbx = TRUE;
      g_string_truncate (d->shape_text, 0);
    }
  else if (g_str_equal (tag, "txbxContent") && d->txbx_depth == 0)
    {
      /* A text box hung on this paragraph: its paragraphs are framed at
       * the side the box is anchored to, and this paragraph goes on
       * afterwards. */
      docx_flush_text (d);
      d->drop_join = d->drop_pending = 0;
      d->txbx_depth = 1;
      d->txbx_side = d->wrap == W42_WRAP_RIGHT ? W42_FRAME_RIGHT : W42_FRAME_LEFT;
      d->txbx_width = (int) CLAMP (d->cx / EMU_PER_TWIP, 0, 31680);
      d->txbx_saved_pa = d->b.pa;
      if (d->b.in_para)
        w42_builder_end_paragraph (&d->b);
      d->in_drawing = FALSE;
      d->txbx_reopened = FALSE;
    }
  else if (g_str_equal (tag, "txbxContent"))
    d->txbx_depth++;
  else if (d->in_drawing && g_str_equal (tag, "wrapTopAndBottom"))
    d->wrap = W42_WRAP_TOP_BOTTOM;
  else if (d->in_drawing && g_str_equal (tag, "wrapNone"))
    d->wrap = d->behind ? W42_WRAP_BEHIND : W42_WRAP_FRONT;
  else if (d->in_drawing && g_str_equal (tag, "align"))
    d->in_align = TRUE;
  else if (d->in_drawing && g_str_equal (tag, "extent"))
    {
      const char *cx = attr (an, av, "cx"), *cy = attr (an, av, "cy");

      if (cx != NULL) d->cx = g_ascii_strtoll (cx, NULL, 10);
      if (cy != NULL) d->cy = g_ascii_strtoll (cy, NULL, 10);
    }
  else if (d->in_drawing && g_str_equal (tag, "blip"))
    {
      const char *embed = attr (an, av, "embed");

      g_free (d->blip);
      d->blip = g_strdup (embed);
    }

  /* Tables. */
  else if (g_str_equal (tag, "tbl"))
    {
      d->depth_tbl++;
      if (d->depth_tbl == 1)
        {
          docx_flush_text (d);
      if (d->drop_join > 0 || d->drop_pending > 0)
        {
          /* A dropped letter's paragraph ends before a table. */
          d->drop_join = d->drop_pending = 0;
          if (d->b.in_para)
            w42_builder_end_paragraph (&d->b);
        }
          g_array_set_size (d->grid, 0);
          d->table_started = FALSE;
          d->tbl_borders = FALSE;
          memset (d->tbl_edge, 0, sizeof d->tbl_edge);
        }
    }
  else if (g_str_equal (tag, "tblBorders") && d->depth_tbl == 1)
    {
      /* The sides it does not name are the table style's: the grid's
       * hairline, or nothing. */
      d->in_tblborders = TRUE;
      for (int e = 0; e < W42_N_EDGES; e++)
        {
          d->tbl_edge[e].style = d->tbl_borders ? W42_BORDER_SINGLE : W42_BORDER_NONE;
          d->tbl_edge[e].width = 0;
          d->tbl_edge[e].color = 0;
        }
    }
  else if (d->in_tblborders && border_edge_index (tag) >= 0)
    {
      /* A table is ruled only where it says so.  Word's own default, and
       * what a table carrying no w:tblBorders at all means, is no rules;
       * the grid everyone recognises comes from the Table Grid style. */
      if (border_element (an, av, &d->tbl_edge[border_edge_index (tag)]))
        d->tbl_borders = TRUE;
    }
  else if (g_str_equal (tag, "tblStyle") && d->depth_tbl == 1)
    {
      const char *val = attr (an, av, "val");

      if (val != NULL && strstr (val, "Grid") != NULL)
        {
          d->tbl_borders = TRUE;
          memset (d->tbl_edge, 0, sizeof d->tbl_edge);
        }
    }
  else if (g_str_equal (tag, "gridCol") && d->depth_tbl == 1)
    {
      int w = CLAMP (attr_int (an, av, "w", 1440), 0, 31680);

      g_array_append_val (d->grid, w);
    }
  else if (g_str_equal (tag, "tr") && d->depth_tbl == 1)
    {
      if (!d->table_started)
        {
          int n = (int) d->grid->len;

          w42_builder_begin_table (&d->b, n > 0 ? n : 1,
                                   n > 0 ? (const int *) d->grid->data : NULL);
          d->table_started = TRUE;
          w42_pt_table_set_borders (d->pt, d->b.table, d->tbl_borders);
          for (int e = 0; e < W42_N_EDGES; e++)
            w42_pt_table_set_edge (d->pt, d->b.table, e, &d->tbl_edge[e]);
        }
    }
  else if (g_str_equal (tag, "trHeight") && d->depth_tbl == 1 && d->table_started)
    {
      const char *rule = attr (an, av, "hRule");

      /* "exact" is treated as "at least": the text is never cut off. */
      if (rule == NULL || !g_str_equal (rule, "auto"))
        w42_pt_table_set_row_height (d->pt, d->b.table, d->b.row, attr_int (an, av, "val", 0));
    }
  else if (g_str_equal (tag, "tblHeader") && d->depth_tbl == 1 && d->table_started)
    {
      const char *val = attr (an, av, "val");

      if (val == NULL || g_str_equal (val, "1") || g_str_equal (val, "true") || g_str_equal (val, "on"))
        w42_pt_table_set_header_rows (d->pt, d->b.table, d->b.row + 1);
    }
  else if (g_str_equal (tag, "tc") && d->depth_tbl == 1)
    {
      d->drop_join = d->drop_pending = 0;
      d->cell_pending = TRUE;
      d->cell_span = 1;
      d->cell_sides = -1;
      d->cell_vmerge = 0;
      d->cell_fill = 0;
      d->cell_has_fill = FALSE;
      memset (d->cell_edge, 0, sizeof d->cell_edge);
      d->cell_edges_set = FALSE;
      d->cell_shading = 0;
      d->cell_valign = W42_CELL_VALIGN_TOP;
    }
  else if (g_str_equal (tag, "vAlign") && d->depth_tbl == 1 && d->cell_pending)
    {
      const char *val = attr (an, av, "val");

      d->cell_valign = val == NULL ? W42_CELL_VALIGN_TOP
                     : g_str_equal (val, "center") ? W42_CELL_VALIGN_CENTER
                     : g_str_equal (val, "bottom") ? W42_CELL_VALIGN_BOTTOM : W42_CELL_VALIGN_TOP;
    }
  else if (g_str_equal (tag, "tcBorders") && d->depth_tbl == 1 && d->cell_pending)
    {
      /* A side the cell does not name is the table's, not a rule of its
       * own, so the sides start where the table left them. */
      d->in_tcborders = TRUE;
      d->cell_sides = d->tbl_borders ? W42_BORDER_BOX : 0;
    }
  else if (d->in_tcborders && (g_str_equal (tag, "top") || g_str_equal (tag, "bottom") ||
                               g_str_equal (tag, "left") || g_str_equal (tag, "right") ||
                               g_str_equal (tag, "start") || g_str_equal (tag, "end")))
    {
      int e = border_edge_index (tag);
      W42BorderEdge edge;

      if (border_element (an, av, &edge))
        {
          d->cell_sides |= 1 << e;
          d->cell_edge[e] = edge;
          d->cell_edges_set = TRUE;
        }
      else
        d->cell_sides &= ~(1 << e);
    }
  else if (g_str_equal (tag, "shd") && d->depth_tbl == 1 && d->cell_pending &&
           !d->in_ppr && !d->in_rpr)
    {
      const char *fill = attr (an, av, "fill");
      const char *val = attr (an, av, "val");

      /* A pattern of black over the fill, "pct25", is a grey when the
       * fill is white or none; a fill is a colour. */
      if (val != NULL && g_str_has_prefix (val, "pct") &&
          (fill == NULL || g_str_equal (fill, "auto") || g_ascii_strcasecmp (fill, "FFFFFF") == 0))
        d->cell_shading = CLAMP (atoi (val + 3), 0, 100);
      else if (fill != NULL && !g_str_equal (fill, "auto"))
        {
          d->cell_fill = (guint32) strtoul (fill, NULL, 16) & 0xFFFFFF;
          d->cell_has_fill = TRUE;
        }
    }
  else if (g_str_equal (tag, "gridSpan") && d->depth_tbl == 1 && d->cell_pending)
    d->cell_span = CLAMP (attr_int (an, av, "val", 1), 1, 63);
  else if (g_str_equal (tag, "vMerge") && d->depth_tbl == 1 && d->cell_pending)
    {
      /* Without a value, the cell carries on the merge above it. */
      const char *val = attr (an, av, "val");

      d->cell_vmerge = (val != NULL && g_str_equal (val, "restart")) ? 1 : 2;
    }

  /* The colour behind the page, which stands outside the body. */
  else if (g_str_equal (tag, "background"))
    {
      const char *colour = attr (an, av, "color");

      if (colour != NULL && strlen (colour) >= 6 && d->page != NULL)
        {
          d->page->background = (guint32) strtoul (colour + (colour[0] == '#'), NULL, 16);
          d->page->has_background = 1;
        }
    }

  /* The body's own section properties: the page. */
  else if (g_str_equal (tag, "sectPr"))
    d->in_sectpr_body = TRUE;
  else if (d->in_sectpr_body)
    {
      if (g_str_equal (tag, "pgSz"))
        {
          d->page->width = CLAMP (attr_int (an, av, "w", d->page->width), 720, 31680);
          d->page->height = CLAMP (attr_int (an, av, "h", d->page->height), 720, 31680);
        }
      else if (g_str_equal (tag, "pgMar"))
        {
          d->page->margin_top = CLAMP (attr_int (an, av, "top", d->page->margin_top), 0, 31680);
          d->page->margin_bottom = CLAMP (attr_int (an, av, "bottom", d->page->margin_bottom), 0, 31680);
          d->page->margin_left = CLAMP (attr_int (an, av, "left", d->page->margin_left), 0, 31680);
          d->page->margin_right = CLAMP (attr_int (an, av, "right", d->page->margin_right), 0, 31680);
        }
      else if (g_str_equal (tag, "cols"))
        docx_apply_section_columns (d, attr_int (an, av, "num", 1), attr_int (an, av, "space", 720));
      else if (g_str_equal (tag, "titlePg"))
        w42_pt_set_title_page (d->pt, TRUE);
      else if (g_str_equal (tag, "evenAndOddHeaders"))
        w42_pt_set_facing_pages (d->pt, TRUE);
      else if (g_str_equal (tag, "headerReference") || g_str_equal (tag, "footerReference"))
        {
          const char *type = attr (an, av, "type");
          const char *id = attr (an, av, "id");
          const char *target = id != NULL ? g_hash_table_lookup (d->rels, id) : NULL;

          if (target != NULL && (type == NULL || g_str_equal (type, "default") ||
                                 g_str_equal (type, "first") || g_str_equal (type, "even")))
            {
              char *part = g_strconcat ("word/", target, NULL);
              PartText p = { .text = g_string_new (NULL), .align = W42_ALIGN_LEFT };

              if (parse_part (d->zip, part, &p) && p.text->len > 0)
                {
                  /* One line: the header is a line of text in word42. */
                  for (char *q = p.text->str; *q; q++)
                    if (*q == '\n') *q = ' ';
                  W42PageTextKind kind = type == NULL || g_str_equal (type, "default") ? W42_PAGE_TEXT_DEFAULT
                                       : g_str_equal (type, "first") ? W42_PAGE_TEXT_FIRST : W42_PAGE_TEXT_EVEN;

                  if (g_str_equal (tag, "headerReference"))
                    w42_pt_set_header_kind (d->pt, kind, p.text->str, p.align);
                  else
                    w42_pt_set_footer_kind (d->pt, kind, p.text->str, p.align);
                  if (kind == W42_PAGE_TEXT_FIRST)
                    w42_pt_set_title_page (d->pt, TRUE);
                  else if (kind == W42_PAGE_TEXT_EVEN)
                    w42_pt_set_facing_pages (d->pt, TRUE);
                }
              g_string_free (p.text, TRUE);
              g_free (part);
            }
        }
    }
}

static void
docx_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  Docx *d = data;
  const char *tag = local (name);

  (void) ctx; (void) error;

  if (d->skip_depth > 0)
    {
      d->skip_depth--;
      return;
    }
  if (d->shape_txbx)
    {
      if (g_str_equal (tag, "t"))
        d->in_t = FALSE;
      else if (g_str_equal (tag, "p"))
        g_string_append_c (d->shape_text, '\n');
      else if (g_str_equal (tag, "txbxContent"))
        {
          d->shape_txbx = FALSE;
          /* The last paragraph's break is not part of the label. */
          while (d->shape_text->len > 0 && d->shape_text->str[d->shape_text->len - 1] == '\n')
            g_string_truncate (d->shape_text, d->shape_text->len - 1);
        }
      return;
    }
  if (g_str_equal (tag, "t") || g_str_equal (tag, "delText"))
    {
      d->in_t = FALSE;
      docx_flush_text (d);
    }
  else if (d->in_drawing && (g_str_equal (tag, "positionH") || g_str_equal (tag, "positionV")))
    d->in_pos_h = d->in_pos_v = FALSE;
  else if (d->in_drawing && g_str_equal (tag, "posOffset"))
    d->in_offset = FALSE;
  else if (d->in_wsp && g_str_equal (tag, "ln"))
    d->in_ln = FALSE;
  else if (d->in_wsp && g_str_equal (tag, "style"))
    d->in_wsp_style = FALSE;
  else if (d->reading_notes &&
           (g_str_equal (tag, "footnote") || g_str_equal (tag, "endnote")))
    {
      /* The span ends where the note's last paragraph's text ends, before
       * the mark that closes it: what is copied into a note's body is
       * paragraphs of text, and the body already has a mark of its own. */
      docx_flush_text (d);
      if (d->note_open != NULL && !d->note_skipped && d->b.pos > d->note_from)
        {
          gsize end = d->b.pos;
          char *tail = w42_pt_get_text (d->pt, end - 1, 1);

          /* The mark the last paragraph's close left behind is not part of
           * the note: what goes into a note's body is paragraphs of text,
           * and the body has a mark of its own already.  Keeping it would
           * add an empty paragraph to every note, and another every time
           * the file was saved. */
          if (tail != NULL && *tail == '\n')
            end--;
          g_free (tail);

          if (end > d->note_from)
            {
              W42NoteSpan *span = g_new0 (W42NoteSpan, 1);

              span->start = d->note_from;
              span->end = end;
              span->pa = w42_ap_table_get (w42_pt_ap_table (d->pt),
                                           w42_pt_block_ap_at (d->pt, d->note_from))->pa;
              g_hash_table_insert (d->note_spans,
                                   g_strdup_printf ("%s%s", d->note_kind, d->note_open),
                                   span);
            }
        }
      if (d->b.in_para)
        w42_builder_end_paragraph (&d->b);
      g_free (d->note_open);
      d->note_open = NULL;
    }
  else if (g_str_equal (tag, "instrText"))
    d->in_instr = FALSE;
  else if (g_str_equal (tag, "fldSimple"))
    {
      docx_apply_field (d);
      d->fld_state = 0;
    }
  else if (g_str_equal (tag, "pPr"))
    {
      /* Word's alignment is relative to the paragraph's direction, and
       * both are known only now that its properties have been read. */
      d->in_ppr = FALSE;
      d->b.pa.align = mirror_align (d->b.pa.align, d->b.pa.rtl);
    }
  else if (g_str_equal (tag, "pBdr"))
    d->in_pbdr = FALSE;
  else if (g_str_equal (tag, "rPr"))
    d->in_rpr = FALSE;
  else if (g_str_equal (tag, "r"))
    docx_flush_text (d);
  else if (g_str_equal (tag, "txbxContent"))
    {
      if (d->txbx_depth == 1)
        {
          docx_flush_text (d);
          if (d->b.in_para)
            w42_builder_end_paragraph (&d->b);
          d->b.pa = d->txbx_saved_pa;
          d->txbx_reopened = TRUE;
        }
      if (d->txbx_depth > 0)
        d->txbx_depth--;
    }
  else if (g_str_equal (tag, "p"))
    {
      docx_flush_text (d);
      if (d->txbx_reopened && !d->b.in_para)
        {
          /* The paragraph the box hung on had nothing else in it. */
          d->txbx_reopened = FALSE;
          return;
        }
      d->txbx_reopened = FALSE;
      if (d->txbx_depth > 0)
        {
          d->b.pa.frame_side = (guint8) d->txbx_side;
          d->b.pa.frame_width = d->txbx_width;
        }
      if (d->cell_pending)
        {
          w42_builder_begin_cell (&d->b, d->cell_span);
          d->cell_pending = FALSE;
          docx_apply_cell_props (d);
        }
      if (d->drop_pending > 0)
        {
          /* Word keeps a dropped letter in a framed paragraph of its own:
           * the letter stays put and the next paragraph carries on from
           * it, dropping it. */
          d->drop_join = d->drop_pending;
          d->drop_pending = 0;
          return;
        }
      if (d->drop_join > 0)
        {
          d->b.pa.drop_cap = (guint8) d->drop_join;
          d->drop_join = 0;
        }
      w42_builder_end_paragraph (&d->b);
    }
  else if (g_str_equal (tag, "hyperlink"))
    {
      docx_flush_text (d);
      d->link = NULL;
    }
  else if (g_str_equal (tag, "ins") || g_str_equal (tag, "del"))
    {
      docx_flush_text (d);
      d->revision = 0;
    }
  else if (g_str_equal (tag, "drawing"))
    {
      const char *target = d->blip != NULL ? g_hash_table_lookup (d->rels, d->blip) : NULL;

      if (d->in_wsp)
        {
          /* A shape: drawn by word42 itself, so nothing to read but its
           * geometry, its outline, its fill and its text. */
          d->b.ch = d->run_ch;
          d->b.ch.link = d->link;
          d->b.ch.revision = (guint8) d->revision;
          w42_builder_shape (&d->b, d->shape,
                             (int) CLAMP (d->cx / EMU_PER_TWIP, 15, 31680),
                             (int) CLAMP (d->cy / EMU_PER_TWIP, 15, 31680),
                             d->has_line ? MAX (d->line_pt, 0.25) : 0.0, d->line_rgb,
                             d->filled, d->fill_rgb,
                             d->shape_text->len > 0 ? d->shape_text->str : NULL);
          if (d->anchored)
            {
              w42_builder_object_wrap (&d->b, d->wrap);
              if (d->pos_h_set || d->pos_v_set)
                w42_builder_object_position (&d->b, (int) (d->pos_x / EMU_PER_TWIP),
                                             (int) (d->pos_y / EMU_PER_TWIP));
            }
          g_string_truncate (d->shape_text, 0);
          d->in_wsp = FALSE;
          d->in_drawing = FALSE;
          return;
        }
      if (target != NULL)
        {
          char *part = target[0] == '/' ? g_strdup (target + 1) : g_strconcat ("word/", target, NULL);
          GBytes *bytes = w42_zip_read (d->zip, part);
          int pw = 0, ph = 0;
          const char *format = NULL;

          if (bytes != NULL && w42_image_probe (bytes, &pw, &ph, &format))
            {
              /* The picture's run carries a font and a size like any other,
               * and the line it sits on is as tall as they make it.  Only
               * flushing text picks the run's formatting up, and a run
               * holding a picture has none. */
              d->b.ch = d->run_ch;
              d->b.ch.link = d->link;
              d->b.ch.revision = (guint8) d->revision;
              w42_builder_object (&d->b, bytes, format, pw, ph,
                                  (int) CLAMP (d->cx / EMU_PER_TWIP, 0, 31680),
                                  (int) CLAMP (d->cy / EMU_PER_TWIP, 0, 31680));
              if (d->anchored)
                {
                  w42_builder_object_wrap (&d->b, d->wrap);
                  if (d->pos_h_set || d->pos_v_set)
                    w42_builder_object_position (&d->b, (int) (d->pos_x / EMU_PER_TWIP),
                                                 (int) (d->pos_y / EMU_PER_TWIP));
                }
            }
          if (bytes != NULL)
            g_bytes_unref (bytes);
          g_free (part);
        }
      d->in_drawing = FALSE;
    }
  else if (g_str_equal (tag, "tc") && d->depth_tbl == 1)
    {
      docx_flush_text (d);
      if (d->cell_pending)
        {
          w42_builder_begin_cell (&d->b, d->cell_span);
          d->cell_pending = FALSE;
          docx_apply_cell_props (d);
        }
      w42_builder_end_cell (&d->b);
    }
  else if (g_str_equal (tag, "tr") && d->depth_tbl == 1)
    w42_builder_end_row (&d->b);
  else if (g_str_equal (tag, "tblBorders"))
    d->in_tblborders = FALSE;
  else if (g_str_equal (tag, "tcBorders"))
    d->in_tcborders = FALSE;
  else if (g_str_equal (tag, "tbl") && d->depth_tbl > 0)
    {
      /* > 0: a stray tbl inside pPr was never counted on the way in, so
       * its close must not walk the depth below the tables that were. */
      if (d->depth_tbl == 1)
        {
          int table = d->b.table;

          w42_builder_end_table (&d->b);
          if (table >= 0)
            w42_pt_resolve_vmerges (d->pt, table);
        }
      d->depth_tbl--;
    }
  else if (g_str_equal (tag, "sectPr"))
    d->in_sectpr_body = FALSE;
}

static void
docx_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  Docx *d = data;

  (void) ctx; (void) error;
  if (d->in_instr)
    g_string_append_len (d->fld_instr, text, len);
  else if (d->shape_txbx)
    {
      if (d->in_t)
        g_string_append_len (d->shape_text, text, len);
    }
  else if (d->in_t)
    g_string_append_len (d->text, text, len);
  else if (d->in_offset)
    {
      char *copy = g_strndup (text, len);
      gint64 v = g_ascii_strtoll (copy, NULL, 10);

      if (d->in_pos_h) { d->pos_x = v; d->pos_h_set = TRUE; }
      if (d->in_pos_v) { d->pos_y = v; d->pos_v_set = TRUE; }
      g_free (copy);
    }
  else if (d->in_align)
    {
      /* wp:align under wp:positionH: which side the picture sits at. */
      if (len >= 5 && strncmp (text, "right", 5) == 0 && d->wrap != W42_WRAP_INLINE)
        d->wrap = W42_WRAP_RIGHT;
      d->in_align = FALSE;
    }
}

/* footnotes.xml and endnotes.xml read the way document.xml is, into a piece
 * table of their own: a note keeps its paragraphs, their formatting and
 * their runs, and the body's references copy the stretch that is theirs.
 * The two parts share one table, told apart by the "f" or "e" in front of
 * each note's id. */
static void
read_note_bodies (Docx *outer, W42Zip *zip, const char *part, const char *kind,
                  int which)
{
  GBytes *xml = w42_zip_read (zip, part);
  GMarkupParser parser = { docx_start, docx_end, docx_text, NULL, NULL };
  GMarkupParseContext *ctx;
  Docx d;

  if (xml == NULL)
    return;

  /* A reader of its own, over the same tables of styles, numbering and
   * relationships, writing into the notes' table. */
  memset (&d, 0, sizeof d);
  w42_builder_init (&d.b, outer->note_pt[which]);
  d.pt = outer->note_pt[which];
  d.page = outer->page;
  d.zip = zip;
  d.rels = outer->rels;
  d.styles = outer->styles;
  d.numbering = outer->numbering;
  /* A note can hold a picture, a comment or a field like any other text,
   * and the parser looks these up without asking whether they are there. */
  d.footnotes = outer->footnotes;
  d.endnotes = outer->endnotes;
  d.comments = outer->comments;
  d.comment_start = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  d.text = g_string_new (NULL);
  d.shape_text = g_string_new (NULL);
  d.fld_instr = g_string_new (NULL);
  d.grid = g_array_new (FALSE, FALSE, sizeof (int));
  d.bookmarks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  d.bookmark_start = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  d.section_first = (gsize) -1;
  d.first_para = TRUE;
  d.reading_notes = TRUE;
  d.note_kind = (char *) kind;
  d.note_spans = outer->note_spans;

  ctx = g_markup_parse_context_new (&parser, 0, &d, NULL);
  if (g_markup_parse_context_parse (ctx, g_bytes_get_data (xml, NULL),
                                    g_bytes_get_size (xml), NULL))
    g_markup_parse_context_end_parse (ctx, NULL);
  g_markup_parse_context_free (ctx);

  docx_flush_text (&d);
  w42_builder_finish (&d.b);

  g_string_free (d.text, TRUE);
  g_string_free (d.shape_text, TRUE);
  g_string_free (d.fld_instr, TRUE);
  g_array_free (d.grid, TRUE);
  g_hash_table_destroy (d.bookmarks);
  g_hash_table_destroy (d.bookmark_start);
  g_hash_table_destroy (d.comment_start);
  g_free (d.note_open);
  g_free (d.blip);
  g_bytes_unref (xml);
}

gboolean
w42_docx_load (W42PieceTable *pt, W42PageSetup *page, GFile *file, GError **error)
{
  W42Zip *zip;
  GBytes *xml;
  Docx d;
  GMarkupParser parser = { docx_start, docx_end, docx_text, NULL, NULL };
  GMarkupParseContext *ctx;
  W42PageSetup local_page;
  gboolean ok;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  zip = w42_zip_open (file, error);
  if (zip == NULL)
    return FALSE;
  xml = w42_zip_read (zip, "word/document.xml");
  if (xml == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "The file is not a Word document: it has no word/document.xml.");
      w42_zip_free (zip);
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

  memset (&d, 0, sizeof d);
  w42_builder_init (&d.b, pt);
  d.pt = pt;
  d.page = page;
  d.zip = zip;
  d.rels = read_rels (zip, "word/_rels/document.xml.rels");
  d.styles = read_styles (zip, w42_pt_stylesheet (pt));
  read_core_props (zip, pt);
  d.numbering = read_numbering (zip);
  d.footnotes = read_notes (zip, "word/footnotes.xml");
  d.endnotes = read_notes (zip, "word/endnotes.xml");
  d.comments = read_notes (zip, "word/comments.xml");
  d.comment_start = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  /* The notes' own parts, read the way the document is, after everything
   * their parser may look something up in. */
  d.note_pt[0] = w42_pt_new ();
  d.note_pt[1] = w42_pt_new ();
  d.note_spans = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  read_note_bodies (&d, zip, "word/footnotes.xml", "f", 0);
  read_note_bodies (&d, zip, "word/endnotes.xml", "e", 1);
  d.text = g_string_new (NULL);
  d.shape_text = g_string_new (NULL);
  d.fld_instr = g_string_new (NULL);
  d.grid = g_array_new (FALSE, FALSE, sizeof (int));
  d.bookmarks = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  d.bookmark_start = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  d.section_first = (gsize) -1;
  d.first_para = TRUE;

  ctx = g_markup_parse_context_new (&parser, 0, &d, NULL);
  ok = g_markup_parse_context_parse (ctx, g_bytes_get_data (xml, NULL), g_bytes_get_size (xml), error) &&
       g_markup_parse_context_end_parse (ctx, error);
  g_markup_parse_context_free (ctx);

  docx_flush_text (&d);
  w42_builder_finish (&d.b);
  w42_pt_clear_undo (pt);

  g_string_free (d.text, TRUE);
  g_string_free (d.shape_text, TRUE);
  g_string_free (d.fld_instr, TRUE);
  g_array_free (d.grid, TRUE);
  g_hash_table_destroy (d.bookmarks);
  g_hash_table_destroy (d.bookmark_start);
  g_hash_table_destroy (d.rels);
  g_hash_table_destroy (d.styles);
  g_hash_table_destroy (d.numbering);
  g_hash_table_destroy (d.footnotes);
  g_hash_table_destroy (d.endnotes);
  g_hash_table_destroy (d.note_spans);
  w42_pt_free (d.note_pt[0]);
  w42_pt_free (d.note_pt[1]);
  g_hash_table_destroy (d.comments);
  g_hash_table_destroy (d.comment_start);
  g_free (d.blip);
  g_bytes_unref (xml);
  w42_zip_free (zip);
  return ok;
}

/* ====================================================================== */
/* Writing                                                                 */
/* ====================================================================== */

#define W_NS "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" " \
             "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" " \
             "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" " \
             "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" " \
             "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\" "              "xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\""

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
        if ((guchar) text[i] < 0x20 && text[i] != '\t')
          break;
        g_string_append_c (out, text[i]);
      }
}

/* The author's name for comments and revisions, XML-safe; and initials. */
static const char *
author_xml (W42PieceTable *pt)
{
  const char *name = w42_pt_get_author (pt);
  static char buf[128];
  GString *tmp;

  if (name == NULL || *name == '\0')
    return "Word42";
  tmp = g_string_new (NULL);
  xml_escape (tmp, name, strlen (name));
  g_strlcpy (buf, tmp->str, sizeof buf);
  g_string_free (tmp, TRUE);
  return buf;
}

static const char *
author_initials (W42PieceTable *pt)
{
  const char *name = w42_pt_get_author (pt);
  static char buf[16];
  gsize n = 0;
  gboolean at_start = TRUE;

  if (name == NULL || *name == '\0')
    return "w";
  for (const char *p = name; *p != '\0' && n < sizeof buf - 5; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isalpha (c) && at_start)
        n += g_unichar_to_utf8 (g_unichar_toupper (c), buf + n);
      at_start = g_unichar_isspace (c);
    }
  buf[n] = '\0';
  return n > 0 ? buf : "w";
}

typedef struct {
  GString   *rels;          /* document.xml.rels body */
  int        next_rel;
  GPtrArray *media_names;   /* char*, in word/media */
  GPtrArray *media_data;    /* GBytes* */
  GHashTable *link_rels;    /* url -> rId (owned strings) */
  gboolean   have_lists[W42_LIST_KINDS];
  int        bookmark_id;
  int        revision_id;
  int        image_id;
  const char *header_rid, *footer_rid;   /* every section's */
  const char *header_first_rid, *footer_first_rid;
  const char *header_even_rid, *footer_even_rid;
  gboolean    title_page, facing_pages;
  GString   *comments;      /* comments.xml body */
  int        comment_id;
} Parts;

static char *
add_rel (Parts *parts, const char *type, const char *target, gboolean external)
{
  char *id = g_strdup_printf ("rId%d", parts->next_rel++);

  g_string_append_printf (parts->rels,
    "<Relationship Id=\"%s\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/%s\" Target=\"%s\"%s/>",
    id, type, target, external ? " TargetMode=\"External\"" : "");
  return id;
}

static const char *
style_id (const char *name)
{
  /* "Heading 1" -> "Heading1", as Word names them; letters and digits
   * only, so the id needs no escaping. */
  static char buf[64];
  gsize n = 0;

  for (const char *p = name != NULL ? name : "Normal"; *p && n + 1 < sizeof buf; p++)
    if (g_ascii_isalnum (*p))
      buf[n++] = *p;
  if (n == 0)
    g_strlcpy (buf, "Style", sizeof buf);
  else
    buf[n] = '\0';
  return buf;
}

static void
append_rfonts (GString *out, const char *family)
{
  GString *esc = g_string_new (NULL);

  xml_escape (esc, family, strlen (family));
  g_string_append_printf (out, "<w:rFonts w:ascii=\"%s\" w:hAnsi=\"%s\" w:cs=\"%s\"/>",
                          esc->str, esc->str, esc->str);
  g_string_free (esc, TRUE);
}

static void
write_rpr (GString *out, const W42CharFmt *ch, const W42CharFmt *base)
{
  GString *rpr = g_string_new (NULL);

  if (ch->family != NULL && ch->family != base->family)
    append_rfonts (rpr, ch->family);
  if (ch->bold)      g_string_append (rpr, "<w:b/>");
  if (ch->italic)    g_string_append (rpr, "<w:i/>");
  if (ch->allcaps)   g_string_append (rpr, "<w:caps/>");
  if (ch->smallcaps) g_string_append (rpr, "<w:smallCaps/>");
  if (ch->strikeout) g_string_append (rpr, "<w:strike/>");
  if (ch->color != 0)
    g_string_append_printf (rpr, "<w:color w:val=\"%06X\"/>", ch->color);
  if (ch->spacing != 0)
    g_string_append_printf (rpr, "<w:spacing w:val=\"%d\"/>", ch->spacing);
  if (ch->size != base->size && ch->size > 0)
    g_string_append_printf (rpr, "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>", ch->size, ch->size);
  if (ch->highlight != 0)
    g_string_append_printf (rpr, "<w:highlight w:val=\"%s\"/>", HIGHLIGHT_NAMES[CLAMP (ch->highlight, 1, 16)]);
  if (ch->underline)
    g_string_append_printf (rpr, "<w:u w:val=\"%s\"/>",
                            underline_val (ch->underline));
  if (ch->script > 0) g_string_append (rpr, "<w:vertAlign w:val=\"superscript\"/>");
  if (ch->script < 0) g_string_append (rpr, "<w:vertAlign w:val=\"subscript\"/>");
  if (ch->lang != NULL)
    {
      /* Word marks a run that is not to be checked with noProof rather
       * than with a language of its own. */
      if (g_strcmp0 (ch->lang, W42_LANG_NONE) == 0)
        g_string_append (rpr, "<w:noProof/>");
      else
        g_string_append_printf (rpr, "<w:lang w:val=\"%s\"/>", ch->lang);
    }

  if (rpr->len > 0)
    g_string_append_printf (out, "<w:rPr>%s</w:rPr>", rpr->str);
  g_string_free (rpr, TRUE);
}

/* The run's text as w:t, w:tab and w:br elements. */
static void
write_run_text (GString *out, const char *text, gsize len, gboolean deleted)
{
  const char *tag = deleted ? "w:delText" : "w:t";
  gsize start = 0;

  for (gsize i = 0; i <= len; i++)
    {
      gboolean is_tab = i < len && text[i] == '\t';
      gboolean is_br = i + 2 < len && (guchar) text[i] == 0xE2 && (guchar) text[i + 1] == 0x80 && (guchar) text[i + 2] == 0xA8;
      gboolean is_shy = i + 1 < len && (guchar) text[i] == 0xC2 && (guchar) text[i + 1] == 0xAD;

      if (i == len || is_tab || is_br || is_shy)
        {
          if (i > start)
            {
              g_string_append_printf (out, "<%s xml:space=\"preserve\">", tag);
              xml_escape (out, text + start, i - start);
              g_string_append_printf (out, "</%s>", tag);
            }
          if (is_tab)
            g_string_append (out, "<w:tab/>");
          if (is_br)
            {
              g_string_append (out, "<w:br/>");
              i += 2;
            }
          if (is_shy)
            {
              g_string_append (out, "<w:softHyphen/>");
              i += 1;
            }
          start = i + 1;
        }
    }
}

static void
write_drawing (GString *out, Parts *parts, W42PieceTable *pt, const W42Run *run,
               const W42CharFmt *ch, const W42CharFmt *base)
{
  const W42Object *object = w42_object_table_get (w42_pt_object_table (pt), run->object);
  GBytes *bytes;
  const char *ext = "png";
  char *name, *target, *rid;
  gint64 cx, cy;
  int id;

  if (object == NULL)
    return;
  if (object->shape == W42_SHAPE_PICTURE)
    {
      bytes = w42_image_for_container (object->data, &ext, NULL);
      if (bytes == NULL)
        return;
    }
  else
    bytes = NULL;

  id = ++parts->image_id;
  name = g_strdup_printf ("image%d.%s", id, ext);
  target = g_strconcat ("media/", name, NULL);
  if (bytes != NULL)
    {
      rid = add_rel (parts, "image", target, FALSE);
      g_ptr_array_add (parts->media_names, name);
      g_ptr_array_add (parts->media_data, bytes);
    }
  else
    rid = NULL;
  cx = (gint64) object->width * EMU_PER_TWIP;
  cy = (gint64) object->height * EMU_PER_TWIP;

  /* The picture's run carries a font and a size like any other, and the
   * line it sits on is as tall as they make it, so they go out with it. */
  g_string_append (out, "<w:r>");
  write_rpr (out, ch, base);
  if (object->wrap == W42_WRAP_INLINE)
    g_string_append_printf (out,
      "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
      "<wp:extent cx=\"%" G_GINT64_FORMAT "\" cy=\"%" G_GINT64_FORMAT "\"/>"
      "<wp:docPr id=\"%d\" name=\"Picture %d\"/>",
      cx, cy, id, id);
  else
    {
      /* Anchored at the paragraph's top, against the column's side, with
       * the text running down the other side; or where it was put. */
      const char *wrap = object->wrap == W42_WRAP_TOP_BOTTOM ? "<wp:wrapTopAndBottom/>"
                       : object->wrap == W42_WRAP_FRONT || object->wrap == W42_WRAP_BEHIND ? "<wp:wrapNone/>"
                       : "<wp:wrapSquare wrapText=\"bothSides\"/>";

      g_string_append_printf (out,
        "<w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"114300\" distR=\"114300\" "
        "simplePos=\"0\" relativeHeight=\"%d\" behindDoc=\"%d\" locked=\"0\" layoutInCell=\"1\" allowOverlap=\"1\">"
        "<wp:simplePos x=\"0\" y=\"0\"/>",
        251658240 + id, object->wrap == W42_WRAP_BEHIND ? 1 : 0);
      if (object->positioned)
        g_string_append_printf (out,
          "<wp:positionH relativeFrom=\"column\"><wp:posOffset>%" G_GINT64_FORMAT "</wp:posOffset></wp:positionH>"
          "<wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>%" G_GINT64_FORMAT "</wp:posOffset></wp:positionV>",
          (gint64) object->pos_x * EMU_PER_TWIP, (gint64) object->pos_y * EMU_PER_TWIP);
      else
        g_string_append_printf (out,
          "<wp:positionH relativeFrom=\"column\"><wp:align>%s</wp:align></wp:positionH>"
          "<wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>0</wp:posOffset></wp:positionV>",
          object->wrap == W42_WRAP_RIGHT ? "right" : "left");
      g_string_append_printf (out,
        "<wp:extent cx=\"%" G_GINT64_FORMAT "\" cy=\"%" G_GINT64_FORMAT "\"/>"
        "<wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>%s"
        "<wp:docPr id=\"%d\" name=\"Picture %d\"/>",
        cx, cy, wrap, id, id);
    }

  if (object->shape != W42_SHAPE_PICTURE)
    {
      /* A shape, as Word 2010 and later say one: its geometry, its fill,
       * its outline and the text in it. */
      const char *prst = object->shape == W42_SHAPE_ELLIPSE ? "ellipse"
                       : object->shape == W42_SHAPE_ROUNDED_RECTANGLE ? "roundRect"
                       : object->shape == W42_SHAPE_LINE || object->shape == W42_SHAPE_ARROW ? "line"
                       : "rect";

      g_string_append_printf (out,
        "<wp:cNvGraphicFramePr/><a:graphic><a:graphicData uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\">"
        "<wps:wsp><wps:cNvSpPr/><wps:spPr>"
        "<a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%" G_GINT64_FORMAT "\" cy=\"%" G_GINT64_FORMAT "\"/></a:xfrm>"
        "<a:prstGeom prst=\"%s\"><a:avLst/></a:prstGeom>",
        cx, cy, prst);
      if (object->filled)
        g_string_append_printf (out, "<a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill>", object->fill_rgb);
      else
        g_string_append (out, "<a:noFill/>");
      if (object->line_pt > 0.0)
        {
          g_string_append_printf (out, "<a:ln w=\"%d\"><a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill>",
                                  (int) (object->line_pt * 12700.0 + 0.5), object->line_rgb);
          if (object->shape == W42_SHAPE_ARROW)
            g_string_append (out, "<a:tailEnd type=\"triangle\"/>");
          g_string_append (out, "</a:ln>");
        }
      else
        g_string_append (out, "<a:ln><a:noFill/></a:ln>");
      g_string_append (out, "</wps:spPr>");
      if (object->text != NULL)
        {
          char **lines = g_strsplit (object->text, "\n", -1);

          g_string_append (out, "<wps:txbx><w:txbxContent>");
          for (int i = 0; lines[i] != NULL; i++)
            {
              g_string_append (out, "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r><w:t xml:space=\"preserve\">");
              xml_escape (out, lines[i], strlen (lines[i]));
              g_string_append (out, "</w:t></w:r></w:p>");
            }
          g_string_append (out, "</w:txbxContent></wps:txbx>");
          g_strfreev (lines);
        }
      g_string_append_printf (out,
        "<wps:bodyPr anchor=\"ctr\" wrap=\"square\" rtlCol=\"0\"><a:noAutofit/></wps:bodyPr></wps:wsp>"
        "</a:graphicData></a:graphic>%s</w:drawing></w:r>",
        object->wrap == W42_WRAP_INLINE ? "</wp:inline>" : "</wp:anchor>");
    }
  else
    g_string_append_printf (out,
      "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
      "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"%s\"/><pic:cNvPicPr/></pic:nvPicPr>"
      "<pic:blipFill><a:blip r:embed=\"%s\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
      "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%" G_GINT64_FORMAT "\" cy=\"%" G_GINT64_FORMAT "\"/></a:xfrm>"
      "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>"
      "</a:graphicData></a:graphic>%s</w:drawing></w:r>",
      id, name, rid, cx, cy, object->wrap == W42_WRAP_INLINE ? "</wp:inline>" : "</wp:anchor>");

  if (bytes == NULL)
    g_free (name);
  g_free (rid);
  g_free (target);
}

static void
write_runs (GString *out, Parts *parts, W42PieceTable *pt, W42ApTable *aps,
            const W42Block *block, const W42CharFmt *base)
{
  const char *open_link = NULL;
  const char *open_bookmark = NULL;
  const char *open_comment = NULL;
  const char *open_field = NULL;
  int open_revision = 0;

  for (guint i = 0; i < block->runs->len; i++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, i);
      const W42CharFmt *ch = &w42_ap_table_get (aps, run->ap)->ch;

      if (open_field != NULL && ch->field != open_field)
        {
          g_string_append (out, "</w:fldSimple>");
          open_field = NULL;
        }
      if (ch->field != NULL && open_field == NULL && run->object == W42_OBJECT_NONE && run->footnote == 0)
        {
          GString *esc = g_string_new (NULL);

          /* An XE code carries the file's own term: escaped, or a quote
           * in it would close the attribute and write markup of its own. */
          xml_escape (esc, ch->field, strlen (ch->field));
          g_string_append_printf (out, "<w:fldSimple w:instr=\" %s \">", esc->str);
          g_string_free (esc, TRUE);
          open_field = ch->field;
        }
      if (open_comment != NULL && ch->comment != open_comment)
        {
          g_string_append_printf (out, "<w:commentRangeEnd w:id=\"%d\"/><w:r><w:commentReference w:id=\"%d\"/></w:r>",
                                  parts->comment_id, parts->comment_id);
          open_comment = NULL;
        }
      if (ch->comment != NULL && open_comment == NULL && parts->comments != NULL)
        {
          parts->comment_id++;
          g_string_append_printf (out, "<w:commentRangeStart w:id=\"%d\"/>", parts->comment_id);
          g_string_append_printf (parts->comments,
            "<w:comment w:id=\"%d\" w:author=\"%s\" w:date=\"2026-01-01T00:00:00Z\" w:initials=\"%s\"><w:p><w:r><w:t xml:space=\"preserve\">",
            parts->comment_id, author_xml (pt), author_initials (pt));
          xml_escape (parts->comments, ch->comment, strlen (ch->comment));
          g_string_append (parts->comments, "</w:t></w:r></w:p></w:comment>");
          open_comment = ch->comment;
        }

      /* Bookmarks, links and revisions wrap runs; close what changed,
       * then open what is new. */
      if (open_revision != 0 && ch->revision != open_revision)
        {
          g_string_append (out, open_revision == 1 ? "</w:ins>" : "</w:del>");
          open_revision = 0;
        }
      if (open_link != NULL && ch->link != open_link)
        {
          g_string_append (out, "</w:hyperlink>");
          open_link = NULL;
        }
      if (open_bookmark != NULL && ch->bookmark != open_bookmark)
        {
          g_string_append_printf (out, "<w:bookmarkEnd w:id=\"%d\"/>", parts->bookmark_id);
          open_bookmark = NULL;
        }
      if (ch->bookmark != NULL && open_bookmark == NULL)
        {
          g_string_append_printf (out, "<w:bookmarkStart w:id=\"%d\" w:name=\"", ++parts->bookmark_id);
          xml_escape (out, ch->bookmark, strlen (ch->bookmark));
          g_string_append (out, "\"/>");
          open_bookmark = ch->bookmark;
        }
      if (ch->link != NULL && open_link == NULL)
        {
          if (ch->link[0] == '#')
            {
              g_string_append (out, "<w:hyperlink w:anchor=\"");
              xml_escape (out, ch->link + 1, strlen (ch->link + 1));
              g_string_append (out, "\">");
            }
          else
            {
              char *rid = g_hash_table_lookup (parts->link_rels, ch->link);

              if (rid == NULL)
                {
                  GString *esc = g_string_new (NULL);

                  xml_escape (esc, ch->link, strlen (ch->link));
                  rid = add_rel (parts, "hyperlink", esc->str, TRUE);
                  g_hash_table_insert (parts->link_rels, g_strdup (ch->link), rid);
                  g_string_free (esc, TRUE);
                }
              g_string_append_printf (out, "<w:hyperlink r:id=\"%s\">", rid);
            }
          open_link = ch->link;
        }
      if (ch->revision != 0 && open_revision == 0)
        {
          g_string_append_printf (out, "<w:%s w:id=\"%d\" w:author=\"%s\" w:date=\"2026-01-01T00:00:00Z\">",
                                  ch->revision == 1 ? "ins" : "del", ++parts->revision_id, author_xml (pt));
          open_revision = ch->revision;
        }

      if (run->object != W42_OBJECT_NONE)
        write_drawing (out, parts, pt, run, ch, base);
      else if (run->footnote > 0)
        g_string_append_printf (out,
          "<w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:%sReference w:id=\"%d\"/></w:r>",
          run->endnote ? "endnote" : "footnote", run->footnote_id + 1);
      else
        {
          g_string_append (out, "<w:r>");
          write_rpr (out, ch, base);
          write_run_text (out, block->text->str + run->byte_offset, run->n_bytes, ch->revision == 2);
          g_string_append (out, "</w:r>");
        }
    }
  if (open_revision != 0)
    g_string_append (out, open_revision == 1 ? "</w:ins>" : "</w:del>");
  if (open_link != NULL)
    g_string_append (out, "</w:hyperlink>");
  if (open_bookmark != NULL)
    g_string_append_printf (out, "<w:bookmarkEnd w:id=\"%d\"/>", parts->bookmark_id);
  if (open_comment != NULL)
    g_string_append_printf (out, "<w:commentRangeEnd w:id=\"%d\"/><w:r><w:commentReference w:id=\"%d\"/></w:r>",
                            parts->comment_id, parts->comment_id);
  if (open_field != NULL)
    g_string_append (out, "</w:fldSimple>");
}

static void
write_sectpr (GString *out, const W42PageSetup *page, int cols, int gap,
              const char *header_rid, const char *footer_rid,
              const Parts *parts)
{
  g_string_append (out, "<w:sectPr>");
  if (parts != NULL && parts->header_first_rid != NULL)
    g_string_append_printf (out, "<w:headerReference w:type=\"first\" r:id=\"%s\"/>", parts->header_first_rid);
  if (parts != NULL && parts->footer_first_rid != NULL)
    g_string_append_printf (out, "<w:footerReference w:type=\"first\" r:id=\"%s\"/>", parts->footer_first_rid);
  if (parts != NULL && parts->header_even_rid != NULL)
    g_string_append_printf (out, "<w:headerReference w:type=\"even\" r:id=\"%s\"/>", parts->header_even_rid);
  if (parts != NULL && parts->footer_even_rid != NULL)
    g_string_append_printf (out, "<w:footerReference w:type=\"even\" r:id=\"%s\"/>", parts->footer_even_rid);
  if (header_rid != NULL)
    g_string_append_printf (out, "<w:headerReference w:type=\"default\" r:id=\"%s\"/>", header_rid);
  if (footer_rid != NULL)
    g_string_append_printf (out, "<w:footerReference w:type=\"default\" r:id=\"%s\"/>", footer_rid);
  g_string_append_printf (out, "<w:pgSz w:w=\"%d\" w:h=\"%d\"/>", page->width, page->height);
  g_string_append_printf (out, "<w:pgMar w:top=\"%d\" w:right=\"%d\" w:bottom=\"%d\" w:left=\"%d\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>",
                          page->margin_top, page->margin_right, page->margin_bottom, page->margin_left);
  if (cols > 1)
    g_string_append_printf (out, "<w:cols w:num=\"%d\" w:space=\"%d\"/>", cols, gap);
  if (parts != NULL && parts->title_page)
    g_string_append (out, "<w:titlePg/>");
  g_string_append (out, "</w:sectPr>");
}

static void
write_ppr (GString *out, Parts *parts, const W42ParaFmt *pa, const W42PageSetup *page,
           gboolean ends_section, int sect_cols, int sect_gap)
{
  g_string_append (out, "<w:pPr>");
  if (pa->style != NULL && g_ascii_strcasecmp (pa->style, "Normal") != 0)
    g_string_append_printf (out, "<w:pStyle w:val=\"%s\"/>", style_id (pa->style));
  if (pa->keep_next)     g_string_append (out, "<w:keepNext/>");
  if (pa->keep_together) g_string_append (out, "<w:keepLines/>");
  if (pa->page_break_before) g_string_append (out, "<w:pageBreakBefore/>");
  if (pa->frame_side != W42_FRAME_NONE)
    g_string_append_printf (out, "<w:framePr w:w=\"%d\" w:wrap=\"around\" w:vAnchor=\"text\" w:hAnchor=\"margin\" w:xAlign=\"%s\" w:y=\"1\"/>",
                            pa->frame_width > 0 ? pa->frame_width : 3120,
                            pa->frame_side == W42_FRAME_LEFT ? "left" : "right");
  if (!pa->widow_control) g_string_append (out, "<w:widowControl w:val=\"0\"/>");
  if (pa->list != W42_LIST_NONE)
    {
      parts->have_lists[pa->list] = TRUE;
      g_string_append_printf (out, "<w:numPr><w:ilvl w:val=\"%d\"/><w:numId w:val=\"%d\"/></w:numPr>",
                              MIN (pa->list_level, 8), (int) pa->list);
    }
  if (pa->rtl) g_string_append (out, "<w:bidi/>");
  if (pa->border != 0)
    {
      /* Word wants them in this order. */
      static const int order[4] = { W42_EDGE_TOP, W42_EDGE_LEFT, W42_EDGE_BOTTOM, W42_EDGE_RIGHT };

      g_string_append (out, "<w:pBdr>");
      for (int i = 0; i < 4; i++)
        if (pa->border & (1 << order[i]))
          write_border_element (out, order[i], &pa->edge[order[i]], TRUE, 1);
      g_string_append (out, "</w:pBdr>");
    }
  if (pa->has_shading_color)
    g_string_append_printf (out, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06X\"/>",
                            pa->shading_color & 0xFFFFFF);
  else if (pa->shading > 0)
    {
      int grey = 255 - pa->shading * 255 / 100;

      g_string_append_printf (out, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%02X%02X%02X\"/>", grey, grey, grey);
    }
  if (pa->n_tabs > 0)
    {
      g_string_append (out, "<w:tabs>");
      for (int i = 0; i < pa->n_tabs; i++)
        g_string_append_printf (out, "<w:tab w:val=\"%s\"%s w:pos=\"%d\"/>",
                                W42_TAB_KIND (pa->tab_kind[i]) == W42_TAB_CENTER ? "center"
                                : W42_TAB_KIND (pa->tab_kind[i]) == W42_TAB_RIGHT ? "right"
                                : W42_TAB_KIND (pa->tab_kind[i]) == W42_TAB_DECIMAL ? "decimal" : "left",
                                W42_TAB_LEADER (pa->tab_kind[i]) == W42_TAB_LEAD_DOT ? " w:leader=\"dot\""
                                : W42_TAB_LEADER (pa->tab_kind[i]) == W42_TAB_LEAD_DASH ? " w:leader=\"hyphen\""
                                : W42_TAB_LEADER (pa->tab_kind[i]) == W42_TAB_LEAD_LINE ? " w:leader=\"underscore\"" : "",
                                pa->tab_pos[i]);
      g_string_append (out, "</w:tabs>");
    }
  if (pa->rtl) g_string_append (out, "<w:bidi/>");
  if (pa->space_before || pa->space_after || pa->line_spacing_pct > 0 || pa->line_spacing > 0)
    {
      g_string_append (out, "<w:spacing");
      if (pa->space_before) g_string_append_printf (out, " w:before=\"%d\"", pa->space_before);
      if (pa->space_after)  g_string_append_printf (out, " w:after=\"%d\"", pa->space_after);
      if (pa->line_spacing_pct > 0)
        g_string_append_printf (out, " w:line=\"%d\" w:lineRule=\"auto\"", pa->line_spacing_pct * 240 / 100);
      else if (pa->line_spacing > 0)
        g_string_append_printf (out, " w:line=\"%d\" w:lineRule=\"exact\"", pa->line_spacing);
      g_string_append (out, "/>");
    }
  if (pa->indent_left || pa->indent_right || pa->indent_first)
    {
      g_string_append (out, "<w:ind");
      if (pa->indent_left)  g_string_append_printf (out, " w:left=\"%d\"", pa->indent_left);
      if (pa->indent_right) g_string_append_printf (out, " w:right=\"%d\"", pa->indent_right);
      if (pa->indent_first > 0) g_string_append_printf (out, " w:firstLine=\"%d\"", pa->indent_first);
      if (pa->indent_first < 0) g_string_append_printf (out, " w:hanging=\"%d\"", -pa->indent_first);
      g_string_append (out, "/>");
    }
  switch (mirror_align (pa->align, pa->rtl))
    {
    case W42_ALIGN_CENTER:  g_string_append (out, "<w:jc w:val=\"center\"/>"); break;
    case W42_ALIGN_RIGHT:   g_string_append (out, "<w:jc w:val=\"right\"/>"); break;
    case W42_ALIGN_JUSTIFY: g_string_append (out, "<w:jc w:val=\"both\"/>"); break;
    case W42_ALIGN_LEFT:
      /* In a right-to-left paragraph Word's "left" is the right margin,
       * so it is worth saying even though it is the default. */
      if (pa->rtl)
        g_string_append (out, "<w:jc w:val=\"left\"/>");
      break;
    default: break;
    }
  if (ends_section)
    write_sectpr (out, page, sect_cols, sect_gap, parts->header_rid, parts->footer_rid, parts);
  g_string_append (out, "</w:pPr>");
}

static void
write_paragraph (GString *out, Parts *parts, W42PieceTable *pt, W42ApTable *aps,
                 const W42Block *block, const W42CharFmt *base, const W42PageSetup *page,
                 gboolean ends_section, int sect_cols, int sect_gap)
{
  const W42ParaFmt *pa = &w42_ap_table_get (aps, block->ap)->pa;

  /* A dropped letter goes in a framed paragraph of its own, as Word keeps
   * it, and the paragraph follows without it. */
  if (pa->drop_cap > 0 && block->text->len > 0 && block->runs->len > 0 &&
      g_array_index (block->runs, W42Run, 0).object == W42_OBJECT_NONE &&
      g_array_index (block->runs, W42Run, 0).footnote == 0 &&
      g_unichar_isgraph (g_utf8_get_char (block->text->str)))
    {
      const char *text = block->text->str;
      guint k = (guint) (g_utf8_next_char (text) - text);
      const W42CharFmt *ch = &w42_ap_table_get (aps, g_array_index (block->runs, W42Run, 0).ap)->ch;
      int size = (ch->size > 0 ? ch->size : 20) * pa->drop_cap * 12 / 10;
      W42Block trimmed = *block;

      g_string_append_printf (out, "<w:p><w:pPr><w:framePr w:dropCap=\"drop\" w:lines=\"%d\" w:wrap=\"around\" w:vAnchor=\"text\" w:hAnchor=\"text\"/>",
                              pa->drop_cap);
      if (pa->style != NULL && g_ascii_strcasecmp (pa->style, "Normal") != 0)
        g_string_append_printf (out, "<w:pStyle w:val=\"%s\"/>", style_id (pa->style));
      g_string_append (out, "</w:pPr><w:r><w:rPr>");
      if (ch->family != NULL)
        append_rfonts (out, ch->family);
      g_string_append_printf (out, "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/></w:rPr><w:t>", size, size);
      xml_escape (out, text, k);
      g_string_append (out, "</w:t></w:r></w:p>");

      trimmed.text = g_string_new (text + k);
      trimmed.runs = g_array_copy (block->runs);
      for (guint i = 0; i < trimmed.runs->len; i++)
        {
          W42Run *run = &g_array_index (trimmed.runs, W42Run, i);

          if (run->byte_offset >= k)
            run->byte_offset -= k;
          else
            {
              run->n_bytes -= MIN (run->n_bytes, k - run->byte_offset);
              run->n_chars -= MIN (run->n_chars, 1);
              run->doc_pos += 1;
              run->byte_offset = 0;
            }
        }
      trimmed.start_pos = block->start_pos;
      g_string_append (out, "<w:p>");
      write_ppr (out, parts, pa, page, ends_section, sect_cols, sect_gap);
      write_runs (out, parts, pt, aps, &trimmed, base);
      g_string_append (out, "</w:p>");
      g_string_free (trimmed.text, TRUE);
      g_array_free (trimmed.runs, TRUE);
      return;
    }

  g_string_append (out, "<w:p>");
  write_ppr (out, parts, pa, page, ends_section, sect_cols, sect_gap);
  write_runs (out, parts, pt, aps, block, base);
  g_string_append (out, "</w:p>");
}

/* One part holding paragraphs of text: a header or footer. */
static char *
page_text_part (const W42PageText *text)
{
  GString *out = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
  const char *jc = text->align == W42_ALIGN_CENTER ? "center"
                 : text->align == W42_ALIGN_RIGHT ? "right" : NULL;

  g_string_append (out, "<w:hdr " W_NS ">");
  g_string_append (out, "<w:p><w:pPr>");
  if (jc != NULL)
    g_string_append_printf (out, "<w:jc w:val=\"%s\"/>", jc);
  g_string_append (out, "</w:pPr>");

  /* {PAGE} and {NUMPAGES} become fields; the rest is text. */
  {
    const char *p = text->text;

    while (*p)
      {
        const char *brace = strchr (p, '{');
        const char *close = brace != NULL ? strchr (brace, '}') : NULL;

        if (brace == NULL || close == NULL)
          {
            g_string_append (out, "<w:r>");
            write_run_text (out, p, strlen (p), FALSE);
            g_string_append (out, "</w:r>");
            break;
          }
        if (brace > p)
          {
            g_string_append (out, "<w:r>");
            write_run_text (out, p, brace - p, FALSE);
            g_string_append (out, "</w:r>");
          }
        {
          char *field = g_strndup (brace + 1, close - brace - 1);
          const char *instr = g_ascii_strcasecmp (field, "PAGE") == 0 ? "PAGE"
                            : g_ascii_strcasecmp (field, "NUMPAGES") == 0 ? "NUMPAGES"
                            : g_ascii_strcasecmp (field, "DATE") == 0 ? "DATE" : NULL;

          if (instr != NULL)
            g_string_append_printf (out,
              "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\"> %s </w:instrText></w:r>"
              "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>1</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r>",
              instr);
          else
            {
              g_string_append (out, "<w:r>");
              write_run_text (out, brace, close - brace + 1, FALSE);
              g_string_append (out, "</w:r>");
            }
          g_free (field);
        }
        p = close + 1;
      }
  }
  g_string_append (out, "</w:p></w:hdr>");
  return g_string_free (out, FALSE);
}

static char *
styles_part (W42StyleSheet *styles)
{
  GString *out = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
  const W42Style *normal = w42_stylesheet_find (styles, "Normal");
  W42Fmt def;

  w42_fmt_init_default (&def);
  g_string_append (out, "<w:styles " W_NS ">");
  g_string_append (out, "<w:docDefaults><w:rPrDefault><w:rPr>");
  append_rfonts (out, normal != NULL ? normal->ch.family : def.ch.family);
  g_string_append_printf (out,
    "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/></w:rPr></w:rPrDefault>"
    "<w:pPrDefault><w:pPr/></w:pPrDefault></w:docDefaults>",
    normal != NULL ? normal->ch.size : def.ch.size, normal != NULL ? normal->ch.size : def.ch.size);

  for (guint i = 0; i < w42_stylesheet_size (styles); i++)
    {
      const W42Style *s = w42_stylesheet_get (styles, i);
      gboolean is_normal = g_ascii_strcasecmp (s->name, "Normal") == 0;

      g_string_append_printf (out, "<w:style w:type=\"%s\" w:styleId=\"%s\"%s><w:name w:val=\"",
                              s->character ? "character" : "paragraph",
                              style_id (s->name), is_normal ? " w:default=\"1\"" : "");
      xml_escape (out, s->name, strlen (s->name));
      g_string_append (out, "\"/>");
      if (s->based_on != NULL && w42_stylesheet_find (styles, s->based_on) != NULL)
        g_string_append_printf (out, "<w:basedOn w:val=\"%s\"/>", style_id (s->based_on));
      else if (!is_normal && !s->character)
        g_string_append (out, "<w:basedOn w:val=\"Normal\"/>");
      if (!is_normal && !s->character)
        g_string_append (out, "<w:next w:val=\"Normal\"/><w:qFormat/>");
      else if (s->character)
        g_string_append (out, "<w:qFormat/>");
      if (!s->character)
        {
          g_string_append (out, "<w:pPr>");
          if (s->outline > 0)
            g_string_append (out, "<w:keepNext/>");
          if (s->pa.space_before || s->pa.space_after)
            g_string_append_printf (out, "<w:spacing w:before=\"%d\" w:after=\"%d\"/>", s->pa.space_before, s->pa.space_after);
          if (s->pa.indent_left != 0 || s->pa.indent_right != 0 || s->pa.indent_first != 0)
            {
              g_string_append_printf (out, "<w:ind w:left=\"%d\" w:right=\"%d\"", s->pa.indent_left, s->pa.indent_right);
              if (s->pa.indent_first < 0)
                g_string_append_printf (out, " w:hanging=\"%d\"", -s->pa.indent_first);
              else if (s->pa.indent_first > 0)
                g_string_append_printf (out, " w:firstLine=\"%d\"", s->pa.indent_first);
              g_string_append (out, "/>");
            }
          if (s->pa.align == W42_ALIGN_CENTER)
            g_string_append (out, "<w:jc w:val=\"center\"/>");
          else if (s->pa.align == W42_ALIGN_RIGHT)
            g_string_append (out, "<w:jc w:val=\"right\"/>");
          else if (s->pa.align == W42_ALIGN_JUSTIFY)
            g_string_append (out, "<w:jc w:val=\"both\"/>");
          if (s->outline > 0)
            g_string_append_printf (out, "<w:outlineLvl w:val=\"%d\"/>", s->outline - 1);
          g_string_append (out, "</w:pPr>");
        }
      g_string_append (out, "<w:rPr>");
      if (s->ch.family != NULL && (normal == NULL || s->ch.family != normal->ch.family))
        append_rfonts (out, s->ch.family);
      if (s->ch.bold)   g_string_append (out, "<w:b/>");
      if (s->ch.italic) g_string_append (out, "<w:i/>");
      if (s->ch.underline)
        g_string_append_printf (out, "<w:u w:val=\"%s\"/>", underline_val (s->ch.underline));
      if (s->ch.color != 0)
        g_string_append_printf (out, "<w:color w:val=\"%06X\"/>", s->ch.color & 0xFFFFFF);
      if (s->ch.size > 0 && (normal == NULL || s->ch.size != normal->ch.size))
        g_string_append_printf (out, "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>", s->ch.size, s->ch.size);
      g_string_append (out, "</w:rPr></w:style>");
    }
  g_string_append (out, "</w:styles>");
  return g_string_free (out, FALSE);
}

static char *
numbering_part (const gboolean *have)
{
  GString *out = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");

  g_string_append (out, "<w:numbering " W_NS ">");
  for (int k = 1; k < W42_LIST_KINDS; k++)
    {
      const char *fmt, *text;
      char marker[16];

      if (!have[k])
        continue;
      w42_list_marker ((W42ListKind) k, 1, marker, sizeof marker);
      if (w42_list_is_bullet ((W42ListKind) k))
        {
          fmt = "bullet";
          text = marker;
        }
      else
        {
          fmt = k == W42_LIST_LOWER_LETTER ? "lowerLetter" : k == W42_LIST_UPPER_LETTER ? "upperLetter"
              : k == W42_LIST_LOWER_ROMAN ? "lowerRoman" : k == W42_LIST_UPPER_ROMAN ? "upperRoman" : "decimal";
          text = "%1.";
        }
      g_string_append_printf (out,
        "<w:abstractNum w:abstractNumId=\"%d\"><w:multiLevelType w:val=\"multilevel\"/>", k);
      for (int lv = 0; lv < 9; lv++)
        g_string_append_printf (out,
          "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"1\"/><w:numFmt w:val=\"%s\"/><w:lvlText w:val=\"%s\"/>"
          "<w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr></w:lvl>",
          lv, fmt, w42_list_is_bullet ((W42ListKind) k) ? text : (lv == 0 ? "%1." : lv == 1 ? "%2." : lv == 2 ? "%3."
              : lv == 3 ? "%4." : lv == 4 ? "%5." : lv == 5 ? "%6." : lv == 6 ? "%7." : lv == 7 ? "%8." : "%9."),
          360 * (lv + 1));
      g_string_append (out, "</w:abstractNum>");
    }
  for (int k = 1; k < W42_LIST_KINDS; k++)
    if (have[k])
      g_string_append_printf (out, "<w:num w:numId=\"%d\"><w:abstractNumId w:val=\"%d\"/></w:num>", k, k);
  g_string_append (out, "</w:numbering>");
  return g_string_free (out, FALSE);
}

gboolean
w42_docx_save (W42PieceTable *pt, const W42PageSetup *page, GFile *file, GError **error)
{
  GPtrArray *blocks;
  W42ApTable *aps;
  W42StyleSheet *styles;
  W42Fmt base;
  Parts parts;
  GString *doc = g_string_new (NULL);
  GString *notes = g_string_new (NULL), *endnotes = g_string_new (NULL);
  gboolean any_notes = FALSE, any_endnotes = FALSE;
  W42ZipWriter *zip;
  W42PageSetup pg;
  int sect_cols, sect_gap;
  int table_open = -1, row_open = -1;
  char *header_rid = NULL, *footer_rid = NULL;
  const W42PageText *header, *footer;
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
  sect_cols = w42_page_columns (&pg);
  sect_gap = w42_page_column_gap (&pg);

  blocks = w42_pt_snapshot_blocks (pt);
  aps = w42_pt_ap_table (pt);
  styles = w42_pt_stylesheet (pt);
  w42_fmt_init_default (&base);
  {
    const W42Style *normal = w42_stylesheet_find (styles, "Normal");
    if (normal != NULL)
      base.ch = normal->ch;
  }

  memset (&parts, 0, sizeof parts);
  parts.rels = g_string_new (NULL);
  parts.next_rel = 1;
  parts.media_names = g_ptr_array_new_with_free_func (g_free);
  parts.media_data = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);
  parts.link_rels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  parts.comments = g_string_new (NULL);
  g_free (add_rel (&parts, "styles", "styles.xml", FALSE));
  if (page != NULL && page->has_background)
    g_free (add_rel (&parts, "settings", "settings.xml", FALSE));

  header = w42_pt_get_header (pt);
  footer = w42_pt_get_footer (pt);
  if (header != NULL && header->text != NULL && *header->text != '\0')
    header_rid = add_rel (&parts, "header", "header1.xml", FALSE);
  if (footer != NULL && footer->text != NULL && *footer->text != '\0')
    footer_rid = add_rel (&parts, "footer", "footer1.xml", FALSE);

  parts.header_rid = header_rid;
  parts.footer_rid = footer_rid;

  /* A title page's and even pages' own, when the document has them. */
  parts.title_page = w42_pt_get_title_page (pt);
  parts.facing_pages = w42_pt_get_facing_pages (pt);
  if (parts.title_page)
    {
      const W42PageText *h = w42_pt_get_header_kind (pt, W42_PAGE_TEXT_FIRST);
      const W42PageText *f = w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_FIRST);

      if (h != NULL && h->text != NULL && *h->text != '\0')
        parts.header_first_rid = add_rel (&parts, "header", "header2.xml", FALSE);
      if (f != NULL && f->text != NULL && *f->text != '\0')
        parts.footer_first_rid = add_rel (&parts, "footer", "footer2.xml", FALSE);
    }
  if (parts.facing_pages)
    {
      const W42PageText *h = w42_pt_get_header_kind (pt, W42_PAGE_TEXT_EVEN);
      const W42PageText *f = w42_pt_get_footer_kind (pt, W42_PAGE_TEXT_EVEN);

      if (h != NULL && h->text != NULL && *h->text != '\0')
        parts.header_even_rid = add_rel (&parts, "header", "header3.xml", FALSE);
      if (f != NULL && f->text != NULL && *f->text != '\0')
        parts.footer_even_rid = add_rel (&parts, "footer", "footer3.xml", FALSE);
    }

  g_string_append (doc, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document " W_NS ">");
  if (page != NULL && page->has_background)
    {
      /* The colour behind the page.  Word shows it only when the
       * settings part says so, which is why that part is written. */
      g_string_append_printf (doc, "<w:background w:color=\"%06X\"/>",
                              page->background & 0xFFFFFF);
    }
  g_string_append (doc, "<w:body>");

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42ParaFmt *pa = &w42_ap_table_get (aps, block->ap)->pa;
      const W42Block *next = b + 1 < blocks->len ? g_ptr_array_index (blocks, b + 1) : NULL;
      const W42ParaFmt *next_pa = next != NULL ? &w42_ap_table_get (aps, next->ap)->pa : NULL;
      gboolean ends_section;

      if (block->note >= 0)
        {
          /* Notes go to their own part, each note's paragraphs together. */
          GString *target = block->note_end ? endnotes : notes;
          const W42Block *prev = b > 0 ? g_ptr_array_index (blocks, b - 1) : NULL;
          gboolean first = prev == NULL || prev->note != block->note;
          gboolean last = next == NULL || next->note != block->note;

          if (block->note_end) any_endnotes = TRUE; else any_notes = TRUE;
          if (first)
            {
              g_string_append_printf (target, "<w:%s w:id=\"%d\">", block->note_end ? "endnote" : "footnote", block->note + 1);
            }
          g_string_append (target, "<w:p>");
          write_ppr (target, &parts, &w42_ap_table_get (aps, block->ap)->pa,
                     &pg, FALSE, 0, 0);
          if (first)
            g_string_append_printf (target,
              "<w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:%sRef/></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>",
              block->note_end ? "endnote" : "footnote");
          write_runs (target, &parts, pt, aps, block, &base.ch);
          g_string_append (target, "</w:p>");
          if (last)
            g_string_append_printf (target, "</w:%s>", block->note_end ? "endnote" : "footnote");
          continue;
        }

      if (pa->section_break && b > 0)
        {
          sect_cols = MAX (pa->columns, 1);
          sect_gap = pa->column_gap > 0 ? pa->column_gap : 720;
        }
      ends_section = next_pa != NULL && next_pa->section_break && next->note < 0 && block->table < 0;

      if (block->table >= 0)
        {
          const W42Block *prev = b > 0 ? g_ptr_array_index (blocks, b - 1) : NULL;
          gboolean cell_start = prev == NULL || prev->table != block->table ||
                                prev->row != block->row || prev->col != block->col;
          gboolean cell_end = next == NULL || next->table != block->table ||
                              next->row != block->row || next->col != block->col;

          if (block->table != table_open)
            {
              const W42TableProps *props = w42_pt_table_props (pt, block->table);

              {
                static const int order[W42_N_EDGES] = { W42_EDGE_TOP, W42_EDGE_LEFT, W42_EDGE_BOTTOM,
                                                        W42_EDGE_RIGHT, W42_EDGE_INSIDE_H, W42_EDGE_INSIDE_V };
                gboolean ruled = props == NULL || props->borders;

                g_string_append (doc, "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/><w:tblBorders>");
                for (int e = 0; e < W42_N_EDGES; e++)
                  write_border_element (doc, order[e], props != NULL ? &props->edge[order[e]] : NULL,
                                        ruled, 0);
                g_string_append (doc, "</w:tblBorders></w:tblPr><w:tblGrid>");
              }
              if (props != NULL)
                for (int c = 0; c < props->n_cols; c++)
                  {
                    int w = g_array_index (props->widths, int, c);

                    /* A width of nothing means an equal share of the column. */
                    if (w <= 0)
                      w = (pg.width - pg.margin_left - pg.margin_right) / MAX (props->n_cols, 1);
                    g_string_append_printf (doc, "<w:gridCol w:w=\"%d\"/>", w);
                  }
              g_string_append (doc, "</w:tblGrid>");
              table_open = block->table;
              row_open = -1;
            }
          if (block->row != row_open)
            {
              const W42TableProps *props = w42_pt_table_props (pt, block->table);
              int least = w42_pt_table_get_row_height (pt, block->table, block->row);
              gboolean repeat = props != NULL && block->row < props->header_rows;

              if (row_open >= 0)
                g_string_append (doc, "</w:tr>");
              g_string_append (doc, "<w:tr>");
              if (least > 0 || repeat)
                {
                  g_string_append (doc, "<w:trPr>");
                  if (least > 0)
                    g_string_append_printf (doc, "<w:trHeight w:val=\"%d\" w:hRule=\"atLeast\"/>", least);
                  if (repeat)
                    g_string_append (doc, "<w:tblHeader/>");
                  g_string_append (doc, "</w:trPr>");
                }
              row_open = block->row;
            }
          if (cell_start)
            {
              const W42TableProps *props = w42_pt_table_props (pt, block->table);
              int width = 0;

              if (props != NULL)
                for (int c = block->col; c < MIN (block->col + block->span, props->n_cols); c++)
                  {
                    int w = g_array_index (props->widths, int, c);

                    width += w > 0 ? w : (pg.width - pg.margin_left - pg.margin_right) / MAX (props->n_cols, 1);
                  }
              g_string_append (doc, "<w:tc><w:tcPr>");
              if (width > 0)
                g_string_append_printf (doc, "<w:tcW w:w=\"%d\" w:type=\"dxa\"/>", width);
              if (block->span > 1)
                g_string_append_printf (doc, "<w:gridSpan w:val=\"%d\"/>", block->span);
              {
                /* A cell merged downwards: Word says where it starts and
                 * which cells it swallows. */
                const W42ParaFmt *cpa = &w42_ap_table_get (aps, block->cell_ap)->pa;

                if (cpa->cell_vspan == W42_CELL_COVERED)
                  g_string_append (doc, "<w:vMerge/>");
                else if (cpa->cell_vspan > 1)
                  g_string_append (doc, "<w:vMerge w:val=\"restart\"/>");
              }
              {
                const W42ParaFmt *cpa = &w42_ap_table_get (aps, block->cell_ap)->pa;

                if (cpa->border & W42_BORDER_CELL_SET)
                  {
                    static const int order[4] = { W42_EDGE_TOP, W42_EDGE_LEFT, W42_EDGE_BOTTOM, W42_EDGE_RIGHT };

                    /* A side that is on but has no line of its own is the
                     * table's, which is what leaving it out means. */
                    g_string_append (doc, "<w:tcBorders>");
                    for (int k = 0; k < 4; k++)
                      {
                        const W42BorderEdge *edge = &cpa->edge[order[k]];
                        gboolean on = (cpa->border & (1 << order[k])) != 0;

                        if (on && edge->width == 0 && edge->style == 0 && edge->color == 0)
                          continue;
                        write_border_element (doc, order[k], edge, on, 0);
                      }
                    g_string_append (doc, "</w:tcBorders>");
                  }
                if (cpa->has_shading_color)
                  g_string_append_printf (doc, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06X\"/>",
                                          cpa->shading_color & 0xFFFFFF);
                else if (cpa->shading > 0)
                  g_string_append_printf (doc, "<w:shd w:val=\"pct%d\" w:color=\"auto\" w:fill=\"auto\"/>",
                                          (int) cpa->shading);
                if (cpa->cell_valign == W42_CELL_VALIGN_CENTER)
                  g_string_append (doc, "<w:vAlign w:val=\"center\"/>");
                else if (cpa->cell_valign == W42_CELL_VALIGN_BOTTOM)
                  g_string_append (doc, "<w:vAlign w:val=\"bottom\"/>");
              }
              g_string_append (doc, "</w:tcPr>");
            }
          write_paragraph (doc, &parts, pt, aps, block, &base.ch, &pg, FALSE, 0, 0);
          if (cell_end)
            g_string_append (doc, "</w:tc>");
          if (next == NULL || next->table != block->table)
            {
              g_string_append (doc, "</w:tr></w:tbl>");
              table_open = -1;
              row_open = -1;
              /* Word wants a paragraph after a table that ends the body
               * or a section; a section's properties go on it. */
              if (next == NULL || next->note >= 0)
                g_string_append (doc, "<w:p/>");
              else if (next_pa != NULL && next_pa->section_break)
                {
                  g_string_append (doc, "<w:p><w:pPr>");
                  write_sectpr (doc, &pg, sect_cols, sect_gap, parts.header_rid, parts.footer_rid, &parts);
                  g_string_append (doc, "</w:pPr></w:p>");
                }
            }
          continue;
        }

      write_paragraph (doc, &parts, pt, aps, block, &base.ch, &pg, ends_section, sect_cols, sect_gap);
    }

  write_sectpr (doc, &pg, sect_cols, sect_gap, header_rid, footer_rid, &parts);
  g_string_append (doc, "</w:body></w:document>");

  /* The parts. */
  zip = w42_zip_writer_new ();
  {
    GString *types = g_string_new (
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Default Extension=\"png\" ContentType=\"image/png\"/>"
      "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
      "<Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>"
      "<Default Extension=\"gif\" ContentType=\"image/gif\"/>"
      "<Default Extension=\"bmp\" ContentType=\"image/bmp\"/>"
      "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
      "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>");
    gboolean any_lists = FALSE;
    char *xml;

    for (int k = 1; k < W42_LIST_KINDS; k++)
      any_lists |= parts.have_lists[k];
    if (any_lists)
      {
        g_string_append (types, "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>");
        g_free (add_rel (&parts, "numbering", "numbering.xml", FALSE));
        xml = numbering_part (parts.have_lists);
        w42_zip_writer_add (zip, "word/numbering.xml", xml, strlen (xml));
        g_free (xml);
      }
    if (any_notes)
      {
        GString *part = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:footnotes " W_NS ">"
          "<w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:footnote>"
          "<w:footnote w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:footnote>");
        g_string_append (part, notes->str);
        g_string_append (part, "</w:footnotes>");
        g_string_append (types, "<Override PartName=\"/word/footnotes.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footnotes+xml\"/>");
        g_free (add_rel (&parts, "footnotes", "footnotes.xml", FALSE));
        w42_zip_writer_add (zip, "word/footnotes.xml", part->str, part->len);
        g_string_free (part, TRUE);
      }
    if (any_endnotes)
      {
        GString *part = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:endnotes " W_NS ">"
          "<w:endnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p></w:endnote>"
          "<w:endnote w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:endnote>");
        g_string_append (part, endnotes->str);
        g_string_append (part, "</w:endnotes>");
        g_string_append (types, "<Override PartName=\"/word/endnotes.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.endnotes+xml\"/>");
        g_free (add_rel (&parts, "endnotes", "endnotes.xml", FALSE));
        w42_zip_writer_add (zip, "word/endnotes.xml", part->str, part->len);
        g_string_free (part, TRUE);
      }
    if (parts.comments->len > 0)
      {
        GString *part = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:comments " W_NS ">");

        g_string_append (part, parts.comments->str);
        g_string_append (part, "</w:comments>");
        g_string_append (types, "<Override PartName=\"/word/comments.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.comments+xml\"/>");
        g_free (add_rel (&parts, "comments", "comments.xml", FALSE));
        w42_zip_writer_add (zip, "word/comments.xml", part->str, part->len);
        g_string_free (part, TRUE);
      }
    if (header_rid != NULL)
      {
        xml = page_text_part (header);
        g_string_append (types, "<Override PartName=\"/word/header1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml\"/>");
        w42_zip_writer_add (zip, "word/header1.xml", xml, strlen (xml));
        g_free (xml);
      }
    if (footer_rid != NULL)
      {
        xml = page_text_part (footer);
        /* A footer is the same part with another root. */
        {
          char *ftr = g_strdup (xml);
          char *p;

          while ((p = strstr (ftr, "w:hdr")) != NULL)
            memcpy (p, "w:ftr", 5);
          g_string_append (types, "<Override PartName=\"/word/footer1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml\"/>");
          w42_zip_writer_add (zip, "word/footer1.xml", ftr, strlen (ftr));
          g_free (ftr);
        }
        g_free (xml);
      }
    {
      /* A title page's and even pages' own headers and footers. */
      static const struct { int kind; const char *header; const char *footer; } more[] = {
        { W42_PAGE_TEXT_FIRST, "word/header2.xml", "word/footer2.xml" },
        { W42_PAGE_TEXT_EVEN,  "word/header3.xml", "word/footer3.xml" },
      };

      for (guint k = 0; k < G_N_ELEMENTS (more); k++)
        {
          const W42PageText *h = w42_pt_get_header_kind (pt, more[k].kind);
          const W42PageText *f = w42_pt_get_footer_kind (pt, more[k].kind);
          const char *hrid = more[k].kind == W42_PAGE_TEXT_FIRST ? parts.header_first_rid : parts.header_even_rid;
          const char *frid = more[k].kind == W42_PAGE_TEXT_FIRST ? parts.footer_first_rid : parts.footer_even_rid;

          if (hrid != NULL && h != NULL)
            {
              char *part = page_text_part (h);

              g_string_append_printf (types, "<Override PartName=\"/%s\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml\"/>", more[k].header);
              w42_zip_writer_add (zip, more[k].header, part, strlen (part));
              g_free (part);
            }
          if (frid != NULL && f != NULL)
            {
              char *part = page_text_part (f);
              char *p;

              while ((p = strstr (part, "w:hdr")) != NULL)
                memcpy (p, "w:ftr", 5);
              g_string_append_printf (types, "<Override PartName=\"/%s\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml\"/>", more[k].footer);
              w42_zip_writer_add (zip, more[k].footer, part, strlen (part));
              g_free (part);
            }
        }
    }
    if (page != NULL && page->has_background)
      g_string_append (types, "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>");
    g_string_append (types, "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>");
    g_string_append (types, "</Types>");
    w42_zip_writer_add (zip, "[Content_Types].xml", types->str, types->len);
    g_string_free (types, TRUE);
  }
  {
    const char *root_rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
      "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/></Relationships>";
    GString *rels = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    char *xml;

    w42_zip_writer_add (zip, "_rels/.rels", root_rels, strlen (root_rels));
    g_string_append (rels, parts.rels->str);
    g_string_append (rels, "</Relationships>");
    w42_zip_writer_add (zip, "word/_rels/document.xml.rels", rels->str, rels->len);
    g_string_free (rels, TRUE);

    w42_zip_writer_add (zip, "word/document.xml", doc->str, doc->len);
    if (page != NULL && page->has_background)
      {
        /* Word paints the background only if it is told to display the
         * background shape; the settings part is where that is said. */
        const char *settings =
          "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
          "<w:settings " W_NS "><w:displayBackgroundShape/></w:settings>";

        w42_zip_writer_add (zip, "word/settings.xml", settings, strlen (settings));
      }
  {
    /* What the document says about itself. */
    const W42DocInfo *info = w42_pt_get_info (pt);
    GString *core = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">");
    static const struct { const char *tag; gsize offset; } fields[] = {
      { "dc:title",       G_STRUCT_OFFSET (W42DocInfo, title) },
      { "dc:subject",     G_STRUCT_OFFSET (W42DocInfo, subject) },
      { "dc:creator",     G_STRUCT_OFFSET (W42DocInfo, author) },
      { "cp:keywords",    G_STRUCT_OFFSET (W42DocInfo, keywords) },
      { "dc:description", G_STRUCT_OFFSET (W42DocInfo, comments) },
    };

    for (guint i = 0; i < G_N_ELEMENTS (fields); i++)
      {
        const char *value = G_STRUCT_MEMBER (const char *, info, fields[i].offset);

        if (value == NULL)
          continue;
        g_string_append_printf (core, "<%s>", fields[i].tag);
        xml_escape (core, value, strlen (value));
        g_string_append_printf (core, "</%s>", fields[i].tag);
      }
    g_string_append (core, "</cp:coreProperties>");
    w42_zip_writer_add (zip, "docProps/core.xml", core->str, core->len);
    g_string_free (core, TRUE);
  }
    xml = styles_part (styles);
    w42_zip_writer_add (zip, "word/styles.xml", xml, strlen (xml));
    g_free (xml);
    for (guint i = 0; i < parts.media_names->len; i++)
      {
        char *name = g_strconcat ("word/media/", (char *) g_ptr_array_index (parts.media_names, i), NULL);
        GBytes *data = g_ptr_array_index (parts.media_data, i);

        w42_zip_writer_add (zip, name, g_bytes_get_data (data, NULL), g_bytes_get_size (data));
        g_free (name);
      }
  }

  ok = w42_zip_writer_save (zip, file, error);

  w42_zip_writer_free (zip);
  g_string_free (doc, TRUE);
  g_string_free (notes, TRUE);
  g_string_free (endnotes, TRUE);
  g_string_free (parts.rels, TRUE);
  g_ptr_array_free (parts.media_names, TRUE);
  g_ptr_array_free (parts.media_data, TRUE);
  g_hash_table_destroy (parts.link_rels);
  g_string_free (parts.comments, TRUE);
  g_free (header_rid);
  g_free (footer_rid);
  g_ptr_array_free (blocks, TRUE);
  return ok;
}
