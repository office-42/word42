/* w42-compare.c - Tools > Track Changes > Compare Documents
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two passes, each the longest common subsequence of a list: first the
 * paragraphs of the two documents are matched by their text, then the
 * paragraphs that did not match are paired off in order and their words
 * matched.  A word only here is marked inserted; a word only in the
 * original is put back, marked deleted, where it stood; a paragraph only
 * in the original comes back whole, mark and all, so that Accept All
 * takes it away again.  The edits are gathered first and applied from
 * the back of the document, so no position moves under another.
 */

#include "w42-compare.h"

#include <string.h>

/* The most cells the exact table may have: 16 MB of it, and about two
 * thousand by two thousand. */
#define TABLE_LIMIT 4000000

/* A stretch of each list still to be matched: [alo, ahi) of one against
 * [blo, bhi) of the other. */
typedef struct {
  guint alo, ahi, blo, bhi;
} Gap;

static void
add_pair (GArray *pairs, guint i, guint j)
{
  guint pair[2] = { i, j };

  g_array_append_val (pairs, pair);
}

static int
pair_cmp (gconstpointer pa, gconstpointer pb)
{
  const guint *a = pa, *b = pb;

  return a[0] < b[0] ? -1 : a[0] > b[0];
}

/* The exact longest common subsequence of a gap, by the table. */
static void
table_pairs (const guint *a, const guint *b, Gap g, GArray *pairs)
{
  guint na = g.ahi - g.alo, nb = g.bhi - g.blo;
  guint *table = g_new0 (guint, (gsize) (na + 1) * (nb + 1));
  guint i = 0, j = 0;

#define AT(i, j) table[(gsize) (i) * (nb + 1) + (j)]
  for (guint x = na; x-- > 0;)
    for (guint y = nb; y-- > 0;)
      {
        if (a[g.alo + x] == b[g.blo + y])
          AT (x, y) = AT (x + 1, y + 1) + 1;
        else
          AT (x, y) = MAX (AT (x + 1, y), AT (x, y + 1));
      }
  while (i < na && j < nb)
    {
      if (a[g.alo + i] == b[g.blo + j])
        {
          add_pair (pairs, g.alo + i, g.blo + j);
          i++;
          j++;
        }
      else if (AT (i + 1, j) >= AT (i, j + 1))
        i++;
      else
        j++;
    }
#undef AT
  g_free (table);
}

/* Patience diff's anchors: the strings that occur exactly once in each
 * side of the gap, paired, and of those pairs the longest chain in order
 * on both sides.  The paragraphs of a book are nearly all different, so
 * the anchors pin nearly everything down and what is left between them
 * is small.  `count_a`, `count_b` and `where_b` are indexed by string;
 * the counts are zero on the way in and are left so on the way out. */
static GArray *
anchors (const guint *a, const guint *b, Gap g,
         guint *count_a, guint *count_b, guint *where_b)
{
  GArray *cand = g_array_new (FALSE, FALSE, sizeof (guint) * 2);
  GArray *chain = g_array_new (FALSE, FALSE, sizeof (guint) * 2);
  guint *tails, *prev;
  guint n_tails = 0;

  for (guint i = g.alo; i < g.ahi; i++)
    count_a[a[i]]++;
  for (guint j = g.blo; j < g.bhi; j++)
    {
      count_b[b[j]]++;
      where_b[b[j]] = j;
    }
  for (guint i = g.alo; i < g.ahi; i++)
    if (count_a[a[i]] == 1 && count_b[a[i]] == 1)
      add_pair (cand, i, where_b[a[i]]);
  for (guint i = g.alo; i < g.ahi; i++)
    count_a[a[i]] = 0;
  for (guint j = g.blo; j < g.bhi; j++)
    count_b[b[j]] = 0;

  if (cand->len == 0)
    {
      g_array_free (cand, TRUE);
      return chain;
    }

  /* The candidates are in order on the first side; the longest run of
   * them in order on the second is the longest increasing subsequence
   * of their second indexes, found by patience sorting: tails[k] is the
   * candidate with the smallest second index that ends a run of k + 1. */
  tails = g_new (guint, cand->len);
  prev = g_new (guint, cand->len);
  for (guint c = 0; c < cand->len; c++)
    {
      guint j = g_array_index (cand, guint, 2 * c + 1);
      guint lo = 0, hi = n_tails;

      while (lo < hi)
        {
          guint mid = lo + (hi - lo) / 2;

          if (g_array_index (cand, guint, 2 * tails[mid] + 1) < j)
            lo = mid + 1;
          else
            hi = mid;
        }
      prev[c] = lo > 0 ? tails[lo - 1] : G_MAXUINT;
      tails[lo] = c;
      if (lo == n_tails)
        n_tails++;
    }
  g_array_set_size (chain, n_tails);
  for (guint c = tails[n_tails - 1], k = n_tails; k-- > 0; c = prev[c])
    {
      guint *to = &g_array_index (chain, guint, 2 * k);
      const guint *from = &g_array_index (cand, guint, 2 * c);

      to[0] = from[0];
      to[1] = from[1];
    }

  g_free (tails);
  g_free (prev);
  g_array_free (cand, TRUE);
  return chain;
}

/* The longest common subsequence of two lists of strings, as pairs of
 * indexes in order -- exactly while the lists are small enough for the
 * table, and after that as patience diff finds it.  What the two ends
 * share is matched first, which is all a paragraph put in at the top of
 * a novel needs; what is left is taken by the table when it fits, and
 * otherwise split at the anchors and each piece taken the same way.  A
 * piece too big for the table with no anchor in it at all -- the same
 * few lines over and over -- is matched in lockstep, which is not the
 * best answer but is a bounded one. */
static GArray *
lcs_pairs (GPtrArray *sa, GPtrArray *sb)
{
  GArray *pairs = g_array_new (FALSE, FALSE, sizeof (guint) * 2);
  guint na = sa->len, nb = sb->len;
  GHashTable *ids;
  guint *a, *b, *count_a, *count_b, *where_b;
  guint n_ids = 0;
  GArray *todo;

  if (na == 0 || nb == 0)
    return pairs;

  /* Each distinct string gets a number, so that everything after this
   * compares numbers rather than whole paragraphs. */
  ids = g_hash_table_new (g_str_hash, g_str_equal);
  a = g_new (guint, na);
  b = g_new (guint, nb);
  for (guint k = 0; k < na + nb; k++)
    {
      const char *s = k < na ? g_ptr_array_index (sa, k)
                             : g_ptr_array_index (sb, k - na);
      gpointer id;

      if (!g_hash_table_lookup_extended (ids, s, NULL, &id))
        {
          id = GUINT_TO_POINTER (n_ids++);
          g_hash_table_insert (ids, (gpointer) s, id);
        }
      if (k < na)
        a[k] = GPOINTER_TO_UINT (id);
      else
        b[k - na] = GPOINTER_TO_UINT (id);
    }
  g_hash_table_destroy (ids);
  count_a = g_new0 (guint, n_ids);
  count_b = g_new0 (guint, n_ids);
  where_b = g_new0 (guint, n_ids);

  /* Every piece yields pairs of its own, so the pieces can be taken in
   * any order and the pairs put in order at the end. */
  todo = g_array_new (FALSE, FALSE, sizeof (Gap));
  {
    Gap whole = { 0, na, 0, nb };

    g_array_append_val (todo, whole);
  }
  while (todo->len > 0)
    {
      Gap g = g_array_index (todo, Gap, todo->len - 1);
      GArray *chain;

      g_array_set_size (todo, todo->len - 1);

      while (g.alo < g.ahi && g.blo < g.bhi && a[g.alo] == b[g.blo])
        add_pair (pairs, g.alo++, g.blo++);
      while (g.alo < g.ahi && g.blo < g.bhi && a[g.ahi - 1] == b[g.bhi - 1])
        add_pair (pairs, --g.ahi, --g.bhi);
      if (g.alo == g.ahi || g.blo == g.bhi)
        continue;

      if ((gsize) (g.ahi - g.alo + 1) * (g.bhi - g.blo + 1) <= TABLE_LIMIT)
        {
          table_pairs (a, b, g, pairs);
          continue;
        }

      chain = anchors (a, b, g, count_a, count_b, where_b);
      if (chain->len == 0)
        {
          for (guint k = 0; k < MIN (g.ahi - g.alo, g.bhi - g.blo); k++)
            if (a[g.alo + k] == b[g.blo + k])
              add_pair (pairs, g.alo + k, g.blo + k);
        }
      else
        {
          Gap piece = g;

          for (guint k = 0; k < chain->len; k++)
            {
              guint i = g_array_index (chain, guint, 2 * k);
              guint j = g_array_index (chain, guint, 2 * k + 1);

              piece.ahi = i;
              piece.bhi = j;
              if (piece.alo < piece.ahi && piece.blo < piece.bhi)
                g_array_append_val (todo, piece);
              add_pair (pairs, i, j);
              piece.alo = i + 1;
              piece.blo = j + 1;
            }
          piece.ahi = g.ahi;
          piece.bhi = g.bhi;
          if (piece.alo < piece.ahi && piece.blo < piece.bhi)
            g_array_append_val (todo, piece);
        }
      g_array_free (chain, TRUE);
    }

  g_array_sort (pairs, pair_cmp);
  g_array_free (todo, TRUE);
  g_free (count_a);
  g_free (count_b);
  g_free (where_b);
  g_free (a);
  g_free (b);
  return pairs;
}

/* A paragraph's words, each with the space that follows it, so that the
 * words put back join up as they were. */
static GPtrArray *
split_words (const char *text)
{
  GPtrArray *words = g_ptr_array_new_with_free_func (g_free);
  const char *p = text;

  while (*p != '\0')
    {
      const char *start = p;

      while (*p != '\0' && !g_unichar_isspace (g_utf8_get_char (p)))
        p = g_utf8_next_char (p);
      while (*p != '\0' && g_unichar_isspace (g_utf8_get_char (p)))
        p = g_utf8_next_char (p);
      g_ptr_array_add (words, g_strndup (start, p - start));
    }
  return words;
}

typedef enum {
  EDIT_MARK_INSERTED,      /* [pos, pos + n) is only here */
  EDIT_PUT_BACK,           /* `text` from the original goes in at pos, deleted */
  EDIT_PUT_BACK_PARAGRAPH  /* the same, as a paragraph of its own before pos */
} EditKind;

typedef struct {
  EditKind kind;
  gsize    pos;
  gsize    n;
  char    *text;
  guint    seq;       /* the order the edits were found in */
} Edit;

static void
edit_clear (gpointer data)
{
  Edit *e = data;

  g_free (e->text);
}

/* Later edits first; at one position the marking goes before the putting
 * back, so that what was deleted reads before what replaced it. */
/* At one position: the marking first; then the paragraphs put back,
 * so that words put back there after them land at the end of the
 * paragraph before and not in the first of them. */
static int
edit_rank (EditKind kind)
{
  switch (kind)
    {
    case EDIT_MARK_INSERTED:      return 0;
    case EDIT_PUT_BACK_PARAGRAPH: return 1;
    default:                      return 2;
    }
}

static int
edit_cmp (gconstpointer pa, gconstpointer pb)
{
  const Edit *a = pa, *b = pb;

  if (a->pos != b->pos)
    return a->pos > b->pos ? -1 : 1;
  if (edit_rank (a->kind) != edit_rank (b->kind))
    return edit_rank (a->kind) < edit_rank (b->kind) ? -1 : 1;
  /* Each thing put in at a place goes in front of the one put there
   * before it, so the ones found later are put in first. */
  return a->seq > b->seq ? -1 : a->seq < b->seq ? 1 : 0;
}

/* The differences between one paragraph here and one in the original,
 * word by word, as edits at the paragraph's positions. */
static void
compare_paragraphs (GArray *edits, const W42Block *here, const W42Block *there)
{
  GPtrArray *a = split_words (here->text->str);
  GPtrArray *b = split_words (there->text->str);
  GArray *pairs = lcs_pairs (a, b);
  guint ia = 0, ib = 0;
  gsize pos = here->start_pos + 1;      /* the paragraph's text begins here */

  for (guint k = 0; k <= pairs->len; k++)
    {
      guint ma = k < pairs->len ? g_array_index (pairs, guint, 2 * k) : a->len;
      guint mb = k < pairs->len ? g_array_index (pairs, guint, 2 * k + 1) : b->len;
      GString *gone = NULL;
      gsize here_n = 0;

      /* The words here up to the next match are insertions ... */
      for (; ia < ma; ia++)
        here_n += g_utf8_strlen (g_ptr_array_index (a, ia), -1);
      /* ... and the original's up to its next match were deleted. */
      for (; ib < mb; ib++)
        {
          if (gone == NULL)
            gone = g_string_new (NULL);
          g_string_append (gone, g_ptr_array_index (b, ib));
        }
      if (here_n > 0)
        {
          Edit e = { EDIT_MARK_INSERTED, pos, here_n, NULL, edits->len };
          g_array_append_val (edits, e);
        }
      if (gone != NULL)
        {
          Edit e = { EDIT_PUT_BACK, pos, 0, g_string_free (gone, FALSE), edits->len };
          g_array_append_val (edits, e);
        }
      pos += here_n;
      if (k < pairs->len)
        {
          pos += g_utf8_strlen (g_ptr_array_index (a, ma), -1);
          ia = ma + 1;
          ib = mb + 1;
        }
    }

  g_array_free (pairs, TRUE);
  g_ptr_array_free (a, TRUE);
  g_ptr_array_free (b, TRUE);
}

int
w42_pt_compare (W42PieceTable *pt, W42PieceTable *original)
{
  GPtrArray *here_all, *there_all, *here, *there, *here_text, *there_text;
  gsize body_end;
  GArray *pairs, *edits;
  guint ih = 0, it = 0;
  int made = 0;

  g_return_val_if_fail (pt != NULL, 0);
  g_return_val_if_fail (original != NULL, 0);

  here_all = w42_pt_snapshot_blocks (pt);
  there_all = w42_pt_snapshot_blocks (original);
  /* The body's paragraphs only: matched against a note's, the body's
   * text would be put back inside the note.  The notes section is the
   * last thing in a document, so what goes back at the end of the body
   * goes in front of it. */
  here = g_ptr_array_new ();
  there = g_ptr_array_new ();
  for (guint i = 0; i < here_all->len; i++)
    if (((W42Block *) g_ptr_array_index (here_all, i))->note < 0)
      g_ptr_array_add (here, g_ptr_array_index (here_all, i));
  for (guint i = 0; i < there_all->len; i++)
    if (((W42Block *) g_ptr_array_index (there_all, i))->note < 0)
      g_ptr_array_add (there, g_ptr_array_index (there_all, i));
  body_end = w42_pt_notes_start (pt);
  if (body_end == (gsize) -1)
    body_end = w42_pt_length (pt);
  here_text = g_ptr_array_new ();
  there_text = g_ptr_array_new ();
  for (guint i = 0; i < here->len; i++)
    g_ptr_array_add (here_text, ((W42Block *) g_ptr_array_index (here, i))->text->str);
  for (guint i = 0; i < there->len; i++)
    g_ptr_array_add (there_text, ((W42Block *) g_ptr_array_index (there, i))->text->str);
  pairs = lcs_pairs (here_text, there_text);
  edits = g_array_new (FALSE, FALSE, sizeof (Edit));
  g_array_set_clear_func (edits, edit_clear);

  for (guint k = 0; k <= pairs->len; k++)
    {
      guint mh = k < pairs->len ? g_array_index (pairs, guint, 2 * k) : here->len;
      guint mt = k < pairs->len ? g_array_index (pairs, guint, 2 * k + 1) : there->len;

      /* The paragraphs that did not match, paired off in order and
       * compared word by word; what is left over on either side is a
       * whole paragraph new or gone. */
      while (ih < mh && it < mt)
        compare_paragraphs (edits, g_ptr_array_index (here, ih++),
                            g_ptr_array_index (there, it++));
      for (; ih < mh; ih++)
        {
          /* A paragraph only here: its mark is inserted along with its
           * text, so that rejecting the change joins its neighbours up
           * again. */
          const W42Block *block = g_ptr_array_index (here, ih);
          gsize n = g_utf8_strlen (block->text->str, -1);
          gboolean first = block->start_pos + 1 == w42_pt_first_caret_pos (pt);
          Edit e = { EDIT_MARK_INSERTED, first ? block->start_pos + 1 : block->start_pos,
                     first ? n : n + 1, NULL, edits->len };

          /* The document's first mark is never inserted: a document has
           * one whatever was done to it. */
          if (e.n > 0)
            g_array_append_val (edits, e);
        }
      for (; it < mt; it++)
        {
          const W42Block *block = g_ptr_array_index (there, it);
          /* Before the next paragraph here that has a match, or at the
           * end of the document. */
          gsize before = mh < here->len
                           ? ((W42Block *) g_ptr_array_index (here, mh))->start_pos
                           : body_end;
          Edit e = { EDIT_PUT_BACK_PARAGRAPH, before, 0, g_strdup (block->text->str),
                     edits->len };
          g_array_append_val (edits, e);
        }
      if (k < pairs->len)
        {
          ih = mh + 1;
          it = mt + 1;
        }
    }

  if (edits->len > 0)
    {
      W42ApTable *aps = w42_pt_ap_table (pt);
      W42CharFmt inserted;

      memset (&inserted, 0, sizeof inserted);
      inserted.revision = 1;

      g_array_sort (edits, edit_cmp);
      w42_pt_begin_group (pt);
      for (guint i = 0; i < edits->len; i++)
        {
          const Edit *e = &g_array_index (edits, Edit, i);

          if (e->kind == EDIT_MARK_INSERTED)
            {
              w42_pt_apply_char_fmt (pt, e->pos, e->n, W42_CHAR_REVISION, &inserted);
            }
          else
            {
              /* The text comes back in the formatting of where it lands,
               * marked deleted.  A whole paragraph gets a mark of its own
               * first, marked deleted too and shaped like the paragraph
               * it stands before, so that Accept All removes the lot. */
              gsize pos = MIN (e->pos, w42_pt_length (pt));
              W42Fmt fmt;
              W42ApIdx ap;

              if (e->kind == EDIT_PUT_BACK_PARAGRAPH)
                {
                  gsize near = pos < w42_pt_length (pt) ? pos : w42_pt_length (pt) - 1;

                  fmt = *w42_ap_table_get (aps, w42_pt_block_ap_at (pt, near));
                  fmt.ch.revision = 2;
                  ap = w42_ap_table_intern (aps, &fmt);
                  w42_pt_insert_block (pt, pos, ap);
                  w42_pt_insert_text (pt, pos + 1, e->text, ap);
                }
              else
                {
                  fmt = *w42_ap_table_get (aps, w42_pt_ap_at (pt, pos));
                  fmt.ch.revision = 2;
                  ap = w42_ap_table_intern (aps, &fmt);
                  w42_pt_insert_text (pt, pos, e->text, ap);
                }
            }
          made++;
        }
      w42_pt_end_group (pt);
    }

  g_array_free (edits, TRUE);
  g_array_free (pairs, TRUE);
  g_ptr_array_free (here_text, TRUE);
  g_ptr_array_free (there_text, TRUE);
  g_ptr_array_free (here, TRUE);
  g_ptr_array_free (there, TRUE);
  g_ptr_array_free (here_all, TRUE);
  g_ptr_array_free (there_all, TRUE);
  return made;
}
