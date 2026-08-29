/* w42-search.c - see w42-search.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-search.h"

#include <string.h>

/* Compares character by character rather than casefolding both strings first,
 * because casefolding can change a string's length and the caller needs byte
 * offsets back that still index the original text.  The cost is that the
 * one-to-many foldings -- Eszett against "ss" and its kin -- do not match. */
static gboolean
match_at (const char *hay, const char *needle, gboolean match_case,
          const char **end_out)
{
  const char *h = hay;
  const char *n = needle;

  while (*n != '\0')
    {
      gunichar hc, nc;

      if (*h == '\0')
        return FALSE;

      hc = g_utf8_get_char (h);
      nc = g_utf8_get_char (n);

      if (!match_case)
        {
          hc = g_unichar_tolower (hc);
          nc = g_unichar_tolower (nc);
        }

      if (hc != nc)
        return FALSE;

      h = g_utf8_next_char (h);
      n = g_utf8_next_char (n);
    }

  *end_out = h;
  return TRUE;
}

static gboolean
is_word_char (gunichar c)
{
  return g_unichar_isalnum (c) || c == '_';
}

static gboolean
on_word_boundary (const char *text, const char *start, const char *end)
{
  if (start > text)
    {
      const char *prev = g_utf8_find_prev_char (text, start);
      if (prev != NULL && is_word_char (g_utf8_get_char (prev)))
        return FALSE;
    }

  if (*end != '\0' && is_word_char (g_utf8_get_char (end)))
    return FALSE;

  return TRUE;
}

/* Searches one paragraph between two byte offsets.  Returns the byte offsets
 * of the hit, or FALSE. */
static gboolean
search_block (const W42Block         *block,
              gsize                   from_byte,
              gsize                   to_byte,
              const char             *needle,
              const W42SearchOptions *options,
              gsize                  *hit_start,
              gsize                  *hit_end)
{
  const char *text = block->text->str;
  const char *limit = text + MIN (to_byte, block->text->len);
  const char *p;
  gboolean found = FALSE;

  if (from_byte > block->text->len)
    return FALSE;

  for (p = text + from_byte; p < limit && *p != '\0'; p = g_utf8_next_char (p))
    {
      const char *end = NULL;

      if (!match_at (p, needle, options->match_case, &end))
        continue;

      if (options->whole_word && !on_word_boundary (text, p, end))
        continue;

      *hit_start = (gsize) (p - text);
      *hit_end   = (gsize) (end - text);
      found = TRUE;

      /* Going backwards means taking the last hit in range, so keep looking. */
      if (!options->backwards)
        return TRUE;
    }

  return found;
}

/* The block's characters run consecutively from just after its paragraph
 * mark, so document position and byte offset convert by counting characters.
 * (The layout engine exposes the same pair; they are repeated here so the
 * model does not have to depend on the layout.) */
static gsize
block_byte_to_pos (const W42Block *block, gsize byte)
{
  return block->start_pos + 1 +
         (gsize) g_utf8_pointer_to_offset (block->text->str,
                                           block->text->str + byte);
}

static gsize
block_pos_to_byte (const W42Block *block, gsize pos)
{
  gsize offset;
  const char *p;

  if (pos <= block->start_pos)
    return 0;

  /* g_utf8_offset_to_pointer walks the string blindly: past the end it
   * reads past the end.  The offset is checked against the characters
   * there are before it is used. */
  offset = pos - block->start_pos - 1;
  if (offset >= (gsize) g_utf8_strlen (block->text->str, (gssize) block->text->len))
    return block->text->len;

  p = g_utf8_offset_to_pointer (block->text->str, (glong) offset);

  if (p < block->text->str)
    return 0;
  if ((gsize) (p - block->text->str) > block->text->len)
    return block->text->len;

  return (gsize) (p - block->text->str);
}

gboolean
w42_search_find (W42PieceTable          *pt,
                 gsize                   from,
                 const char             *needle,
                 const W42SearchOptions *options,
                 gsize                  *match_start,
                 gsize                  *match_end)
{
  GPtrArray *blocks;
  int start_block = 0;
  gboolean found = FALSE;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (options != NULL, FALSE);

  if (needle == NULL || *needle == '\0')
    return FALSE;

  blocks = w42_pt_snapshot_blocks (pt);
  if (blocks->len == 0)
    {
      g_ptr_array_free (blocks, TRUE);
      return FALSE;
    }

  for (guint i = 0; i < blocks->len; i++)
    {
      const W42Block *block = g_ptr_array_index (blocks, i);
      if (block->start_pos < from)
        start_block = (int) i;
      else
        break;
    }

  /* Two sweeps: from the caret to the end of the document, then -- if wrap is
   * on -- from the start back to the caret.  Backwards search runs the same
   * two sweeps in the other order. */
  for (int pass = 0; pass < 2 && !found; pass++)
    {
      int step = options->backwards ? -1 : 1;
      int first = (pass == 0)
                    ? start_block
                    : (options->backwards ? (int) blocks->len - 1 : 0);
      int last = (pass == 0)
                   ? (options->backwards ? 0 : (int) blocks->len - 1)
                   : start_block;

      if (pass == 1 && !options->wrap)
        break;

      for (int i = first; ; i += step)
        {
          const W42Block *block = g_ptr_array_index (blocks, (guint) i);
          gsize lo = 0;
          gsize hi = block->text->len;
          gsize hs = 0, he = 0;

          /* The block the caret is in is only half in play. */
          if (i == start_block)
            {
              gsize caret = block_pos_to_byte (block, from);

              if (pass == 0)
                {
                  if (options->backwards)
                    hi = caret;
                  else
                    lo = caret;
                }
              else
                {
                  if (options->backwards)
                    lo = caret;
                  else
                    hi = caret;
                }
            }

          if (lo <= hi &&
              search_block (block, lo, hi, needle, options, &hs, &he))
            {
              *match_start = block_byte_to_pos (block, hs);
              *match_end   = block_byte_to_pos (block, he);
              found = TRUE;
              break;
            }

          if (i == last)
            break;
        }
    }

  g_ptr_array_free (blocks, TRUE);
  return found;
}

gsize
w42_search_replace_all (W42PieceTable          *pt,
                        const char             *needle,
                        const char             *replacement,
                        const W42SearchOptions *options)
{
  W42SearchOptions sweep;
  gsize pos;
  gsize count = 0;

  g_return_val_if_fail (pt != NULL, 0);
  g_return_val_if_fail (options != NULL, 0);

  if (needle == NULL || *needle == '\0')
    return 0;

  if (replacement == NULL)
    replacement = "";

  /* Replace-all always runs forwards from the top and never wraps; wrapping
   * would put it straight back over its own replacements. */
  sweep = *options;
  sweep.backwards = FALSE;
  sweep.wrap = FALSE;

  pos = w42_pt_first_caret_pos (pt);

  w42_pt_begin_group (pt);

  for (;;)
    {
      gsize start = 0, end = 0;
      W42ApIdx ap;

      if (!w42_search_find (pt, pos, needle, &sweep, &start, &end))
        break;

      /* Take the formatting of the text being replaced, so replacing a word
       * inside a bold run leaves the replacement bold. */
      ap = w42_pt_ap_at (pt, start + 1);

      w42_pt_delete (pt, start, end - start);
      if (*replacement != '\0')
        w42_pt_insert_text (pt, start, replacement, ap);

      pos = start + g_utf8_strlen (replacement, -1);
      count++;

      /* Deleting to nothing and inserting nothing would not advance. */
      if (pos <= start && *replacement == '\0')
        pos = start;
    }

  w42_pt_end_group (pt);

  return count;
}
