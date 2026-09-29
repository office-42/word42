/* w42-latex.c - writing documents as LaTeX
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The document is written as the article class writes one: its title as
 * \maketitle, its headings as sections, its paragraphs justified with
 * their first lines indented, in Latin Modern at the size of its Normal
 * style.  Explicit fonts and spacing are left to LaTeX, which is the
 * point of setting a document in it.  The preamble asks the engine what
 * it is, so the same source compiles with pdfLaTeX (T1 Latin Modern)
 * and with XeLaTeX, LuaLaTeX or Tectonic (fontspec's Latin Modern).
 */

#include "w42-latex.h"

#include <string.h>
#include <glib/gi18n.h>

#include "w42-image.h"

/* A paragraph's source line is broken at the first space after this many
 * bytes: short enough that SyncTeX's lines say where in a paragraph the
 * caret is, long enough to read. */
#define WRAP_AT 78

typedef struct {
  W42PieceTable     *pt;
  const W42PageSetup *page;
  W42ApTable        *aps;
  W42StyleSheet     *sheet;
  GPtrArray         *blocks;
  GString           *out;
  GFile             *dir;          /* where the pictures go */
  char              *stem;         /* the .tex file's name, without .tex */
  guint              figures;
  GError            *error;        /* the first picture that could not be written */

  /* What the body needs of the preamble. */
  gboolean           need_lastpage;
  gboolean           need_endnotes;
  gboolean           need_multirow;
  gboolean           need_ulem;

  /* The lists open, outermost first: the kind of each level. */
  int                list_depth;
  guint8             list_kind[9];

  /* The lines of `out` counted so far, for the anchors. */
  GArray            *anchors;      /* W42LatexAnchor, or NULL when not wanted */
  gsize              counted;
  guint              lines;

  GHashTable        *labels;       /* the bookmarks already given a \label */
  GString           *held_labels;  /* labels for after a heading or caption,
                                    * whose text must hold nothing fragile */
  gboolean           hold_labels;
  gboolean           toc_done, tof_done;
  gboolean           last_plain;   /* the last thing written was a paragraph
                                    * of text, which a display can join */
} Writer;

/* ---- lines and anchors ------------------------------------------------ */

/* The line of the source the next character will go on, counting from 1
 * at the top of the body. */
static guint
current_line (Writer *w)
{
  for (; w->counted < w->out->len; w->counted++)
    if (w->out->str[w->counted] == '\n')
      w->lines++;
  return w->lines + 1;
}

/* Bytes since the last line end: how long the line being written is. */
static gsize
line_length (const GString *out)
{
  gsize n = 0;

  while (n < out->len && out->str[out->len - 1 - n] != '\n')
    n++;
  return n;
}

/* The text of `block` from byte `byte` on starts on the current line. */
static void
anchor (Writer *w, const W42Block *block, gsize byte)
{
  W42LatexAnchor a;

  if (w->anchors == NULL)
    return;
  a.pos = block->start_pos + 1 + (gsize) g_utf8_pointer_to_offset (block->text->str,
                                                                   block->text->str + byte);
  a.line = current_line (w);
  g_array_append_val (w->anchors, a);
}

/* ---- text ------------------------------------------------------------- */

/* Whether a straight quote at the end of `out` would open a quotation:
 * at the start, or after a space or an opening bracket. */
static gboolean
opens_quote (const GString *out)
{
  char before = out->len > 0 ? out->str[out->len - 1] : ' ';

  return g_ascii_isspace (before) || strchr ("([{~`", before) != NULL;
}

/* Text as LaTeX reads it: its ten special characters made safe, the
 * characters Word42 keeps for breaks as LaTeX's own, and typewriter
 * quotes as a typesetter's. */
static void
put_text (GString *out, const char *text, gsize n)
{
  for (const char *p = text; p < text + n; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      switch (c)
        {
        case '"':  g_string_append (out, opens_quote (out) ? "``" : "''"); break;
        case '\'': g_string_append_c (out, opens_quote (out) ? '`' : '\''); break;
        case '\\': g_string_append (out, "\\textbackslash{}"); break;
        case '{':  g_string_append (out, "\\{"); break;
        case '}':  g_string_append (out, "\\}"); break;
        case '$':  g_string_append (out, "\\$"); break;
        case '&':  g_string_append (out, "\\&"); break;
        case '#':  g_string_append (out, "\\#"); break;
        case '%':  g_string_append (out, "\\%"); break;
        case '_':  g_string_append (out, "\\_"); break;
        case '^':  g_string_append (out, "\\textasciicircum{}"); break;
        case '~':  g_string_append (out, "\\textasciitilde{}"); break;
        case '<':  g_string_append (out, "\\textless{}"); break;
        case '>':  g_string_append (out, "\\textgreater{}"); break;
        case '|':  g_string_append (out, "\\textbar{}"); break;
        case '\t': g_string_append (out, "\\quad{}"); break;
        case 0x00A0: g_string_append_c (out, '~'); break;
        case 0x00AD: g_string_append (out, "\\-"); break;
        case 0x2011: g_string_append (out, "\\mbox{-}"); break;
        case 0x2028: g_string_append (out, "\\newline{}"); break;
        case 0xFFFC: break;
        default:
          if (c >= 0x20)
            g_string_append_unichar (out, c);
          break;
        }
    }
}

/* A stretch of a paragraph's text, bytes `from` to `to`, with the line
 * broken at a space once it has grown long.  TeX reads a line end as a
 * space, so the page is the same; each break is an anchor. */
static void
put_run_text (Writer *w, const W42Block *block, gsize from, gsize to)
{
  const char *text = block->text->str;
  const char *p = text + from, *end = text + to;

  while (p < end)
    {
      const char *next = g_utf8_next_char (p);

      if (*p == ' ' && next < end && line_length (w->out) >= WRAP_AT)
        {
          g_string_append_c (w->out, '\n');
          anchor (w, block, (gsize) (next - text));
        }
      else if (*p == '\\' && next < end && *next == '$')
        {
          /* \$ is a dollar sign, as LaTeX spells one: the way to write a
           * price the rules above would take for mathematics. */
          g_string_append (w->out, "\\$");
          next++;
        }
      else
        put_text (w->out, p, (gsize) (next - p));
      p = next;
    }
}

/* A link's address, as \href wants it. */
static void
put_url (GString *out, const char *url)
{
  for (const char *p = url; *p != '\0'; p++)
    {
      if (*p == '%' || *p == '#' || *p == '\\' || *p == '{' || *p == '}')
        g_string_append_c (out, '\\');
      g_string_append_c (out, *p);
    }
}

/* A bookmark's name as a \label: letters, digits and a few marks, and a
 * prefix of its own.  A colon, the usual, is a character babel's French
 * makes active. */
static void
put_label (GString *out, const char *name)
{
  g_string_append (out, "w42-");
  for (const char *p = name; *p != '\0'; p++)
    g_string_append_c (out, g_ascii_isalnum (*p) || *p == '-' || *p == '_' || *p == '.' ? *p : '-');
}

/* ---- mathematics ------------------------------------------------------ */

typedef struct {
  gsize    start, end;       /* the whole, delimiters and all, in bytes */
  gsize    body, body_end;   /* what is between them */
  gboolean display;
  const char *env;           /* an amsmath environment, written whole */
} MathSpan;

static const char * const MATH_ENVS[] = {
  "equation*", "equation", "align*", "align", "gather*", "gather",
  "multline*", "multline", "flalign*", "flalign", "alignat*", "alignat",
  "eqnarray*", "eqnarray", "displaymath", "math",
};

/* Where `close` next comes in text[from, len), or len. */
static gsize
find_from (const char *text, gsize len, gsize from, const char *close)
{
  gsize n = strlen (close);

  for (gsize i = from; i + n <= len; i++)
    if (memcmp (text + i, close, n) == 0)
      return i;
  return len;
}

/* The mathematics in a paragraph, in order.  A span that would take in a
 * picture or a note's mark is not mathematics: those are not LaTeX. */
static GArray *
find_math (const char *text, gsize len)
{
  GArray *spans = g_array_new (FALSE, TRUE, sizeof (MathSpan));

  for (gsize i = 0; i < len; )
    {
      MathSpan s = { 0 };
      gboolean found = FALSE;

      if (text[i] == '\\' && i + 1 < len && (text[i + 1] == '(' || text[i + 1] == '['))
        {
          gsize close = find_from (text, len, i + 2, text[i + 1] == '(' ? "\\)" : "\\]");

          if (close < len)
            {
              s.start = i;
              s.body = i + 2;
              s.body_end = close;
              s.end = close + 2;
              s.display = text[i + 1] == '[';
              found = TRUE;
            }
        }
      else if (text[i] == '\\' && g_str_has_prefix (text + i, "\\begin{"))
        {
          for (guint e = 0; e < G_N_ELEMENTS (MATH_ENVS) && !found; e++)
            {
              char *open = g_strdup_printf ("\\begin{%s}", MATH_ENVS[e]);
              char *close = g_strdup_printf ("\\end{%s}", MATH_ENVS[e]);

              if (g_str_has_prefix (text + i, open))
                {
                  gsize at = find_from (text, len, i + strlen (open), close);

                  if (at < len)
                    {
                      s.start = s.body = i;
                      s.end = s.body_end = at + strlen (close);
                      s.display = TRUE;
                      s.env = MATH_ENVS[e];
                      found = TRUE;
                    }
                }
              g_free (open);
              g_free (close);
            }
        }
      else if (text[i] == '$' && i + 1 < len && text[i + 1] == '$')
        {
          gsize close = find_from (text, len, i + 2, "$$");

          if (close < len && close > i + 2)
            {
              s.start = i;
              s.body = i + 2;
              s.body_end = close;
              s.end = close + 2;
              s.display = TRUE;
              found = TRUE;
            }
        }
      else if (text[i] == '$' && (i == 0 || text[i - 1] != '\\') &&
               i + 1 < len && !g_ascii_isspace (text[i + 1]))
        {
          /* Pandoc's rule: the closing dollar has no space before it and
           * no digit after it.  And it is the next one: "$5 to $10, or
           * US$ 12" is three prices, not a formula with a dollar in it. */
          gsize k = i + 1;

          while (k < len && (text[k] != '$' || text[k - 1] == '\\'))
            k++;
          if (k < len && !g_ascii_isspace (text[k - 1]) &&
              (k + 1 >= len || !g_ascii_isdigit (text[k + 1])))
            {
              s.start = i;
              s.body = i + 1;
              s.body_end = k;
              s.end = k + 1;
              found = TRUE;
            }
        }

      /* 0xFFFC is EF BF BC in UTF-8. */
      if (found && g_strstr_len (text + s.start, (gssize) (s.end - s.start), "\357\277\274") == NULL)
        {
          g_array_append_val (spans, s);
          i = s.end;
        }
      else
        i++;
    }
  return spans;
}

/* What AutoCorrect and the Symbol box put in for what a mathematician
 * types: TeX's own spellings of them, inside mathematics, where the
 * characters themselves would not be set. */
static const char *
math_spelling (gunichar c)
{
  static const struct { gunichar c; const char *tex; } MAP[] = {
    { 0x2018, "'" }, { 0x2019, "'" }, { 0x2032, "'" }, { 0x2033, "''" },
    { 0x201C, "\"" }, { 0x201D, "\"" },
    { 0x2013, "-" }, { 0x2014, "-" }, { 0x2212, "-" },
    { 0x00A0, " " }, { 0x2028, " " }, { '\t', " " },
    { 0x00D7, "\\times " }, { 0x00B7, "\\cdot " }, { 0x22C5, "\\cdot " },
    { 0x00F7, "\\div " }, { 0x00B1, "\\pm " }, { 0x2213, "\\mp " },
    { 0x2264, "\\le " }, { 0x2265, "\\ge " }, { 0x2260, "\\ne " },
    { 0x2248, "\\approx " }, { 0x2261, "\\equiv " }, { 0x221D, "\\propto " },
    { 0x2192, "\\to " }, { 0x2190, "\\leftarrow " }, { 0x21D2, "\\Rightarrow " },
    { 0x21D4, "\\Leftrightarrow " }, { 0x221E, "\\infty " }, { 0x2026, "\\ldots " },
    { 0x2202, "\\partial " }, { 0x2207, "\\nabla " }, { 0x2211, "\\sum " },
    { 0x220F, "\\prod " }, { 0x222B, "\\int " }, { 0x221A, "\\sqrt " },
    { 0x2208, "\\in " }, { 0x2209, "\\notin " }, { 0x2282, "\\subset " },
    { 0x2286, "\\subseteq " }, { 0x222A, "\\cup " }, { 0x2229, "\\cap " },
    { 0x2205, "\\emptyset " }, { 0x2200, "\\forall " }, { 0x2203, "\\exists " },
    { 0x00B0, "^\\circ " },
    { 0x03B1, "\\alpha " }, { 0x03B2, "\\beta " }, { 0x03B3, "\\gamma " },
    { 0x03B4, "\\delta " }, { 0x03B5, "\\epsilon " }, { 0x03B6, "\\zeta " },
    { 0x03B7, "\\eta " }, { 0x03B8, "\\theta " }, { 0x03B9, "\\iota " },
    { 0x03BA, "\\kappa " }, { 0x03BB, "\\lambda " }, { 0x03BC, "\\mu " },
    { 0x03BD, "\\nu " }, { 0x03BE, "\\xi " }, { 0x03C0, "\\pi " },
    { 0x03C1, "\\rho " }, { 0x03C3, "\\sigma " }, { 0x03C4, "\\tau " },
    { 0x03C5, "\\upsilon " }, { 0x03C6, "\\phi " }, { 0x03C7, "\\chi " },
    { 0x03C8, "\\psi " }, { 0x03C9, "\\omega " },
    { 0x0393, "\\Gamma " }, { 0x0394, "\\Delta " }, { 0x0398, "\\Theta " },
    { 0x039B, "\\Lambda " }, { 0x039E, "\\Xi " }, { 0x03A0, "\\Pi " },
    { 0x03A3, "\\Sigma " }, { 0x03A6, "\\Phi " }, { 0x03A8, "\\Psi " },
    { 0x03A9, "\\Omega " },
  };

  for (guint i = 0; i < G_N_ELEMENTS (MAP); i++)
    if (MAP[i].c == c)
      return MAP[i].tex;
  return NULL;
}

static void
put_math (Writer *w, const char *text, const MathSpan *s)
{
  GString *o = w->out;

  if (s->env == NULL)
    g_string_append (o, s->display ? "\\[" : "\\(");
  for (const char *p = text + s->body; p < text + s->body_end; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      const char *tex = math_spelling (c);

      if (tex != NULL)
        g_string_append (o, tex);
      else if ((c == '%' || c == '#') && (p == text || p[-1] != '\\'))
        {
          /* A bare % would make the rest of the line a comment, taking
           * the closing delimiter with it; a bare # is a macro's
           * parameter.  Both are meant as the signs. */
          g_string_append_c (o, '\\');
          g_string_append_c (o, (char) c);
        }
      else
        g_string_append_unichar (o, c);
    }
  if (s->env == NULL)
    g_string_append (o, s->display ? "\\]" : "\\)");
}

/* ---- pictures --------------------------------------------------------- */

/* A picture as a file beside the source, and \includegraphics at the
 * width it is shown at, no wider than the line. */
static void
put_picture (Writer *w, W42ObjectIdx idx)
{
  const W42Object *obj = w42_object_table_get (w42_pt_object_table (w->pt), idx);
  gboolean jpeg = obj != NULL && obj->format != NULL && g_str_equal (obj->format, "jpeg");
  GBytes *bytes;
  char *name;
  GFile *file;
  GError *error = NULL;

  if (obj == NULL || obj->data == NULL)
    return;
  bytes = jpeg || (obj->format != NULL && g_str_equal (obj->format, "png"))
          ? g_bytes_ref (obj->data) : w42_image_to_png (obj->data);
  if (bytes == NULL)
    return;
  name = g_strdup_printf ("%s-fig%u.%s", w->stem, ++w->figures, jpeg ? "jpg" : "png");
  file = g_file_get_child (w->dir, name);
  if (!g_file_replace_contents (file, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes),
                                NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error))
    {
      if (w->error == NULL)
        w->error = error;
      else
        g_error_free (error);
    }
  else
    {
      char width[G_ASCII_DTOSTR_BUF_SIZE];

      g_ascii_formatd (width, sizeof width, "%.3f", MAX (obj->width, 15) / 1440.0);
      g_string_append_printf (w->out, "\\includegraphics[width=\\minof{%sin}{\\linewidth}]{%s}", width, name);
    }
  g_object_unref (file);
  g_free (name);
  g_bytes_unref (bytes);
}

/* ---- runs ------------------------------------------------------------- */

static void write_note (Writer *w, int id, gboolean endnote);
static const W42CharFmt *style_char (Writer *w, const W42Fmt *fmt);

/* A run's formatting as the commands that wrap it, and how many braces
 * close them. */
static int
open_format (Writer *w, const W42CharFmt *ch, const W42CharFmt *para)
{
  GString *o = w->out;
  int n = 0;

  if (ch->revision == 2)
    return -1;                                   /* a deletion: not in the text */
  if (ch->link != NULL && ch->link[0] == '#')
    {
      /* A link to a bookmark: to its \label, in the same PDF. */
      g_string_append (o, "\\hyperref[");
      put_label (o, ch->link + 1);
      g_string_append (o, "]{");
      n++;
    }
  else if (ch->link != NULL)
    {
      g_string_append (o, "\\href{");
      put_url (o, ch->link);
      g_string_append (o, "}{");
      n++;
    }
  /* A link is known by being one: the blue and the underline a word
   * processor dresses it in are not LaTeX's. */
  if (ch->link == NULL && (ch->color & 0xFFFFFF) != 0 && (ch->color & 0xFFFFFF) != (para->color & 0xFFFFFF))
    {
      g_string_append_printf (o, "\\textcolor[HTML]{%06X}{", ch->color & 0xFFFFFF);
      n++;
    }
  if (ch->bold && !para->bold)            { g_string_append (o, "\\textbf{"); n++; }
  if (ch->italic && !para->italic)        { g_string_append (o, "\\emph{"); n++; }
  if (ch->link == NULL && ch->underline != W42_UNDERLINE_NONE && para->underline == W42_UNDERLINE_NONE)
    {
      g_string_append (o, ch->underline == W42_UNDERLINE_DOUBLE ? "\\uuline{"
                        : ch->underline == W42_UNDERLINE_WAVE ? "\\uwave{" : "\\uline{");
      w->need_ulem = TRUE;
      n++;
    }
  if (ch->strikeout || ch->dstrike)
    {
      g_string_append (o, ch->dstrike ? "\\xout{" : "\\sout{");
      w->need_ulem = TRUE;
      n++;
    }
  if (ch->smallcaps && !para->smallcaps)  { g_string_append (o, "\\textsc{"); n++; }
  if (ch->allcaps && !para->allcaps)      { g_string_append (o, "\\MakeUppercase{"); n++; }
  if (ch->script > 0)                     { g_string_append (o, "\\textsuperscript{"); n++; }
  else if (ch->script < 0)                { g_string_append (o, "\\textsubscript{"); n++; }
  return n;
}

/* A bookmark the reader may be sent to: a \label where it starts, with
 * an anchor of its own there, or -- in a heading or a caption, whose
 * text is copied into the table of contents -- after it. */
static void
put_bookmark (Writer *w, const char *name)
{
  GString *o;

  if (name == NULL || name[0] == '_' || g_hash_table_contains (w->labels, name))
    return;
  g_hash_table_add (w->labels, (gpointer) name);
  o = w->hold_labels ? w->held_labels : w->out;
  if (!w->hold_labels)
    g_string_append (o, "\\phantomsection");
  g_string_append (o, "\\label{");
  put_label (o, name);
  g_string_append_c (o, '}');
}

/* A paragraph's runs from byte `from` of its text on, with its notes,
 * fields, pictures and mathematics. */
static void
write_runs (Writer *w, const W42Block *block, const W42CharFmt *para, gsize from)
{
  const char *field = NULL;
  const char *text = block->text->str;
  GArray *math = find_math (text, block->text->len);
  guint m = 0;
  gsize done = from;             /* the text written up to here */

  anchor (w, block, from);
  for (guint r = 0; r < block->runs->len; r++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, r);
      const W42CharFmt *ch = &w42_ap_table_get (w->aps, run->ap)->ch;
      gsize start = run->byte_offset, end = run->byte_offset + run->n_bytes;
      int close;

      /* Before `from`, or inside mathematics already written whole. */
      if (end <= done && run->n_bytes > 0)
        continue;
      start = MAX (start, done);

      if (run->footnote > 0)
        {
          write_note (w, run->footnote_id, run->endnote);
          done = end;
          continue;
        }
      if (ch->revision != 2)
        put_bookmark (w, ch->bookmark);
      if (run->object != W42_OBJECT_NONE || (ch->field != NULL &&
          (g_str_equal (ch->field, "PAGE") || g_str_equal (ch->field, "NUMPAGES") ||
           g_str_equal (ch->field, "DATE"))))
        {
          close = open_format (w, ch, para);
          if (close < 0)
            {
              done = end;
              continue;
            }
          if (run->object != W42_OBJECT_NONE)
            put_picture (w, run->object);
          else if (g_str_equal (ch->field, "PAGE"))
            {
              if (field != ch->field)
                g_string_append (w->out, "\\thepage{}");
            }
          else if (g_str_equal (ch->field, "NUMPAGES"))
            {
              if (field != ch->field)
                g_string_append (w->out, "\\pageref{LastPage}");
              w->need_lastpage = TRUE;
            }
          else if (field != ch->field)
            g_string_append (w->out, "\\today{}");
          field = ch->field;
          while (close-- > 0)
            g_string_append_c (w->out, '}');
          done = end;
          continue;
        }
      field = ch->field;

      /* Text, in pieces around the mathematics in it.  Mathematics is
       * written whole where it starts, outside the run's formatting, and
       * may reach into the runs after. */
      while (start < end)
        {
          const MathSpan *span = m < math->len ? &g_array_index (math, MathSpan, m) : NULL;
          gsize piece;

          if (span != NULL && span->end <= start)
            {
              m++;
              continue;
            }
          if (span != NULL && span->start <= start)
            {
              if (ch->revision != 2)
                put_math (w, text, span);
              start = done = span->end;
              m++;
              continue;
            }
          piece = span != NULL && span->start < end ? span->start : end;
          close = open_format (w, ch, para);
          if (close >= 0)
            {
              put_run_text (w, block, start, piece);
              while (close-- > 0)
                g_string_append_c (w->out, '}');
            }
          start = done = piece;
        }
    }
  g_array_free (math, TRUE);
}

/* A note's paragraphs inside \footnote or \endnote. */
static void
write_note (Writer *w, int id, gboolean endnote)
{
  gboolean first = TRUE;
  gsize start;

  g_string_append (w->out, endnote ? "\\endnote{" : "\\footnote{");
  start = w->out->len;
  if (endnote)
    w->need_endnotes = TRUE;
  for (guint i = 0; i < w->blocks->len; i++)
    {
      const W42Block *b = g_ptr_array_index (w->blocks, i);
      const W42Fmt *fmt;

      if (b->note != id)
        continue;
      if (!first)
        g_string_append (w->out, "\\par ");
      first = FALSE;
      fmt = w42_ap_table_get (w->aps, b->ap);
      write_runs (w, b, style_char (w, fmt), 0);
    }
  /* The space a word processor puts after the note's number. */
  while (start < w->out->len && w->out->str[start] == ' ')
    g_string_erase (w->out, start, 1);
  g_string_append_c (w->out, '}');
}

/* What a paragraph's runs are measured against: its style's formatting,
 * so that a heading's own blue and bold are the section's, not the
 * run's. */
static const W42CharFmt *
style_char (Writer *w, const W42Fmt *fmt)
{
  const W42Style *style = fmt->pa.style != NULL ? w42_stylesheet_find (w->sheet, fmt->pa.style) : NULL;

  return style != NULL ? &style->ch : &fmt->ch;
}

/* The labels held back while a heading's or a caption's text was
 * written, now that it is closed. */
static void
put_held_labels (Writer *w)
{
  g_string_append_len (w->out, w->held_labels->str, (gssize) w->held_labels->len);
  g_string_truncate (w->held_labels, 0);
}

/* ---- lists ------------------------------------------------------------ */

static void
close_lists (Writer *w, int depth)
{
  while (w->list_depth > depth)
    {
      w->list_depth--;
      g_string_append (w->out, w42_list_is_numbered ((W42ListKind) w->list_kind[w->list_depth])
                               ? "\\end{enumerate}\n" : "\\end{itemize}\n");
    }
}

static void
open_list (Writer *w, W42ListKind kind, int start)
{
  GString *o = w->out;

  if (w42_list_is_numbered (kind))
    {
      static const char *LABEL[W42_LIST_KINDS] = {
        [W42_LIST_NUMBER] = "\\arabic*.", [W42_LIST_LOWER_LETTER] = "\\alph*.",
        [W42_LIST_UPPER_LETTER] = "\\Alph*.", [W42_LIST_LOWER_ROMAN] = "\\roman*.",
        [W42_LIST_UPPER_ROMAN] = "\\Roman*.",
      };

      g_string_append_printf (o, "\\begin{enumerate}[label=%s", LABEL[kind] != NULL ? LABEL[kind] : "\\arabic*.");
      if (start > 1)
        g_string_append_printf (o, ", start=%d", start);
      g_string_append (o, "]\n");
    }
  else
    {
      const char *label = kind == W42_LIST_BULLET_CIRCLE ? "$\\circ$"
                        : kind == W42_LIST_BULLET_SQUARE ? "\\tiny$\\blacksquare$"
                        : kind == W42_LIST_BULLET_DASH ? "\\textendash" : "\\textbullet";

      g_string_append_printf (o, "\\begin{itemize}[label=%s]\n", label);
    }
  w->list_kind[w->list_depth++] = (guint8) kind;
}

/* ---- tables ----------------------------------------------------------- */

/* The rule under a table's row: across the whole table, or in pieces
 * where a merged cell goes on down into the next row. */
static void
row_rule (Writer *w, int table, int next_row, int n_cols)
{
  gboolean *covered = g_new0 (gboolean, n_cols);
  gboolean any = FALSE;

  for (int c = 0; c < n_cols; c++)
    if (w42_pt_cell_vspan (w->pt, table, next_row, c) == W42_CELL_COVERED)
      any = covered[c] = TRUE;
  if (!any)
    g_string_append (w->out, "\\hline\n");
  else
    for (int c = 0; c < n_cols; )
      {
        int end = c;

        if (covered[c])
          {
            c++;
            continue;
          }
        while (end + 1 < n_cols && !covered[end + 1])
          end++;
        g_string_append_printf (w->out, "\\cline{%d-%d}", c + 1, end + 1);
        c = end + 1;
      }
  if (any)
    g_string_append_c (w->out, '\n');
  g_free (covered);
}

/* The cell block `i` is in at nesting level `level`, or one of table -1. */
static W42BlockCell
level_cell (Writer *w, guint i, int level)
{
  W42BlockCell c = { -1, 0, 0, 1, 0 };

  w42_block_cell (g_ptr_array_index (w->blocks, i), level, &c);
  return c;
}

/* A table as a longtable, which breaks across pages: a column each at its
 * width, ruled when the table is, its header rows repeated, its merged
 * cells as \multicolumn and \multirow, its shaded ones coloured.  A
 * table in a cell -- at nesting level `level`, more than 0 -- is a
 * tabular in the cell, `room` inches wide, since a longtable goes in no
 * table. */
static guint
write_table (Writer *w, guint first, int level, double room)
{
  int table = level_cell (w, first, level).table;
  const W42TableProps *props = w42_pt_table_props (w->pt, table);
  int n_cols = props != NULL ? MAX (props->n_cols, 1) : 1;
  gboolean ruled = props == NULL || props->borders;
  int text_w = w->page != NULL ? w->page->width - w->page->margin_left - w->page->margin_right : 9360;
  int header_rows = props != NULL && level == 0 ? props->header_rows : 0;
  GString *o = w->out;
  guint i = first;
  int row = -1;
  double *widths = g_new (double, n_cols);

  if (level > 0)
    text_w = (int) (room * 1440.0);
  for (int c = 0; c < n_cols; c++)
    {
      int cw = props != NULL && props->widths != NULL && (guint) c < props->widths->len
               ? g_array_index (props->widths, int, c) : 0;

      widths[c] = (cw > 0 ? cw : text_w / n_cols) / 1440.0;
    }

  if (level == 0)
    {
      close_lists (w, 0);
      g_string_append (o, "\n\\begin{longtable}{");
    }
  else
    g_string_append (o, "\\begin{tabular}[t]{");
  if (ruled)
    g_string_append_c (o, '|');
  for (int c = 0; c < n_cols; c++)
    {
      char width[G_ASCII_DTOSTR_BUF_SIZE];

      g_ascii_formatd (width, sizeof width, "%.3f", widths[c]);
      g_string_append_printf (o, "p{\\dimexpr %sin-2\\tabcolsep\\relax}%s", width, ruled ? "|" : "");
    }
  g_string_append (o, "}\n");
  if (ruled)
    g_string_append (o, "\\hline\n");

  while (i < w->blocks->len)
    {
      const W42Block *b = g_ptr_array_index (w->blocks, i);
      W42BlockCell bc = level_cell (w, i, level);
      const W42ParaFmt *cell;
      int col = bc.col, span = MAX (bc.span, 1), vspan;
      char width[G_ASCII_DTOSTR_BUF_SIZE];
      double spanned = 0;

      if (b->note >= 0)
        {
          i++;
          continue;
        }
      if (bc.table != table)
        break;
      if (bc.row != row)
        {
          if (row >= 0)
            {
              g_string_append (o, " \\\\\n");
              if (ruled)
                row_rule (w, table, bc.row, n_cols);
              if (row + 1 == header_rows)
                g_string_append (o, "\\endhead\n");
            }
          row = bc.row;
        }
      else
        g_string_append (o, " & ");

      cell = &w42_ap_table_get (w->aps, bc.cell_ap)->pa;
      vspan = cell->cell_vspan == W42_CELL_COVERED ? 0 : cell->cell_vspan > 1 ? cell->cell_vspan : 1;
      for (int k = col; k < col + span && k < n_cols; k++)
        spanned += widths[k];
      g_ascii_formatd (width, sizeof width, "%.3f", spanned);
      if (span > 1)
        g_string_append_printf (o, "\\multicolumn{%d}{%sp{\\dimexpr %sin-2\\tabcolsep\\relax}%s}{",
                                span, ruled && col == 0 ? "|" : "", width, ruled ? "|" : "");
      if (cell->has_shading_color)
        g_string_append_printf (o, "\\cellcolor[HTML]{%06X}", cell->shading_color & 0xFFFFFF);
      if (vspan > 1)
        {
          g_string_append_printf (o, "\\multirow{%d}{=}{", vspan);
          w->need_multirow = TRUE;
        }

      /* The cell's paragraphs, one after another. */
      {
        gboolean first_para = TRUE;

        while (i < w->blocks->len)
          {
            const W42Block *p = g_ptr_array_index (w->blocks, i);
            W42BlockCell pc;

            if (p->note >= 0)
              {
                i++;
                continue;
              }
            pc = level_cell (w, i, level);
            if (pc.table != table || pc.row != bc.row || pc.col != bc.col)
              break;
            if (vspan != 0 && p->depth > level + 1)
              {
                /* A table in the cell, the cell's width less its padding. */
                if (!first_para)
                  g_string_append (o, "\\newline ");
                i = write_table (w, i, level + 1, MAX (spanned - 0.1, 0.3));
                first_para = FALSE;
                continue;
              }
            if (vspan != 0)
              {
                if (!first_para)
                  g_string_append (o, "\\newline ");
                write_runs (w, p, style_char (w, w42_ap_table_get (w->aps, p->ap)), 0);
              }
            first_para = FALSE;
            i++;
          }
      }
      if (vspan > 1)
        g_string_append_c (o, '}');
      if (span > 1)
        g_string_append_c (o, '}');
    }
  if (row >= 0)
    {
      g_string_append (o, " \\\\\n");
      if (ruled)
        g_string_append (o, "\\hline\n");
    }
  g_string_append (o, level == 0 ? "\\end{longtable}\n\n" : "\\end{tabular}");
  g_free (widths);
  return i;
}

/* ---- paragraphs ------------------------------------------------------- */

static const char * const SECTIONS[] = {
  "section", "subsection", "subsubsection", "paragraph", "subparagraph",
};

/* The table Word42 generated that a paragraph belongs to -- "_Toc" for
 * the contents, "_Tof" for the figures -- or NULL.  LaTeX makes its own. */
static const char *
generated_table (Writer *w, const W42Block *b)
{
  for (guint r = 0; r < b->runs->len; r++)
    {
      const W42Run *run = &g_array_index (b->runs, W42Run, r);
      const char *mark = w42_ap_table_get (w->aps, run->ap)->ch.bookmark;

      if (mark != NULL && (g_str_equal (mark, "_Toc") || g_str_equal (mark, "_Tof")))
        return mark;
      if (run->n_bytes > 0)
        break;
    }
  return NULL;
}

/* A paragraph holding a picture and nothing else but spaces. */
static gboolean
picture_only (Writer *w, const W42Block *b)
{
  const W42ParaFmt *pa = &w42_ap_table_get (w->aps, b->ap)->pa;
  int pictures = 0;

  if (b->table >= 0 || b->note >= 0 || pa->list != W42_LIST_NONE)
    return FALSE;
  for (guint r = 0; r < b->runs->len; r++)
    {
      const W42Run *run = &g_array_index (b->runs, W42Run, r);

      if (run->object != W42_OBJECT_NONE)
        pictures++;
      else if (run->footnote > 0)
        return FALSE;
      else
        for (gsize k = 0; k < run->n_bytes; k++)
          if (!g_ascii_isspace (b->text->str[run->byte_offset + k]))
            return FALSE;
    }
  return pictures == 1;
}

static gboolean
is_caption (Writer *w, const W42Block *b)
{
  const char *style = w42_ap_table_get (w->aps, b->ap)->pa.style;

  return b->table < 0 && b->note < 0 && style != NULL && g_ascii_strcasecmp (style, "Caption") == 0 &&
         b->text->len > 0;
}

/* The next paragraph of the body after `i`, notes being elsewhere. */
static guint
next_body (Writer *w, guint i)
{
  while (i < w->blocks->len && ((const W42Block *) g_ptr_array_index (w->blocks, i))->note >= 0)
    i++;
  return i;
}

/* How many bytes the "Figure 3: " label at the front of a caption takes:
 * a word, a number and a colon, a full stop or a dash.  LaTeX numbers
 * its figures itself, in the document's language. */
static gsize
caption_label (const char *text)
{
  const char *p = text;

  while (*p != '\0' && !g_ascii_isspace (*p))
    p = g_utf8_next_char (p);
  if (p == text)
    return 0;
  while (*p == ' ')
    p++;
  if (!g_ascii_isdigit (*p))
    return 0;
  while (g_ascii_isdigit (*p))
    p++;
  while (*p == ' ')
    p++;
  if (*p == ':' || *p == '.' || *p == '-')
    p++;
  else if (g_str_has_prefix (p, "\342\200\223") || g_str_has_prefix (p, "\342\200\224"))
    p += 3;                                              /* an en or em dash */
  else if (*p != '\0')
    return 0;
  while (*p == ' ')
    p++;
  return (gsize) (p - text);
}

/* A picture and its caption as a figure, which LaTeX places, numbers and
 * lists in \listoffigures; the caption above the picture or below it,
 * where the document had it. */
static void
write_figure (Writer *w, const W42Block *picture, const W42Block *caption, gboolean below)
{
  const W42Fmt *cfmt = w42_ap_table_get (w->aps, caption->ap);

  close_lists (w, 0);
  g_string_append (w->out, "\\begin{figure}[htbp]\n\\centering\n");
  for (int part = 0; part < 2; part++)
    if ((part == 0) != below)
      {
        g_string_append (w->out, "\\caption{");
        w->hold_labels = TRUE;
        write_runs (w, caption, style_char (w, cfmt), caption_label (caption->text->str));
        w->hold_labels = FALSE;
        g_string_append (w->out, "}");
        put_held_labels (w);
        g_string_append_c (w->out, '\n');
      }
    else
      {
        write_runs (w, picture, style_char (w, w42_ap_table_get (w->aps, picture->ap)), 0);
        g_string_append_c (w->out, '\n');
      }
  g_string_append (w->out, "\\end{figure}\n\n");
}

/* A paragraph that is one displayed formula and nothing else. */
static gboolean
display_only (const W42Block *b)
{
  GArray *math = find_math (b->text->str, b->text->len);
  gboolean only = FALSE;

  if (math->len == 1 && g_array_index (math, MathSpan, 0).display)
    {
      const MathSpan *s = &g_array_index (math, MathSpan, 0);

      only = TRUE;
      for (gsize k = 0; k < b->text->len && only; k++)
        if ((k < s->start || k >= s->end) && !g_ascii_isspace (b->text->str[k]))
          only = FALSE;
    }
  g_array_free (math, TRUE);
  return only;
}

static void
write_body (Writer *w, guint from)
{
  gboolean numbered = w42_stylesheet_get_number_headings (w->sheet);
  gboolean blank = FALSE;

  for (guint i = from; i < w->blocks->len; )
    {
      const W42Block *b = g_ptr_array_index (w->blocks, i);
      const W42Fmt *fmt = w42_ap_table_get (w->aps, b->ap);
      const W42ParaFmt *pa = &fmt->pa;
      int outline = pa->style != NULL ? w42_stylesheet_outline (w->sheet, pa->style) : 0;
      const char *generated;
      gboolean plain = FALSE;

      if (b->note >= 0)
        {
          i++;
          continue;
        }
      if (b->table >= 0)
        {
          i = write_table (w, i, 0, 0.0);
          w->last_plain = FALSE;
          continue;
        }
      if ((pa->page_break_before || pa->section_break) && i > from)
        {
          close_lists (w, 0);
          g_string_append (w->out, "\\clearpage\n\n");
        }

      /* The table of contents and the table of figures Word42 made from
       * the page numbers of its own layout: LaTeX's, from its own. */
      generated = generated_table (w, b);
      if (generated != NULL)
        {
          gboolean toc = g_str_equal (generated, "_Toc");
          gboolean *done = toc ? &w->toc_done : &w->tof_done;

          close_lists (w, 0);
          if (!*done)
            g_string_append (w->out, toc ? "\\tableofcontents\n\n" : "\\listoffigures\n\n");
          *done = TRUE;
          w->last_plain = FALSE;
          i++;
          continue;
        }

      /* A picture with its caption under it or over it. */
      {
        guint next = next_body (w, i + 1);
        const W42Block *nb = next < w->blocks->len ? g_ptr_array_index (w->blocks, next) : NULL;

        if (nb != NULL && ((picture_only (w, b) && is_caption (w, nb)) ||
                           (is_caption (w, b) && picture_only (w, nb))))
          {
            gboolean below = picture_only (w, b);

            write_figure (w, below ? b : nb, below ? nb : b, below);
            w->last_plain = FALSE;
            blank = FALSE;
            i = next + 1;
            continue;
          }
      }

      /* A list item: the lists open down to its level, of its kind. */
      if (pa->list != W42_LIST_NONE && pa->list < W42_LIST_KINDS && outline == 0)
        {
          int depth = MIN (pa->list_level, 8) + 1;

          close_lists (w, depth);
          if (w->list_depth == depth &&
              (w42_list_is_numbered ((W42ListKind) w->list_kind[depth - 1]) != w42_list_is_numbered ((W42ListKind) pa->list) ||
               pa->list_start > 0))
            close_lists (w, depth - 1);
          while (w->list_depth < depth)
            open_list (w, (W42ListKind) pa->list, w->list_depth == depth - 1 ? pa->list_start : 0);
          g_string_append (w->out, "\\item ");
          write_runs (w, b, style_char (w, fmt), 0);
          g_string_append (w->out, "\n");
          blank = FALSE;
          w->last_plain = FALSE;
          i++;
          continue;
        }
      close_lists (w, 0);

      if (b->text->len == 0 && b->runs->len == 0)
        {
          /* An empty paragraph: a little space, once. */
          if (!blank && i > from)
            g_string_append (w->out, "\\medskip\n\n");
          blank = TRUE;
          w->last_plain = FALSE;
          i++;
          continue;
        }
      blank = FALSE;

      if (outline > 0)
        {
          int level = CLAMP (outline, 1, 5);
          gsize at;
          char *title;

          g_string_append_printf (w->out, "\\%s%s{", SECTIONS[level - 1], numbered ? "" : "*");
          at = w->out->len;
          w->hold_labels = TRUE;
          write_runs (w, b, style_char (w, fmt), 0);
          w->hold_labels = FALSE;
          title = g_strndup (w->out->str + at, w->out->len - at);
          g_string_append (w->out, "}");
          /* An unnumbered heading is in neither the table of contents nor
           * the PDF's outline unless it is put there. */
          if (!numbered && strstr (title, "note{") == NULL)
            g_string_append_printf (w->out, "\\addcontentsline{toc}{%s}{%s}", SECTIONS[level - 1], title);
          put_held_labels (w);
          g_string_append (w->out, "\n\n");
          g_free (title);
          w->last_plain = FALSE;
          i++;
          continue;
        }
      if (display_only (b))
        {
          /* A formula displayed on its own belongs to the paragraph it
           * follows: begun as a paragraph of its own, it would leave an
           * empty line above it.  LaTeX centres it, whatever alignment
           * the paragraph was given to centre it by hand. */
          if (w->last_plain && g_str_has_suffix (w->out->str, "\n\n"))
            g_string_truncate (w->out, w->out->len - 1);
          write_runs (w, b, style_char (w, fmt), 0);
          g_string_append (w->out, "\n\n");
          plain = TRUE;
        }
      else if (pa->align == W42_ALIGN_CENTER || pa->align == W42_ALIGN_RIGHT)
        {
          const char *env = pa->align == W42_ALIGN_CENTER ? "center" : "flushright";

          g_string_append_printf (w->out, "\\begin{%s}\n", env);
          write_runs (w, b, style_char (w, fmt), 0);
          g_string_append_printf (w->out, "\n\\end{%s}\n\n", env);
        }
      else
        {
          write_runs (w, b, style_char (w, fmt), 0);
          g_string_append (w->out, "\n\n");
          plain = TRUE;
        }
      w->last_plain = plain;
      i++;
    }
  close_lists (w, 0);
}

/* ---- the preamble ----------------------------------------------------- */

/* Babel's name for the document's language, or NULL for English. */
static const char *
babel_language (const char *tag)
{
  static const struct { const char *prefix, *name; } LANGS[] = {
    { "en-GB", "british" }, { "en-US", "american" }, { "en", "english" },
    { "nb", "norsk" }, { "no", "norsk" }, { "nn", "nynorsk" }, { "da", "danish" },
    { "sv", "swedish" }, { "fi", "finnish" }, { "is", "icelandic" }, { "de", "ngerman" },
    { "nl", "dutch" }, { "fr", "french" }, { "es", "spanish" }, { "it", "italian" },
    { "pt", "portuguese" }, { "pl", "polish" }, { "cs", "czech" }, { "sk", "slovak" },
    { "hu", "hungarian" }, { "ro", "romanian" }, { "hr", "croatian" }, { "sl", "slovene" },
    { "et", "estonian" }, { "lv", "latvian" }, { "lt", "lithuanian" }, { "tr", "turkish" },
    { "ca", "catalan" }, { "ga", "irish" }, { "cy", "welsh" }, { "la", "latin" },
  };

  if (tag == NULL)
    return NULL;
  for (guint i = 0; i < G_N_ELEMENTS (LANGS); i++)
    if (g_ascii_strncasecmp (tag, LANGS[i].prefix, strlen (LANGS[i].prefix)) == 0)
      return LANGS[i].name;
  return NULL;
}

static void
put_inches (GString *o, const char *key, int twips)
{
  char v[G_ASCII_DTOSTR_BUF_SIZE];

  g_ascii_formatd (v, sizeof v, "%.3f", twips / 1440.0);
  g_string_append_printf (o, "%s=%sin", key, v);
}

/* A header's or footer's words, with its fields as LaTeX's. */
static void
put_page_text (Writer *w, GString *o, const char *text)
{
  for (const char *p = text; *p != '\0'; )
    {
      if (g_str_has_prefix (p, "{PAGE}"))
        {
          g_string_append (o, "\\thepage{}");
          p += 6;
        }
      else if (g_str_has_prefix (p, "{NUMPAGES}"))
        {
          g_string_append (o, "\\pageref{LastPage}");
          w->need_lastpage = TRUE;
          p += 10;
        }
      else if (g_str_has_prefix (p, "{DATE}"))
        {
          g_string_append (o, "\\today{}");
          p += 6;
        }
      else
        {
          const char *next = g_utf8_next_char (p);

          put_text (o, p, (gsize) (next - p));
          p = next;
        }
    }
}

static void
write_preamble (Writer *w, GString *pre, const char *title, const char *subtitle)
{
  const W42Style *normal = w42_stylesheet_find (w->sheet, "Normal");
  int size = normal != NULL && normal->ch.size > 0 ? normal->ch.size : 24;
  int pt = size <= 21 ? 10 : size <= 23 ? 11 : 12;
  const W42PageSetup *page = w->page;
  const W42DocInfo *info = w42_pt_get_info (w->pt);
  const char *lang = babel_language (w42_stylesheet_language (w->sheet));
  int cols = w42_page_columns (page);
  const W42PageText *header = w42_pt_get_header (w->pt), *footer = w42_pt_get_footer (w->pt);
  gboolean fancy = (header != NULL && header->text != NULL && *header->text != '\0') ||
                   (footer != NULL && footer->text != NULL && *footer->text != '\0');
  int from, start;

  g_string_append (pre, "% Written by Word42, https://word42.org/\n");
  g_string_append_printf (pre, "\\documentclass[%dpt%s]{article}\n", pt, cols == 2 ? ",twocolumn" : "");
  g_string_append (pre,
    "\\usepackage{iftex}\n"
    "\\ifPDFTeX\n"
    "  \\usepackage[T1]{fontenc}\n"
    "  \\usepackage[utf8]{inputenc}\n"
    "  \\usepackage{lmodern}\n"
    "  \\usepackage{textcomp}\n"
    "\\else\n"
    "  \\usepackage{fontspec}\n"
    "\\fi\n");
  if (lang != NULL)
    g_string_append_printf (pre, "\\usepackage[%s]{babel}\n", lang);
  g_string_append (pre, "\\usepackage{microtype}\n\\usepackage[");
  if (page != NULL)
    {
      put_inches (pre, "paperwidth", page->width);
      g_string_append_c (pre, ',');
      put_inches (pre, "paperheight", page->height);
      g_string_append_c (pre, ',');
      put_inches (pre, "left", page->margin_left);
      g_string_append_c (pre, ',');
      put_inches (pre, "right", page->margin_right);
      g_string_append_c (pre, ',');
      put_inches (pre, "top", page->margin_top);
      g_string_append_c (pre, ',');
      put_inches (pre, "bottom", page->margin_bottom);
    }
  else
    g_string_append (pre, "a4paper,margin=1in");
  g_string_append (pre, "]{geometry}\n"
                        "\\usepackage{graphicx}\n"
                        "\\usepackage[table]{xcolor}\n"
                        "\\usepackage{amsmath}\n"
                        "\\usepackage{amssymb}\n"
                        "\\usepackage{enumitem}\n"
                        "\\usepackage{array}\n"
                        "\\usepackage{longtable}\n");
  if (w->need_ulem)
    g_string_append (pre, "\\usepackage[normalem]{ulem}\n");
  if (w->need_multirow)
    g_string_append (pre, "\\usepackage{multirow}\n");
  if (w->need_endnotes)
    g_string_append (pre, "\\usepackage{endnotes}\n");
  if (cols > 2)
    g_string_append (pre, "\\usepackage{multicol}\n");
  if (fancy)
    {
      GString *hf = g_string_new (NULL);
      static const char * const WHERE[] = { "L", "C", "R", "C" };

      g_string_append (pre, "\\usepackage{fancyhdr}\n\\pagestyle{fancy}\n\\fancyhf{}\n"
                            "\\renewcommand{\\headrulewidth}{0pt}\n");
      if (header != NULL && header->text != NULL && *header->text != '\0')
        {
          put_page_text (w, hf, header->text);
          g_string_append_printf (pre, "\\fancyhead[%s]{%s}\n", WHERE[header->align & 3], hf->str);
          g_string_truncate (hf, 0);
        }
      if (footer != NULL && footer->text != NULL && *footer->text != '\0')
        {
          put_page_text (w, hf, footer->text);
          g_string_append_printf (pre, "\\fancyfoot[%s]{%s}\n", WHERE[footer->align & 3], hf->str);
        }
      g_string_free (hf, TRUE);
    }
  /* Read after the page's own text, in case that asks for the count. */
  if (w->need_lastpage)
    g_string_append (pre, "\\usepackage{lastpage}\n");
  g_string_append (pre, "\\usepackage[hidelinks]{hyperref}\n");
  /* \includegraphics' width: the picture's own, or the line's when that
   * is narrower. */
  g_string_append (pre, "\\makeatletter\\newcommand{\\minof}[2]{\\ifdim#1<#2 #1\\else#2\\fi}\\makeatother\n");

  if (info != NULL && (info->title != NULL || info->author != NULL || info->subject != NULL))
    {
      g_string_append (pre, "\\hypersetup{");
      if (info->title != NULL)
        {
          g_string_append (pre, "pdftitle={");
          put_text (pre, info->title, strlen (info->title));
          g_string_append (pre, "},");
        }
      if (info->author != NULL)
        {
          g_string_append (pre, "pdfauthor={");
          put_text (pre, info->author, strlen (info->author));
          g_string_append (pre, "},");
        }
      if (info->subject != NULL)
        {
          g_string_append (pre, "pdfsubject={");
          put_text (pre, info->subject, strlen (info->subject));
          g_string_append (pre, "}");
        }
      g_string_append (pre, "}\n");
    }

  if (title != NULL)
    {
      g_string_append_printf (pre, "\\title{%s%s%s}\n", title,
                              subtitle != NULL ? "\\\\[0.5ex]\\large " : "", subtitle != NULL ? subtitle : "");
      g_string_append (pre, "\\author{");
      if (info != NULL && info->author != NULL)
        put_text (pre, info->author, strlen (info->author));
      g_string_append (pre, "}\n\\date{\\today}\n");
    }

  g_string_append (pre, "\n\\begin{document}\n");
  w42_pt_get_page_numbering (w->pt, &from, &start);
  if (start != 1 || from > 1)
    g_string_append_printf (pre, "\\setcounter{page}{%d}\n", MAX (start - (from - 1), 0));
  if (title != NULL)
    g_string_append (pre, "\\maketitle\n");
  if (cols > 2)
    g_string_append_printf (pre, "\\begin{multicols}{%d}\n", cols);
  g_string_append_c (pre, '\n');
}

/* ---------------------------------------------------------------------- */

static gint
anchor_order (gconstpointer a, gconstpointer b)
{
  const W42LatexAnchor *x = a, *y = b;

  return x->pos < y->pos ? -1 : x->pos > y->pos ? 1 : (int) x->line - (int) y->line;
}

gboolean
w42_latex_export_anchored (W42PieceTable *pt, const W42PageSetup *page, GFile *file,
                           GArray **anchors, GError **error)
{
  Writer w;
  GString *pre = g_string_new (NULL);
  char *title = NULL, *subtitle = NULL;
  guint from = 0, pre_lines = 0;
  gboolean ok;

  memset (&w, 0, sizeof w);
  w.pt = pt;
  w.page = page;
  w.aps = w42_pt_ap_table (pt);
  w.sheet = w42_pt_stylesheet (pt);
  w.blocks = w42_pt_snapshot_blocks (pt);
  w.out = g_string_new (NULL);
  w.dir = g_file_get_parent (file);
  w.labels = g_hash_table_new (g_direct_hash, g_direct_equal);    /* interned names */
  w.held_labels = g_string_new (NULL);
  if (anchors != NULL)
    w.anchors = g_array_new (FALSE, FALSE, sizeof (W42LatexAnchor));
  {
    char *base = g_file_get_basename (file);
    char *dot = strrchr (base, '.');

    if (dot != NULL)
      *dot = '\0';
    /* A picture's name is TeX's to read: nothing in it TeX would stop at. */
    for (char *p = base; *p != '\0'; p++)
      if (!g_ascii_isalnum (*p) && *p != '-' && *p != '_')
        *p = '-';
    w.stem = base;
  }

  /* The title, and a subtitle after it, as \maketitle sets them. */
  while (from < w.blocks->len)
    {
      const W42Block *b = g_ptr_array_index (w.blocks, from);
      const char *style = w42_ap_table_get (w.aps, b->ap)->pa.style;
      GString *s;

      if (b->note >= 0 || b->table >= 0 || style == NULL ||
          (title == NULL ? !g_str_equal (style, "Title") : !g_str_equal (style, "Subtitle")) ||
          (title != NULL && subtitle != NULL))
        break;
      s = g_string_new (NULL);
      put_text (s, b->text->str, b->text->len);
      if (title == NULL)
        title = g_string_free (s, FALSE);
      else
        subtitle = g_string_free (s, FALSE);
      from++;
    }

  write_body (&w, from);
  write_preamble (&w, pre, title, subtitle);
  for (gsize k = 0; k < pre->len; k++)
    if (pre->str[k] == '\n')
      pre_lines++;
  g_string_append (pre, w.out->str);
  if (w42_page_columns (page) > 2)
    g_string_append (pre, "\\end{multicols}\n");
  if (w.need_endnotes)
    g_string_append (pre, "\\theendnotes\n");
  g_string_append (pre, "\\end{document}\n");

  if (w.error != NULL)
    {
      g_propagate_error (error, w.error);
      ok = FALSE;
    }
  else
    ok = g_file_replace_contents (file, pre->str, pre->len, NULL, FALSE,
                                  G_FILE_CREATE_NONE, NULL, NULL, error);

  if (w.anchors != NULL)
    {
      /* The body's lines, counted from the top of the whole file. */
      for (guint k = 0; k < w.anchors->len; k++)
        g_array_index (w.anchors, W42LatexAnchor, k).line += pre_lines;
      g_array_sort (w.anchors, anchor_order);
      *anchors = w.anchors;
    }

  g_string_free (pre, TRUE);
  g_string_free (w.out, TRUE);
  g_string_free (w.held_labels, TRUE);
  g_hash_table_destroy (w.labels);
  g_ptr_array_free (w.blocks, TRUE);
  g_object_unref (w.dir);
  g_free (w.stem);
  g_free (title);
  g_free (subtitle);
  return ok;
}

gboolean
w42_latex_export (W42PieceTable *pt, const W42PageSetup *page, GFile *file, GError **error)
{
  return w42_latex_export_anchored (pt, page, file, NULL, error);
}
