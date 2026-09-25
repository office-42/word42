/* w42-epub.c - see w42-epub.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The export is two passes over the paragraphs.  The first decides what
 * each paragraph is -- a heading, a list item, a cell, a blank line, a
 * scene break -- which chapter file it goes in, where each bookmark and
 * note will be, and what most of the text looks like; the second writes
 * the files.  The first pass has to come first because a link may point
 * forward to a bookmark in a chapter not yet written, and because the
 * stylesheet is made from what the body text mostly is, so that each
 * paragraph need only say how it differs.
 */

#include "w42-epub.h"

#include <string.h>

#include "w42-html.h"
#include "w42-image.h"
#include "w42-lang.h"
#include "w42-zip.h"

#define EPUB_MIME "application/epub+zip"

/* The classes of paragraph the stylesheet has a rule for.  A paragraph
 * is written against its class's rule and says only where it differs. */
enum {
  CLS_P = 0,
  CLS_H1,                  /* to CLS_H1 + 5 for Heading 6 */
  CLS_TITLE = CLS_H1 + 6,
  CLS_NOTE,
  N_CLS
};

typedef enum {
  K_NOTE = 0,   /* a footnote's or endnote's paragraph */
  K_BLANK,      /* nothing on it but white space */
  K_PARA,
  K_HEADING,
  K_SCENE,      /* "* * *" and its kind, alone on the line */
  K_LIST,
  K_CELL
} Kind;

typedef struct {
  W42Align align;
  int      indent_left;
  int      indent_right;
  int      indent_first;
  int      space_before;
  int      space_after;
} ParaLook;

typedef struct {
  const char *family;
  int         size;
  gboolean    bold;
  gboolean    italic;
} CharLook;

typedef struct { ParaLook look; gsize n; } ParaVote;
typedef struct { CharLook look; gsize n; } CharVote;

typedef struct {
  gboolean  used;
  ParaLook  pa;
  CharLook  ch;
  GArray   *pa_votes;   /* ParaVote */
  GArray   *ch_votes;   /* CharVote */
} Look;

/* A document formatted by hand has as many looks as it has paragraphs,
 * near enough; past this many the rare ones are not worth counting. */
#define MAX_VOTES 128

typedef struct {
  guint8   kind;       /* Kind */
  guint8   cls;        /* CLS_*: what it is written against */
  guint8   tag;        /* 1 to 6 for a heading's hN */
  gboolean image;      /* pictures and nothing else */
  gboolean written;    /* its text reaches the book */
  gboolean note_first; /* the first paragraph of its note */
  int      chapter;    /* -1 for a note nothing refers to */
  int      heading;    /* its index in Epub.headings, or -1 */
} BlockInfo;

typedef struct {
  int   level;         /* 1 to 6; 0 for the Title */
  int   chapter;
  int   id;            /* the element's id is heading<id> */
  char *prefix;        /* the number the page shows before it, or NULL */
  char *label;         /* its text, plain, for the table of contents */
} Heading;

typedef struct {
  char    *file;       /* "chapter3.xhtml" */
  char    *title;      /* its first heading, or NULL */
  GString *body;
  GArray  *notes;      /* int: the ids of the notes it refers to, in order */
  gboolean opens_with_heading;
} Chapter;

typedef struct {
  int      chapter;
  char    *id;
  gboolean placed;
} Anchor;

typedef struct {
  char       *id;
  char       *href;
  const char *mime;
  GBytes     *bytes;
  gboolean    cover;
} Image;

typedef struct {
  int      depth;
  int      kind[9];
  gboolean li_open[9];  /* the list at that depth has an item open */
} ListState;

typedef struct {
  W42PieceTable  *pt;
  W42ApTable     *aps;
  W42StyleSheet  *styles;
  GPtrArray      *blocks;
  BlockInfo      *info;
  Look            look[N_CLS];
  const char     *lang;         /* the book's language */
  GHashTable     *lang_votes;   /* interned tag -> characters in it */
  int             column;       /* twips: what a picture's width is a share of */
  GArray         *headings;     /* Heading */
  GPtrArray      *chapters;     /* Chapter* */
  GHashTable     *notes;        /* note id -> GArray of block indexes */
  GHashTable     *note_chapter; /* note id -> chapter + 1 */
  GHashTable     *bookmarks;    /* name -> Anchor* */
  GHashTable     *ids;          /* every id given out, so none is given twice */
  GHashTable     *images;       /* object index -> Image*, or NULL for none */
  GHashTable     *image_bytes;  /* GBytes -> Image* */
  GPtrArray      *image_list;   /* Image*, in the order met */
  W42ObjectIdx    cover;
} Epub;

/* ---- Text ------------------------------------------------------------- */

/* Text for an XML file.  `markup` is the body of a paragraph, where a
 * line break is an element; elsewhere -- an attribute, a title, a label
 * -- it is a space.  What XML cannot carry at all, the C0 and C1 control
 * characters above all, is left out, and so is the object replacement
 * character that stands in for a picture in the paragraph's text. */
static void
append_xml (GString *out, const char *text, gssize len, gboolean markup)
{
  const char *p = text;
  const char *end = len < 0 ? text + strlen (text) : text + len;

  while (p < end)
    {
      gunichar c = g_utf8_get_char_validated (p, end - p);
      const char *next;

      if (c == (gunichar) -1 || c == (gunichar) -2)
        {
          p++;
          continue;
        }
      next = g_utf8_next_char (p);
      switch (c)
        {
        case '&': g_string_append (out, "&amp;"); break;
        case '<': g_string_append (out, "&lt;"); break;
        case '>': g_string_append (out, "&gt;"); break;
        case '"': g_string_append (out, "&quot;"); break;
        case '\n':
        case '\r':
          g_string_append_c (out, ' ');
          break;
        case '\t':
          /* A reader has no tab stops to line anything up at; an em
           * space keeps the gap a tab was put there to make. */
          g_string_append (out, markup ? "&#8195;" : " ");
          break;
        case 0x2028:
          g_string_append (out, markup ? "<br/>" : " ");
          break;
        case 0x00A0:
          g_string_append (out, "&#160;");
          break;
        case 0x00AD:
          /* Where a word may break: the reader's own line breaking
           * wants it, a label does not. */
          if (markup)
            g_string_append (out, "&#173;");
          break;
        default:
          if (c < 0x20 || (c >= 0x7F && c <= 0x9F) ||
              c == 0xFFFC || c == 0xFFFE || c == 0xFFFF)
            break;
          g_string_append_len (out, p, next - p);
          break;
        }
      p = next;
    }
}

static void
append_attr (GString *out, const char *name, const char *value)
{
  g_string_append_printf (out, " %s=\"", name);
  append_xml (out, value, -1, FALSE);
  g_string_append_c (out, '"');
}

/* A URL as an attribute: what may not stand in one bare -- a space, a
 * quote, a letter outside ASCII -- percent-encoded, as a browser would
 * send it, since the validator holds an e-book to the letter of it. */
static void
append_href (GString *out, const char *url)
{
  for (const guchar *p = (const guchar *) url; *p != '\0'; p++)
    {
      if (*p <= 0x20 || *p >= 0x7F || strchr ("\"<>\\^`{|}", *p) != NULL)
        g_string_append_printf (out, "%%%02X", *p);
      else if (*p == '&')
        g_string_append (out, "&amp;");
      else
        g_string_append_c (out, (char) *p);
    }
}

/* Whether a language tag is one the validator will take: letters, then
 * dash-separated letters and digits.  A tag a file brought in may be
 * anything. */
static gboolean
lang_tag_ok (const char *tag)
{
  gsize part = 0;
  gboolean first = TRUE;

  if (tag == NULL || *tag == '\0')
    return FALSE;
  for (const char *p = tag; ; p++)
    {
      if (*p == '-' || *p == '\0')
        {
          if (part == 0 || part > 8 || (first && part < 2))
            return FALSE;
          if (*p == '\0')
            return TRUE;
          first = FALSE;
          part = 0;
        }
      else if (g_ascii_isalpha (*p) || (!first && g_ascii_isdigit (*p)))
        part++;
      else
        return FALSE;
    }
}

/* White space: what a blank paragraph has and nothing else.  The text
 * may be Summary Info's, from a file, and is not trusted to be UTF-8. */
static gboolean
is_blank_text (const char *text)
{
  const char *end = text + strlen (text);

  for (const char *p = text; p < end; )
    {
      gunichar c = g_utf8_get_char_validated (p, end - p);

      if (c == (gunichar) -1 || c == (gunichar) -2)
        return FALSE;
      if (!g_unichar_isspace (c))
        return FALSE;
      p = g_utf8_next_char (p);
    }
  return TRUE;
}

/* A paragraph whose whole text is a row of asterisks, an asterism, a
 * hash or the like: the break between two scenes that a novel shows
 * with a gap and an ornament. */
static gboolean
is_scene_break (const char *text)
{
  gunichar mark = 0;
  int n = 0;

  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isspace (c))
        continue;
      if (c != '*' && c != '#' && c != '~' && c != 0x2042 && c != 0x2217 &&
          c != 0x2022)
        return FALSE;
      if (mark != 0 && c != mark)
        return FALSE;
      mark = c;
      n++;
    }
  return n > 0 && n <= 5;
}

/* A number for a stylesheet: a full stop whatever the locale, and no
 * trailing noughts. */
static void
css_number (GString *css, double value)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  char *end;

  g_ascii_formatd (buf, sizeof buf, "%.3f", value);
  end = buf + strlen (buf);
  while (end > buf && end[-1] == '0')
    *--end = '\0';
  if (end > buf && end[-1] == '.')
    *--end = '\0';
  if (g_str_equal (buf, "-0") || buf[0] == '\0')
    g_strlcpy (buf, "0", sizeof buf);
  g_string_append (css, buf);
}

/* A length, in ems of the type it is set in: `twips` of it at `em`
 * twips to the em.  Ems rather than points because the reader chooses
 * the size of the type, and space measured in it grows with it. */
static void
css_length (GString *css, int twips, double em)
{
  css_number (css, twips / em);
  if (twips != 0)
    g_string_append (css, "em");
}

static void
css_em (GString *css, const char *prop, int twips, double em)
{
  g_string_append_printf (css, "%s:", prop);
  css_length (css, twips, em);
  g_string_append_c (css, ';');
}

/* The generic family a face belongs to, for a reader that has not got
 * the face: a guess from its name, which is what a browser's font
 * matching does too. */
static const char *
generic_family (const char *family)
{
  static const char *const SANS[] = {
    "sans", "arial", "helvetica", "calibri", "verdana", "tahoma", "segoe",
    "trebuchet", "carlito", "gill", "futura", "franklin", "univers",
    "frutiger", "myriad", "roboto", "lato", "ubuntu", "cantarell", "aptos"
  };
  char *lower;
  const char *generic = "serif";

  if (family == NULL)
    return generic;
  lower = g_ascii_strdown (family, -1);
  if (strstr (lower, "mono") != NULL || strstr (lower, "courier") != NULL ||
      strstr (lower, "consol") != NULL || strstr (lower, "code") != NULL)
    generic = "monospace";
  else
    for (guint i = 0; i < G_N_ELEMENTS (SANS); i++)
      if (strstr (lower, SANS[i]) != NULL)
        {
          generic = "sans-serif";
          break;
        }
  g_free (lower);
  return generic;
}

/* A face as a stylesheet names it, quoted, with its generic family
 * behind it.  The name came from a file: nothing in it may end the
 * quotes, the declaration or the rule.  Single quotes, which a style
 * attribute's double ones hold without escaping. */
static void
css_family (GString *css, const char *family)
{
  g_string_append (css, "font-family:");
  if (family != NULL && *family != '\0')
    {
      g_string_append_c (css, '\'');
      for (const char *p = family; *p != '\0'; p++)
        if ((guchar) *p >= 0x20 && strchr ("\"'\\<>{};", *p) == NULL)
          g_string_append_c (css, *p);
      g_string_append (css, "',");
    }
  g_string_append_printf (css, "%s;", generic_family (family));
}

/* ---- Looks ------------------------------------------------------------ */

static void
para_look (const W42ParaFmt *pa, ParaLook *look)
{
  look->align = pa->align;
  look->indent_left = pa->indent_left;
  look->indent_right = pa->indent_right;
  look->indent_first = pa->indent_first;
  look->space_before = pa->space_before;
  look->space_after = pa->space_after;
}

static gboolean
para_look_equal (const ParaLook *a, const ParaLook *b)
{
  return a->align == b->align && a->indent_left == b->indent_left &&
         a->indent_right == b->indent_right && a->indent_first == b->indent_first &&
         a->space_before == b->space_before && a->space_after == b->space_after;
}

static void
char_look (const W42CharFmt *ch, CharLook *look)
{
  look->family = ch->family;
  look->size = ch->size;
  look->bold = ch->bold;
  look->italic = ch->italic;
}

static gboolean
char_look_equal (const CharLook *a, const CharLook *b)
{
  return g_strcmp0 (a->family, b->family) == 0 && a->size == b->size &&
         a->bold == b->bold && a->italic == b->italic;
}

static void
vote_para (Look *look, const W42ParaFmt *pa)
{
  ParaVote v;

  para_look (pa, &v.look);
  v.n = 1;
  look->used = TRUE;
  for (guint i = 0; i < look->pa_votes->len; i++)
    {
      ParaVote *seen = &g_array_index (look->pa_votes, ParaVote, i);

      if (para_look_equal (&seen->look, &v.look))
        {
          seen->n++;
          return;
        }
    }
  if (look->pa_votes->len < MAX_VOTES)
    g_array_append_val (look->pa_votes, v);
}

static void
vote_char (Look *look, const W42CharFmt *ch, gsize n)
{
  CharVote v;

  char_look (ch, &v.look);
  v.n = n;
  for (guint i = 0; i < look->ch_votes->len; i++)
    {
      CharVote *seen = &g_array_index (look->ch_votes, CharVote, i);

      if (char_look_equal (&seen->look, &v.look))
        {
          seen->n += n;
          return;
        }
    }
  if (look->ch_votes->len < MAX_VOTES)
    g_array_append_val (look->ch_votes, v);
}

/* The class's rule is what most of its paragraphs and most of its text
 * are: in a document that uses its styles that is the style, and in one
 * formatted by hand it is the formatting the hand kept to.  A class
 * with nothing in it takes `fallback`'s. */
static void
decide_look (Look *look, const W42Style *fallback)
{
  gsize best = 0;

  if (fallback != NULL)
    {
      para_look (&fallback->pa, &look->pa);
      char_look (&fallback->ch, &look->ch);
    }
  for (guint i = 0; i < look->pa_votes->len; i++)
    {
      const ParaVote *v = &g_array_index (look->pa_votes, ParaVote, i);

      if (v->n > best)
        {
          best = v->n;
          look->pa = v->look;
        }
    }
  best = 0;
  for (guint i = 0; i < look->ch_votes->len; i++)
    {
      const CharVote *v = &g_array_index (look->ch_votes, CharVote, i);

      if (v->n > best)
        {
          best = v->n;
          look->ch = v->look;
        }
    }
  if (look->ch.size <= 0)
    look->ch.size = 24;
}

/* Twips to the em of a class's type. */
static double
look_em (const Look *look)
{
  return MAX (look->ch.size, 2) * 10.0;
}

/* Where a paragraph differs from its class's rule: its alignment, its
 * indents unless its list's marker has them, and the space round it. */
static void
para_style (GString *css, const W42ParaFmt *pa, const ParaLook *look,
            double em, gboolean indents)
{
  /* Alignment is to an edge, not to where the line starts: a right-
   * to-left paragraph says which, since a reader would otherwise put it
   * at the right. */
  if (pa->align != look->align || pa->rtl)
    {
      static const char *const ALIGN[] = { "left", "center", "right", "justify" };

      g_string_append_printf (css, "text-align:%s;", ALIGN[MIN ((guint) pa->align, 3)]);
    }
  if (indents)
    {
      if (pa->indent_first != look->indent_first)
        css_em (css, "text-indent", pa->indent_first, em);
      if (pa->indent_left != look->indent_left)
        css_em (css, "margin-left", pa->indent_left, em);
      if (pa->indent_right != look->indent_right)
        css_em (css, "margin-right", pa->indent_right, em);
    }
  if (pa->space_before != look->space_before)
    css_em (css, "margin-top", pa->space_before, em);
  if (pa->space_after != look->space_after)
    css_em (css, "margin-bottom", pa->space_after, em);
}

static void
open_element (GString *out, const char *tag, const char *cls,
              gboolean rtl, const GString *css)
{
  g_string_append_printf (out, "<%s", tag);
  if (cls != NULL)
    g_string_append_printf (out, " class=\"%s\"", cls);
  if (rtl)
    g_string_append (out, " dir=\"rtl\"");
  if (css != NULL && css->len > 0)
    append_attr (out, "style", css->str);
  g_string_append_c (out, '>');
}

/* ---- Ids -------------------------------------------------------------- */

/* An id no other element in the book has: `want`, or `want` with a
 * number after it. */
static const char *
claim_id (Epub *e, const char *want)
{
  char *id = g_strdup (want);

  for (int n = 2; g_hash_table_contains (e->ids, id); n++)
    {
      g_free (id);
      id = g_strdup_printf ("%s-%d", want, n);
    }
  g_hash_table_add (e->ids, id);
  return id;
}

/* A bookmark's id: its name as far as an XML name can spell it, which is
 * ASCII letters, digits, "-", "_" and ".", behind a prefix that keeps it
 * apart from the ids the export makes up. */
static const char *
bookmark_id (Epub *e, const char *name)
{
  GString *id = g_string_new ("bm-");
  const char *claimed;

  for (const char *p = name; *p != '\0'; p++)
    g_string_append_c (id, g_ascii_isalnum (*p) || *p == '-' || *p == '_' || *p == '.'
                             ? *p : '_');
  claimed = claim_id (e, id->str);
  g_string_free (id, TRUE);
  return claimed;
}

/* ---- Pictures --------------------------------------------------------- */

/* The picture's file in the book, made the first time it is met.  An
 * EPUB reader need only show PNG, JPEG, GIF, SVG and WebP, so a BMP is
 * made a PNG, as is anything else the containers do not keep as it is.
 * A picture used twice is one file. */
static Image *
image_for (Epub *e, W42ObjectIdx idx, const W42Object *object)
{
  Image *image;
  gpointer known;
  const char *ext = "png", *mime = "image/png";
  GBytes *bytes;

  if (g_hash_table_lookup_extended (e->images, GUINT_TO_POINTER (idx), NULL, &known))
    return known;

  bytes = object->data != NULL ? w42_image_for_container (object->data, &ext, &mime) : NULL;
  if (bytes != NULL && g_str_equal (mime, "image/bmp"))
    {
      GBytes *png = w42_image_to_png (bytes);

      g_bytes_unref (bytes);
      bytes = png;
      ext = "png";
      mime = "image/png";
    }
  if (bytes == NULL)
    {
      g_hash_table_insert (e->images, GUINT_TO_POINTER (idx), NULL);
      return NULL;
    }
  if (g_str_equal (mime, "image/jpeg"))
    ext = "jpg";

  image = g_hash_table_lookup (e->image_bytes, bytes);
  if (image != NULL)
    g_bytes_unref (bytes);
  else
    {
      guint n = e->image_list->len + 1;

      image = g_new0 (Image, 1);
      image->id = g_strdup_printf ("img%u", n);
      image->href = g_strdup_printf ("images/img%u.%s", n, ext);
      image->mime = mime;
      image->bytes = bytes;
      g_ptr_array_add (e->image_list, image);
      g_hash_table_insert (e->image_bytes, bytes, image);
    }
  if (idx == e->cover)
    {
      /* One cover: the first big picture before the first heading. */
      gboolean taken = FALSE;

      for (guint i = 0; i < e->image_list->len; i++)
        taken |= ((Image *) g_ptr_array_index (e->image_list, i))->cover;
      image->cover = !taken;
    }
  g_hash_table_insert (e->images, GUINT_TO_POINTER (idx), image);
  return image;
}

static void
write_picture (Epub *e, GString *out, W42ObjectIdx idx)
{
  const W42Object *object = w42_object_table_get (w42_pt_object_table (e->pt), idx);
  const Image *image;
  GString *css;

  if (object == NULL || (image = image_for (e, idx, object)) == NULL)
    return;

  css = g_string_new (NULL);
  if (object->width > 0)
    {
      double share = object->width * 100.0 / MAX (e->column, 1440);

      /* A picture that is a good part of the page's width is that part
       * of the reader's; a small one, an icon in the text, keeps its
       * size against the type.  Neither may be wider than the screen. */
      g_string_append (css, "width:");
      if (share >= 100.0)
        g_string_append (css, "100%;");
      else if (share >= 25.0)
        {
          css_number (css, share);
          g_string_append (css, "%;");
        }
      else
        {
          css_number (css, object->width / look_em (&e->look[CLS_P]));
          g_string_append (css, "em;");
        }
    }
  g_string_append (out, "<img src=\"");
  append_href (out, image->href);
  g_string_append_c (out, '"');
  /* A shape's text is what it says; a photograph has no words to give. */
  append_attr (out, "alt", object->text != NULL ? object->text : "");
  if (object->wrap == W42_WRAP_LEFT)
    g_string_append (out, " class=\"left\"");
  else if (object->wrap == W42_WRAP_RIGHT)
    g_string_append (out, " class=\"right\"");
  if (css->len > 0)
    append_attr (out, "style", css->str);
  g_string_append (out, "/>");
  g_string_free (css, TRUE);
}

/* ---- Runs ------------------------------------------------------------- */

static const char *
chapter_file (Epub *e, int chapter)
{
  return ((Chapter *) g_ptr_array_index (e->chapters, chapter))->file;
}

/* Where a link goes in the book, or NULL when it goes nowhere a reader
 * can follow: a bookmark becomes the chapter file it ended up in and its
 * id there; a web or mail address stays what it was; a script, a path
 * on the author's disk, or a bookmark that is not there is not a link. */
static char *
link_href (Epub *e, const char *target, int chapter)
{
  const char *p;

  if (target == NULL)
    return NULL;
  while (*target == ' ' || *target == '\t')
    target++;
  if (*target == '#')
    {
      const Anchor *a = g_hash_table_lookup (e->bookmarks, target + 1);

      if (a == NULL)
        return NULL;
      if (a->chapter == chapter)
        return g_strconcat ("#", a->id, NULL);
      return g_strconcat (chapter_file (e, a->chapter), "#", a->id, NULL);
    }
  /* Word makes a link of an address typed without its scheme. */
  if (g_ascii_strncasecmp (target, "www.", 4) == 0)
    return g_strconcat ("http://", target, NULL);
  /* An absolute URL: a scheme of two letters or more, since "c:" is a
   * drive and not a scheme. */
  for (p = target; g_ascii_isalnum (*p) || *p == '+' || *p == '-' || *p == '.'; p++)
    ;
  if (*p != ':' || p - target < 2 || !g_ascii_isalpha (*target) ||
      w42_html_link_is_script (target) || g_ascii_strncasecmp (target, "file:", 5) == 0)
    return NULL;
  return g_strdup (target);
}

/* A run's language, where it is not the book's.  "Not language" is not
 * a language the reader can do anything with, so it is not said. */
static const char *
run_lang (Epub *e, const W42CharFmt *ch)
{
  if (ch->lang == NULL || g_ascii_strcasecmp (ch->lang, W42_LANG_NONE) == 0 ||
      g_ascii_strcasecmp (ch->lang, e->lang) == 0 || !lang_tag_ok (ch->lang))
    return NULL;
  return ch->lang;
}

static void
write_noteref (Epub *e, GString *out, const W42Run *run, int chapter)
{
  Chapter *c = g_ptr_array_index (e->chapters, chapter);
  char label[16];
  char want[32];
  gboolean known = FALSE;
  int note_chapter;

  if (run->endnote)
    w42_roman_lower (run->footnote, label, sizeof label);
  else
    g_snprintf (label, sizeof label, "%d", run->footnote);

  /* A mark with no note behind it, or whose note is written with another
   * chapter -- a note's own reference to another note -- is the number
   * and nothing to follow. */
  note_chapter = GPOINTER_TO_INT (g_hash_table_lookup (e->note_chapter,
                                                       GINT_TO_POINTER (run->footnote_id)));
  if (!g_hash_table_contains (e->notes, GINT_TO_POINTER (run->footnote_id)) ||
      note_chapter != chapter + 1)
    {
      g_string_append_printf (out, "<sup>%s</sup>", label);
      return;
    }
  for (guint i = 0; i < c->notes->len; i++)
    known |= g_array_index (c->notes, int, i) == run->footnote_id;
  if (!known)
    g_array_append_val (c->notes, run->footnote_id);

  g_string_append_printf (out, "<sup><a class=\"noteref\" epub:type=\"noteref\""
                               " href=\"#note%d\"", run->footnote_id);
  g_snprintf (want, sizeof want, "ref%d", run->footnote_id);
  if (!g_hash_table_contains (e->ids, want))
    append_attr (out, "id", claim_id (e, want));
  g_string_append_printf (out, ">%s</a></sup>", label);
}

/* A run of text, with what makes it differ from `base`, its paragraph's
 * class, and nothing else: emphasis as elements, the rest as a span. */
static void
write_text_run (Epub *e, GString *out, const W42Block *block, const W42Run *run,
                const W42CharFmt *ch, const CharLook *base, int chapter)
{
  GString *css = g_string_new (NULL);
  const char *lang = run_lang (e, ch);
  char *href = link_href (e, ch->link, chapter);
  const char *id = NULL;
  const char *cls = NULL;
  gboolean span;

  if (ch->bookmark != NULL)
    {
      Anchor *a = g_hash_table_lookup (e->bookmarks, ch->bookmark);

      /* A bookmark's id goes on its first run in the chapter it was
       * found in, which is where links to it were pointed. */
      if (a != NULL && !a->placed && a->chapter == chapter)
        {
          id = a->id;
          a->placed = TRUE;
        }
    }
  if (ch->family != NULL && base->family != NULL &&
      g_ascii_strcasecmp (ch->family, base->family) != 0)
    css_family (css, ch->family);
  if (ch->size > 0 && ch->size != base->size)
    {
      g_string_append (css, "font-size:");
      css_number (css, (double) ch->size / MAX (base->size, 1));
      g_string_append (css, "em;");
    }
  if (!ch->bold && base->bold)
    g_string_append (css, "font-weight:normal;");
  if (!ch->italic && base->italic)
    g_string_append (css, "font-style:normal;");
  if (ch->smallcaps && ch->allcaps)
    cls = "sc caps";
  else if (ch->smallcaps)
    cls = "sc";
  else if (ch->allcaps)
    cls = "caps";
  span = css->len > 0 || cls != NULL || lang != NULL || (id != NULL && href == NULL);

  if (href != NULL)
    {
      g_string_append (out, "<a href=\"");
      append_href (out, href);
      g_string_append_c (out, '"');
      if (id != NULL)
        append_attr (out, "id", id);
      g_string_append_c (out, '>');
    }
  if (span)
    {
      g_string_append (out, "<span");
      if (id != NULL && href == NULL)
        append_attr (out, "id", id);
      if (cls != NULL)
        g_string_append_printf (out, " class=\"%s\"", cls);
      if (css->len > 0)
        append_attr (out, "style", css->str);
      if (lang != NULL)
        {
          append_attr (out, "lang", lang);
          append_attr (out, "xml:lang", lang);
        }
      g_string_append_c (out, '>');
    }
  if (ch->script > 0) g_string_append (out, "<sup>");
  if (ch->script < 0) g_string_append (out, "<sub>");
  if (ch->bold && !base->bold) g_string_append (out, "<b>");
  if (ch->italic && !base->italic) g_string_append (out, "<i>");
  if (ch->underline && href == NULL) g_string_append (out, "<u>");
  if (ch->strikeout || ch->dstrike) g_string_append (out, "<s>");

  append_xml (out, block->text->str + run->byte_offset, run->n_bytes, TRUE);

  if (ch->strikeout || ch->dstrike) g_string_append (out, "</s>");
  if (ch->underline && href == NULL) g_string_append (out, "</u>");
  if (ch->italic && !base->italic) g_string_append (out, "</i>");
  if (ch->bold && !base->bold) g_string_append (out, "</b>");
  if (ch->script < 0) g_string_append (out, "</sub>");
  if (ch->script > 0) g_string_append (out, "</sup>");
  if (span)
    g_string_append (out, "</span>");
  if (href != NULL)
    g_string_append (out, "</a>");

  g_free (href);
  g_string_free (css, TRUE);
}

static void
write_runs (Epub *e, GString *out, guint b, const CharLook *base, int chapter)
{
  const W42Block *block = g_ptr_array_index (e->blocks, b);

  for (guint r = 0; r < block->runs->len; r++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, r);
      const W42CharFmt *ch = &w42_ap_table_get (e->aps, run->ap)->ch;

      /* The book is the text as it reads once the tracked changes are
       * accepted: what was deleted is not in it. */
      if (ch->revision == 2)
        continue;
      if (run->object != W42_OBJECT_NONE)
        write_picture (e, out, run->object);
      else if (run->footnote > 0)
        write_noteref (e, out, run, chapter);
      else if (run->n_bytes > 0)
        write_text_run (e, out, block, run, ch, base, chapter);
    }
}

/* ---- The first pass --------------------------------------------------- */

/* The paragraph's text as the book will have it, less what a tracked
 * change deleted.  `things` says whether it has a picture or a note's
 * mark, which a paragraph of white space and nothing else has not;
 * `pictures` whether pictures are all it has. */
static char *
visible_text (Epub *e, const W42Block *block, gboolean *things, gboolean *pictures)
{
  GString *text = g_string_new (NULL);
  gboolean any_picture = FALSE, any_note = FALSE;

  for (guint r = 0; r < block->runs->len; r++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, r);

      if (w42_ap_table_get (e->aps, run->ap)->ch.revision == 2)
        continue;
      if (run->object != W42_OBJECT_NONE)
        any_picture = TRUE;
      else if (run->footnote > 0)
        any_note = TRUE;
      else
        g_string_append_len (text, block->text->str + run->byte_offset, run->n_bytes);
    }
  *things = any_picture || any_note;
  *pictures = any_picture && !any_note && is_blank_text (text->str);
  return g_string_free (text, FALSE);
}

/* A heading's text for the table of contents: one line, its white space
 * folded, behind the number the page shows it with. */
static char *
heading_label (const char *prefix, const char *text)
{
  GString *label = g_string_new (prefix);
  gboolean space = prefix != NULL;

  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isspace (c))
        space = label->len > 0;
      else if (c != 0x00AD && c != 0xFFFC)
        {
          if (space)
            g_string_append_c (label, ' ');
          space = FALSE;
          g_string_append_unichar (label, c);
        }
    }
  return g_string_free (label, FALSE);
}

static Chapter *
chapter_new (guint n)
{
  Chapter *c = g_new0 (Chapter, 1);

  c->file = g_strdup_printf ("chapter%u.xhtml", n);
  c->body = g_string_new (NULL);
  c->notes = g_array_new (FALSE, FALSE, sizeof (int));
  return c;
}

static void
chapter_free (gpointer data)
{
  Chapter *c = data;

  g_free (c->file);
  g_free (c->title);
  g_string_free (c->body, TRUE);
  g_array_free (c->notes, TRUE);
  g_free (c);
}

/* What each paragraph is, and which chapter it goes in.  A chapter
 * starts at every top-level heading, and at any heading that starts a
 * page; what comes before the first is the front matter, a file of its
 * own.  The numbers the page shows in front of headings -- a numbered
 * list's, or Heading Numbering's -- are counted as the layout counts
 * them, so that the book says what the page says. */
static void
plan_chapters (Epub *e)
{
  gboolean numbering = w42_stylesheet_get_number_headings (e->styles);
  int counters[10] = { 0 };
  int level_n[9] = { 0 };
  W42ListKind level_kind[9] = { W42_LIST_NONE };
  int chapter = 0;
  gboolean content = FALSE;          /* the chapter has something in it */
  gboolean seen_heading = FALSE;

  g_ptr_array_add (e->chapters, chapter_new (1));

  for (guint b = 0; b < e->blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (e->blocks, b);
      const W42ParaFmt *pa = &w42_ap_table_get (e->aps, block->ap)->pa;
      BlockInfo *info = &e->info[b];
      int level = w42_stylesheet_outline (e->styles, pa->style);
      gboolean title = pa->style != NULL && g_ascii_strcasecmp (pa->style, "Title") == 0;
      gboolean things, pictures;
      char *text;
      char *prefix = NULL;

      info->chapter = -1;
      info->heading = -1;
      if (block->note >= 0)
        {
          info->kind = K_NOTE;
          info->cls = CLS_NOTE;
          info->note_first = b == 0 ||
            ((const W42Block *) g_ptr_array_index (e->blocks, b - 1))->note != block->note;
          continue;
        }

      if (block->table < 0)
        {
          int list_n = 0;

          if (w42_list_is_numbered (pa->list))
            {
              int lv = MIN (pa->list_level, 8);

              if (pa->list_start > 0)
                level_n[lv] = pa->list_start;
              else if (pa->list != level_kind[lv])
                level_n[lv] = 1;
              else
                level_n[lv]++;
              level_kind[lv] = pa->list;
              for (int deeper = lv + 1; deeper < 9; deeper++)
                {
                  level_n[deeper] = 0;
                  level_kind[deeper] = W42_LIST_NONE;
                }
              list_n = level_n[lv];
            }
          else if (pa->list == W42_LIST_NONE)
            for (int lv = 0; lv < 9; lv++)
              {
                level_n[lv] = 0;
                level_kind[lv] = W42_LIST_NONE;
              }

          if (w42_list_is_numbered (pa->list))
            {
              char marker[16];

              w42_list_marker (pa->list, list_n, marker, sizeof marker);
              prefix = g_strdup (marker);
            }
          else if (pa->list == W42_LIST_NONE && level > 0 && level < 10)
            {
              counters[level]++;
              for (int l = level + 1; l < 10; l++)
                counters[l] = 0;
              if (numbering)
                {
                  GString *number = g_string_new (NULL);

                  for (int l = 1; l <= level; l++)
                    g_string_append_printf (number, l > 1 ? ".%d" : "%d", counters[l]);
                  prefix = g_string_free (number, FALSE);
                }
            }
        }

      text = visible_text (e, block, &things, &pictures);
      info->image = pictures;
      if (block->table >= 0)
        info->kind = K_CELL;
      else if (!things && is_blank_text (text))
        info->kind = K_BLANK;
      else if ((level >= 1 && level <= 6) || title)
        info->kind = K_HEADING;
      else if (pa->list != W42_LIST_NONE)
        info->kind = K_LIST;
      else if (!things && is_scene_break (text))
        info->kind = K_SCENE;
      else
        info->kind = K_PARA;

      info->cls = CLS_P;
      if (info->kind == K_HEADING)
        {
          Heading h = { 0 };
          Chapter *c;

          info->tag = title ? 1 : level;
          info->cls = title ? CLS_TITLE : CLS_H1 + level - 1;
          if (!title)
            {
              if ((level == 1 || pa->page_break_before || pa->section_break) && content)
                {
                  chapter++;
                  g_ptr_array_add (e->chapters, chapter_new (chapter + 1));
                  content = FALSE;
                }
              seen_heading = TRUE;
            }
          c = g_ptr_array_index (e->chapters, chapter);
          if (!content && !title)
            c->opens_with_heading = TRUE;
          h.level = title ? 0 : level;
          h.chapter = chapter;
          h.id = e->headings->len + 1;
          h.prefix = prefix;
          h.label = heading_label (prefix, text);
          prefix = NULL;
          if (c->title == NULL && *h.label != '\0')
            c->title = g_strdup (h.label);
          info->heading = e->headings->len;
          g_array_append_val (e->headings, h);
        }
      else if (info->kind == K_PARA && pictures && !seen_heading &&
               e->cover == W42_OBJECT_NONE)
        {
          /* A big picture in the front matter is the book's cover, as
           * far as a reader's library shelf is concerned. */
          for (guint r = 0; r < block->runs->len; r++)
            {
              const W42Run *run = &g_array_index (block->runs, W42Run, r);
              const W42Object *object;

              if (run->object == W42_OBJECT_NONE)
                continue;
              object = w42_object_table_get (w42_pt_object_table (e->pt), run->object);
              if (object != NULL && object->width >= 2880)
                e->cover = run->object;
              break;
            }
        }
      if (info->kind != K_BLANK)
        content = TRUE;
      info->chapter = chapter;
      if (info->kind == K_CELL)
        info->written = w42_ap_table_get (e->aps, block->cell_ap)->pa.cell_vspan
                          != W42_CELL_COVERED;
      else
        info->written = info->kind != K_BLANK;
      g_free (prefix);
      g_free (text);
    }
}

/* Which notes are referred to and from which chapter, and so which of
 * their paragraphs reach the book: a note goes at the end of the chapter
 * its mark is in. */
static void
plan_notes (Epub *e)
{
  for (guint b = 0; b < e->blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (e->blocks, b);
      const BlockInfo *info = &e->info[b];

      if (info->kind == K_NOTE)
        {
          GArray *list = g_hash_table_lookup (e->notes, GINT_TO_POINTER (block->note));

          if (list == NULL)
            {
              list = g_array_new (FALSE, FALSE, sizeof (guint));
              g_hash_table_insert (e->notes, GINT_TO_POINTER (block->note), list);
            }
          g_array_append_val (list, b);
          continue;
        }
      if (!info->written)
        continue;
      for (guint r = 0; r < block->runs->len; r++)
        {
          const W42Run *run = &g_array_index (block->runs, W42Run, r);

          if (run->footnote > 0 &&
              w42_ap_table_get (e->aps, run->ap)->ch.revision != 2 &&
              !g_hash_table_contains (e->note_chapter, GINT_TO_POINTER (run->footnote_id)))
            g_hash_table_insert (e->note_chapter, GINT_TO_POINTER (run->footnote_id),
                                 GINT_TO_POINTER (info->chapter + 1));
        }
    }

  for (guint b = 0; b < e->blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (e->blocks, b);
      BlockInfo *info = &e->info[b];
      gboolean things, pictures;
      char *text;

      if (info->kind != K_NOTE)
        continue;
      info->chapter = GPOINTER_TO_INT (g_hash_table_lookup (e->note_chapter,
                                                            GINT_TO_POINTER (block->note)));
      info->chapter--;
      if (info->chapter < 0)
        continue;
      /* The first paragraph carries the note's number, whatever else
       * it has; a blank one after it is nothing. */
      text = visible_text (e, block, &things, &pictures);
      info->written = info->note_first || things || !is_blank_text (text);
      g_free (text);
    }
}

/* What each class's rule is, from what its paragraphs are; and the
 * book's language, from what its text is marked with. */
static void
plan_looks (Epub *e)
{
  const W42Style *normal = w42_stylesheet_find (e->styles, "Normal");

  for (guint b = 0; b < e->blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (e->blocks, b);
      const BlockInfo *info = &e->info[b];
      Look *look = &e->look[info->cls];
      gboolean votes = info->written &&
        (info->kind == K_PARA || info->kind == K_HEADING || info->kind == K_NOTE);

      if (!info->written)
        continue;
      if (votes && !info->image)
        vote_para (look, &w42_ap_table_get (e->aps, block->ap)->pa);
      for (guint r = 0; r < block->runs->len; r++)
        {
          const W42Run *run = &g_array_index (block->runs, W42Run, r);
          const W42CharFmt *ch = &w42_ap_table_get (e->aps, run->ap)->ch;

          if (run->object != W42_OBJECT_NONE || run->footnote > 0 ||
              run->n_chars == 0 || ch->revision == 2)
            continue;
          if (votes)
            vote_char (look, ch, run->n_chars);
          if (ch->lang != NULL)
            {
              gsize n = GPOINTER_TO_SIZE (g_hash_table_lookup (e->lang_votes, ch->lang));

              g_hash_table_insert (e->lang_votes, (gpointer) ch->lang,
                                   GSIZE_TO_POINTER (n + run->n_chars));
            }
        }
    }

  decide_look (&e->look[CLS_P], normal);
  for (int level = 1; level <= 6; level++)
    {
      char name[16];
      const W42Style *style;

      g_snprintf (name, sizeof name, "Heading %d", level);
      style = w42_stylesheet_find (e->styles, name);
      decide_look (&e->look[CLS_H1 + level - 1], style != NULL ? style : normal);
    }
  decide_look (&e->look[CLS_TITLE], w42_stylesheet_find (e->styles, "Title"));
  decide_look (&e->look[CLS_NOTE], normal);

  /* The book's language: Normal's, as the document says it; else what
   * most of its text is marked as, since a file may mark the text and
   * not the style; else the desktop's, as the document's is when it
   * says nothing. */
  e->lang = normal != NULL ? normal->ch.lang : NULL;
  if (e->lang == NULL || g_ascii_strcasecmp (e->lang, W42_LANG_NONE) == 0 ||
      !lang_tag_ok (e->lang))
    {
      GHashTableIter iter;
      gpointer key, value;
      gsize best = 0;

      e->lang = NULL;
      g_hash_table_iter_init (&iter, e->lang_votes);
      while (g_hash_table_iter_next (&iter, &key, &value))
        if (GPOINTER_TO_SIZE (value) > best &&
            g_ascii_strcasecmp (key, W42_LANG_NONE) != 0 && lang_tag_ok (key))
          {
            best = GPOINTER_TO_SIZE (value);
            e->lang = key;
          }
    }
  if (e->lang == NULL)
    e->lang = w42_lang_default ();
  if (!lang_tag_ok (e->lang))
    e->lang = "en-US";
}

/* Every bookmark's chapter and id, before anything is written, since a
 * link may come before the text it points to. */
static void
plan_bookmarks (Epub *e)
{
  for (int pass = 0; pass < 2; pass++)
    for (guint b = 0; b < e->blocks->len; b++)
      {
        const W42Block *block = g_ptr_array_index (e->blocks, b);
        const BlockInfo *info = &e->info[b];

        /* The body first, then the notes, which is the order the ids
         * are placed in. */
        if (!info->written || info->chapter < 0 || (info->kind == K_NOTE) != (pass == 1))
          continue;
        for (guint r = 0; r < block->runs->len; r++)
          {
            const W42Run *run = &g_array_index (block->runs, W42Run, r);
            const W42CharFmt *ch = &w42_ap_table_get (e->aps, run->ap)->ch;
            Anchor *a;

            if (ch->bookmark == NULL || run->object != W42_OBJECT_NONE ||
                run->footnote > 0 || run->n_bytes == 0 || ch->revision == 2 ||
                g_hash_table_contains (e->bookmarks, ch->bookmark))
              continue;
            a = g_new0 (Anchor, 1);
            a->chapter = info->chapter;
            a->id = g_strdup (bookmark_id (e, ch->bookmark));
            g_hash_table_insert (e->bookmarks, (gpointer) ch->bookmark, a);
          }
      }
}

/* ---- The second pass -------------------------------------------------- */

typedef struct {
  ListState lists;
  int       table_open;
  int       row_open;
  gboolean  wrote;          /* the file has something in it */
  gboolean  pending_blank;  /* blank paragraphs since the last thing written */
  gboolean  pending_break;  /* one of them started a page */
  gboolean  after_mark;     /* the last thing was a heading or a scene break */
} BodyState;

static gboolean
list_breaks (const ListState *ls, int depth)
{
  return depth == 0 || !ls->li_open[depth - 1];
}

static void
lists_close (GString *out, ListState *ls, int want)
{
  while (ls->depth > want)
    {
      ls->depth--;
      if (ls->li_open[ls->depth])
        g_string_append (out, "</li>");
      ls->li_open[ls->depth] = FALSE;
      g_string_append (out, w42_list_is_bullet (ls->kind[ls->depth]) ? "</ul>" : "</ol>");
      if (list_breaks (ls, ls->depth))
        g_string_append_c (out, '\n');
    }
}

/* Lists nested by level, a deeper list inside the item before it, as
 * HTML has them.  A list that starts deeper than the one round it has
 * no item there to go in, and an item without a marker is made for it,
 * since a list directly inside a list is not HTML. */
static void
lists_open (GString *out, ListState *ls, const W42ParaFmt *pa)
{
  int want = MIN (pa->list_level, 8) + 1;

  while (ls->depth > want ||
         (ls->depth > 0 && ls->depth == want &&
          (ls->kind[ls->depth - 1] != pa->list ||
           (pa->list_start > 0 && w42_list_is_numbered (pa->list)))))
    lists_close (out, ls, ls->depth - 1);
  if (ls->depth == want && ls->li_open[want - 1])
    {
      g_string_append (out, "</li>");
      ls->li_open[want - 1] = FALSE;
      if (list_breaks (ls, want - 1))
        g_string_append_c (out, '\n');
    }
  while (ls->depth < want)
    {
      if (ls->depth > 0 && !ls->li_open[ls->depth - 1])
        {
          g_string_append (out, "<li class=\"nomark\">");
          ls->li_open[ls->depth - 1] = TRUE;
        }
      if (w42_list_is_bullet (pa->list))
        {
          const char *style = pa->list == W42_LIST_BULLET_CIRCLE ? "circle"
                            : pa->list == W42_LIST_BULLET_SQUARE ? "square" : NULL;

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
          if (pa->list_start > 0 && ls->depth + 1 == want)
            g_string_append_printf (out, " start=\"%d\"", pa->list_start);
          g_string_append_c (out, '>');
        }
      if (list_breaks (ls, ls->depth))
        g_string_append_c (out, '\n');
      ls->li_open[ls->depth] = FALSE;
      ls->kind[ls->depth++] = pa->list;
    }
}

static void
write_cell (Epub *e, GString *out, guint b, BodyState *s)
{
  const W42Block *block = g_ptr_array_index (e->blocks, b);
  const W42Block *prev = b > 0 ? g_ptr_array_index (e->blocks, b - 1) : NULL;
  const W42Block *next = b + 1 < e->blocks->len ? g_ptr_array_index (e->blocks, b + 1) : NULL;
  const W42ParaFmt *pa = &w42_ap_table_get (e->aps, block->ap)->pa;
  const W42ParaFmt *cpa = &w42_ap_table_get (e->aps, block->cell_ap)->pa;
  const W42TableProps *props = w42_pt_table_props (e->pt, block->table);
  /* The rows that repeat at the top of every page are the table's
   * header, which a reader reading aloud announces as one. */
  const char *cell = props != NULL && block->row < props->header_rows ? "th" : "td";
  gboolean cell_start, cell_end;

  if (next != NULL && next->note >= 0)
    next = NULL;
  if (block->table != s->table_open)
    {
      const W42TableProps *tp = props;

      g_string_append_printf (out, "<table%s>\n",
                              tp == NULL || tp->borders ? " class=\"ruled\"" : "");
      if (tp != NULL && tp->widths != NULL && tp->n_cols > 0 &&
          (int) tp->widths->len >= tp->n_cols)
        {
          /* The columns' shares of the table, which is as wide as the
           * screen allows; widths of their own would not fit a phone. */
          double total = 0;
          gboolean all = TRUE;

          for (int c = 0; c < tp->n_cols; c++)
            {
              int cw = g_array_index (tp->widths, int, c);

              all &= cw > 0;
              total += MAX (cw, 0);
            }
          if (all && total > 0)
            {
              g_string_append (out, "<colgroup>");
              for (int c = 0; c < tp->n_cols; c++)
                {
                  g_string_append (out, "<col style=\"width:");
                  css_number (out, g_array_index (tp->widths, int, c) * 100.0 / total);
                  g_string_append (out, "%\"/>");
                }
              g_string_append (out, "</colgroup>\n");
            }
        }
      s->table_open = block->table;
      s->row_open = -1;
    }
  if (block->row != s->row_open)
    {
      if (s->row_open >= 0)
        g_string_append (out, "</tr>\n");
      g_string_append (out, "<tr>");
      s->row_open = block->row;
    }

  cell_start = prev == NULL || prev->table != block->table ||
               prev->row != block->row || prev->col != block->col;
  cell_end = next == NULL || next->table != block->table ||
             next->row != block->row || next->col != block->col;

  /* A cell a merge from above swallowed is the merging cell's rowspan,
   * and not written. */
  if (e->info[b].written)
    {
      static const ParaLook CELL = { W42_ALIGN_LEFT, 0, 0, 0, 0, 0 };
      GString *css = g_string_new (NULL);

      if (cell_start)
        {
          g_string_append_printf (out, "<%s", cell);
          if (block->span > 1)
            g_string_append_printf (out, " colspan=\"%d\"", block->span);
          if (cpa->cell_vspan > 1)
            {
              /* No further than the table goes, whatever the mark says. */
              int rows = w42_pt_table_rows (e->pt, block->table) - block->row;
              int vspan = MIN ((int) cpa->cell_vspan, MAX (rows, 1));

              if (vspan > 1)
                g_string_append_printf (out, " rowspan=\"%d\"", vspan);
            }
          g_string_append_c (out, '>');
        }
      para_style (css, pa, &CELL, look_em (&e->look[CLS_P]), TRUE);
      open_element (out, "p", NULL, pa->rtl, css);
      if (block->runs->len == 0)
        g_string_append (out, "&#160;");
      write_runs (e, out, b, &e->look[CLS_P].ch, e->info[b].chapter);
      g_string_append (out, "</p>");
      if (cell_end)
        g_string_append_printf (out, "</%s>", cell);
      g_string_free (css, TRUE);
    }

  if (next == NULL || next->table != block->table)
    {
      g_string_append (out, "</tr>\n</table>\n");
      s->table_open = -1;
      s->row_open = -1;
    }
}

static void
write_body (Epub *e)
{
  static const char *const H[] = { "h1", "h2", "h3", "h4", "h5", "h6" };
  BodyState s;
  int cur = -1;
  Chapter *chapter = NULL;

  memset (&s, 0, sizeof s);
  for (guint b = 0; b < e->blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (e->blocks, b);
      const BlockInfo *info = &e->info[b];
      const W42ParaFmt *pa = &w42_ap_table_get (e->aps, block->ap)->pa;
      const Look *look = &e->look[info->cls];
      GString *out, *css;
      gboolean new_page;

      if (info->kind == K_NOTE)
        continue;
      if (info->chapter != cur)
        {
          if (chapter != NULL)
            lists_close (chapter->body, &s.lists, 0);
          cur = info->chapter;
          chapter = g_ptr_array_index (e->chapters, cur);
          memset (&s, 0, sizeof s);
          s.table_open = s.row_open = -1;
        }
      out = chapter->body;

      /* A blank paragraph ends a list, as it starts the numbering over
       * on the page. */
      if (info->kind != K_LIST)
        lists_close (out, &s.lists, 0);
      if (info->kind == K_BLANK)
        {
          s.pending_blank = TRUE;
          if (pa->page_break_before || pa->section_break)
            s.pending_break = TRUE;
          continue;
        }

      /* A page break at the top of a file is the file's own; elsewhere
       * a reader that pages its text can honour it. */
      new_page = s.wrote && (s.pending_break || pa->page_break_before || pa->section_break);
      /* Blank lines, a run of them as one, where they part two pieces
       * of text; next to a heading, a scene break or a new page, or at
       * either end of a file, they say nothing the margins do not. */
      if (s.pending_blank && s.wrote && !s.after_mark && !new_page &&
          (info->kind == K_PARA || (info->kind == K_LIST && s.lists.depth == 0) ||
           (info->kind == K_CELL && block->table != s.table_open)))
        g_string_append (out, "<p class=\"empty\">&#160;</p>\n");
      s.pending_blank = FALSE;
      s.pending_break = FALSE;

      css = g_string_new (NULL);
      switch ((Kind) info->kind)
        {
        case K_CELL:
          write_cell (e, out, b, &s);
          break;

        case K_LIST:
          lists_open (out, &s.lists, pa);
          para_style (css, pa, &look->pa, look_em (look), FALSE);
          if (new_page)
            g_string_append (css, "page-break-before:always;break-before:page;");
          open_element (out, "li", NULL, pa->rtl, css);
          write_runs (e, out, b, &look->ch, cur);
          s.lists.li_open[s.lists.depth - 1] = TRUE;
          break;

        case K_HEADING:
          {
            const Heading *h = &g_array_index (e->headings, Heading, info->heading);
            const char *tag = H[CLAMP (info->tag, 1, 6) - 1];

            para_style (css, pa, &look->pa, look_em (look), TRUE);
            /* A chapter's heading starts a page by the stylesheet. */
            if (new_page && (info->tag != 1 || info->cls == CLS_TITLE))
              g_string_append (css, "page-break-before:always;break-before:page;");
            g_string_append_printf (out, "<%s id=\"heading%d\"", tag, h->id);
            if (info->cls == CLS_TITLE)
              g_string_append (out, " class=\"title\"");
            if (pa->rtl)
              g_string_append (out, " dir=\"rtl\"");
            if (css->len > 0)
              append_attr (out, "style", css->str);
            g_string_append_c (out, '>');
            if (h->prefix != NULL)
              {
                append_xml (out, h->prefix, -1, TRUE);
                g_string_append_c (out, ' ');
              }
            write_runs (e, out, b, &look->ch, cur);
            g_string_append_printf (out, "</%s>\n", tag);
          }
          break;

        case K_SCENE:
          if (new_page)
            g_string_append (css, "page-break-before:always;break-before:page;");
          open_element (out, "p", "scenebreak", pa->rtl, css);
          write_runs (e, out, b, &look->ch, cur);
          g_string_append (out, "</p>\n");
          break;

        case K_PARA:
        default:
          /* A picture on its own is centred, whatever the paragraph
           * round it says. */
          if (!info->image)
            para_style (css, pa, &look->pa, look_em (look), TRUE);
          if (new_page)
            g_string_append (css, "page-break-before:always;break-before:page;");
          open_element (out, "p", info->image ? "image" : NULL, pa->rtl, css);
          write_runs (e, out, b, &look->ch, cur);
          g_string_append (out, "</p>\n");
          break;
        }
      g_string_free (css, TRUE);
      s.wrote = TRUE;
      s.after_mark = info->kind == K_HEADING || info->kind == K_SCENE;
    }
  if (chapter != NULL)
    lists_close (chapter->body, &s.lists, 0);
}

/* The notes a chapter refers to, at its end, in the order it refers to
 * them: each an aside a reader can show where the mark is tapped. */
static void
write_notes (Epub *e)
{
  const Look *look = &e->look[CLS_NOTE];

  for (guint c = 0; c < e->chapters->len; c++)
    {
      Chapter *chapter = g_ptr_array_index (e->chapters, c);

      /* A note may refer to another, which joins the list as it goes. */
      for (guint i = 0; i < chapter->notes->len; i++)
        {
          int id = g_array_index (chapter->notes, int, i);
          GArray *list = g_hash_table_lookup (e->notes, GINT_TO_POINTER (id));
          GString *out = chapter->body;
          gboolean first = TRUE;

          if (list == NULL || list->len == 0)
            continue;
          for (guint k = 0; k < list->len; k++)
            {
              guint b = g_array_index (list, guint, k);
              const W42Block *block = g_ptr_array_index (e->blocks, b);
              const W42ParaFmt *pa = &w42_ap_table_get (e->aps, block->ap)->pa;
              GString *css;

              if (!e->info[b].written)
                continue;
              if (first)
                {
                  char want[32];

                  g_snprintf (want, sizeof want, "note%d", id);
                  g_string_append_printf (out, "<aside class=\"note\" epub:type=\"%s\"",
                                          block->note_end ? "endnote" : "footnote");
                  append_attr (out, "id", claim_id (e, want));
                  g_string_append (out, ">\n");
                }
              css = g_string_new (NULL);
              para_style (css, pa, &look->pa, look_em (look), TRUE);
              open_element (out, "p", NULL, pa->rtl, css);
              g_string_free (css, TRUE);
              if (first)
                {
                  char label[16];

                  if (block->note_end)
                    w42_roman_lower (block->note_number, label, sizeof label);
                  else
                    g_snprintf (label, sizeof label, "%d", block->note_number);
                  g_string_append_printf (out, "<a class=\"noteback\" href=\"#ref%d\">%s</a> ",
                                          id, label);
                  first = FALSE;
                }
              write_runs (e, out, b, &look->ch, (int) c);
              g_string_append (out, "</p>\n");
            }
          if (!first)
            g_string_append (out, "</aside>\n");
        }
    }
}

/* ---- The stylesheet --------------------------------------------------- */

/* A class's rule: its face and size where they differ from the body's,
 * its weight and slant, its alignment, and the space round it. */
static void
css_look (GString *css, const char *selector, const Look *look, const Look *body,
          gboolean heading)
{
  static const char *const ALIGN[] = { NULL, "center", "right", "justify" };
  double em = look_em (look);
  GString *rule = g_string_new (NULL);

  if (look != body && look->ch.family != NULL &&
      g_strcmp0 (look->ch.family, body->ch.family) != 0)
    css_family (rule, look->ch.family);
  if (look != body && look->ch.size != body->ch.size)
    {
      g_string_append (rule, "font-size:");
      css_number (rule, (double) look->ch.size / MAX (body->ch.size, 1));
      g_string_append (rule, "em;");
    }
  if (heading || look->ch.bold)
    g_string_append_printf (rule, "font-weight:%s;", look->ch.bold ? "bold" : "normal");
  if (heading || look->ch.italic)
    g_string_append_printf (rule, "font-style:%s;", look->ch.italic ? "italic" : "normal");
  /* Left is the default, except where the rule is for something that
   * would otherwise take the body's alignment or a reader's own. */
  if (ALIGN[MIN ((guint) look->pa.align, 3)] != NULL)
    g_string_append_printf (rule, "text-align:%s;", ALIGN[MIN ((guint) look->pa.align, 3)]);
  else if (heading || look != body)
    g_string_append (rule, "text-align:left;");
  css_em (rule, "text-indent", look->pa.indent_first, em);
  g_string_append (rule, "margin:");
  if (look->pa.space_before == 0 && look->pa.indent_right == 0 &&
      look->pa.space_after == 0 && look->pa.indent_left == 0)
    g_string_append_c (rule, '0');
  else
    {
      css_length (rule, look->pa.space_before, em);
      g_string_append_c (rule, ' ');
      css_length (rule, look->pa.indent_right, em);
      g_string_append_c (rule, ' ');
      css_length (rule, look->pa.space_after, em);
      g_string_append_c (rule, ' ');
      css_length (rule, look->pa.indent_left, em);
    }
  g_string_append_c (rule, ';');
  if (heading)
    g_string_append (rule, "page-break-after:avoid;break-after:avoid;");

  g_string_append_printf (css, "%s {", selector);
  for (const char *p = rule->str; *p != '\0'; p++)
    {
      if (p == rule->str || p[-1] == ';')
        g_string_append_c (css, ' ');
      g_string_append_c (css, *p);
    }
  g_string_append (css, " }\n");
  g_string_free (rule, TRUE);
}

static char *
make_css (Epub *e)
{
  const Look *body = &e->look[CLS_P];
  GString *css = g_string_new ("@charset \"UTF-8\";\n");

  g_string_append (css, "body { ");
  css_family (css, body->ch.family);
  g_string_append (css, " margin:0; padding:0; }\n");
  css_look (css, "p", body, body, FALSE);
  for (int level = 1; level <= 6; level++)
    if (e->look[CLS_H1 + level - 1].used)
      {
        char selector[4] = { 'h', (char) ('0' + level), '\0', '\0' };

        css_look (css, selector, &e->look[CLS_H1 + level - 1], body, TRUE);
      }
  /* A chapter's heading starts a page, as a chapter does in a book;
   * the book's title does whatever its page says. */
  g_string_append (css, "h1 { page-break-before:always; break-before:page; }\n");
  if (e->look[CLS_TITLE].used)
    {
      css_look (css, "h1.title", &e->look[CLS_TITLE], body, TRUE);
      g_string_append (css, "h1.title { page-break-before:auto; break-before:auto; }\n");
    }

  g_string_append (css, "li { text-indent:0; margin-top:");
  css_number (css, body->pa.space_before / look_em (body));
  g_string_append (css, body->pa.space_before != 0 ? "em;" : ";");
  g_string_append (css, " margin-bottom:");
  css_number (css, body->pa.space_after / look_em (body));
  g_string_append (css, body->pa.space_after != 0 ? "em;" : ";");
  if (body->pa.align != W42_ALIGN_LEFT)
    g_string_append_printf (css, " text-align:%s;",
                            body->pa.align == W42_ALIGN_CENTER ? "center"
                            : body->pa.align == W42_ALIGN_RIGHT ? "right" : "justify");
  g_string_append (css, " }\n");
  g_string_append (css,
    "li.nomark { list-style-type:none; }\n"
    "p.empty { text-indent:0; }\n"
    "p.scenebreak { text-align:center; text-indent:0; margin:1em 0; }\n"
    "p.image { text-align:center; text-indent:0; }\n"
    "img { max-width:100%; height:auto; }\n"
    "img.left { float:left; max-width:50%; margin:0 1em 0.5em 0; }\n"
    "img.right { float:right; max-width:50%; margin:0 0 0.5em 1em; }\n"
    ".sc { font-variant:small-caps; }\n"
    ".caps { text-transform:uppercase; }\n"
    "table { border-collapse:collapse; margin:0.5em 0; max-width:100%; }\n"
    "td, th { padding:0.2em 0.4em; vertical-align:top; }\n"
    "th { font-weight:inherit; }\n"
    "table.ruled td, table.ruled th { border:1px solid; }\n"
    "td p, th p { text-indent:0; margin:0; text-align:left; }\n"
    "a.noteref, a.noteback { text-decoration:none; }\n");
  if (e->look[CLS_NOTE].used)
    {
      const Look *note = &e->look[CLS_NOTE];

      g_string_append (css, "aside.note { margin:0.5em 0;");
      if (note->ch.size != body->ch.size)
        {
          g_string_append (css, " font-size:");
          css_number (css, (double) note->ch.size / MAX (body->ch.size, 1));
          g_string_append (css, "em;");
        }
      g_string_append (css, " }\n");
      /* Inside the aside, which has the note's size already: measured
       * against that, and saying nothing of it again. */
      {
        Look inner = *note, outer = *body;

        inner.ch.family = body->ch.family;
        outer.ch.size = note->ch.size;
        css_look (css, "aside.note p", &inner, &outer, FALSE);
      }
    }
  return g_string_free (css, FALSE);
}

/* ---- The package ------------------------------------------------------ */

static void
begin_xhtml (GString *out, const char *lang, const char *title, gboolean stylesheet)
{
  g_string_append (out,
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE html>\n"
    "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
    "xmlns:epub=\"http://www.idpf.org/2007/ops\"");
  append_attr (out, "lang", lang);
  append_attr (out, "xml:lang", lang);
  g_string_append (out, ">\n<head>\n<meta charset=\"utf-8\"/>\n<title>");
  append_xml (out, title, -1, FALSE);
  g_string_append (out, "</title>\n");
  if (stylesheet)
    g_string_append (out, "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n");
  g_string_append (out, "</head>\n");
}

/* The headings the table of contents lists: the top two levels there
 * are, which in most books is the chapters and their sections.  The
 * title is the book's name, not an entry in it. */
static void
toc_levels (Epub *e, int *top)
{
  *top = 0;
  for (guint i = 0; i < e->headings->len; i++)
    {
      const Heading *h = &g_array_index (e->headings, Heading, i);

      if (h->level > 0 && *h->label != '\0' && (*top == 0 || h->level < *top))
        *top = h->level;
    }
}

static gboolean
toc_entry (const Heading *h, int top)
{
  return top > 0 && h->level >= top && h->level <= top + 1 && *h->label != '\0';
}

static char *
make_nav (Epub *e, const char *title)
{
  GString *out = g_string_new (NULL);
  int top;
  gboolean li_open = FALSE, sub_open = FALSE, nest = FALSE, any = FALSE;
  int body_chapter = 0;

  toc_levels (e, &top);
  begin_xhtml (out, e->lang, title, FALSE);
  g_string_append (out, "<body>\n<nav epub:type=\"toc\" id=\"toc\">\n<ol>\n");
  for (guint i = 0; i < e->headings->len; i++)
    {
      const Heading *h = &g_array_index (e->headings, Heading, i);

      if (!toc_entry (h, top))
        continue;
      if (h->level == top + 1 && nest)
        {
          if (!sub_open)
            g_string_append (out, "\n<ol>\n");
          sub_open = TRUE;
          g_string_append (out, "<li>");
        }
      else
        {
          if (sub_open)
            g_string_append (out, "</ol>\n");
          if (li_open)
            g_string_append (out, "</li>\n");
          sub_open = FALSE;
          li_open = TRUE;
          nest = h->level == top;
          g_string_append (out, "<li>");
        }
      g_string_append (out, "<a href=\"");
      append_href (out, chapter_file (e, h->chapter));
      g_string_append_printf (out, "#heading%d\">", h->id);
      append_xml (out, h->label, -1, FALSE);
      g_string_append (out, "</a>");
      if (sub_open)
        g_string_append (out, "</li>\n");
      any = TRUE;
    }
  if (sub_open)
    g_string_append (out, "</ol>\n");
  if (li_open)
    g_string_append (out, "</li>\n");
  if (!any)
    {
      /* A table of contents may not be empty: a book without headings
       * has one entry, itself. */
      g_string_append (out, "<li><a href=\"");
      append_href (out, chapter_file (e, 0));
      g_string_append (out, "\">");
      append_xml (out, title, -1, FALSE);
      g_string_append (out, "</a></li>\n");
    }
  g_string_append (out, "</ol>\n</nav>\n");

  /* Where the text proper starts, past the front matter, which is where
   * a reader opens a book it has not opened before. */
  for (guint c = 0; c < e->chapters->len; c++)
    if (((Chapter *) g_ptr_array_index (e->chapters, c))->opens_with_heading)
      {
        body_chapter = (int) c;
        break;
      }
  g_string_append (out, "<nav epub:type=\"landmarks\" hidden=\"hidden\">\n<ol>\n"
                        "<li><a epub:type=\"bodymatter\" href=\"");
  append_href (out, chapter_file (e, body_chapter));
  g_string_append (out, "\">Start</a></li>\n</ol>\n</nav>\n</body>\n</html>\n");
  return g_string_free (out, FALSE);
}

/* The same table of contents for readers that know only EPUB 2. */
static char *
make_ncx (Epub *e, const char *uid, const char *title)
{
  GString *out = g_string_new (NULL);
  int top, order = 0;
  gboolean point_open = FALSE, nest = FALSE, any = FALSE;

  toc_levels (e, &top);
  g_string_append (out,
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"");
  append_attr (out, "xml:lang", e->lang);
  g_string_append (out, ">\n<head>\n<meta name=\"dtb:uid\"");
  append_attr (out, "content", uid);
  g_string_append_printf (out, "/>\n<meta name=\"dtb:depth\" content=\"%d\"/>\n"
                          "<meta name=\"dtb:totalPageCount\" content=\"0\"/>\n"
                          "<meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n"
                          "</head>\n<docTitle><text>", top > 0 ? 2 : 1);
  append_xml (out, title, -1, FALSE);
  g_string_append (out, "</text></docTitle>\n<navMap>\n");
  for (guint i = 0; i < e->headings->len; i++)
    {
      const Heading *h = &g_array_index (e->headings, Heading, i);
      gboolean sub;

      if (!toc_entry (h, top))
        continue;
      sub = h->level == top + 1 && nest;
      if (!sub)
        {
          if (point_open)
            g_string_append (out, "</navPoint>\n");
          point_open = TRUE;
          nest = h->level == top;
        }
      g_string_append_printf (out, "<navPoint id=\"nav%d\" playOrder=\"%d\"><navLabel><text>",
                              h->id, ++order);
      append_xml (out, h->label, -1, FALSE);
      g_string_append (out, "</text></navLabel><content src=\"");
      append_href (out, chapter_file (e, h->chapter));
      g_string_append_printf (out, "#heading%d\"/>", h->id);
      g_string_append (out, sub ? "</navPoint>\n" : "\n");
      any = TRUE;
    }
  if (point_open)
    g_string_append (out, "</navPoint>\n");
  if (!any)
    {
      g_string_append (out, "<navPoint id=\"nav0\" playOrder=\"1\"><navLabel><text>");
      append_xml (out, title, -1, FALSE);
      g_string_append (out, "</text></navLabel><content src=\"");
      append_href (out, chapter_file (e, 0));
      g_string_append (out, "\"/></navPoint>\n");
    }
  g_string_append (out, "</navMap>\n</ncx>\n");
  return g_string_free (out, FALSE);
}

static void
opf_element (GString *out, const char *tag, const char *value)
{
  if (value == NULL || is_blank_text (value))
    return;
  g_string_append_printf (out, "<%s>", tag);
  append_xml (out, value, -1, FALSE);
  g_string_append_printf (out, "</%s>\n", tag);
}

static char *
make_opf (Epub *e, const char *uid, const char *title)
{
  const W42DocInfo *info = w42_pt_get_info (e->pt);
  GString *out = g_string_new (NULL);
  GDateTime *now = g_date_time_new_now_utc ();
  char *modified = g_date_time_format (now, "%Y-%m-%dT%H:%M:%SZ");
  const Image *cover = NULL;

  g_string_append (out,
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
    "unique-identifier=\"book-id\"");
  append_attr (out, "xml:lang", e->lang);
  g_string_append (out, ">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                        "<dc:identifier id=\"book-id\">");
  append_xml (out, uid, -1, FALSE);
  g_string_append (out, "</dc:identifier>\n");
  opf_element (out, "dc:title", title);
  opf_element (out, "dc:language", e->lang);
  if (info != NULL)
    {
      opf_element (out, "dc:creator", info->author);
      opf_element (out, "dc:subject", info->subject);
      if (info->keywords != NULL)
        {
          /* Keywords are subjects too, one to an element. */
          char **words = g_strsplit_set (info->keywords, ",;", -1);

          for (char **w = words; *w != NULL; w++)
            opf_element (out, "dc:subject", g_strstrip (*w));
          g_strfreev (words);
        }
      opf_element (out, "dc:description", info->comments);
    }
  g_string_append_printf (out, "<meta property=\"dcterms:modified\">%s</meta>\n", modified);
  for (guint i = 0; i < e->image_list->len; i++)
    if (((const Image *) g_ptr_array_index (e->image_list, i))->cover)
      cover = g_ptr_array_index (e->image_list, i);
  /* EPUB 2's way of saying which picture is the cover, which some
   * readers still look for first. */
  if (cover != NULL)
    g_string_append_printf (out, "<meta name=\"cover\" content=\"%s\"/>\n", cover->id);
  g_string_append (out,
    "<meta name=\"generator\" content=\"Word42\"/>\n</metadata>\n<manifest>\n"
    "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\""
    " properties=\"nav\"/>\n"
    "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
    "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n");
  for (guint c = 0; c < e->chapters->len; c++)
    {
      g_string_append_printf (out, "<item id=\"c%u\" href=\"", c + 1);
      append_href (out, chapter_file (e, (int) c));
      g_string_append (out, "\" media-type=\"application/xhtml+xml\"/>\n");
    }
  for (guint i = 0; i < e->image_list->len; i++)
    {
      const Image *image = g_ptr_array_index (e->image_list, i);

      g_string_append_printf (out, "<item id=\"%s\" href=\"", image->id);
      append_href (out, image->href);
      g_string_append_printf (out, "\" media-type=\"%s\"%s/>\n", image->mime,
                              image->cover ? " properties=\"cover-image\"" : "");
    }
  g_string_append (out, "</manifest>\n<spine toc=\"ncx\">\n");
  for (guint c = 0; c < e->chapters->len; c++)
    g_string_append_printf (out, "<itemref idref=\"c%u\"/>\n", c + 1);
  g_string_append (out, "</spine>\n</package>\n");

  g_free (modified);
  g_date_time_unref (now);
  return g_string_free (out, FALSE);
}

/* The book's name: File > Summary Info's title, else the file's own
 * name, as a reader's library would otherwise show it. */
static char *
book_title (Epub *e, GFile *file)
{
  const W42DocInfo *info = w42_pt_get_info (e->pt);
  char *name, *title, *dot;

  if (info != NULL && info->title != NULL && !is_blank_text (info->title))
    return g_strstrip (g_strdup (info->title));

  name = g_file_get_basename (file);
  title = g_filename_display_name (name != NULL ? name : "");
  g_free (name);
  dot = strrchr (title, '.');
  if (dot != NULL && dot != title)
    *dot = '\0';
  if (is_blank_text (title))
    {
      g_free (title);
      title = g_strdup ("Untitled");
    }
  return title;
}

static void
image_free (gpointer data)
{
  Image *image = data;

  g_free (image->id);
  g_free (image->href);
  g_bytes_unref (image->bytes);
  g_free (image);
}

static void
anchor_free (gpointer data)
{
  Anchor *a = data;

  g_free (a->id);
  g_free (a);
}

static void
note_list_free (gpointer data)
{
  g_array_free (data, TRUE);
}

gboolean
w42_epub_export (W42PieceTable *pt, const W42PageSetup *page, GFile *file, GError **error)
{
  Epub e;
  W42ZipWriter *zip;
  char *title, *uid, *css, *nav, *ncx, *opf;
  gboolean ok;
  static const char CONTAINER[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "<rootfiles>\n"
    "<rootfile full-path=\"OEBPS/content.opf\""
    " media-type=\"application/oebps-package+xml\"/>\n"
    "</rootfiles>\n"
    "</container>\n";

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  memset (&e, 0, sizeof e);
  e.pt = pt;
  e.aps = w42_pt_ap_table (pt);
  e.styles = w42_pt_stylesheet (pt);
  e.blocks = w42_pt_snapshot_blocks (pt);
  e.info = g_new0 (BlockInfo, MAX (e.blocks->len, 1));
  e.column = page != NULL ? page->width - page->margin_left - page->margin_right : 9360;
  e.headings = g_array_new (FALSE, TRUE, sizeof (Heading));
  e.chapters = g_ptr_array_new_with_free_func (chapter_free);
  e.notes = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, note_list_free);
  e.note_chapter = g_hash_table_new (g_direct_hash, g_direct_equal);
  e.bookmarks = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, anchor_free);
  e.ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  e.images = g_hash_table_new (g_direct_hash, g_direct_equal);
  e.image_bytes = g_hash_table_new (g_bytes_hash, g_bytes_equal);
  e.image_list = g_ptr_array_new_with_free_func (image_free);
  e.lang_votes = g_hash_table_new (g_str_hash, g_str_equal);
  e.cover = W42_OBJECT_NONE;
  for (int i = 0; i < N_CLS; i++)
    {
      e.look[i].pa_votes = g_array_new (FALSE, FALSE, sizeof (ParaVote));
      e.look[i].ch_votes = g_array_new (FALSE, FALSE, sizeof (CharVote));
    }

  plan_chapters (&e);
  plan_notes (&e);
  plan_looks (&e);
  plan_bookmarks (&e);
  write_body (&e);
  write_notes (&e);

  title = book_title (&e, file);
  {
    char *uuid = g_uuid_string_random ();

    uid = g_strconcat ("urn:uuid:", uuid, NULL);
    g_free (uuid);
  }
  css = make_css (&e);
  nav = make_nav (&e, title);
  ncx = make_ncx (&e, uid, title);
  opf = make_opf (&e, uid, title);

  /* The mimetype first and stored, where a reader looks for it without
   * unpacking anything; then the container that says where the rest is. */
  zip = w42_zip_writer_new ();
  w42_zip_writer_add_stored (zip, "mimetype", EPUB_MIME, strlen (EPUB_MIME));
  w42_zip_writer_add (zip, "META-INF/container.xml", CONTAINER, strlen (CONTAINER));
  w42_zip_writer_add (zip, "OEBPS/content.opf", opf, strlen (opf));
  w42_zip_writer_add (zip, "OEBPS/nav.xhtml", nav, strlen (nav));
  w42_zip_writer_add (zip, "OEBPS/toc.ncx", ncx, strlen (ncx));
  w42_zip_writer_add (zip, "OEBPS/style.css", css, strlen (css));
  for (guint c = 0; c < e.chapters->len; c++)
    {
      Chapter *chapter = g_ptr_array_index (e.chapters, c);
      GString *xhtml = g_string_new (NULL);
      char *name;

      begin_xhtml (xhtml, e.lang, chapter->title != NULL ? chapter->title : title, TRUE);
      g_string_append (xhtml, "<body>\n");
      g_string_append_len (xhtml, chapter->body->str, chapter->body->len);
      g_string_append (xhtml, "</body>\n</html>\n");
      name = g_strconcat ("OEBPS/", chapter->file, NULL);
      w42_zip_writer_add (zip, name, xhtml->str, xhtml->len);
      g_free (name);
      g_string_free (xhtml, TRUE);
    }
  for (guint i = 0; i < e.image_list->len; i++)
    {
      const Image *image = g_ptr_array_index (e.image_list, i);
      char *name = g_strconcat ("OEBPS/", image->href, NULL);
      gsize size;
      const void *data = g_bytes_get_data (image->bytes, &size);

      w42_zip_writer_add (zip, name, data, size);
      g_free (name);
    }
  ok = w42_zip_writer_save (zip, file, error);

  w42_zip_writer_free (zip);
  g_free (title);
  g_free (uid);
  g_free (css);
  g_free (nav);
  g_free (ncx);
  g_free (opf);
  for (guint i = 0; i < e.headings->len; i++)
    {
      Heading *h = &g_array_index (e.headings, Heading, i);

      g_free (h->prefix);
      g_free (h->label);
    }
  g_array_free (e.headings, TRUE);
  for (int i = 0; i < N_CLS; i++)
    {
      g_array_free (e.look[i].pa_votes, TRUE);
      g_array_free (e.look[i].ch_votes, TRUE);
    }
  g_ptr_array_free (e.chapters, TRUE);
  g_hash_table_destroy (e.notes);
  g_hash_table_destroy (e.note_chapter);
  g_hash_table_destroy (e.bookmarks);
  g_hash_table_destroy (e.ids);
  g_hash_table_destroy (e.images);
  g_hash_table_destroy (e.image_bytes);
  g_ptr_array_free (e.image_list, TRUE);
  g_hash_table_destroy (e.lang_votes);
  g_free (e.info);
  g_ptr_array_free (e.blocks, TRUE);
  return ok;
}
