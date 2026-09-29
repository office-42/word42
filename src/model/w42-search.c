/* w42-search.c - see w42-search.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-search.h"

#include <string.h>

/* Tools > Hyphenation puts one inside words, where nobody can see it, so a
 * search looks straight through it: "vanskelige" finds "van-ske-lige". */
#define SOFT_HYPHEN 0x00AD

static const char *
skip_soft_hyphens (const char *p)
{
  while (g_utf8_get_char (p) == SOFT_HYPHEN)
    p = g_utf8_next_char (p);
  return p;
}

/* A needle of nothing but soft hyphens would match nothing everywhere. */
static gboolean
needle_is_empty (const char *needle)
{
  return *skip_soft_hyphens (needle) == '\0';
}

/* Compares character by character rather than casefolding both strings first,
 * because casefolding can change a string's length and the caller needs byte
 * offsets back that still index the original text.  The cost is that the
 * one-to-many foldings -- Eszett against "ss" and its kin -- do not match.
 * Soft hyphens are passed over on both sides; the match starts on a real
 * character, and one inside it is part of it, so replacing it replaces the
 * lot. */
static gboolean
match_at (const char *hay, const char *needle, gboolean match_case,
          const char **end_out)
{
  const char *h = hay;
  const char *n = skip_soft_hyphens (needle);

  if (g_utf8_get_char (h) == SOFT_HYPHEN)
    return FALSE;

  while (*n != '\0')
    {
      gunichar hc, nc;

      h = skip_soft_hyphens (h);
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
      n = skip_soft_hyphens (g_utf8_next_char (n));
    }

  *end_out = h;
  return TRUE;
}

static gboolean
is_word_char (gunichar c)
{
  return g_unichar_isalnum (c) || c == '_';
}

/* A soft hyphen is inside a word, not the end of one: "vanske" is not a
 * whole word of "van-ske-lige". */
static gboolean
on_word_boundary (const char *text, const char *start, const char *end)
{
  const char *prev = start;

  while (prev > text)
    {
      gunichar c;

      prev = g_utf8_find_prev_char (text, prev);
      if (prev == NULL)
        break;
      c = g_utf8_get_char (prev);
      if (c == SOFT_HYPHEN)
        continue;
      if (is_word_char (c))
        return FALSE;
      break;
    }

  end = skip_soft_hyphens (end);
  if (*end != '\0' && is_word_char (g_utf8_get_char (end)))
    return FALSE;

  return TRUE;
}

/* ---- regular expressions ------------------------------------------- */

/* A needle that is a regular expression, compiled for the options: case
 * folded unless they match case, and UTF-8 throughout.  A paragraph is
 * the text it runs over, so ^ and $ are the paragraph's ends. */
static GRegex *
compile_regex (const char *needle, const W42SearchOptions *options, GError **error)
{
  GRegexCompileFlags flags = G_REGEX_OPTIMIZE;

  if (!options->match_case)
    flags |= G_REGEX_CASELESS;
  return g_regex_new (needle, flags, G_REGEX_MATCH_DEFAULT, error);
}

/* A paragraph's text with its soft hyphens taken out, for a regular
 * expression to run over -- "vanskelige" finds "van-ske-lige", as the
 * plain search finds it -- and where each of its bytes was. */
typedef struct {
  const char *str;
  gsize       len;
  GString    *own;      /* the copy, when there were soft hyphens */
  GArray     *at;       /* gsize: each byte's offset in the text, and one
                         * past its end; NULL when they are the same */
} Clean;

static void
clean_init (Clean *c, const GString *text)
{
  memset (c, 0, sizeof *c);
  c->str = text->str;
  c->len = text->len;
  if (g_strstr_len (text->str, (gssize) text->len, "\302\255") == NULL)
    return;

  c->own = g_string_sized_new (text->len);
  c->at = g_array_sized_new (FALSE, FALSE, sizeof (gsize), (guint) text->len + 1);
  for (const char *p = text->str; p < text->str + text->len; )
    {
      const char *next = g_utf8_next_char (p);

      if (g_utf8_get_char (p) != SOFT_HYPHEN)
        for (const char *q = p; q < next; q++)
          {
            gsize off = (gsize) (q - text->str);

            g_string_append_c (c->own, *q);
            g_array_append_val (c->at, off);
          }
      p = next;
    }
  {
    gsize end = text->len;

    g_array_append_val (c->at, end);
  }
  c->str = c->own->str;
  c->len = c->own->len;
}

static void
clean_free (Clean *c)
{
  if (c->own != NULL)
    g_string_free (c->own, TRUE);
  if (c->at != NULL)
    g_array_free (c->at, TRUE);
}

/* The text's offset of the clean text's byte `i`. */
static gsize
clean_to_text (const Clean *c, gsize i)
{
  return c->at != NULL ? g_array_index (c->at, gsize, MIN (i, c->len)) : i;
}

/* The first byte of the clean text at or after the text's offset `off`. */
static gsize
text_to_clean (const Clean *c, gsize off)
{
  gsize lo = 0, hi = c->len;

  if (c->at == NULL)
    return MIN (off, c->len);
  while (lo < hi)
    {
      gsize mid = lo + (hi - lo) / 2;

      if (g_array_index (c->at, gsize, mid) < off)
        lo = mid + 1;
      else
        hi = mid;
    }
  return lo;
}

/* The regular expression's hits in one paragraph between two byte
 * offsets: the first, or when searching up the last.  A match of nothing
 * counts only when `empty` says so, which Replace All does -- ^ puts
 * something in front of every paragraph -- and Find does not, having
 * nothing to select.  With `expanded`, the replacement for the hit, with
 * what its groups hold put in for their names. */
static gboolean
regex_search_block (const W42Block         *block,
                    gsize                   from_byte,
                    gsize                   to_byte,
                    GRegex                 *re,
                    const W42SearchOptions *options,
                    gboolean                empty,
                    const char             *replacement,
                    char                  **expanded,
                    gsize                  *hit_start,
                    gsize                  *hit_end)
{
  const char *text = block->text->str;
  gboolean found = FALSE;
  Clean c;
  gsize pos;

  if (from_byte > block->text->len)
    return FALSE;

  clean_init (&c, block->text);
  pos = text_to_clean (&c, from_byte);
  while (pos <= c.len)
    {
      GMatchInfo *mi = NULL;
      gint ms = 0, me = 0;
      gsize ts, te;

      if (!g_regex_match_full (re, c.str, (gssize) c.len, (gint) pos, 0, &mi, NULL))
        {
          g_match_info_free (mi);
          break;
        }
      g_match_info_fetch_pos (mi, 0, &ms, &me);
      ts = clean_to_text (&c, (gsize) ms);
      te = me > ms ? clean_to_text (&c, (gsize) me - 1) + 1 : ts;
      if (ts > to_byte || (ts == to_byte && me > ms))
        {
          g_match_info_free (mi);
          break;
        }
      if ((me > ms || empty) && te <= to_byte &&
          (!options->whole_word || on_word_boundary (text, text + ts, text + te)))
        {
          *hit_start = ts;
          *hit_end = te;
          found = TRUE;
          if (expanded != NULL)
            {
              char *with = g_match_info_expand_references (mi, replacement != NULL ? replacement : "",
                                                           NULL);

              /* A name the pattern has no group for: the replacement as
               * it was typed. */
              g_free (*expanded);
              *expanded = with != NULL ? with : g_strdup (replacement != NULL ? replacement : "");
            }
          if (!options->backwards)
            {
              g_match_info_free (mi);
              break;
            }
        }
      g_match_info_free (mi);
      /* On from the character after where it started: a later start may
       * still end in range, or be the last one there. */
      pos = (gsize) ms < c.len ? (gsize) (g_utf8_next_char (c.str + ms) - c.str) : c.len + 1;
    }
  clean_free (&c);
  return found;
}

gboolean
w42_search_check (const char             *needle,
                  const W42SearchOptions *options,
                  GError                **error)
{
  GRegex *re;

  g_return_val_if_fail (options != NULL, FALSE);

  if (needle == NULL || needle_is_empty (needle))
    return FALSE;
  if (!options->regex)
    return TRUE;
  re = compile_regex (needle, options, error);
  if (re == NULL)
    return FALSE;
  g_regex_unref (re);
  return TRUE;
}

/* Searches one paragraph between two byte offsets.  Returns the byte offsets
 * of the hit, or FALSE.  `re` is the needle compiled, when it is a regular
 * expression. */
static gboolean
search_block (const W42Block         *block,
              gsize                   from_byte,
              gsize                   to_byte,
              const char             *needle,
              GRegex                 *re,
              const W42SearchOptions *options,
              gsize                  *hit_start,
              gsize                  *hit_end)
{
  const char *text = block->text->str;
  const char *limit = text + MIN (to_byte, block->text->len);
  const char *p;
  gboolean found = FALSE;

  if (re != NULL)
    return regex_search_block (block, from_byte, to_byte, re, options, FALSE, NULL, NULL,
                               hit_start, hit_end);
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
w42_search_is_match (const char             *text,
                     const char             *needle,
                     const W42SearchOptions *options)
{
  const char *end = NULL;

  g_return_val_if_fail (options != NULL, FALSE);

  if (text == NULL || needle == NULL || needle_is_empty (needle))
    return FALSE;

  if (options->regex)
    {
      /* The whole of it, one match. */
      GRegex *re = compile_regex (needle, options, NULL);
      GMatchInfo *mi = NULL;
      gboolean all = FALSE;

      if (re == NULL)
        return FALSE;
      if (g_regex_match_full (re, text, -1, 0, G_REGEX_MATCH_ANCHORED, &mi, NULL))
        {
          gint ms = 0, me = 0;

          g_match_info_fetch_pos (mi, 0, &ms, &me);
          all = me > ms && (gsize) me == strlen (text);
        }
      g_match_info_free (mi);
      g_regex_unref (re);
      return all;
    }

  text = skip_soft_hyphens (text);
  return match_at (text, needle, options->match_case, &end) &&
         *skip_soft_hyphens (end) == '\0';
}

char *
w42_search_replacement_at (W42PieceTable          *pt,
                           gsize                   start,
                           gsize                   end,
                           const char             *needle,
                           const char             *replacement,
                           const W42SearchOptions *options)
{
  W42SearchOptions forward;
  GPtrArray *blocks;
  GRegex *re;
  char *expanded = NULL;

  g_return_val_if_fail (pt != NULL, NULL);
  g_return_val_if_fail (options != NULL, NULL);

  if (needle == NULL || needle_is_empty (needle) || end <= start)
    return NULL;
  if (replacement == NULL)
    replacement = "";

  if (!options->regex)
    {
      char *text = w42_pt_get_text (pt, start, end - start);
      gboolean ok = w42_search_is_match (text, needle, options);

      g_free (text);
      return ok ? g_strdup (replacement) : NULL;
    }

  re = compile_regex (needle, options, NULL);
  if (re == NULL)
    return NULL;
  forward = *options;
  forward.backwards = FALSE;

  /* Run over the paragraph from the start, so that what comes before --
   * a \b, a look behind -- is seen as the search saw it. */
  blocks = w42_pt_snapshot_blocks (pt);
  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      gsize first = block->start_pos + 1;
      gsize n_chars;
      gsize from, to, hs = 0, he = 0;

      if (start < first)
        break;
      n_chars = (gsize) g_utf8_strlen (block->text->str, (gssize) block->text->len);
      if (end > first + n_chars)
        continue;
      from = block_pos_to_byte (block, start);
      to = block_pos_to_byte (block, end);
      if (!regex_search_block (block, from, to, re, &forward, FALSE, replacement, &expanded,
                               &hs, &he) ||
          hs != from || he != to)
        g_clear_pointer (&expanded, g_free);
      break;
    }
  g_ptr_array_free (blocks, TRUE);
  g_regex_unref (re);
  return expanded;
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
  GRegex *re = NULL;
  int start_block = 0;
  gboolean found = FALSE;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (options != NULL, FALSE);

  if (needle == NULL || needle_is_empty (needle))
    return FALSE;
  if (options->regex && (re = compile_regex (needle, options, NULL)) == NULL)
    return FALSE;

  blocks = w42_pt_snapshot_blocks (pt);
  if (blocks->len == 0)
    {
      g_ptr_array_free (blocks, TRUE);
      g_clear_pointer (&re, g_regex_unref);
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
              search_block (block, lo, hi, needle, re, options, &hs, &he))
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
  g_clear_pointer (&re, g_regex_unref);
  return found;
}

gsize
w42_search_replace_all (W42PieceTable          *pt,
                        const char             *needle,
                        const char             *replacement,
                        const W42SearchOptions *options)
{
  W42SearchOptions sweep;
  GPtrArray *blocks;
  GArray *hits;                     /* gsize pairs: start, end */
  GPtrArray *withs = NULL;          /* char*: each hit's replacement, for a
                                     * regular expression */
  GRegex *re = NULL;
  gsize count = 0;

  g_return_val_if_fail (pt != NULL, 0);
  g_return_val_if_fail (options != NULL, 0);

  if (needle == NULL || needle_is_empty (needle))
    return 0;

  if (replacement == NULL)
    replacement = "";
  if (options->regex)
    {
      re = compile_regex (needle, options, NULL);
      if (re == NULL)
        return 0;
      withs = g_ptr_array_new_with_free_func (g_free);
    }

  sweep = *options;
  sweep.backwards = FALSE;
  sweep.wrap = FALSE;

  /* Every hit, from one snapshot, then replaced from the back so that no
   * replacement moves a hit still to be made.  A snapshot for each hit
   * made this the square of the document: 848 names in the Bible sample
   * took 52 seconds. */
  blocks = w42_pt_snapshot_blocks (pt);
  hits = g_array_new (FALSE, FALSE, sizeof (gsize));
  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      gsize from = 0, hs = 0, he = 0;

      if (re != NULL)
        {
          /* A match of nothing replaces too -- ^ puts something before
           * every paragraph -- but not one just where the last match
           * ended, which would put it in twice. */
          gsize last_end = (gsize) -1;
          char *with = NULL;

          while (from <= block->text->len &&
                 regex_search_block (block, from, block->text->len, re, &sweep, TRUE,
                                     replacement, &with, &hs, &he))
            {
              gsize at, to;

              if (he == hs && hs == last_end)
                {
                  if (hs >= block->text->len)
                    break;
                  from = (gsize) (g_utf8_next_char (block->text->str + hs) - block->text->str);
                  continue;
                }
              at = block_byte_to_pos (block, hs);
              to = block_byte_to_pos (block, he);
              g_array_append_val (hits, at);
              g_array_append_val (hits, to);
              g_ptr_array_add (withs, with);
              with = NULL;
              last_end = he;
              if (he > hs)
                from = he;
              else if (hs >= block->text->len)
                break;
              else
                from = (gsize) (g_utf8_next_char (block->text->str + hs) - block->text->str);
            }
          g_free (with);
          continue;
        }
      while (from < block->text->len &&
             search_block (block, from, block->text->len, needle, NULL, &sweep, &hs, &he))
        {
          gsize at = block_byte_to_pos (block, hs), to = block_byte_to_pos (block, he);

          g_array_append_val (hits, at);
          g_array_append_val (hits, to);
          from = he;
        }
    }
  g_ptr_array_free (blocks, TRUE);

  w42_pt_begin_group (pt);
  for (guint i = hits->len; i >= 2; i -= 2)
    {
      gsize start = g_array_index (hits, gsize, i - 2);
      gsize end = g_array_index (hits, gsize, i - 1);
      const char *with = withs != NULL ? g_ptr_array_index (withs, i / 2 - 1) : replacement;
      /* The formatting of the text being replaced: of the character
       * before, for a match of nothing. */
      W42ApIdx ap = w42_pt_ap_at (pt, end > start ? start + 1 : start);

      if (end > start)
        w42_pt_delete (pt, start, end - start);
      if (*with != '\0')
        w42_pt_insert_text (pt, start, with, ap);
      if (end > start || *with != '\0')
        count++;
    }
  w42_pt_end_group (pt);
  g_array_free (hits, TRUE);
  if (withs != NULL)
    g_ptr_array_free (withs, TRUE);
  g_clear_pointer (&re, g_regex_unref);

  return count;
}
