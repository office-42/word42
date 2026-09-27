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
} Writer;

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
  if (ch->link != NULL)
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

/* A paragraph's runs, with its notes, fields and pictures. */
static void
write_runs (Writer *w, const W42Block *block, const W42CharFmt *para)
{
  const char *field = NULL;

  for (guint r = 0; r < block->runs->len; r++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, r);
      const W42CharFmt *ch = &w42_ap_table_get (w->aps, run->ap)->ch;
      int close;

      if (run->footnote > 0)
        {
          write_note (w, run->footnote_id, run->endnote);
          continue;
        }
      close = open_format (w, ch, para);
      if (close < 0)
        continue;
      if (run->object != W42_OBJECT_NONE)
        put_picture (w, run->object);
      else if (ch->field != NULL && g_str_equal (ch->field, "PAGE"))
        {
          if (field != ch->field)
            g_string_append (w->out, "\\thepage{}");
        }
      else if (ch->field != NULL && g_str_equal (ch->field, "NUMPAGES"))
        {
          if (field != ch->field)
            g_string_append (w->out, "\\pageref{LastPage}");
          w->need_lastpage = TRUE;
        }
      else if (ch->field != NULL && g_str_equal (ch->field, "DATE"))
        {
          if (field != ch->field)
            g_string_append (w->out, "\\today{}");
        }
      else
        put_text (w->out, block->text->str + run->byte_offset, run->n_bytes);
      field = ch->field;
      while (close-- > 0)
        g_string_append_c (w->out, '}');
    }
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
      write_runs (w, b, style_char (w, fmt));
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

/* A table as a longtable, which breaks across pages: a column each at its
 * width, ruled when the table is, its header rows repeated, its merged
 * cells as \multicolumn and \multirow, its shaded ones coloured. */
static guint
write_table (Writer *w, guint first)
{
  const W42Block *head = g_ptr_array_index (w->blocks, first);
  int table = head->table;
  const W42TableProps *props = w42_pt_table_props (w->pt, table);
  int n_cols = props != NULL ? MAX (props->n_cols, 1) : 1;
  gboolean ruled = props == NULL || props->borders;
  int text_w = w->page != NULL ? w->page->width - w->page->margin_left - w->page->margin_right : 9360;
  int header_rows = props != NULL ? props->header_rows : 0;
  GString *o = w->out;
  guint i = first;
  int row = -1;
  double *widths = g_new (double, n_cols);

  for (int c = 0; c < n_cols; c++)
    {
      int cw = props != NULL && props->widths != NULL && (guint) c < props->widths->len
               ? g_array_index (props->widths, int, c) : 0;

      widths[c] = (cw > 0 ? cw : text_w / n_cols) / 1440.0;
    }

  close_lists (w, 0);
  g_string_append (o, "\n\\begin{longtable}{");
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
      const W42ParaFmt *cell;
      int col = b->col, span = MAX (b->span, 1), vspan;
      char width[G_ASCII_DTOSTR_BUF_SIZE];
      double spanned = 0;

      if (b->note >= 0)
        {
          i++;
          continue;
        }
      if (b->table != table)
        break;
      if (b->row != row)
        {
          if (row >= 0)
            {
              g_string_append (o, " \\\\\n");
              if (ruled)
                row_rule (w, table, b->row, n_cols);
              if (row + 1 == header_rows)
                g_string_append (o, "\\endhead\n");
            }
          row = b->row;
        }
      else
        g_string_append (o, " & ");

      cell = &w42_ap_table_get (w->aps, b->cell_ap)->pa;
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

            if (p->note >= 0)
              {
                i++;
                continue;
              }
            if (p->table != table || p->row != b->row || p->col != b->col)
              break;
            if (vspan != 0)
              {
                if (!first_para)
                  g_string_append (o, "\\newline ");
                write_runs (w, p, style_char (w, w42_ap_table_get (w->aps, p->ap)));
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
  g_string_append (o, "\\end{longtable}\n\n");
  g_free (widths);
  return i;
}

/* ---- paragraphs ------------------------------------------------------- */

static const char *
section_command (int level, gboolean numbered)
{
  static const char * const COMMANDS[] = {
    "section", "subsection", "subsubsection", "paragraph", "subparagraph",
  };
  static char buf[32];

  g_snprintf (buf, sizeof buf, "\\%s%s{", COMMANDS[CLAMP (level, 1, 5) - 1], numbered ? "" : "*");
  return buf;
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

      if (b->note >= 0)
        {
          i++;
          continue;
        }
      if (b->table >= 0)
        {
          i = write_table (w, i);
          continue;
        }
      if ((pa->page_break_before || pa->section_break) && i > from)
        {
          close_lists (w, 0);
          g_string_append (w->out, "\\clearpage\n\n");
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
          write_runs (w, b, style_char (w, fmt));
          g_string_append (w->out, "\n");
          blank = FALSE;
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
          i++;
          continue;
        }
      blank = FALSE;

      if (outline > 0)
        {
          g_string_append (w->out, section_command (outline, numbered));
          write_runs (w, b, style_char (w, fmt));
          g_string_append (w->out, "}\n\n");
          i++;
          continue;
        }
      if (pa->align == W42_ALIGN_CENTER || pa->align == W42_ALIGN_RIGHT)
        {
          const char *env = pa->align == W42_ALIGN_CENTER ? "center" : "flushright";

          g_string_append_printf (w->out, "\\begin{%s}\n", env);
          write_runs (w, b, style_char (w, fmt));
          g_string_append_printf (w->out, "\n\\end{%s}\n\n", env);
        }
      else
        {
          write_runs (w, b, style_char (w, fmt));
          g_string_append (w->out, "\n\n");
        }
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

gboolean
w42_latex_export (W42PieceTable *pt, const W42PageSetup *page, GFile *file, GError **error)
{
  Writer w;
  GString *pre = g_string_new (NULL);
  char *title = NULL, *subtitle = NULL;
  guint from = 0;
  gboolean ok;

  memset (&w, 0, sizeof w);
  w.pt = pt;
  w.page = page;
  w.aps = w42_pt_ap_table (pt);
  w.sheet = w42_pt_stylesheet (pt);
  w.blocks = w42_pt_snapshot_blocks (pt);
  w.out = g_string_new (NULL);
  w.dir = g_file_get_parent (file);
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

  g_string_free (pre, TRUE);
  g_string_free (w.out, TRUE);
  g_ptr_array_free (w.blocks, TRUE);
  g_object_unref (w.dir);
  g_free (w.stem);
  g_free (title);
  g_free (subtitle);
  return ok;
}
