/* w42-autoformat.c - see w42-autoformat.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-autoformat.h"

#include <string.h>

void
w42_autoformat_defaults (W42AutoFormat *what)
{
  g_return_if_fail (what != NULL);

  what->headings = TRUE;
  what->lists = TRUE;
  what->quotes = TRUE;
  what->blanks = TRUE;
}

/* How many characters of a bullet's marker a line starts with, or 0.
 * "- ", "* " and the bullet characters themselves count. */
static int
bullet_marker (const char *text)
{
  static const char *const marks[] = { "- ", "* ", "\342\200\242 ", "\302\267 ", "o " };

  for (guint i = 0; i < G_N_ELEMENTS (marks); i++)
    if (g_str_has_prefix (text, marks[i]))
      return (int) g_utf8_strlen (marks[i], -1);
  return 0;
}

/* "1. ", "12) ", "a. ": the characters to take off, and the kind of list
 * it should be.  0 when the line does not start with a number. */
static int
number_marker (const char *text, W42ListKind *kind)
{
  const char *p = text;
  int digits = 0;

  while (g_ascii_isdigit (*p))
    {
      p++;
      digits++;
    }
  if (digits > 0 && digits < 4 && (*p == '.' || *p == ')') && p[1] == ' ')
    {
      *kind = W42_LIST_NUMBER;
      return digits + 2;
    }

  /* A single letter: a. b. c. */
  if (g_ascii_isalpha (text[0]) && (text[1] == '.' || text[1] == ')') && text[2] == ' ')
    {
      *kind = g_ascii_isupper (text[0]) ? W42_LIST_UPPER_LETTER : W42_LIST_LOWER_LETTER;
      return 3;
    }
  return 0;
}

/* Whether a paragraph reads as a heading: short, standing on its own,
 * and not ending in the punctuation a sentence ends with. */
static gboolean
looks_like_a_heading (const char *text, gsize chars)
{
  gunichar last;

  if (chars == 0 || chars > 60)
    return FALSE;
  last = g_utf8_get_char (g_utf8_prev_char (text + strlen (text)));
  if (last == '.' || last == ',' || last == ';' || last == ':' ||
      last == '!' || last == '?' || last == '-')
    return FALSE;
  /* A line of one word that is a number is a page number, not a
   * heading. */
  if (g_ascii_isdigit (text[0]) && strchr (text, ' ') == NULL)
    return FALSE;
  return TRUE;
}

/* The quotes and dashes of a typewriter, as a printer would set them.
 * Returns the text to put in place of `text`, or NULL when nothing in it
 * would change. */
static char *
printers_marks (const char *text)
{
  GString *out = g_string_new (NULL);
  gboolean changed = FALSE;
  const char *p = text;
  gunichar before = ' ';

  while (*p != '\0')
    {
      gunichar c = g_utf8_get_char (p);
      const char *next = g_utf8_next_char (p);

      if (c == '"' || c == '\'')
        {
          /* An opening mark after a space or a bracket, a closing one
           * after anything else. */
          gboolean opening = before == ' ' || before == '\t' || before == '(' ||
                             before == '[' || before == '{' || before == 0x2018 ||
                             before == 0x201C || p == text;

          if (c == '"')
            g_string_append_unichar (out, opening ? 0x201C : 0x201D);
          else
            g_string_append_unichar (out, opening ? 0x2018 : 0x2019);
          changed = TRUE;
        }
      else if (c == '-' && *next == '-')
        {
          /* Two hyphens are an en dash, three an em dash. */
          const char *third = g_utf8_next_char (next);

          if (*third == '-')
            {
              g_string_append_unichar (out, 0x2014);
              next = g_utf8_next_char (third);
            }
          else
            {
              g_string_append_unichar (out, 0x2013);
              next = g_utf8_next_char (next);
            }
          changed = TRUE;
        }
      else
        {
          g_string_append_unichar (out, c);
        }

      before = c;
      p = next;
    }

  if (!changed)
    {
      g_string_free (out, TRUE);
      return NULL;
    }
  return g_string_free (out, FALSE);
}

int
w42_pt_autoformat (W42PieceTable *pt, const W42AutoFormat *what)
{
  GPtrArray *blocks;
  int changed = 0;

  g_return_val_if_fail (pt != NULL, 0);
  g_return_val_if_fail (what != NULL, 0);

  blocks = w42_pt_snapshot_blocks (pt);
  w42_pt_begin_group (pt);

  /* Back to front, so that a change never moves a paragraph that has
   * yet to be looked at. */
  for (guint i = blocks->len; i > 0; i--)
    {
      const W42Block *block = g_ptr_array_index (blocks, i - 1);
      const W42Block *next = i < blocks->len ? g_ptr_array_index (blocks, i) : NULL;
      const W42Block *previous = i > 1 ? g_ptr_array_index (blocks, i - 2) : NULL;
      const W42Fmt *fmt = w42_ap_table_get (w42_pt_ap_table (pt), block->ap);
      const char *text = block->text->str;
      gsize chars = (gsize) g_utf8_strlen (text, -1);
      gsize start = block->start_pos + 1;
      int marker;
      W42ListKind kind = W42_LIST_NONE;

      if (block->note >= 0)
        continue;                       /* notes are left as they were */

      /* A run of empty paragraphs becomes one. */
      if (what->blanks && chars == 0 && previous != NULL &&
          previous->text->len == 0 && block->table < 0 && previous->table < 0)
        {
          w42_pt_delete (pt, block->start_pos, 1);
          changed++;
          continue;
        }

      if (chars == 0)
        continue;

      /* The quotes and dashes, wherever they are. */
      if (what->quotes)
        {
          char *fixed = printers_marks (text);

          if (fixed != NULL)
            {
              W42ApIdx ap = w42_pt_ap_at (pt, start);

              w42_pt_delete (pt, start, chars);
              w42_pt_insert_text (pt, start, fixed, ap);
              chars = (gsize) g_utf8_strlen (fixed, -1);
              changed++;
              g_free (fixed);
              /* The text moved; the rest of the tests read the old one,
               * which is the same but for the marks. */
            }
        }

      /* A line that starts with a marker becomes an item of a list. */
      marker = 0;
      if (what->lists && block->table < 0 && fmt->pa.list == W42_LIST_NONE)
        {
          marker = bullet_marker (text);
          if (marker > 0)
            kind = W42_LIST_BULLET;
          else
            marker = number_marker (text, &kind);
        }
      if (marker > 0)
        {
          W42ParaFmt pa = fmt->pa;

          w42_pt_delete (pt, start, (gsize) marker);
          pa.list = (guint8) kind;
          pa.indent_left = MAX (pa.indent_left, 360);
          pa.indent_first = -360;
          w42_pt_apply_para_fmt (pt, start, 0,
                                 W42_PARA_LIST | W42_PARA_INDENT_LEFT | W42_PARA_INDENT_FIRST,
                                 &pa);
          changed++;
          continue;                     /* an item is not a heading */
        }

      /* A short line that stands alone becomes a heading. */
      if (what->headings && block->table < 0 && fmt->pa.list == W42_LIST_NONE &&
          (fmt->pa.style == NULL || g_ascii_strcasecmp (fmt->pa.style, "Normal") == 0) &&
          next != NULL && next->text->len > 0 &&
          (previous == NULL || previous->text->len == 0) &&
          looks_like_a_heading (text, chars))
        {
          w42_pt_apply_style (pt, start, chars,
                              fmt->pa.indent_left >= 360 ? "Heading 2" : "Heading 1");
          changed++;
        }
    }

  w42_pt_end_group (pt);
  g_ptr_array_free (blocks, TRUE);
  return changed;
}
