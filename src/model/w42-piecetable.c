/* w42-piecetable.c - see w42-piecetable.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-piecetable.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* Pieces                                                                  */
/* ---------------------------------------------------------------------- */

typedef struct _W42Piece W42Piece;

struct _W42Piece {
  W42Piece *prev;
  W42Piece *next;
  guint8    type;       /* W42PieceType */
  guint8    strux;      /* W42StruxType, meaningful when type is STRUX */
  guint8    in_change;  /* which buffer a TEXT piece points into */
  W42ApIdx  ap;
  gsize     offset;     /* character offset into that buffer */
  gsize     length;     /* characters; a strux is always 1 */
};

/* ---------------------------------------------------------------------- */
/* Change records                                                          */
/* ---------------------------------------------------------------------- */

/* A record describes an edit that has already happened.  Applying it performs
 * the reverse of that edit and yields a record describing the reverse, so one
 * routine drives both undo and redo and a stack entry simply flips direction
 * each time it is used. */
typedef enum {
  CR_INSERT,   /* len things appeared at pos */
  CR_DELETE,   /* the content in runs/chars vanished from pos */
  CR_FMT,      /* the AP runs in runs used to cover [pos, pos+len) */
  CR_TABLE     /* table pos had the column widths in runs (int) */
} CRType;

typedef struct {
  guint8       type;       /* W42PieceType */
  guint8       strux;
  W42ApIdx     ap;
  W42ObjectIdx object;     /* for an OBJECT piece */
  gsize        n;          /* characters, 1 for a strux or an object */
  gsize        chars_off;  /* index into the record's chars array */
} W42SavedRun;

typedef struct {
  gsize    n;
  W42ApIdx ap;
} W42ApRun;

typedef struct {
  CRType   type;
  gsize    pos;
  gsize    len;
  guint32  group;
  GArray  *runs;    /* W42SavedRun for CR_DELETE, W42ApRun for CR_FMT */
  GArray  *chars;   /* gunichar, for CR_DELETE */
} W42CR;

/* ---------------------------------------------------------------------- */

struct _W42PieceTable {
  GArray     *initial;   /* gunichar, immutable once loaded */
  GArray     *change;    /* gunichar, append-only */
  W42ApTable *aps;
  W42ObjectTable *objects;
  W42StyleSheet *styles;
  W42PageText header;
  W42PageText header_first, header_even;   /* a title page's, and even pages' */
  W42PageText footer_first, footer_even;
  guint8      title_page, facing_pages;
  W42PageText footer;
  const char *author;    /* interned, or NULL */
  W42DocInfo  info;      /* what File > Summary Info says */
  GPtrArray  *tables;    /* W42TableProps*, by table id */
  int         next_note; /* footnote ids, never reused */

  W42Piece   *head;
  W42Piece   *tail;
  gsize       length;

  /* The last piece pt_find() landed on and where it starts: a caret
   * moving through a document asks for neighbouring positions over and
   * over, and walking from the head each time is what the roadmap
   * called the first thing worth fixing.  Cleared whenever the piece
   * list changes shape. */
  W42Piece   *find_cache;
  gsize       find_cache_start;

  GPtrArray  *records;   /* W42CR* */
  gsize       undo_pos;  /* records [0, undo_pos) can be undone */
  guint32     next_group;
  guint32     group_id;
  int         group_depth;
  gboolean    coalescing;
  guint64     edit_serial;   /* grows when the history is rewritten under
                              * the records already in it: a coalesced
                              * insertion, a dropped redo, or a change
                              * that was never recorded at all.  With
                              * undo_pos it says which document this is,
                              * so that undoing back to where the file was
                              * saved can be recognised. */
};

static gsize pt_block_start (W42PieceTable *pt, gsize pos);
static gsize pt_block_end (W42PieceTable *pt, gsize block);

static void
table_props_free (gpointer data)
{
  W42TableProps *props = data;

  if (props->widths != NULL)
    g_array_free (props->widths, TRUE);
  if (props->row_heights != NULL)
    g_array_free (props->row_heights, TRUE);
  g_free (props);
}

static inline gboolean
piece_is_strux (const W42Piece *p, W42StruxType which)
{
  return p->type == W42_PIECE_STRUX && (W42StruxType) p->strux == which;
}

/* A FOOTNOTE mark's payload: the note's id, and whether it is an endnote. */
#define NOTE_END_BIT     ((gsize) 1 << 30)
#define NOTE_ID(o)       ((int) ((o) & ~NOTE_END_BIT))
#define NOTE_IS_END(o)   (((o) & NOTE_END_BIT) != 0)

/* A CELL mark's payload: the row, the column, and how many columns the
 * cell spans, which is 1 unless cells have been merged. */
#define CELL_ROW(o)     ((int) (((o) >> 20) & 0xfff))
#define CELL_COL(o)     ((int) (((o) >> 10) & 0x3ff))
#define CELL_SPAN(o)    (MAX ((int) ((o) & 0x3ff), 1))
#define CELL_PAYLOAD(r, c, s) \
  (((gsize) (r) << 20) | ((gsize) (c) << 10) | (gsize) MAX ((s), 1))

/* ---------------------------------------------------------------------- */
/* Record helpers                                                          */
/* ---------------------------------------------------------------------- */

static W42CR *
cr_new (CRType type, gsize pos, gsize len)
{
  W42CR *cr = g_new0 (W42CR, 1);

  cr->type = type;
  cr->pos  = pos;
  cr->len  = len;

  if (type == CR_DELETE)
    {
      cr->runs  = g_array_new (FALSE, FALSE, sizeof (W42SavedRun));
      cr->chars = g_array_new (FALSE, FALSE, sizeof (gunichar));
    }
  else if (type == CR_FMT)
    {
      cr->runs = g_array_new (FALSE, FALSE, sizeof (W42ApRun));
    }
  else if (type == CR_TABLE)
    {
      cr->runs = g_array_new (FALSE, FALSE, sizeof (int));
    }

  return cr;
}

static void
cr_free (W42CR *cr)
{
  if (cr == NULL)
    return;

  if (cr->runs != NULL)
    g_array_free (cr->runs, TRUE);
  if (cr->chars != NULL)
    g_array_free (cr->chars, TRUE);

  g_free (cr);
}

/* ---------------------------------------------------------------------- */
/* Piece list primitives                                                   */
/* ---------------------------------------------------------------------- */

static const gunichar *
pt_buffer (W42PieceTable *pt, gboolean in_change)
{
  GArray *array = in_change ? pt->change : pt->initial;
  return (const gunichar *) array->data;
}

static W42Piece *
pt_find (W42PieceTable *pt, gsize pos, gsize *offset)
{
  gsize start = 0;
  W42Piece *p = pt->head;

  /* Start from the cached piece when the position is at or after it,
   * or walk back from it when it is not far before. */
  if (pt->find_cache != NULL)
    {
      if (pos >= pt->find_cache_start)
        {
          p = pt->find_cache;
          start = pt->find_cache_start;
        }
      else
        {
          W42Piece *q = pt->find_cache;
          gsize qstart = pt->find_cache_start;

          while (q->prev != NULL && qstart > pos)
            {
              q = q->prev;
              qstart -= q->length;
            }
          if (qstart <= pos)
            {
              p = q;
              start = qstart;
            }
        }
    }

  for (; p != NULL; p = p->next)
    {
      if (pos < start + p->length)
        {
          *offset = pos - start;
          pt->find_cache = p;
          pt->find_cache_start = start;
          return p;
        }
      start += p->length;
    }

  *offset = 0;
  return NULL;
}

#define PT_RESHAPED(pt) ((pt)->find_cache = NULL)

static void
pt_link_before (W42PieceTable *pt, W42Piece *ref, W42Piece *piece)
{
  PT_RESHAPED (pt);
  if (ref == NULL)
    {
      piece->prev = pt->tail;
      piece->next = NULL;
      if (pt->tail != NULL)
        pt->tail->next = piece;
      else
        pt->head = piece;
      pt->tail = piece;
    }
  else
    {
      piece->prev = ref->prev;
      piece->next = ref;
      if (ref->prev != NULL)
        ref->prev->next = piece;
      else
        pt->head = piece;
      ref->prev = piece;
    }

  pt->length += piece->length;
}

static void
pt_unlink (W42PieceTable *pt, W42Piece *piece)
{
  PT_RESHAPED (pt);
  if (piece->prev != NULL)
    piece->prev->next = piece->next;
  else
    pt->head = piece->next;

  if (piece->next != NULL)
    piece->next->prev = piece->prev;
  else
    pt->tail = piece->prev;

  pt->length -= piece->length;
  g_free (piece);
}

/* Guarantees a piece boundary at pos and returns the piece that starts there,
 * or NULL when pos is the end of the document. */
static W42Piece *
pt_split_at (W42PieceTable *pt, gsize pos)
{
  PT_RESHAPED (pt);
  W42Piece *piece, *tail;
  gsize offset = 0;

  if (pos >= pt->length)
    return NULL;

  piece = pt_find (pt, pos, &offset);
  if (piece == NULL || offset == 0)
    return piece;

  /* Only text pieces are longer than one position, so only they can split. */
  g_assert (piece->type == W42_PIECE_TEXT);

  tail = g_new0 (W42Piece, 1);
  tail->type      = piece->type;
  tail->strux     = piece->strux;
  tail->in_change = piece->in_change;
  tail->ap        = piece->ap;
  tail->offset    = piece->offset + offset;
  tail->length    = piece->length - offset;

  piece->length = offset;

  tail->prev = piece;
  tail->next = piece->next;
  if (piece->next != NULL)
    piece->next->prev = tail;
  else
    pt->tail = tail;
  piece->next = tail;

  return tail;
}

/* Fold neighbouring text pieces that came from the same run of the same
 * buffer with the same formatting back into one.  Without this, editing in
 * the middle of a paragraph would fragment it without bound. */
static void
pt_coalesce (W42PieceTable *pt)
{
  PT_RESHAPED (pt);
  W42Piece *p = pt->head;

  while (p != NULL && p->next != NULL)
    {
      W42Piece *n = p->next;

      if (p->type == W42_PIECE_TEXT && n->type == W42_PIECE_TEXT &&
          p->ap == n->ap && p->in_change == n->in_change &&
          p->offset + p->length == n->offset)
        {
          p->length += n->length;
          /* pt_unlink() subtracts n->length from the document total, but the
           * characters have just moved into p, so put it back. */
          pt->length += n->length;
          pt_unlink (pt, n);
          continue;
        }

      p = n;
    }
}

static void
pt_clear_pieces (W42PieceTable *pt)
{
  PT_RESHAPED (pt);
  W42Piece *p = pt->head;

  while (p != NULL)
    {
      W42Piece *next = p->next;
      g_free (p);
      p = next;
    }

  pt->head = pt->tail = NULL;
  pt->length = 0;
}

/* ---------------------------------------------------------------------- */
/* Primitive operations, each returning the record that reverses it        */
/* ---------------------------------------------------------------------- */

static W42CR *
pt_do_delete (W42PieceTable *pt, gsize pos, gsize n)
{
  W42CR *inverse;
  W42Piece *p;
  gsize remaining;

  if (n == 0 || pos >= pt->length)
    return NULL;

  n = MIN (n, pt->length - pos);
  remaining = n;

  pt_split_at (pt, pos + n);
  p = pt_split_at (pt, pos);

  inverse = cr_new (CR_DELETE, pos, n);

  while (remaining > 0 && p != NULL)
    {
      W42Piece *next = p->next;
      W42SavedRun run;

      run.type      = p->type;
      run.strux     = p->strux;
      run.ap        = p->ap;
      /* An object piece's picture and a strux's payload both live in the
       * piece's offset, and both have to come back with the piece. */
      run.object    = (p->type == W42_PIECE_TEXT) ? W42_OBJECT_NONE
                                                  : (W42ObjectIdx) p->offset;
      run.n         = p->length;
      run.chars_off = 0;

      if (p->type == W42_PIECE_TEXT)
        {
          run.chars_off = inverse->chars->len;
          g_array_append_vals (inverse->chars,
                               pt_buffer (pt, p->in_change) + p->offset,
                               p->length);
        }

      g_array_append_val (inverse->runs, run);

      g_assert (remaining >= p->length);
      remaining -= p->length;
      pt_unlink (pt, p);
      p = next;
    }

  g_assert (remaining == 0);
  return inverse;
}

static W42CR *
pt_do_insert_runs (W42PieceTable *pt, gsize pos, GArray *runs, GArray *chars)
{
  W42Piece *ref = pt_split_at (pt, pos);
  gsize total = 0;

  for (guint i = 0; i < runs->len; i++)
    {
      const W42SavedRun *run = &g_array_index (runs, W42SavedRun, i);
      W42Piece *piece = g_new0 (W42Piece, 1);

      piece->type   = run->type;
      piece->strux  = run->strux;
      piece->ap     = run->ap;
      piece->length = run->n;

      if (run->type != W42_PIECE_TEXT)
        piece->offset = run->object;

      if (run->type == W42_PIECE_TEXT)
        {
          /* Reinstated text always lands in the change buffer: the initial
           * buffer is never appended to after load. */
          piece->in_change = 1;
          piece->offset    = pt->change->len;
          g_array_append_vals (pt->change,
                               &g_array_index (chars, gunichar, run->chars_off),
                               run->n);
        }

      pt_link_before (pt, ref, piece);
      total += run->n;
    }

  return cr_new (CR_INSERT, pos, total);
}

static W42CR *
pt_do_set_aps (W42PieceTable *pt, gsize pos, gsize len, GArray *ap_runs)
{
  W42CR *inverse = cr_new (CR_FMT, pos, len);
  gsize p = pos;

  for (guint i = 0; i < ap_runs->len; i++)
    {
      const W42ApRun *want = &g_array_index (ap_runs, W42ApRun, i);
      gsize end = p + want->n;
      gsize done = 0;
      W42Piece *piece;

      pt_split_at (pt, end);
      piece = pt_split_at (pt, p);

      while (done < want->n && piece != NULL)
        {
          W42ApRun old = { piece->length, piece->ap };

          g_array_append_val (inverse->runs, old);
          piece->ap = want->ap;

          done += piece->length;
          piece = piece->next;
        }

      p = end;
    }

  return inverse;
}

static void pt_table_renumber (W42PieceTable *pt, int table);

/* A CR_TABLE record holds the whole of a table's properties as ints:
 * n_cols, borders, header_rows, the number of row heights, then the
 * widths and the row heights.  Applying one sets them all and returns
 * the record that puts back what was there. */
static void
table_snapshot (const W42TableProps *props, GArray *out)
{
  int n_rows = props->row_heights != NULL ? (int) props->row_heights->len : 0;
  int v;

  g_array_set_size (out, 0);
  v = props->n_cols;      g_array_append_val (out, v);
  v = props->borders;     g_array_append_val (out, v);
  v = props->header_rows; g_array_append_val (out, v);
  v = n_rows;             g_array_append_val (out, v);
  for (int c = 0; c < props->n_cols; c++)
    {
      v = c < (int) props->widths->len ? g_array_index (props->widths, int, c) : 0;
      g_array_append_val (out, v);
    }
  for (int r = 0; r < n_rows; r++)
    {
      v = g_array_index (props->row_heights, int, r);
      g_array_append_val (out, v);
    }
}

static W42CR *
pt_do_set_table (W42PieceTable *pt, int table, GArray *snap)
{
  W42TableProps *props;
  W42CR *inverse;
  int n_cols, n_rows;

  if (table < 0 || (guint) table >= pt->tables->len || snap->len < 4)
    return NULL;

  props = g_ptr_array_index (pt->tables, table);
  inverse = cr_new (CR_TABLE, (gsize) table, 0);
  table_snapshot (props, inverse->runs);

  n_cols = CLAMP (g_array_index (snap, int, 0), 1, 1023);
  n_rows = MAX (g_array_index (snap, int, 3), 0);
  props->n_cols = n_cols;
  props->borders = g_array_index (snap, int, 1) ? 1 : 0;
  props->header_rows = (guint8) CLAMP (g_array_index (snap, int, 2), 0, 255);
  g_array_set_size (props->widths, 0);
  for (int c = 0; c < n_cols; c++)
    {
      int w = 4 + c < (int) snap->len ? g_array_index (snap, int, 4 + c) : 0;
      g_array_append_val (props->widths, w);
    }
  if (props->row_heights == NULL)
    props->row_heights = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_set_size (props->row_heights, 0);
  for (int r = 0; r < n_rows; r++)
    {
      int h = 4 + n_cols + r < (int) snap->len ? g_array_index (snap, int, 4 + n_cols + r) : 0;
      g_array_append_val (props->row_heights, h);
    }
  return inverse;
}

/* Gives table `table` the column widths in `widths`, the rest of its
 * properties as they are, and returns the record that would give it
 * back the ones it had.  The widths given are the columns there are
 * now: a column added or taken out changes the count, and undoing it
 * changes it back. */
static W42CR *
pt_do_set_widths (W42PieceTable *pt, int table, GArray *widths)
{
  W42TableProps *props;
  GArray *snap;
  W42CR *inverse;

  if (table < 0 || (guint) table >= pt->tables->len)
    return NULL;
  props = g_ptr_array_index (pt->tables, table);
  snap = g_array_new (FALSE, FALSE, sizeof (int));
  table_snapshot (props, snap);
  if (widths->len > 0)
    {
      int n = (int) widths->len;
      int n_rows = g_array_index (snap, int, 3);

      g_array_index (snap, int, 0) = n;
      g_array_remove_range (snap, 4, snap->len - 4);
      for (int c = 0; c < n; c++)
        g_array_append_val (snap, g_array_index (widths, int, c));
      for (int r = 0; r < n_rows; r++)
        g_array_append_val (snap, g_array_index (props->row_heights, int, r));
    }
  inverse = pt_do_set_table (pt, table, snap);
  g_array_free (snap, TRUE);
  return inverse;
}

static W42CR *
pt_apply (W42PieceTable *pt, W42CR *cr)
{
  W42CR *inverse = NULL;

  switch (cr->type)
    {
    case CR_TABLE:
      inverse = pt_do_set_table (pt, (int) cr->pos, cr->runs);
      break;
    case CR_INSERT:
      inverse = pt_do_delete (pt, cr->pos, cr->len);
      break;
    case CR_DELETE:
      inverse = pt_do_insert_runs (pt, cr->pos, cr->runs, cr->chars);
      break;
    case CR_FMT:
      inverse = pt_do_set_aps (pt, cr->pos, cr->len, cr->runs);
      break;
    default:
      g_assert_not_reached ();
    }

  if (inverse != NULL)
    inverse->group = cr->group;

  pt_coalesce (pt);

  /* Rows that came or went by undo need their cells renumbered, the same
   * as when they came or went the first time. */
  for (guint t = 0; t < pt->tables->len; t++)
    pt_table_renumber (pt, (int) t);

  return inverse;
}

/* ---------------------------------------------------------------------- */
/* Undo stack                                                              */
/* ---------------------------------------------------------------------- */

static void
pt_drop_redo (W42PieceTable *pt)
{
  if (pt->records->len > pt->undo_pos)
    pt->edit_serial++;      /* the history from here on is a different one */
  while (pt->records->len > pt->undo_pos)
    {
      W42CR *cr = g_ptr_array_index (pt->records, pt->records->len - 1);

      g_ptr_array_remove_index (pt->records, pt->records->len - 1);
      cr_free (cr);
    }
}

static void
pt_push (W42PieceTable *pt, W42CR *cr)
{
  if (cr == NULL)
    {
      /* A change with no record: undo cannot take it back, so the
       * document is a new one whatever the undo position says. */
      pt->edit_serial++;
      return;
    }

  pt_drop_redo (pt);

  if (pt->group_depth > 0)
    cr->group = pt->group_id;
  else
    cr->group = ++pt->next_group;

  g_ptr_array_add (pt->records, cr);
  pt->undo_pos = pt->records->len;
}

/* Merge a fresh insertion into the insertion that precedes it, so that a
 * typed word undoes as a word rather than one letter at a time. */
static gboolean
pt_try_coalesce_insert (W42PieceTable *pt, gsize pos, gsize len)
{
  W42CR *last;

  if (!pt->coalescing || pt->group_depth > 0 || pt->undo_pos == 0 ||
      pt->undo_pos != pt->records->len)
    return FALSE;

  last = g_ptr_array_index (pt->records, pt->undo_pos - 1);

  if (last->type != CR_INSERT || last->pos + last->len != pos)
    return FALSE;

  last->len += len;
  return TRUE;                      /* w42_pt_undo_state reports the new extent */
}

void
w42_pt_break_undo_coalesce (W42PieceTable *pt)
{
  g_return_if_fail (pt != NULL);
  pt->coalescing = FALSE;
}

void
w42_pt_begin_group (W42PieceTable *pt)
{
  g_return_if_fail (pt != NULL);

  if (pt->group_depth == 0)
    pt->group_id = ++pt->next_group;

  pt->group_depth++;
  pt->coalescing = FALSE;
}

void
w42_pt_end_group (W42PieceTable *pt)
{
  g_return_if_fail (pt != NULL);

  if (pt->group_depth > 0)
    pt->group_depth--;
  if (pt->group_depth == 0)
    pt->coalescing = FALSE;         /* typing after a grouped edit is its own step */
}

gboolean
w42_pt_can_undo (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, FALSE);
  return pt->undo_pos > 0;
}

/* Which document this is, as far as the undo history can say: the
 * position in the history, the number of times the history has been
 * rewritten under that position, and how far the record the position
 * stands on now reaches -- a run of typing grows that one record rather
 * than adding another.  Undoing back to a state reported earlier gives
 * all three back, and nothing else does. */
void
w42_pt_undo_state (W42PieceTable *pt, gsize *undo_pos, guint64 *serial)
{
  g_return_if_fail (pt != NULL);
  if (undo_pos != NULL) *undo_pos = pt->undo_pos;
  if (serial != NULL)
    {
      guint64 tail = 0;

      if (pt->undo_pos > 0 && pt->undo_pos <= pt->records->len)
        {
          const W42CR *cr = g_ptr_array_index (pt->records, pt->undo_pos - 1);

          tail = (guint64) cr->len;
        }
      *serial = (pt->edit_serial << 32) | (tail & 0xFFFFFFFFu);
    }
}

gboolean
w42_pt_can_redo (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, FALSE);
  return pt->undo_pos < pt->records->len;
}

/* The record handed in describes what was just done to the document.  An
 * insertion leaves the caret after the new content; a deletion leaves it
 * where the content used to be. */
static gsize
cr_caret_after (const W42CR *done)
{
  if (done->type == CR_INSERT)
    return done->pos + done->len;
  if (done->type == CR_TABLE)
    return (gsize) -1;              /* pos is a table id, not a place */
  return done->pos;
}

gsize
w42_pt_undo (W42PieceTable *pt)
{
  gsize caret = (gsize) -1;
  guint32 group;

  g_return_val_if_fail (pt != NULL, caret);

  if (pt->undo_pos == 0)
    return caret;

  pt->coalescing = FALSE;
  group = ((W42CR *) g_ptr_array_index (pt->records, pt->undo_pos - 1))->group;

  while (pt->undo_pos > 0)
    {
      W42CR *cr = g_ptr_array_index (pt->records, pt->undo_pos - 1);
      W42CR *inverse;

      if (cr->group != group)
        break;

      inverse = pt_apply (pt, cr);
      if (inverse == NULL)
        break;

      caret = cr_caret_after (inverse);
      g_ptr_array_index (pt->records, pt->undo_pos - 1) = inverse;
      cr_free (cr);
      pt->undo_pos--;
    }

  return caret;
}

gsize
w42_pt_redo (W42PieceTable *pt)
{
  gsize caret = (gsize) -1;
  guint32 group;

  g_return_val_if_fail (pt != NULL, caret);

  if (pt->undo_pos >= pt->records->len)
    return caret;

  pt->coalescing = FALSE;
  group = ((W42CR *) g_ptr_array_index (pt->records, pt->undo_pos))->group;

  while (pt->undo_pos < pt->records->len)
    {
      W42CR *cr = g_ptr_array_index (pt->records, pt->undo_pos);
      W42CR *inverse;

      if (cr->group != group)
        break;

      inverse = pt_apply (pt, cr);
      if (inverse == NULL)
        break;

      caret = cr_caret_after (inverse);
      g_ptr_array_index (pt->records, pt->undo_pos) = inverse;
      cr_free (cr);
      pt->undo_pos++;
    }

  return caret;
}

void
w42_pt_clear_undo (W42PieceTable *pt)
{
  g_return_if_fail (pt != NULL);

  for (guint i = 0; i < pt->records->len; i++)
    cr_free (g_ptr_array_index (pt->records, i));

  g_ptr_array_set_size (pt->records, 0);
  pt->undo_pos    = 0;
  pt->coalescing  = FALSE;
  pt->group_depth = 0;
}

/* ---------------------------------------------------------------------- */
/* Lifecycle                                                               */
/* ---------------------------------------------------------------------- */

static W42Piece *
pt_append_strux (W42PieceTable *pt, W42StruxType strux, W42ApIdx ap)
{
  W42Piece *piece = g_new0 (W42Piece, 1);

  piece->type   = W42_PIECE_STRUX;
  piece->strux  = strux;
  piece->ap     = ap;
  piece->length = 1;

  pt_link_before (pt, NULL, piece);
  return piece;
}

W42PieceTable *
w42_pt_new (void)
{
  W42PieceTable *pt = g_new0 (W42PieceTable, 1);
  W42ApIdx ap;

  pt->initial = g_array_new (FALSE, FALSE, sizeof (gunichar));
  pt->change  = g_array_new (FALSE, FALSE, sizeof (gunichar));
  pt->aps     = w42_ap_table_new ();
  pt->objects = w42_object_table_new ();
  pt->styles  = w42_stylesheet_new ();
  pt->tables  = g_ptr_array_new_with_free_func ((GDestroyNotify) table_props_free);
  pt->records = g_ptr_array_new ();

  ap = w42_ap_table_default (pt->aps);
  pt_append_strux (pt, W42_STRUX_SECTION, ap);
  pt_append_strux (pt, W42_STRUX_BLOCK, ap);

  return pt;
}

void
w42_pt_free (W42PieceTable *pt)
{
  if (pt == NULL)
    return;

  w42_pt_clear_undo (pt);
  g_ptr_array_free (pt->records, TRUE);
  pt_clear_pieces (pt);
  w42_ap_table_free (pt->aps);
  w42_object_table_free (pt->objects);
  w42_stylesheet_free (pt->styles);
  g_ptr_array_free (pt->tables, TRUE);
  g_free (pt->header.text);
  g_free (pt->footer.text);
  g_array_free (pt->change, TRUE);
  g_array_free (pt->initial, TRUE);
  g_free (pt);
}

void
w42_pt_load_text (W42PieceTable *pt, const char *utf8)
{
  W42ApIdx ap;
  const char *p;
  gsize offset = 0;

  g_return_if_fail (pt != NULL);

  w42_pt_clear_undo (pt);
  pt->next_note = 0;                /* a fresh document numbers its notes from 0 */
  pt_clear_pieces (pt);
  g_array_set_size (pt->initial, 0);
  g_array_set_size (pt->change, 0);
  g_clear_pointer (&pt->header.text, g_free);
  g_clear_pointer (&pt->footer.text, g_free);

  ap = w42_ap_table_default (pt->aps);
  pt_append_strux (pt, W42_STRUX_SECTION, ap);

  if (utf8 == NULL)
    {
      pt_append_strux (pt, W42_STRUX_BLOCK, ap);
      return;
    }

  /* One pass: copy the file into the immutable initial buffer as UCS-4 and
   * remember where each paragraph starts and ends. */
  pt_append_strux (pt, W42_STRUX_BLOCK, ap);

  for (p = utf8; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == '\r')
        {
          const char *next = g_utf8_next_char (p);
          if (*next == '\n')
            p = next;
          c = '\n';
        }

      if (c == '\n')
        {
          gsize len = pt->initial->len - offset;

          if (len > 0)
            {
              W42Piece *piece = g_new0 (W42Piece, 1);
              piece->type   = W42_PIECE_TEXT;
              piece->ap     = ap;
              piece->offset = offset;
              piece->length = len;
              pt_link_before (pt, NULL, piece);
            }

          pt_append_strux (pt, W42_STRUX_BLOCK, ap);
          offset = pt->initial->len;
          continue;
        }

      g_array_append_val (pt->initial, c);
    }

  if (pt->initial->len > offset)
    {
      W42Piece *piece = g_new0 (W42Piece, 1);
      piece->type   = W42_PIECE_TEXT;
      piece->ap     = ap;
      piece->offset = offset;
      piece->length = pt->initial->len - offset;
      pt_link_before (pt, NULL, piece);
    }
}

W42ApTable *
w42_pt_ap_table (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return pt->aps;
}

W42ObjectTable *
w42_pt_object_table (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return pt->objects;
}

W42StyleSheet *
w42_pt_stylesheet (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return pt->styles;
}

const W42PageText *
w42_pt_get_header (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return &pt->header;
}

const W42PageText *
w42_pt_get_footer (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return &pt->footer;
}

static void
page_text_set (W42PageText *slot, const char *text, W42Align align)
{
  g_free (slot->text);
  slot->text = (text != NULL && *text != '\0') ? g_strdup (text) : NULL;
  slot->align = align;
}

void
w42_pt_set_header (W42PieceTable *pt, const char *text, W42Align align)
{
  g_return_if_fail (pt != NULL);
  page_text_set (&pt->header, text, align);
}

void
w42_pt_set_footer (W42PieceTable *pt, const char *text, W42Align align)
{
  g_return_if_fail (pt != NULL);
  page_text_set (&pt->footer, text, align);
}

gsize
w42_pt_length (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, 0);
  return pt->length;
}

/* ---------------------------------------------------------------------- */
/* Queries                                                                 */
/* ---------------------------------------------------------------------- */

/* The caret sits between things, so position p is legal when the thing just
 * before it is text or a paragraph mark.  That rules out the slot between a
 * section mark and the paragraph mark that follows it. */
gboolean
w42_pt_is_caret_pos (W42PieceTable *pt, gsize pos)
{
  W42Piece *piece;
  gsize offset = 0;

  g_return_val_if_fail (pt != NULL, FALSE);

  if (pos == 0 || pos > pt->length)
    return FALSE;

  piece = pt_find (pt, pos - 1, &offset);
  if (piece == NULL)
    return FALSE;

  return piece->type == W42_PIECE_TEXT ||
         piece->type == W42_PIECE_OBJECT ||
         (piece->type == W42_PIECE_STRUX &&
          ((W42StruxType) piece->strux == W42_STRUX_BLOCK ||
           (W42StruxType) piece->strux == W42_STRUX_FOOTNOTE));
}

gsize
w42_pt_first_caret_pos (W42PieceTable *pt)
{
  gsize start = 0;

  g_return_val_if_fail (pt != NULL, 0);

  for (W42Piece *p = pt->head; p != NULL; p = p->next)
    {
      if (p->type == W42_PIECE_STRUX &&
          (W42StruxType) p->strux == W42_STRUX_BLOCK)
        return start + 1;
      start += p->length;
    }

  return pt->length;
}

gsize
w42_pt_clamp_pos (W42PieceTable *pt, gsize pos)
{
  gsize first;

  g_return_val_if_fail (pt != NULL, 0);

  first = w42_pt_first_caret_pos (pt);

  if (pos < first)
    return first;
  if (pos > pt->length)
    return pt->length;
  if (w42_pt_is_caret_pos (pt, pos))
    return pos;

  return w42_pt_next_pos (pt, pos);
}

gsize
w42_pt_next_pos (W42PieceTable *pt, gsize pos)
{
  g_return_val_if_fail (pt != NULL, 0);

  for (gsize p = pos + 1; p <= pt->length; p++)
    {
      if (w42_pt_is_caret_pos (pt, p))
        return p;
    }

  return w42_pt_clamp_pos (pt, pos > pt->length ? pt->length : pos);
}

gsize
w42_pt_prev_pos (W42PieceTable *pt, gsize pos)
{
  gsize first;

  g_return_val_if_fail (pt != NULL, 0);

  first = w42_pt_first_caret_pos (pt);

  for (gsize p = pos; p > first; p--)
    {
      if (w42_pt_is_caret_pos (pt, p - 1))
        return p - 1;
    }

  return first;
}

W42ApIdx
w42_pt_ap_at (W42PieceTable *pt, gsize pos)
{
  W42Piece *piece;
  gsize offset = 0;

  g_return_val_if_fail (pt != NULL, 0);

  /* Prefer the character to the left, which is what Word does when you put
   * the caret down and start typing. */
  if (pos > 0)
    {
      piece = pt_find (pt, pos - 1, &offset);
      if (piece != NULL && piece->type != W42_PIECE_STRUX)
        return piece->ap;
    }

  piece = pt_find (pt, pos, &offset);
  if (piece != NULL && piece->type != W42_PIECE_STRUX)
    return piece->ap;

  return w42_pt_block_ap_at (pt, pos);
}

W42ApIdx
w42_pt_block_ap_at (W42PieceTable *pt, gsize pos)
{
  W42Piece *piece;
  gsize offset = 0;

  g_return_val_if_fail (pt != NULL, 0);

  if (pos >= pt->length)
    piece = pt->tail;
  else
    piece = pt_find (pt, pos, &offset);

  for (; piece != NULL; piece = piece->prev)
    {
      if (piece->type == W42_PIECE_STRUX &&
          (W42StruxType) piece->strux == W42_STRUX_BLOCK)
        return piece->ap;
    }

  return w42_ap_table_default (pt->aps);
}

/* ---------------------------------------------------------------------- */
/* Block snapshots                                                         */
/* ---------------------------------------------------------------------- */

void
w42_block_free (W42Block *block)
{
  if (block == NULL)
    return;

  if (block->text != NULL)
    g_string_free (block->text, TRUE);
  if (block->runs != NULL)
    g_array_free (block->runs, TRUE);

  g_free (block);
}

static W42Block *
block_new (gsize start_pos, W42ApIdx ap)
{
  W42Block *block = g_new0 (W42Block, 1);

  block->start_pos = start_pos;
  block->ap        = ap;
  block->text      = g_string_new (NULL);
  block->runs      = g_array_new (FALSE, FALSE, sizeof (W42Run));
  block->table     = -1;
  block->note      = -1;

  return block;
}

/* What one stretch of text comes to.  A word is a run of anything that
 * is not white space; the object mark a picture or a note reference
 * stands in for is not a character at all. */
static void
count_text (const char *text, gsize len, W42Stats *out)
{
  const char *stop = text + len;
  gboolean in_word = FALSE;

  for (const char *p = text; p < stop; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == 0xFFFC)
        {
          in_word = FALSE;      /* a picture: not a character, not a word */
          continue;
        }
      out->characters++;
      if (g_unichar_isspace (c))
        {
          in_word = FALSE;
        }
      else
        {
          out->characters_no_spaces++;
          if (!in_word)
            {
              in_word = TRUE;
              out->words++;
            }
        }
    }
}

void
w42_pt_statistics (W42PieceTable *pt, gboolean with_notes, W42Stats *out)
{
  GPtrArray *blocks;

  g_return_if_fail (pt != NULL);
  g_return_if_fail (out != NULL);

  memset (out, 0, sizeof *out);
  blocks = w42_pt_snapshot_blocks (pt);
  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);

      if (block->note >= 0 && !with_notes)
        continue;
      out->paragraphs++;
      count_text (block->text->str, block->text->len, out);
    }
  g_ptr_array_free (blocks, TRUE);
}

void
w42_pt_statistics_range (W42PieceTable *pt, gsize start, gsize end, W42Stats *out)
{
  GPtrArray *blocks;

  g_return_if_fail (pt != NULL);
  g_return_if_fail (out != NULL);

  memset (out, 0, sizeof *out);
  if (end <= start)
    return;

  blocks = w42_pt_snapshot_blocks (pt);
  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      gsize first = block->start_pos + 1;          /* the text after the mark */
      gsize n_chars = g_utf8_strlen (block->text->str, (gssize) block->text->len);
      gsize from, to;
      const char *p, *q;

      if (first + n_chars <= start || first >= end)
        continue;

      from = start > first ? start - first : 0;
      to = end < first + n_chars ? end - first : n_chars;
      if (to <= from)
        continue;

      p = g_utf8_offset_to_pointer (block->text->str, (glong) from);
      q = g_utf8_offset_to_pointer (block->text->str, (glong) to);
      out->paragraphs++;
      count_text (p, (gsize) (q - p), out);
    }
  g_ptr_array_free (blocks, TRUE);
}

GPtrArray *
w42_pt_snapshot_blocks (W42PieceTable *pt)
{
  GPtrArray *blocks;
  W42Block *current = NULL;
  gsize pos = 0;

  g_return_val_if_fail (pt != NULL, NULL);

  blocks = g_ptr_array_new_with_free_func ((GDestroyNotify) w42_block_free);
  /* `numbers` is freed where the blocks are returned. */

  int table = -1, row = 0, col = 0, span = 1;
  W42ApIdx cell_ap = w42_ap_table_default (pt->aps);
  int note = -1;
  GArray *numbers = g_array_new (FALSE, TRUE, sizeof (int));   /* by id */
  GArray *ends = g_array_new (FALSE, TRUE, sizeof (gboolean)); /* by id */
  int next_number = 0, next_end = 0;

  /* Footnotes are numbered by the order of their reference marks, and
   * endnotes likewise, each from one. */
  for (W42Piece *p = pt->head; p != NULL; p = p->next)
    if (piece_is_strux (p, W42_STRUX_FOOTNOTE))
      {
        guint id = (guint) NOTE_ID (p->offset);
        gboolean end = NOTE_IS_END (p->offset);

        if (id >= numbers->len)
          {
            g_array_set_size (numbers, id + 1);
            g_array_set_size (ends, id + 1);
          }
        g_array_index (numbers, int, id) = end ? ++next_end : ++next_number;
        g_array_index (ends, gboolean, id) = end;
      }

  for (W42Piece *p = pt->head; p != NULL; p = p->next)
    {
      if (p->type == W42_PIECE_STRUX && (W42StruxType) p->strux == W42_STRUX_FOOTNOTE)
        {
          /* The reference mark: one U+FFFC in the paragraph, drawn as the
           * note's number. */
          if (current != NULL)
            {
              W42Run run;
              guint id = (guint) NOTE_ID (p->offset);

              run.doc_pos     = pos;
              run.byte_offset = current->text->len;
              run.n_bytes     = 3;
              run.n_chars     = 1;
              run.ap          = p->ap;
              run.object      = W42_OBJECT_NONE;
              run.footnote    = id < numbers->len ? g_array_index (numbers, int, id) : 0;
              run.footnote_id = (int) id;
              run.endnote     = NOTE_IS_END (p->offset);

              g_string_append (current->text, "\357\277\274");
              g_array_append_val (current->runs, run);
            }
          pos += p->length;
          continue;
        }

      if (p->type == W42_PIECE_STRUX)
        {
          switch ((W42StruxType) p->strux)
            {
            case W42_STRUX_NOTES:
              table = -1;
              current = NULL;
              break;
            case W42_STRUX_NOTE:
              note = (int) p->offset;
              current = NULL;
              break;
            case W42_STRUX_TABLE:
              table = (int) p->offset;
              row = col = 0;
              span = 1;
              break;
            case W42_STRUX_CELL:
              row = CELL_ROW (p->offset);
              col = CELL_COL (p->offset);
              span = CELL_SPAN (p->offset);
              cell_ap = p->ap;
              break;
            case W42_STRUX_ENDTABLE:
              table = -1;
              break;
            case W42_STRUX_BLOCK:
              current = block_new (pos, p->ap);
              current->table = table;
              current->row = row;
              current->span = span;
              current->cell_ap = cell_ap;
              current->col = col;
              current->note = note;
              current->note_number =
                (note >= 0 && (guint) note < numbers->len)
                  ? g_array_index (numbers, int, note) : 0;
              current->note_end =
                (note >= 0 && (guint) note < ends->len)
                  ? g_array_index (ends, gboolean, note) : FALSE;
              g_ptr_array_add (blocks, current);
              break;
            default:
              break;
            }
          pos += p->length;
          continue;
        }

      if (current != NULL && p->type == W42_PIECE_OBJECT)
        {
          W42Run run;

          run.doc_pos     = pos;
          run.byte_offset = current->text->len;
          run.n_bytes     = 3;                 /* U+FFFC is three bytes */
          run.n_chars     = 1;
          run.ap          = p->ap;
          run.object      = (W42ObjectIdx) p->offset;
          run.footnote    = 0;
          run.footnote_id = -1;
          run.endnote     = FALSE;

          g_string_append (current->text, "\357\277\274");
          g_array_append_val (current->runs, run);
          pos += p->length;
          continue;
        }

      if (current != NULL)
        {
          const gunichar *buf = pt_buffer (pt, p->in_change) + p->offset;
          gsize byte_start = current->text->len;
          W42Run *last = NULL;

          for (gsize i = 0; i < p->length; i++)
            {
              char utf8[8];
              int n = g_unichar_to_utf8 (buf[i], utf8);
              g_string_append_len (current->text, utf8, n);
            }

          if (current->runs->len > 0)
            last = &g_array_index (current->runs, W42Run,
                                   current->runs->len - 1);

          if (last != NULL && last->ap == p->ap &&
              last->object == W42_OBJECT_NONE && last->footnote == 0 &&
              last->byte_offset + last->n_bytes == byte_start)
            {
              last->n_bytes += current->text->len - byte_start;
              last->n_chars += p->length;
            }
          else
            {
              W42Run run;
              run.doc_pos     = pos;
              run.byte_offset = byte_start;
              run.n_bytes     = current->text->len - byte_start;
              run.n_chars     = p->length;
              run.ap          = p->ap;
              run.object      = W42_OBJECT_NONE;
              run.footnote    = 0;
              run.footnote_id = -1;
              run.endnote     = FALSE;
              g_array_append_val (current->runs, run);
            }
        }

      pos += p->length;
    }

  g_array_free (numbers, TRUE);
  g_array_free (ends, TRUE);
  return blocks;
}

/* ---------------------------------------------------------------------- */
/* Mutation                                                                */
/* ---------------------------------------------------------------------- */

/* A run of text holds characters, not structure: a paragraph break is a
 * strux and a line break is U+2028.  A control character that reached a
 * run would be invisible on the page and yet be saved with the document,
 * so a newline becomes the line break it means and the rest go.  Returns
 * how many characters are left. */
static glong
run_chars_only (gunichar *ucs4, glong n_chars)
{
  glong out = 0;

  for (glong i = 0; i < n_chars; i++)
    {
      gunichar c = ucs4[i];

      if (c == '\n')
        c = 0x2028;                       /* the line break the model uses */
      else if (c == '\r')
        {
          /* CR LF is one break, a lone CR is one too. */
          if (i + 1 < n_chars && ucs4[i + 1] == '\n')
            continue;
          c = 0x2028;
        }
      else if (c < 0x20 && c != '\t')
        continue;
      else if (c == 0x7F)
        continue;

      ucs4[out++] = c;
    }
  return out;
}

void
w42_pt_insert_text (W42PieceTable *pt, gsize pos, const char *utf8, W42ApIdx ap)
{
  glong n_chars = 0;
  gunichar *ucs4;
  W42Piece *ref, *piece;

  g_return_if_fail (pt != NULL);
  g_return_if_fail (utf8 != NULL);

  if (*utf8 == '\0')
    return;

  pos = w42_pt_clamp_pos (pt, pos);

  ucs4 = g_utf8_to_ucs4_fast (utf8, -1, &n_chars);
  n_chars = run_chars_only (ucs4, n_chars);
  if (n_chars <= 0)
    {
      g_free (ucs4);
      return;
    }

  ref = pt_split_at (pt, pos);

  piece = g_new0 (W42Piece, 1);
  piece->type      = W42_PIECE_TEXT;
  piece->in_change = 1;
  piece->ap        = ap;
  piece->offset    = pt->change->len;
  piece->length    = (gsize) n_chars;

  g_array_append_vals (pt->change, ucs4, (guint) n_chars);
  g_free (ucs4);

  pt_link_before (pt, ref, piece);

  if (!pt_try_coalesce_insert (pt, pos, (gsize) n_chars))
    {
      pt_push (pt, cr_new (CR_INSERT, pos, (gsize) n_chars));
      pt->coalescing = TRUE;
    }

  pt_coalesce (pt);
}

void
w42_pt_insert_block (W42PieceTable *pt, gsize pos, W42ApIdx ap)
{
  W42Piece *ref, *piece;

  g_return_if_fail (pt != NULL);

  pos = w42_pt_clamp_pos (pt, pos);
  ref = pt_split_at (pt, pos);

  piece = g_new0 (W42Piece, 1);
  piece->type   = W42_PIECE_STRUX;
  piece->strux  = W42_STRUX_BLOCK;
  piece->ap     = ap;
  piece->length = 1;

  pt_link_before (pt, ref, piece);

  pt->coalescing = FALSE;
  pt_push (pt, cr_new (CR_INSERT, pos, 1));
  pt_coalesce (pt);
}

void
w42_pt_insert_object (W42PieceTable *pt, gsize pos, W42ObjectIdx object, W42ApIdx ap)
{
  W42Piece *ref, *piece;

  g_return_if_fail (pt != NULL);
  g_return_if_fail (object != W42_OBJECT_NONE);

  pos = w42_pt_clamp_pos (pt, pos);
  ref = pt_split_at (pt, pos);

  piece = g_new0 (W42Piece, 1);
  piece->type   = W42_PIECE_OBJECT;
  piece->ap     = ap;
  piece->offset = object;
  piece->length = 1;

  pt_link_before (pt, ref, piece);

  pt->coalescing = FALSE;
  pt_push (pt, cr_new (CR_INSERT, pos, 1));
  pt_coalesce (pt);
}

W42ObjectIdx
w42_pt_object_at (W42PieceTable *pt, gsize pos)
{
  gsize offset = 0;
  W42Piece *piece;

  g_return_val_if_fail (pt != NULL, W42_OBJECT_NONE);

  piece = pt_find (pt, pos, &offset);
  if (piece == NULL || piece->type != W42_PIECE_OBJECT)
    return W42_OBJECT_NONE;

  return (W42ObjectIdx) piece->offset;
}

void
w42_pt_resize_object (W42PieceTable *pt, gsize pos, int width, int height)
{
  W42ObjectIdx old, fresh;
  const W42Object *object;
  W42ApIdx ap;

  g_return_if_fail (pt != NULL);

  old = w42_pt_object_at (pt, pos);
  if (old == W42_OBJECT_NONE)
    return;

  object = w42_object_table_get (pt->objects, old);
  if (object == NULL || (object->width == width && object->height == height))
    return;

  ap = w42_pt_ap_at (pt, pos);
  fresh = w42_object_table_add (pt->objects, object->data, object->format,
                                object->pixel_w, object->pixel_h,
                                MAX (width, 15), MAX (height, 15));
  w42_object_table_set_wrap (pt->objects, fresh, object->wrap);

  w42_pt_begin_group (pt);
  w42_pt_delete (pt, pos, 1);
  w42_pt_insert_object (pt, pos, fresh, ap);
  w42_pt_end_group (pt);
}

void
w42_pt_set_object_wrap (W42PieceTable *pt, gsize pos, W42Wrap wrap)
{
  W42ObjectIdx old, fresh;
  const W42Object *object;
  W42ApIdx ap;

  g_return_if_fail (pt != NULL);

  old = w42_pt_object_at (pt, pos);
  if (old == W42_OBJECT_NONE)
    return;

  object = w42_object_table_get (pt->objects, old);
  if (object == NULL || object->wrap == wrap)
    return;

  /* A fresh object, so that undo brings the old one back. */
  ap = w42_pt_ap_at (pt, pos);
  fresh = w42_object_table_add (pt->objects, object->data, object->format,
                                object->pixel_w, object->pixel_h,
                                object->width, object->height);
  w42_object_table_set_wrap (pt->objects, fresh, wrap);

  w42_pt_begin_group (pt);
  w42_pt_delete (pt, pos, 1);
  w42_pt_insert_object (pt, pos, fresh, ap);
  w42_pt_end_group (pt);
}

/* The marks that hold a table together may only go when the whole table
 * goes.  A TABLE, CELL or ENDTABLE mark, and the paragraph mark that opens
 * a cell or that follows a table, are all protected unless the range covers
 * the table from its TABLE mark through its ENDTABLE mark. */
static gboolean
pt_position_protected (W42PieceTable *pt, gsize pos, gsize range_start, gsize range_end)
{
  gsize offset = 0;
  W42Piece *piece = pt_find (pt, pos, &offset);
  W42Piece *prev;
  gboolean structural;

  if (piece == NULL || piece->type != W42_PIECE_STRUX)
    return FALSE;

  prev = piece->prev;

  if (piece_is_strux (piece, W42_STRUX_NOTES) || piece_is_strux (piece, W42_STRUX_NOTE) ||
      (piece_is_strux (piece, W42_STRUX_BLOCK) && prev != NULL &&
       piece_is_strux (prev, W42_STRUX_NOTE)))
    return TRUE;

  structural = piece_is_strux (piece, W42_STRUX_TABLE) ||
               piece_is_strux (piece, W42_STRUX_CELL) ||
               piece_is_strux (piece, W42_STRUX_ENDTABLE) ||
               (piece_is_strux (piece, W42_STRUX_BLOCK) && prev != NULL &&
                (piece_is_strux (prev, W42_STRUX_CELL) ||
                 piece_is_strux (prev, W42_STRUX_ENDTABLE)));

  if (!structural)
    return FALSE;

  /* Find the table this mark belongs to and see whether the range takes
   * all of it. */
  {
    gsize start = 0, end = 0, p = 0;
    gboolean found = FALSE;

    for (W42Piece *q = pt->head; q != NULL; q = q->next)
      {
        if (piece_is_strux (q, W42_STRUX_TABLE))
          start = p;
        if (q == piece || (q == prev && piece_is_strux (piece, W42_STRUX_BLOCK)))
          found = TRUE;
        if (piece_is_strux (q, W42_STRUX_ENDTABLE) && found)
          {
            end = p + 1;              /* through the ENDTABLE mark */
            /* and the paragraph mark after it */
            if (q->next != NULL && piece_is_strux (q->next, W42_STRUX_BLOCK))
              end += 1;
            break;
          }
        p += q->length;
      }

    if (!found)
      return FALSE;

    return !(range_start <= start && range_end >= end);
  }
}

static void pt_delete_range (W42PieceTable *pt, gsize pos, gsize n);

/* The stretch of the notes section that is note `id`: its NOTE mark
 * through the position before the next NOTE mark or the end. */
static gboolean
pt_note_span (W42PieceTable *pt, int id, gsize *start, gsize *end)
{
  gsize p = 0;
  gboolean found = FALSE;

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (piece_is_strux (q, W42_STRUX_NOTE))
        {
          if (found)
            {
              *end = p;
              return TRUE;
            }
          if ((int) q->offset == id)
            {
              *start = p;
              found = TRUE;
            }
        }
      p += q->length;
    }

  if (found)
    *end = p;
  return found;
}

void
w42_pt_delete (W42PieceTable *pt, gsize pos, gsize n)
{
  GArray *ids = NULL;
  gsize end;

  g_return_if_fail (pt != NULL);

  if (n == 0)
    return;

  /* Reference marks in the range: their notes go too, as one step. */
  end = MIN (pos + n, pt->length);
  {
    gsize p = 0;

    for (W42Piece *q = pt->head; q != NULL && p < end; q = q->next)
      {
        if (piece_is_strux (q, W42_STRUX_FOOTNOTE) && p >= pos)
          {
            int id = NOTE_ID (q->offset);

            if (ids == NULL)
              ids = g_array_new (FALSE, FALSE, sizeof (int));
            g_array_append_val (ids, id);
          }
        p += q->length;
      }
  }

  if (ids == NULL)
    {
      pt_delete_range (pt, pos, n);
      return;
    }

  w42_pt_begin_group (pt);
  pt_delete_range (pt, pos, n);
  for (guint i = 0; i < ids->len; i++)
    {
      gsize start = 0, stop = 0;

      if (pt_note_span (pt, g_array_index (ids, int, i), &start, &stop) && stop > start)
        {
          pt->coalescing = FALSE;
          pt_push (pt, pt_do_delete (pt, start, stop - start));
        }
    }
  /* An empty notes section is no section. */
  if (pt->tail != NULL && piece_is_strux (pt->tail, W42_STRUX_NOTES))
    pt_push (pt, pt_do_delete (pt, pt->length - 1, 1));
  w42_pt_end_group (pt);
  pt_coalesce (pt);
  g_array_free (ids, TRUE);
}

static void
pt_delete_range (W42PieceTable *pt, gsize pos, gsize n)
{
  gsize first;
  W42CR *record;

  if (n == 0)
    return;

  /* Carve the range into the stretches that may be deleted, and delete
   * those from the back so the positions in front stay valid. */
  {
    gsize end = MIN (pos + n, pt->length);
    GArray *stretches = g_array_new (FALSE, FALSE, sizeof (gsize));
    gsize run_start = (gsize) -1;
    gboolean any_protected = FALSE;

    for (gsize p = pos; p < end; p++)
      {
        if (pt_position_protected (pt, p, pos, end))
          {
            any_protected = TRUE;
            if (run_start != (gsize) -1)
              {
                g_array_append_val (stretches, run_start);
                g_array_append_val (stretches, p);
                run_start = (gsize) -1;
              }
          }
        else if (run_start == (gsize) -1)
          run_start = p;
      }
    if (run_start != (gsize) -1)
      {
        g_array_append_val (stretches, run_start);
        g_array_append_val (stretches, end);
      }

    if (any_protected)
      {
        w42_pt_begin_group (pt);
        for (guint i = stretches->len; i >= 2; i -= 2)
          {
            gsize a = g_array_index (stretches, gsize, i - 2);
            gsize b = g_array_index (stretches, gsize, i - 1);
            pt_delete_range (pt, a, b - a);
          }
        w42_pt_end_group (pt);
        g_array_free (stretches, TRUE);
        return;
      }
    g_array_free (stretches, TRUE);
  }

  /* The opening section mark and the first paragraph mark are structural:
   * a document without them has nowhere to put the caret. */
  first = w42_pt_first_caret_pos (pt);
  if (pos < first)
    {
      if (pos + n <= first)
        return;
      n -= first - pos;
      pos = first;
    }

  if (pos >= pt->length)
    return;

  n = MIN (n, pt->length - pos);

  pt->coalescing = FALSE;
  record = pt_do_delete (pt, pos, n);
  pt_push (pt, record);
  pt_coalesce (pt);
}

static void
char_fmt_apply_mask (W42CharFmt *fmt, W42CharMask mask, const W42CharFmt *value)
{
  if (mask & W42_CHAR_FAMILY)    fmt->family    = value->family;
  if (mask & W42_CHAR_SIZE)      fmt->size      = value->size;
  if (mask & W42_CHAR_BOLD)      fmt->bold      = value->bold;
  if (mask & W42_CHAR_ITALIC)    fmt->italic    = value->italic;
  if (mask & W42_CHAR_UNDERLINE) fmt->underline = value->underline;
  if (mask & W42_CHAR_STRIKEOUT) fmt->strikeout = value->strikeout;
  if (mask & W42_CHAR_SCRIPT)    fmt->script    = value->script;
  if (mask & W42_CHAR_COLOR)     fmt->color     = value->color;
  if (mask & W42_CHAR_LINK)      fmt->link      = value->link;
  if (mask & W42_CHAR_SMALLCAPS) fmt->smallcaps = value->smallcaps;
  if (mask & W42_CHAR_ALLCAPS)   fmt->allcaps   = value->allcaps;
  if (mask & W42_CHAR_HIGHLIGHT) fmt->highlight = value->highlight;
  if (mask & W42_CHAR_SPACING)   fmt->spacing   = value->spacing;
  if (mask & W42_CHAR_COMMENT)   fmt->comment   = value->comment;
  if (mask & W42_CHAR_REVISION)  fmt->revision  = value->revision;
  if (mask & W42_CHAR_OVERLINE)  fmt->overline  = value->overline;
  if (mask & W42_CHAR_FIELD)     fmt->field     = value->field;
  if (mask & W42_CHAR_LANG)      fmt->lang      = value->lang;
  if (mask & W42_CHAR_BOOKMARK)  fmt->bookmark  = value->bookmark;
}

static void
para_fmt_apply_mask (W42ParaFmt *fmt, W42ParaMask mask, const W42ParaFmt *value)
{
  if (mask & W42_PARA_STYLE)        fmt->style        = value->style;
  if (mask & W42_PARA_ALIGN)
    {
      fmt->align = value->align;
      fmt->rtl   = value->rtl;
    }
  if (mask & W42_PARA_INDENT_LEFT)  fmt->indent_left  = value->indent_left;
  if (mask & W42_PARA_INDENT_RIGHT) fmt->indent_right = value->indent_right;
  if (mask & W42_PARA_INDENT_FIRST) fmt->indent_first = value->indent_first;
  if (mask & W42_PARA_SPACE_BEFORE) fmt->space_before = value->space_before;
  if (mask & W42_PARA_SPACE_AFTER)  fmt->space_after  = value->space_after;
  if (mask & W42_PARA_LINE_SPACING) fmt->line_spacing = value->line_spacing;
  if (mask & W42_PARA_LINE_SPACING_PCT) fmt->line_spacing_pct = value->line_spacing_pct;
  if (mask & W42_PARA_PAGE_BREAK)   fmt->page_break_before = value->page_break_before;
  if (mask & W42_PARA_LIST)
    {
      fmt->list = value->list;
      fmt->list_start = value->list_start;
      fmt->list_level = value->list_level;
    }
  if (mask & W42_PARA_BORDER)
    {
      fmt->border = value->border;
      fmt->border_width = value->border_width;
    }
  if (mask & W42_PARA_SHADING)      fmt->shading = value->shading;
  if (mask & W42_PARA_CELL_SPAN)    fmt->cell_vspan = value->cell_vspan;
  if (mask & W42_PARA_SECTION)
    {
      fmt->section_break = value->section_break;
      fmt->columns       = value->columns;
      fmt->column_gap    = value->column_gap;
    }
  if (mask & W42_PARA_FRAME)
    {
      fmt->drop_cap    = value->drop_cap;
      fmt->frame_side  = value->frame_side;
      fmt->frame_width = value->frame_width;
    }
  if (mask & W42_PARA_FLOW)
    {
      fmt->keep_next     = value->keep_next;
      fmt->keep_together = value->keep_together;
      fmt->widow_control = value->widow_control;
    }
  if (mask & W42_PARA_TABS)
    {
      fmt->n_tabs = value->n_tabs;
      memcpy (fmt->tab_pos, value->tab_pos, sizeof fmt->tab_pos);
      memcpy (fmt->tab_kind, value->tab_kind, sizeof fmt->tab_kind);
    }
}

/* Both formatting entry points work the same way: walk the range building the
 * list of APs it should end up with, then hand that list to the primitive
 * that swaps them in and hands back the old ones for undo.  Positions the
 * caller's mask does not apply to keep the AP they already had. */
typedef enum { FMT_CHAR, FMT_PARA } FmtKind;

static void
pt_apply_fmt (W42PieceTable *pt,
              gsize          pos,
              gsize          n,
              FmtKind        kind,
              guint          mask,
              gconstpointer  value)
{
  GArray *runs;
  gsize p = pos;
  gsize end;

  if (n == 0 || pos >= pt->length)
    return;

  end = MIN (pos + n, pt->length);
  runs = g_array_new (FALSE, FALSE, sizeof (W42ApRun));

  while (p < end)
    {
      gsize offset = 0;
      W42Piece *piece = pt_find (pt, p, &offset);
      gsize take;
      W42ApRun run;
      gboolean touched;

      if (piece == NULL)
        break;

      take = MIN (piece->length - offset, end - p);

      touched = (kind == FMT_CHAR)
                  ? (piece->type == W42_PIECE_TEXT)
                  : (piece->type == W42_PIECE_STRUX &&
                     (W42StruxType) piece->strux == W42_STRUX_BLOCK);

      run.n  = take;
      run.ap = piece->ap;

      if (touched)
        {
          W42Fmt fmt = *w42_ap_table_get (pt->aps, piece->ap);

          if (kind == FMT_CHAR)
            char_fmt_apply_mask (&fmt.ch, mask, value);
          else
            para_fmt_apply_mask (&fmt.pa, mask, value);

          run.ap = w42_ap_table_intern (pt->aps, &fmt);
        }

      g_array_append_val (runs, run);
      p += take;
    }

  if (runs->len > 0)
    {
      pt->coalescing = FALSE;
      pt_push (pt, pt_do_set_aps (pt, pos, end - pos, runs));
      pt_coalesce (pt);
    }

  g_array_free (runs, TRUE);
}

void
w42_pt_apply_char_fmt (W42PieceTable    *pt,
                       gsize             pos,
                       gsize             n,
                       W42CharMask       mask,
                       const W42CharFmt *value)
{
  g_return_if_fail (pt != NULL);
  g_return_if_fail (value != NULL);

  pt_apply_fmt (pt, pos, n, FMT_CHAR, mask, value);
}

void
w42_pt_apply_para_fmt (W42PieceTable    *pt,
                       gsize             pos,
                       gsize             n,
                       W42ParaMask       mask,
                       const W42ParaFmt *value)
{
  gsize block_pos;

  g_return_if_fail (pt != NULL);
  g_return_if_fail (value != NULL);

  /* Paragraph formatting lives on the paragraph mark, which sits before the
   * caret, so widen the range backwards to take it in. */
  block_pos = pos;
  while (block_pos > 0)
    {
      gsize offset = 0;
      W42Piece *piece = pt_find (pt, block_pos, &offset);

      if (piece != NULL && piece->type == W42_PIECE_STRUX &&
          (W42StruxType) piece->strux == W42_STRUX_BLOCK && offset == 0)
        break;

      block_pos--;
    }

  pt_apply_fmt (pt, block_pos, (pos - block_pos) + MAX (n, 1),
                FMT_PARA, mask, value);
}

/* ---------------------------------------------------------------------- */
/* Tables                                                                  */
/* ---------------------------------------------------------------------- */

static void
pt_insert_strux_at (W42PieceTable *pt, gsize pos, W42StruxType strux,
                    gsize payload, W42ApIdx ap)
{
  W42Piece *ref = pt_split_at (pt, pos);
  W42Piece *piece = g_new0 (W42Piece, 1);

  piece->type   = W42_PIECE_STRUX;
  piece->strux  = strux;
  piece->ap     = ap;
  piece->offset = payload;
  piece->length = 1;

  pt_link_before (pt, ref, piece);
}

int
w42_pt_insert_table_start (W42PieceTable *pt, gsize pos, int n_cols,
                           const int *widths)
{
  W42TableProps *props;
  int id;

  g_return_val_if_fail (pt != NULL, -1);
  g_return_val_if_fail (n_cols > 0, -1);
  n_cols = MIN (n_cols, 1023);         /* the CELL mark packs the column in 10 bits */

  props = g_new0 (W42TableProps, 1);

  props->borders = 1;
  props->n_cols = n_cols;
  props->row_heights = g_array_new (FALSE, TRUE, sizeof (int));
  props->widths = g_array_new (FALSE, TRUE, sizeof (int));
  g_array_set_size (props->widths, n_cols);
  if (widths != NULL)
    for (int c = 0; c < n_cols; c++)
      g_array_index (props->widths, int, c) = widths[c];

  g_ptr_array_add (pt->tables, props);
  id = (int) pt->tables->len - 1;

  pos = MIN (pos, pt->length);
  pt_insert_strux_at (pt, pos, W42_STRUX_TABLE, (gsize) id,
                      w42_ap_table_default (pt->aps));
  pt->coalescing = FALSE;
  pt_push (pt, cr_new (CR_INSERT, pos, 1));

  return id;
}

void
w42_pt_insert_cell (W42PieceTable *pt, gsize pos, int table, int row, int col,
                    W42ApIdx ap)
{
  g_return_if_fail (pt != NULL);

  pos = MIN (pos, pt->length);
  pt_insert_strux_at (pt, pos, W42_STRUX_CELL,
                      CELL_PAYLOAD (row, col, 1),
                      w42_ap_table_default (pt->aps));
  pt_insert_strux_at (pt, pos + 1, W42_STRUX_BLOCK, 0, ap);
  pt->coalescing = FALSE;
  pt_push (pt, cr_new (CR_INSERT, pos, 2));
  (void) table;
}

void
w42_pt_insert_table_end (W42PieceTable *pt, gsize pos, W42ApIdx ap)
{
  g_return_if_fail (pt != NULL);

  pos = MIN (pos, pt->length);
  pt_insert_strux_at (pt, pos, W42_STRUX_ENDTABLE, 0,
                      w42_ap_table_default (pt->aps));
  pt_insert_strux_at (pt, pos + 1, W42_STRUX_BLOCK, 0, ap);
  pt->coalescing = FALSE;
  pt_push (pt, cr_new (CR_INSERT, pos, 2));
}

void
w42_pt_insert_table_end_only (W42PieceTable *pt, gsize pos)
{
  g_return_if_fail (pt != NULL);

  pos = MIN (pos, pt->length);
  pt_insert_strux_at (pt, pos, W42_STRUX_ENDTABLE, 0,
                      w42_ap_table_default (pt->aps));
  pt->coalescing = FALSE;
  pt_push (pt, cr_new (CR_INSERT, pos, 1));
}

void
w42_pt_insert_table (W42PieceTable *pt, gsize pos, int rows, int cols,
                     W42ApIdx ap)
{
  int id;
  gsize p;

  g_return_if_fail (pt != NULL);
  g_return_if_fail (rows > 0 && cols > 0);

  w42_pt_begin_group (pt);

  p = MIN (pos, pt->length);
  id = w42_pt_insert_table_start (pt, p, cols, NULL);
  p += 1;

  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        w42_pt_insert_cell (pt, p, id, r, c, ap);
        p += 2;
      }

  /* A paragraph must follow the table.  If the table went in ahead of an
   * existing paragraph that one serves; only at the end of the document
   * does a new one have to be made. */
  {
    gsize offset = 0;
    W42Piece *next = pt_find (pt, p, &offset);

    if (next != NULL && offset == 0 && piece_is_strux (next, W42_STRUX_BLOCK))
      {
        pt_insert_strux_at (pt, p, W42_STRUX_ENDTABLE, 0,
                            w42_ap_table_default (pt->aps));
        pt->coalescing = FALSE;
        pt_push (pt, cr_new (CR_INSERT, p, 1));
      }
    else
      w42_pt_insert_table_end (pt, p, ap);
  }

  w42_pt_end_group (pt);
  pt_coalesce (pt);
}

const W42TableProps *
w42_pt_table_props (W42PieceTable *pt, int table)
{
  g_return_val_if_fail (pt != NULL, NULL);

  if (table < 0 || (guint) table >= pt->tables->len)
    return NULL;

  return g_ptr_array_index (pt->tables, table);
}

/* ---- vertical merges --------------------------------------------------- */

/* The AP of the CELL mark that owns (row, col), or -1. */
static gsize
cell_mark_pos (W42PieceTable *pt, int table, int row, int col)
{
  gsize start = w42_pt_cell_start (pt, table, row, col);

  /* A cell's text begins one position after its own mark and the BLOCK
   * that follows it. */
  return start == (gsize) -1 || start < 2 ? (gsize) -1 : start - 2;
}

/* The CELL mark's own piece, which carries the cell's properties. */
static W42Piece *
cell_mark_piece (W42PieceTable *pt, int table, int row, int col, gsize *pos_out)
{
  gsize at = cell_mark_pos (pt, table, row, col);
  gsize offset = 0;
  W42Piece *piece;

  if (at == (gsize) -1)
    return NULL;
  piece = pt_find (pt, at, &offset);
  if (piece == NULL || offset != 0 || !piece_is_strux (piece, W42_STRUX_CELL))
    return NULL;
  if (pos_out != NULL)
    *pos_out = at;
  return piece;
}

int
w42_pt_cell_vspan (W42PieceTable *pt, int table, int row, int col)
{
  W42Piece *piece;
  const W42Fmt *fmt;

  g_return_val_if_fail (pt != NULL, 0);
  piece = cell_mark_piece (pt, table, row, col, NULL);
  if (piece == NULL)
    return 0;
  fmt = w42_ap_table_get (pt->aps, piece->ap);
  if (fmt == NULL)
    return 1;
  if (fmt->pa.cell_vspan == W42_CELL_COVERED)
    return W42_CELL_COVERED;
  return MAX ((int) fmt->pa.cell_vspan, 1);
}

/* The mark is replaced rather than edited, so that undo puts the old one
 * back; everything else about the cell is kept. */
static void
cell_set_vspan (W42PieceTable *pt, int table, int row, int col, int vspan)
{
  gsize at = 0;
  W42Piece *piece = cell_mark_piece (pt, table, row, col, &at);
  W42Fmt fmt;
  gsize payload;

  if (piece == NULL)
    return;
  fmt = *w42_ap_table_get (pt->aps, piece->ap);
  fmt.pa.cell_vspan = (guint8) CLAMP (vspan, 0, 255);
  payload = piece->offset;

  pt_push (pt, pt_do_delete (pt, at, 1));
  pt_insert_strux_at (pt, at, W42_STRUX_CELL, payload,
                      w42_ap_table_intern (pt->aps, &fmt));
  pt_push (pt, cr_new (CR_INSERT, at, 1));
}

void
w42_pt_set_cell_vspan (W42PieceTable *pt, gsize cell_pos, int vspan)
{
  W42Piece *piece;
  gsize offset = 0;
  W42Fmt fmt;

  g_return_if_fail (pt != NULL);

  piece = pt_find (pt, cell_pos, &offset);
  if (piece == NULL || piece->type != W42_PIECE_STRUX ||
      piece->strux != W42_STRUX_CELL)
    return;

  fmt = *w42_ap_table_get (pt->aps, piece->ap);
  fmt.pa.cell_vspan = (guint8) CLAMP (vspan, 0, 255);
  piece->ap = w42_ap_table_intern (pt->aps, &fmt);
}

gboolean
w42_pt_merge_cells_down (W42PieceTable *pt, int table, int row, int col, int rows)
{
  gsize owner;

  g_return_val_if_fail (pt != NULL, FALSE);
  if (rows < 2)
    return FALSE;

  owner = cell_mark_pos (pt, table, row, col);
  if (owner == (gsize) -1)
    return FALSE;

  /* Every row under it must have a cell in that column, and none of
   * them may already be part of another merge. */
  for (int r = row; r < row + rows; r++)
    {
      int v = w42_pt_cell_vspan (pt, table, r, col);

      if (v == 0)
        return FALSE;
      if (r > row && v != 1)
        return FALSE;
    }

  w42_pt_begin_group (pt);
  /* The covered cells first: setting the owner's span would otherwise
   * change what w42_pt_cell_start finds under it. */
  for (int r = row + rows - 1; r > row; r--)
    cell_set_vspan (pt, table, r, col, W42_CELL_COVERED);
  cell_set_vspan (pt, table, row, col, rows);
  w42_pt_end_group (pt);
  pt->coalescing = FALSE;
  return TRUE;
}

/* The other way about: the cell at (row, col) — or the merge that has
 * swallowed it — is given its rows back. */
gboolean
w42_pt_split_cells_down (W42PieceTable *pt, int table, int row, int col)
{
  int owner = row, span;

  g_return_val_if_fail (pt != NULL, FALSE);

  /* A covered cell is split by splitting whatever covers it. */
  while (owner >= 0 && w42_pt_cell_vspan (pt, table, owner, col) == W42_CELL_COVERED)
    owner--;
  if (owner < 0)
    return FALSE;
  span = w42_pt_cell_vspan (pt, table, owner, col);
  if (span < 2 || span == W42_CELL_COVERED)
    return FALSE;

  w42_pt_begin_group (pt);
  cell_set_vspan (pt, table, owner, col, 1);
  for (int r = owner + 1; r < owner + span; r++)
    cell_set_vspan (pt, table, r, col, 1);
  w42_pt_end_group (pt);
  pt->coalescing = FALSE;
  return TRUE;
}

/* After a table has been read: a file says only where a merge starts and
 * which cells it swallows, so the owner is given the number of rows it
 * really covers, and a covered cell with nothing above it is set free. */
void
w42_pt_resolve_vmerges (W42PieceTable *pt, int table)
{
  const W42TableProps *props = w42_pt_table_props (pt, table);
  int rows = 0, cols;

  g_return_if_fail (pt != NULL);
  if (props == NULL)
    return;
  cols = props->n_cols;

  /* How many rows the table has: the last one that has a first cell. */
  while (rows < 4096 && w42_pt_cell_start (pt, table, rows, 0) != (gsize) -1)
    rows++;
  if (rows == 0 || cols <= 0)
    return;

  for (int col = 0; col < cols; col++)
    {
      for (int row = 0; row < rows; row++)
        {
          int v = w42_pt_cell_vspan (pt, table, row, col);
          int n = 1;

          if (v == 0 || v == W42_CELL_COVERED)
            {
              /* Covered by nothing: an ordinary cell after all. */
              if (v == W42_CELL_COVERED)
                cell_set_vspan (pt, table, row, col, 1);
              continue;
            }
          if (v < 2)
            continue;

          while (row + n < rows &&
                 w42_pt_cell_vspan (pt, table, row + n, col) == W42_CELL_COVERED)
            n++;
          cell_set_vspan (pt, table, row, col, n > 1 ? n : 1);
          row += n - 1;
        }
    }
  w42_pt_clear_undo (pt);
}

int
w42_pt_cell_span (W42PieceTable *pt, int table, int row, int col)
{
  int cur_table = -1;

  g_return_val_if_fail (pt != NULL, 0);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (piece_is_strux (q, W42_STRUX_TABLE))
        cur_table = (int) q->offset;
      else if (piece_is_strux (q, W42_STRUX_ENDTABLE))
        cur_table = -1;
      else if (piece_is_strux (q, W42_STRUX_CELL) && cur_table == table &&
               CELL_ROW (q->offset) == row && CELL_COL (q->offset) == col)
        return CELL_SPAN (q->offset);
    }

  return 0;
}

void
w42_pt_set_cell_span (W42PieceTable *pt, gsize cell_pos, int span)
{
  gsize offset = 0;
  W42Piece *piece;

  g_return_if_fail (pt != NULL);

  piece = pt_find (pt, cell_pos, &offset);
  if (piece == NULL || !piece_is_strux (piece, W42_STRUX_CELL))
    return;

  piece->offset = CELL_PAYLOAD (CELL_ROW (piece->offset),
                                CELL_COL (piece->offset), span);
}

void
w42_pt_table_merge_cells (W42PieceTable *pt, int table, int row,
                          int col_from, int col_to)
{
  int last, total;
  gsize first_start;

  g_return_if_fail (pt != NULL);

  if (col_to <= col_from)
    return;

  first_start = w42_pt_cell_start (pt, table, row, col_from);
  if (first_start == (gsize) -1)
    return;

  /* The last column may already be inside a merged cell: find the cell
   * that owns it, and span up to that cell's far edge. */
  last = col_to;
  while (last > col_from && w42_pt_cell_start (pt, table, row, last) == (gsize) -1)
    last--;
  total = last + w42_pt_cell_span (pt, table, row, last) - col_from;
  if (total <= w42_pt_cell_span (pt, table, row, col_from))
    return;

  w42_pt_begin_group (pt);

  /* Take the marks out from the right, so the positions to the left of
   * each stay where they were.  Each cell's paragraphs stay behind and
   * become paragraphs of the merged cell, as Word keeps them. */
  for (int c = last; c > col_from; c--)
    {
      gsize start = w42_pt_cell_start (pt, table, row, c);

      if (start != (gsize) -1)
        pt_push (pt, pt_do_delete (pt, start - 2, 1));
    }

  /* The first cell's mark is replaced by one that spans the lot. */
  pt_push (pt, pt_do_delete (pt, first_start - 2, 1));
  pt_insert_strux_at (pt, first_start - 2, W42_STRUX_CELL,
                      CELL_PAYLOAD (row, col_from, total),
                      w42_ap_table_default (pt->aps));
  pt_push (pt, cr_new (CR_INSERT, first_start - 2, 1));

  w42_pt_end_group (pt);
  pt->coalescing = FALSE;
  pt_table_renumber (pt, table);
}

/* ---------------------------------------------------------------------- */
/* Annotations                                                             */
/* ---------------------------------------------------------------------- */

GArray *
w42_pt_annotations (W42PieceTable *pt)
{
  GArray *out = g_array_new (FALSE, FALSE, sizeof (W42Annotation));
  gsize p = 0;
  const char *open = NULL;
  W42Annotation cur = { 0, 0, NULL };

  g_return_val_if_fail (pt != NULL, out);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      const char *text = NULL;

      if (q->type == W42_PIECE_TEXT || q->type == W42_PIECE_OBJECT)
        text = w42_ap_table_get (pt->aps, q->ap)->ch.comment;

      if (text != open)
        {
          if (open != NULL)
            g_array_append_val (out, cur);
          if (text != NULL)
            {
              cur.start = p;
              cur.text = text;
            }
          open = text;
        }
      if (text != NULL)
        cur.end = p + q->length;
      p += q->length;
    }
  if (open != NULL)
    g_array_append_val (out, cur);

  return out;
}

/* ---------------------------------------------------------------------- */
/* Bookmarks                                                               */
/* ---------------------------------------------------------------------- */

gboolean
w42_pt_find_bookmark (W42PieceTable *pt, const char *name, gsize *start, gsize *end)
{
  gsize p = 0;
  gboolean found = FALSE;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (name != NULL, FALSE);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (q->type == W42_PIECE_TEXT || q->type == W42_PIECE_OBJECT)
        {
          const W42Fmt *fmt = w42_ap_table_get (pt->aps, q->ap);

          if (fmt->ch.bookmark != NULL && g_str_equal (fmt->ch.bookmark, name))
            {
              if (!found)
                *start = p;
              *end = p + q->length;
              found = TRUE;
            }
          else if (found)
            break;
        }
      else if (found)
        break;
      p += q->length;
    }

  return found;
}

char **
w42_pt_bookmark_names (W42PieceTable *pt)
{
  GPtrArray *names = g_ptr_array_new ();
  const char *last = NULL;

  g_return_val_if_fail (pt != NULL, NULL);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      const W42Fmt *fmt;

      if (q->type != W42_PIECE_TEXT && q->type != W42_PIECE_OBJECT)
        continue;
      fmt = w42_ap_table_get (pt->aps, q->ap);
      if (fmt->ch.bookmark == NULL || fmt->ch.bookmark == last)
        continue;
      last = fmt->ch.bookmark;
      {
        gboolean seen = FALSE;
        for (guint i = 0; i < names->len; i++)
          if (g_str_equal (g_ptr_array_index (names, i), last))
            seen = TRUE;
        if (!seen)
          g_ptr_array_add (names, g_strdup (last));
      }
    }

  g_ptr_array_sort_values (names, (GCompareFunc) g_ascii_strcasecmp);
  g_ptr_array_add (names, NULL);
  return (char **) g_ptr_array_free (names, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Footnotes                                                               */
/* ---------------------------------------------------------------------- */

gsize
w42_pt_insert_footnote (W42PieceTable *pt, gsize pos, W42ApIdx ap)
{
  int id;
  gsize body;

  g_return_val_if_fail (pt != NULL, 0);

  pos = w42_pt_clamp_pos (pt, pos);
  id = pt->next_note++;

  w42_pt_begin_group (pt);
  pt->coalescing = FALSE;

  pt_insert_strux_at (pt, pos, W42_STRUX_FOOTNOTE, (gsize) id, ap);
  pt_push (pt, cr_new (CR_INSERT, pos, 1));

  {
    gboolean have_section = FALSE;

    for (W42Piece *q = pt->head; q != NULL; q = q->next)
      if (piece_is_strux (q, W42_STRUX_NOTES))
        {
          have_section = TRUE;
          break;
        }
    if (!have_section)
      {
        pt_insert_strux_at (pt, pt->length, W42_STRUX_NOTES, 0,
                            w42_ap_table_default (pt->aps));
        pt_push (pt, cr_new (CR_INSERT, pt->length - 1, 1));
      }
  }

  body = pt->length;
  pt_insert_strux_at (pt, body, W42_STRUX_NOTE, (gsize) id,
                      w42_ap_table_default (pt->aps));
  pt_insert_strux_at (pt, body + 1, W42_STRUX_BLOCK, 0, ap);
  pt_push (pt, cr_new (CR_INSERT, body, 2));

  w42_pt_end_group (pt);
  return body + 2;
}

int
w42_pt_footnote_at (W42PieceTable *pt, gsize pos)
{
  gsize offset = 0;
  W42Piece *piece;

  g_return_val_if_fail (pt != NULL, -1);

  piece = pt_find (pt, pos, &offset);
  if (piece == NULL || !piece_is_strux (piece, W42_STRUX_FOOTNOTE))
    return -1;
  return NOTE_ID (piece->offset);
}

gsize
w42_pt_note_body (W42PieceTable *pt, int id)
{
  gsize p = 0;

  g_return_val_if_fail (pt != NULL, (gsize) -1);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (piece_is_strux (q, W42_STRUX_NOTE) && (int) q->offset == id)
        return p + 2;       /* past the NOTE mark and the paragraph mark */
      p += q->length;
    }
  return (gsize) -1;
}

gsize
w42_pt_notes_start (W42PieceTable *pt)
{
  gsize p = 0;

  g_return_val_if_fail (pt != NULL, (gsize) -1);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (piece_is_strux (q, W42_STRUX_NOTES))
        return p;
      p += q->length;
    }
  return (gsize) -1;
}

gsize
w42_pt_note_reference (W42PieceTable *pt, int id)
{
  gsize p = 0;

  g_return_val_if_fail (pt != NULL, (gsize) -1);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (piece_is_strux (q, W42_STRUX_FOOTNOTE) && NOTE_ID (q->offset) == id)
        return p;
      p += q->length;
    }
  return (gsize) -1;
}

gboolean
w42_pt_note_is_endnote (W42PieceTable *pt, int id)
{
  g_return_val_if_fail (pt != NULL, FALSE);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    if (piece_is_strux (q, W42_STRUX_FOOTNOTE) && NOTE_ID (q->offset) == id)
      return NOTE_IS_END (q->offset);
  return FALSE;
}

void
w42_pt_set_note_endnote (W42PieceTable *pt, int id, gboolean endnote)
{
  g_return_if_fail (pt != NULL);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    if (piece_is_strux (q, W42_STRUX_FOOTNOTE) && NOTE_ID (q->offset) == id)
      q->offset = endnote ? ((gsize) id | NOTE_END_BIT) : (gsize) id;
}

gsize
w42_pt_insert_endnote (W42PieceTable *pt, gsize pos, W42ApIdx ap)
{
  gsize body;
  int id;

  g_return_val_if_fail (pt != NULL, 0);

  w42_pt_begin_group (pt);
  body = w42_pt_insert_footnote (pt, pos, ap);
  id = w42_pt_footnote_at (pt, w42_pt_clamp_pos (pt, pos));
  if (id >= 0)
    w42_pt_set_note_endnote (pt, id, TRUE);
  w42_pt_end_group (pt);
  return body;
}

void
w42_pt_table_set_widths (W42PieceTable *pt, int table, const int *widths, int n)
{
  GArray *array;

  g_return_if_fail (pt != NULL);
  g_return_if_fail (widths != NULL || n == 0);

  array = g_array_sized_new (FALSE, FALSE, sizeof (int), (guint) MAX (n, 0));
  g_array_append_vals (array, widths, (guint) MAX (n, 0));

  pt->coalescing = FALSE;
  pt_push (pt, pt_do_set_widths (pt, table, array));

  g_array_free (array, TRUE);
}

gboolean
w42_pt_cell_at (W42PieceTable *pt, gsize pos, int *table, int *row, int *col)
{
  gsize p = 0;
  int cur_table = -1, cur_row = 0, cur_col = 0;

  g_return_val_if_fail (pt != NULL, FALSE);

  for (W42Piece *q = pt->head; q != NULL && p < pos; q = q->next)
    {
      if (piece_is_strux (q, W42_STRUX_TABLE))
        cur_table = (int) q->offset;
      else if (piece_is_strux (q, W42_STRUX_CELL))
        {
          cur_row = CELL_ROW (q->offset);
          cur_col = CELL_COL (q->offset);
        }
      else if (piece_is_strux (q, W42_STRUX_ENDTABLE))
        cur_table = -1;

      p += q->length;
    }

  if (cur_table < 0)
    return FALSE;

  if (table) *table = cur_table;
  if (row)   *row = cur_row;
  if (col)   *col = cur_col;
  return TRUE;
}

gsize
w42_pt_cell_start (W42PieceTable *pt, int table, int row, int col)
{
  gsize p = 0;
  int cur_table = -1;

  g_return_val_if_fail (pt != NULL, (gsize) -1);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (piece_is_strux (q, W42_STRUX_TABLE))
        cur_table = (int) q->offset;
      else if (piece_is_strux (q, W42_STRUX_ENDTABLE))
        cur_table = -1;
      else if (piece_is_strux (q, W42_STRUX_CELL) && cur_table == table &&
               CELL_ROW (q->offset) == row && CELL_COL (q->offset) == col)
        return p + 2;          /* past the CELL mark and the BLOCK mark */

      p += q->length;
    }

  return (gsize) -1;
}

int
w42_pt_table_rows (W42PieceTable *pt, int table)
{
  int rows = 0;
  int cur_table = -1;

  g_return_val_if_fail (pt != NULL, 0);

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (piece_is_strux (q, W42_STRUX_TABLE))
        cur_table = (int) q->offset;
      else if (piece_is_strux (q, W42_STRUX_ENDTABLE))
        cur_table = -1;
      else if (piece_is_strux (q, W42_STRUX_CELL) && cur_table == table)
        rows = MAX (rows, CELL_ROW (q->offset) + 1);
    }

  return rows;
}

/* The document positions of a table's TABLE mark and of the position after
 * its ENDTABLE mark, and where each row starts. */
static gboolean
pt_table_span (W42PieceTable *pt, int table, gsize *start, gsize *end,
               GArray *row_starts)
{
  gsize p = 0;
  gboolean inside = FALSE;

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (piece_is_strux (q, W42_STRUX_TABLE) && (int) q->offset == table)
        {
          inside = TRUE;
          *start = p;
        }
      else if (inside && piece_is_strux (q, W42_STRUX_CELL) &&
               CELL_COL (q->offset) == 0 && row_starts != NULL)
        g_array_append_val (row_starts, p);
      else if (inside && piece_is_strux (q, W42_STRUX_ENDTABLE))
        {
          *end = p + 1;
          return TRUE;
        }

      p += q->length;
    }

  return FALSE;
}

/* Renumbers the CELL marks of a table after a row came or went. */
static void
pt_table_renumber (W42PieceTable *pt, int table)
{
  int cur_table = -1, row = -1, col = 0;

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (piece_is_strux (q, W42_STRUX_TABLE))
        cur_table = (int) q->offset;
      else if (piece_is_strux (q, W42_STRUX_ENDTABLE))
        cur_table = -1;
      else if (piece_is_strux (q, W42_STRUX_CELL) && cur_table == table)
        {
          int span = CELL_SPAN (q->offset);

          /* A row starts where a cell says it is column 0.  Halfway through
           * an undo a row's first cell may be gone for a moment, and the
           * next one must not be made column 0 in its place, or it would
           * start a row of its own for good. */
          if (CELL_COL (q->offset) == 0)
            {
              row++;
              col = 0;
            }
          else
            col = MAX (col, 1);
          q->offset = CELL_PAYLOAD (MAX (row, 0), col, span);
          col += span;
        }
    }
}

void
w42_pt_table_insert_row (W42PieceTable *pt, int table, int row)
{
  const W42TableProps *props;
  GArray *rows;
  gsize start = 0, end = 0, at;
  int n_rows;

  g_return_if_fail (pt != NULL);

  props = w42_pt_table_props (pt, table);
  rows = g_array_new (FALSE, FALSE, sizeof (gsize));
  if (props == NULL || !pt_table_span (pt, table, &start, &end, rows))
    {
      g_array_free (rows, TRUE);
      return;
    }

  n_rows = (int) rows->len;
  row = CLAMP (row, 0, n_rows - 1);

  /* The new row goes where the next row starts, or at the ENDTABLE mark. */
  at = (row + 1 < n_rows) ? g_array_index (rows, gsize, row + 1) : end - 1;

  w42_pt_begin_group (pt);
  for (int c = 0; c < props->n_cols; c++)
    {
      /* The row number is provisional; renumbering below settles it. */
      w42_pt_insert_cell (pt, at, table, row + 1, c,
                          w42_ap_table_default (pt->aps));
      at += 2;
    }
  if ((guint) row + 1 < props->row_heights->len)
    {
      GArray *snap = g_array_new (FALSE, FALSE, sizeof (int));
      int zero = 0;

      table_snapshot (props, snap);
      g_array_insert_val (snap, 4 + props->n_cols + row + 1, zero);
      g_array_index (snap, int, 3) += 1;
      pt->coalescing = FALSE;
      pt_push (pt, pt_do_set_table (pt, table, snap));
      g_array_free (snap, TRUE);
    }
  pt_table_renumber (pt, table);
  w42_pt_end_group (pt);

  g_array_free (rows, TRUE);
  pt_coalesce (pt);
}

void
w42_pt_table_delete_row (W42PieceTable *pt, int table, int row)
{
  GArray *rows;
  gsize start = 0, end = 0, from, to;
  int n_rows;
  W42CR *record;

  g_return_if_fail (pt != NULL);

  rows = g_array_new (FALSE, FALSE, sizeof (gsize));
  if (!pt_table_span (pt, table, &start, &end, rows))
    {
      g_array_free (rows, TRUE);
      return;
    }

  n_rows = (int) rows->len;

  if (n_rows <= 1)
    {
      /* The last row: the table goes, and its paragraph mark with it. */
      gsize whole_end = end;
      gsize offset = 0;
      W42Piece *after = pt_find (pt, end, &offset);

      if (after != NULL && piece_is_strux (after, W42_STRUX_BLOCK))
        whole_end += 1;

      /* The paragraph after the table stays when no paragraph comes
       * before it: the text that follows needs a mark to belong to. */
      {
        gboolean block_before = FALSE;
        gsize p = 0;

        for (W42Piece *q = pt->head; q != NULL && p < start; q = q->next)
          {
            if (piece_is_strux (q, W42_STRUX_BLOCK))
              block_before = TRUE;
            p += q->length;
          }
        if (!block_before)
          whole_end = end;
      }

      pt->coalescing = FALSE;
      record = pt_do_delete (pt, start, whole_end - start);
      pt_push (pt, record);
      pt_coalesce (pt);
      g_array_free (rows, TRUE);
      return;
    }

  row = CLAMP (row, 0, n_rows - 1);
  from = g_array_index (rows, gsize, row);
  to = (row + 1 < n_rows) ? g_array_index (rows, gsize, row + 1) : end - 1;

  w42_pt_begin_group (pt);
  pt->coalescing = FALSE;
  record = pt_do_delete (pt, from, to - from);
  pt_push (pt, record);
  {
    W42TableProps *props = g_ptr_array_index (pt->tables, table);

    if ((guint) row < props->row_heights->len)
      {
        GArray *snap = g_array_new (FALSE, FALSE, sizeof (int));

        table_snapshot (props, snap);
        g_array_remove_index (snap, 4 + props->n_cols + row);
        g_array_index (snap, int, 3) -= 1;
        pt_push (pt, pt_do_set_table (pt, table, snap));
        g_array_free (snap, TRUE);
      }
  }
  pt_table_renumber (pt, table);
  w42_pt_end_group (pt);

  g_array_free (rows, TRUE);
  pt_coalesce (pt);
}

gsize
w42_pt_paragraph_end (W42PieceTable *pt, gsize pos)
{
  g_return_val_if_fail (pt != NULL, 0);
  return pt_block_end (pt, pt_block_start (pt, pos));
}

gsize
w42_pt_paragraph_start (W42PieceTable *pt, gsize pos)
{
  g_return_val_if_fail (pt != NULL, 0);
  return pt_block_start (pt, pos);
}

/* ---------------------------------------------------------------------- */
/* Styles                                                                  */
/* ---------------------------------------------------------------------- */

/* The position of the paragraph mark that governs `pos`. */
static gsize
pt_block_start (W42PieceTable *pt, gsize pos)
{
  gsize p = MIN (pos, pt->length);

  while (p > 0)
    {
      gsize offset = 0;
      W42Piece *piece = pt_find (pt, p, &offset);

      if (piece != NULL && piece->type == W42_PIECE_STRUX &&
          (W42StruxType) piece->strux == W42_STRUX_BLOCK && offset == 0)
        break;

      p--;
    }

  return p;
}

/* The position just after the last thing in the paragraph whose mark is at
 * `block`: the next mark of any kind, or the end of the document.  Any
 * kind, because the last paragraph of a table cell ends at the next CELL
 * mark, not at the BLOCK that follows it. */
static gsize
pt_block_end (W42PieceTable *pt, gsize block)
{
  gsize p = block + 1;

  while (p < pt->length)
    {
      gsize offset = 0;
      W42Piece *piece = pt_find (pt, p, &offset);

      if (piece == NULL)
        break;

      if (piece->type == W42_PIECE_STRUX && offset == 0 &&
          (W42StruxType) piece->strux != W42_STRUX_FOOTNOTE)
        break;

      p += piece->length - offset;
    }

  return MIN (p, pt->length);
}

static void
pt_style_block (W42PieceTable *pt, gsize block, const W42Style *style)
{
  gsize end = pt_block_end (pt, block);

  w42_pt_apply_para_fmt (pt, block, 0,
                         W42_PARA_STYLE | W42_PARA_ALIGN | W42_PARA_INDENT_LEFT |
                         W42_PARA_INDENT_RIGHT | W42_PARA_INDENT_FIRST |
                         W42_PARA_SPACE_BEFORE | W42_PARA_SPACE_AFTER |
                         W42_PARA_LINE_SPACING | W42_PARA_LINE_SPACING_PCT |
                         W42_PARA_FLOW,
                         &style->pa);

  /* The style's character formatting goes on the text as a base.  Word
   * keeps direct formatting on top of a style; here it is replaced, which
   * is simpler and rarely what anyone notices. */
  if (end > block + 1)
    w42_pt_apply_char_fmt (pt, block + 1, end - block - 1,
                           W42_CHAR_FAMILY | W42_CHAR_SIZE |
                           W42_CHAR_BOLD | W42_CHAR_ITALIC,
                           &style->ch);
}

void
w42_pt_apply_style (W42PieceTable *pt, gsize pos, gsize n, const char *name)
{
  const W42Style *style;
  gsize block, end;

  g_return_if_fail (pt != NULL);

  style = w42_stylesheet_find (pt->styles, name);
  if (style == NULL)
    return;
  if (style->character)
    {
      w42_pt_apply_char_style (pt, pos, n, name);
      return;
    }

  block = pt_block_start (pt, pos);
  end = MIN (pos + MAX (n, 1), pt->length);

  w42_pt_begin_group (pt);

  while (block < pt->length && block < end)
    {
      gsize next = pt_block_end (pt, block);

      pt_style_block (pt, block, style);
      block = next;
    }

  w42_pt_end_group (pt);
  pt->coalescing = FALSE;
}

void
w42_pt_restyle (W42PieceTable *pt, const char *name)
{
  const W42Style *style;
  GArray *blocks;
  gsize pos = 0;

  g_return_if_fail (pt != NULL);

  style = w42_stylesheet_find (pt->styles, name);
  if (style == NULL)
    return;

  /* Collect first: styling a block changes pieces under the walk. */
  blocks = g_array_new (FALSE, FALSE, sizeof (gsize));
  for (W42Piece *p = pt->head; p != NULL; p = p->next)
    {
      if (p->type == W42_PIECE_STRUX &&
          (W42StruxType) p->strux == W42_STRUX_BLOCK)
        {
          const W42Fmt *fmt = w42_ap_table_get (pt->aps, p->ap);

          if (fmt->pa.style != NULL &&
              g_ascii_strcasecmp (fmt->pa.style, style->name) == 0)
            g_array_append_val (blocks, pos);
        }
      pos += p->length;
    }

  w42_pt_begin_group (pt);
  for (guint i = 0; i < blocks->len; i++)
    pt_style_block (pt, g_array_index (blocks, gsize, i), style);
  w42_pt_end_group (pt);

  g_array_free (blocks, TRUE);
  pt->coalescing = FALSE;
}

void
w42_pt_apply_char_style (W42PieceTable *pt, gsize pos, gsize n, const char *name)
{
  const W42Style *style;

  g_return_if_fail (pt != NULL);

  style = w42_stylesheet_find (pt->styles, name);
  if (style == NULL || n == 0)
    return;
  w42_pt_apply_char_fmt (pt, pos, n,
                         W42_CHAR_FAMILY | W42_CHAR_SIZE | W42_CHAR_BOLD |
                         W42_CHAR_ITALIC | W42_CHAR_UNDERLINE | W42_CHAR_STRIKEOUT |
                         W42_CHAR_COLOR | W42_CHAR_SMALLCAPS | W42_CHAR_ALLCAPS |
                         W42_CHAR_SCRIPT,
                         &style->ch);
}

void
w42_pt_replace_style (W42PieceTable *pt, const char *from, const char *to)
{
  const W42Style *style;
  GArray *blocks;
  gsize pos = 0;

  g_return_if_fail (pt != NULL);
  g_return_if_fail (from != NULL);

  style = w42_stylesheet_find (pt->styles, to);
  if (style == NULL)
    return;

  blocks = g_array_new (FALSE, FALSE, sizeof (gsize));
  for (W42Piece *p = pt->head; p != NULL; p = p->next)
    {
      if (p->type == W42_PIECE_STRUX && (W42StruxType) p->strux == W42_STRUX_BLOCK)
        {
          const W42Fmt *fmt = w42_ap_table_get (pt->aps, p->ap);

          if (fmt->pa.style != NULL && g_ascii_strcasecmp (fmt->pa.style, from) == 0)
            g_array_append_val (blocks, pos);
        }
      pos += p->length;
    }

  w42_pt_begin_group (pt);
  for (guint i = 0; i < blocks->len; i++)
    pt_style_block (pt, g_array_index (blocks, gsize, i), style);
  w42_pt_end_group (pt);
  g_array_free (blocks, TRUE);
  pt->coalescing = FALSE;
}

gboolean
w42_pt_is_block_mark (W42PieceTable *pt, gsize pos)
{
  gsize offset = 0;
  W42Piece *piece;

  g_return_val_if_fail (pt != NULL, FALSE);
  if (pos >= pt->length)
    return FALSE;
  piece = pt_find (pt, pos, &offset);
  return piece != NULL && piece->type == W42_PIECE_STRUX &&
         (W42StruxType) piece->strux == W42_STRUX_BLOCK && offset == 0;
}

/* ---------------------------------------------------------------------- */
/* Text extraction                                                         */
/* ---------------------------------------------------------------------- */

char *
w42_pt_get_text (W42PieceTable *pt, gsize pos, gsize n)
{
  GString *out;
  gsize p, end;
  gboolean after_cell = FALSE;

  g_return_val_if_fail (pt != NULL, NULL);

  out = g_string_new (NULL);

  if (pos >= pt->length || n == 0)
    return g_string_free (out, FALSE);

  end = MIN (pos + n, pt->length);

  for (p = pos; p < end; )
    {
      gsize offset = 0;
      W42Piece *piece = pt_find (pt, p, &offset);
      gsize take;

      if (piece == NULL)
        break;

      take = MIN (piece->length - offset, end - p);

      if (piece->type == W42_PIECE_TEXT)
        {
          const gunichar *buf = pt_buffer (pt, piece->in_change) +
                                piece->offset + offset;

          for (gsize i = 0; i < take; i++)
            {
              char utf8[8];
              int len = g_unichar_to_utf8 (buf[i], utf8);
              g_string_append_len (out, utf8, len);
            }
        }
      else if ((W42StruxType) piece->strux == W42_STRUX_BLOCK)
        {
          /* The paragraph mark that opens a cell is the cell's start, and
           * the CELL mark before it has already said where the cell goes. */
          if (!after_cell)
            g_string_append_c (out, '\n');
          after_cell = FALSE;
        }
      else if ((W42StruxType) piece->strux == W42_STRUX_FOOTNOTE)
        {
          /* A reference mark is not text. */
        }
      else if ((W42StruxType) piece->strux == W42_STRUX_NOTE)
        {
          after_cell = TRUE;     /* the BLOCK after it is the note's start */
          g_string_append_c (out, '\n');
        }
      else if ((W42StruxType) piece->strux == W42_STRUX_CELL)
        {
          g_string_append_c (out, CELL_COL (piece->offset) == 0 ? '\n' : '\t');
          after_cell = TRUE;
        }

      p += take;
    }

  return g_string_free (out, FALSE);
}

/* ---- Revision marks --------------------------------------------------- */

static guint8
revision_at (W42PieceTable *pt, gsize pos)
{
  /* The piece at `pos` itself: the character, or a paragraph mark. */
  gsize offset = 0;
  W42Piece *piece = pt_find (pt, pos, &offset);
  const W42Fmt *fmt;

  if (piece == NULL)
    return 0;
  fmt = w42_ap_table_get (w42_pt_ap_table (pt), piece->ap);
  return fmt->ch.revision;
}

gboolean
w42_pt_has_revisions (W42PieceTable *pt)
{
  gsize len;

  g_return_val_if_fail (pt != NULL, FALSE);

  len = w42_pt_length (pt);
  for (gsize pos = 0; pos < len; pos++)
    if (revision_at (pt, pos) != 0)
      return TRUE;
  return FALSE;
}

gboolean
w42_pt_resolve_revisions (W42PieceTable *pt, gboolean accept)
{
  GArray *ranges;             /* start, end, revision */
  gsize len, pos;
  gboolean changed = FALSE;

  g_return_val_if_fail (pt != NULL, FALSE);

  len = w42_pt_length (pt);
  ranges = g_array_new (FALSE, FALSE, sizeof (gsize) * 3);
  pos = 0;
  while (pos < len)
    {
      guint8 rev = revision_at (pt, pos);
      gsize end = pos + 1;

      while (end < len && revision_at (pt, end) == rev)
        end++;
      if (rev != 0)
        {
          gsize entry[3] = { pos, end, rev };
          g_array_append_val (ranges, entry);
        }
      pos = end;
    }

  if (ranges->len > 0)
    {
      W42CharFmt clear;

      memset (&clear, 0, sizeof clear);
      w42_pt_begin_group (pt);
      /* Back to front, so earlier offsets stay valid. */
      for (guint i = ranges->len; i > 0; i--)
        {
          gsize *entry = &g_array_index (ranges, gsize, (i - 1) * 3);
          gboolean drop = accept ? entry[2] == 2 : entry[2] == 1;

          if (drop)
            w42_pt_delete (pt, entry[0], entry[1] - entry[0]);
          else
            w42_pt_apply_char_fmt (pt, entry[0], entry[1] - entry[0],
                                   W42_CHAR_REVISION, &clear);
        }
      w42_pt_end_group (pt);
      changed = TRUE;
    }

  g_array_free (ranges, TRUE);
  return changed;
}

/* ---- Columns ------------------------------------------------------------ */

/* Pushes a change to one of a table's properties as a record. */
static void
pt_table_change (W42PieceTable *pt, int table, int index, int value)
{
  W42TableProps *props;
  GArray *snap;

  if (table < 0 || (guint) table >= pt->tables->len)
    return;
  props = g_ptr_array_index (pt->tables, table);
  snap = g_array_new (FALSE, FALSE, sizeof (int));
  table_snapshot (props, snap);
  if (index >= (int) snap->len)
    {
      g_array_free (snap, TRUE);
      return;
    }
  g_array_index (snap, int, index) = value;
  pt->coalescing = FALSE;
  pt_push (pt, pt_do_set_table (pt, table, snap));
  g_array_free (snap, TRUE);
}

void
w42_pt_table_set_borders (W42PieceTable *pt, int table, gboolean borders)
{
  g_return_if_fail (pt != NULL);
  pt_table_change (pt, table, 1, borders ? 1 : 0);
}

void
w42_pt_table_set_row_height (W42PieceTable *pt, int table, int row, int twips)
{
  W42TableProps *props;

  g_return_if_fail (pt != NULL);
  if (table < 0 || (guint) table >= pt->tables->len || row < 0 || row > 4095)
    return;
  props = g_ptr_array_index (pt->tables, table);
  {
    GArray *snap = g_array_new (FALSE, FALSE, sizeof (int));
    int n_rows;

    table_snapshot (props, snap);
    n_rows = g_array_index (snap, int, 3);
    if (row >= n_rows)
      {
        int zero = 0;

        for (int r = n_rows; r <= row; r++)
          g_array_append_val (snap, zero);
        g_array_index (snap, int, 3) = row + 1;
      }
    g_array_index (snap, int, 4 + props->n_cols + row) = CLAMP (twips, 0, 31680);
    pt->coalescing = FALSE;
    pt_push (pt, pt_do_set_table (pt, table, snap));
    g_array_free (snap, TRUE);
  }
}

int
w42_pt_table_get_row_height (W42PieceTable *pt, int table, int row)
{
  const W42TableProps *props;

  g_return_val_if_fail (pt != NULL, 0);
  if (table < 0 || (guint) table >= pt->tables->len || row < 0)
    return 0;
  props = g_ptr_array_index (pt->tables, table);
  return (guint) row < props->row_heights->len
           ? g_array_index (props->row_heights, int, row) : 0;
}

void
w42_pt_table_set_header_rows (W42PieceTable *pt, int table, int n)
{
  W42TableProps *props;

  g_return_if_fail (pt != NULL);
  if (table < 0 || (guint) table >= pt->tables->len)
    return;
  (void) props;
  pt_table_change (pt, table, 2, CLAMP (n, 0, 255));
}

void
w42_pt_cell_set_borders_at (W42PieceTable *pt, gsize cell_pos, int sides)
{
  gsize offset = 0;
  W42Piece *piece;
  W42Fmt fmt;

  g_return_if_fail (pt != NULL);
  piece = pt_find (pt, cell_pos, &offset);
  if (piece == NULL || offset != 0 || !piece_is_strux (piece, W42_STRUX_CELL))
    return;
  w42_fmt_init_default (&fmt);
  if (sides >= 0)
    fmt.pa.border = (guint8) (W42_BORDER_CELL_SET | (sides & W42_BORDER_BOX));
  piece->ap = w42_ap_table_intern (pt->aps, &fmt);
}

void
w42_pt_cell_set_borders (W42PieceTable *pt, int table, int row, int col, int sides)
{
  gsize start, offset = 0;
  W42Piece *piece;
  W42Fmt fmt;
  gsize payload;

  g_return_if_fail (pt != NULL);
  start = w42_pt_cell_start (pt, table, row, col);
  if (start == (gsize) -1 || start < 2)
    return;
  piece = pt_find (pt, start - 2, &offset);
  if (piece == NULL || offset != 0 || !piece_is_strux (piece, W42_STRUX_CELL))
    return;
  payload = piece->offset;
  w42_fmt_init_default (&fmt);
  if (sides >= 0)
    fmt.pa.border = (guint8) (W42_BORDER_CELL_SET | (sides & W42_BORDER_BOX));

  /* The mark is replaced, so that undo puts the old one back. */
  w42_pt_begin_group (pt);
  pt_push (pt, pt_do_delete (pt, start - 2, 1));
  pt_insert_strux_at (pt, start - 2, W42_STRUX_CELL, payload, w42_ap_table_intern (pt->aps, &fmt));
  pt_push (pt, cr_new (CR_INSERT, start - 2, 1));
  w42_pt_end_group (pt);
  pt->coalescing = FALSE;
}

int
w42_pt_cell_get_borders (W42PieceTable *pt, int table, int row, int col)
{
  gsize start, offset = 0;
  W42Piece *piece;
  const W42Fmt *fmt;

  g_return_val_if_fail (pt != NULL, -1);
  start = w42_pt_cell_start (pt, table, row, col);
  if (start == (gsize) -1 || start < 2)
    return -1;
  piece = pt_find (pt, start - 2, &offset);
  if (piece == NULL || !piece_is_strux (piece, W42_STRUX_CELL))
    return -1;
  fmt = w42_ap_table_get (pt->aps, piece->ap);
  return (fmt->pa.border & W42_BORDER_CELL_SET) ? fmt->pa.border & W42_BORDER_BOX : -1;
}

void
w42_pt_table_split_cell (W42PieceTable *pt, int table, int row, int col)
{
  gsize start, end = 0, p = 0;
  int span;

  g_return_if_fail (pt != NULL);

  start = w42_pt_cell_start (pt, table, row, col);
  span = w42_pt_cell_span (pt, table, row, col);
  if (start == (gsize) -1 || span <= 1)
    return;

  /* The cell ends where the next cell, or the table, begins. */
  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (p >= start && (piece_is_strux (q, W42_STRUX_CELL) ||
                         piece_is_strux (q, W42_STRUX_ENDTABLE)))
        {
          end = p;
          break;
        }
      p += q->length;
    }
  if (end == 0)
    return;

  w42_pt_begin_group (pt);

  /* Empty cells for the columns given up, then a mark of one column in
   * place of the wide one. */
  for (int c = col + 1; c < col + span; c++)
    {
      w42_pt_insert_cell (pt, end, table, row, c, w42_ap_table_default (pt->aps));
      end += 2;
    }
  pt_push (pt, pt_do_delete (pt, start - 2, 1));
  pt_insert_strux_at (pt, start - 2, W42_STRUX_CELL, CELL_PAYLOAD (row, col, 1),
                      w42_ap_table_default (pt->aps));
  pt_push (pt, cr_new (CR_INSERT, start - 2, 1));

  w42_pt_end_group (pt);
  pt->coalescing = FALSE;
  pt_table_renumber (pt, table);
}

/* The CELL mark of the cell covering column `col` in the row starting at
 * `row_start`, its first column and its span; or (gsize) -1. */
static gsize
row_cell_covering (W42PieceTable *pt, int table, gsize row_start, gsize row_end,
                   int col, int *first_col, int *span)
{
  gsize p = 0;

  for (W42Piece *q = pt->head; q != NULL; q = q->next)
    {
      if (p >= row_start && p < row_end && piece_is_strux (q, W42_STRUX_CELL))
        {
          int c = CELL_COL (q->offset), sp = MAX (CELL_SPAN (q->offset), 1);

          if (col >= c && col < c + sp)
            {
              *first_col = c;
              *span = sp;
              return p;
            }
        }
      p += q->length;
      (void) table;
    }
  return (gsize) -1;
}

void
w42_pt_table_insert_column (W42PieceTable *pt, int table, int col)
{
  const W42TableProps *props;
  GArray *rows, *widths;
  gsize start = 0, end = 0;
  int n_rows;

  g_return_if_fail (pt != NULL);

  props = w42_pt_table_props (pt, table);
  rows = g_array_new (FALSE, FALSE, sizeof (gsize));
  if (props == NULL || !pt_table_span (pt, table, &start, &end, rows))
    {
      g_array_free (rows, TRUE);
      return;
    }
  n_rows = (int) rows->len;
  col = CLAMP (col, 0, props->n_cols - 1);

  w42_pt_begin_group (pt);
  /* Back to front, so the rows before keep their positions. */
  for (int r = n_rows - 1; r >= 0; r--)
    {
      gsize row_start = g_array_index (rows, gsize, r);
      gsize row_end = r + 1 < n_rows ? g_array_index (rows, gsize, r + 1) : end - 1;
      int first = 0, span = 1;
      gsize cell = row_cell_covering (pt, table, row_start, row_end, col, &first, &span);

      if (cell != (gsize) -1 && first + span - 1 > col)
        {
          /* A merged cell reaches past the new column: it grows over it. */
          w42_pt_set_cell_span (pt, cell, span + 1);
          continue;
        }
      /* The new cell goes before the cell after `col`, or at the row's end. */
      {
        int nf = 0, nsp = 1;
        gsize next = col + 1 < props->n_cols
                       ? row_cell_covering (pt, table, row_start, row_end, col + 1, &nf, &nsp)
                       : (gsize) -1;
        gsize at = next != (gsize) -1 ? next : row_end;

        w42_pt_insert_cell (pt, at, table, r, col + 1, w42_ap_table_default (pt->aps));
      }
    }
  pt_table_renumber (pt, table);

  /* One more width: the new column takes the old one's. */
  widths = g_array_new (FALSE, FALSE, sizeof (int));
  for (int c = 0; c < props->n_cols; c++)
    {
      int w = c < (int) props->widths->len ? g_array_index (props->widths, int, c) : 0;

      g_array_append_val (widths, w);
      if (c == col)
        g_array_append_val (widths, w);
    }
  pt->coalescing = FALSE;
  pt_push (pt, pt_do_set_widths (pt, table, widths));
  g_array_free (widths, TRUE);
  w42_pt_end_group (pt);

  g_array_free (rows, TRUE);
  pt_coalesce (pt);
}

void
w42_pt_table_delete_column (W42PieceTable *pt, int table, int col)
{
  const W42TableProps *props;
  GArray *rows, *widths;
  gsize start = 0, end = 0;
  int n_rows;

  g_return_if_fail (pt != NULL);

  props = w42_pt_table_props (pt, table);
  rows = g_array_new (FALSE, FALSE, sizeof (gsize));
  if (props == NULL || props->n_cols <= 1 || !pt_table_span (pt, table, &start, &end, rows))
    {
      g_array_free (rows, TRUE);
      return;
    }
  n_rows = (int) rows->len;
  col = CLAMP (col, 0, props->n_cols - 1);

  w42_pt_begin_group (pt);
  /* The widths first, while the columns are still what they were. */
  widths = g_array_new (FALSE, FALSE, sizeof (int));
  for (int c = 0; c < props->n_cols; c++)
    if (c != col)
      {
        int w = c < (int) props->widths->len ? g_array_index (props->widths, int, c) : 0;
        g_array_append_val (widths, w);
      }
  for (int r = n_rows - 1; r >= 0; r--)
    {
      gsize row_start = g_array_index (rows, gsize, r);
      gsize row_end = r + 1 < n_rows ? g_array_index (rows, gsize, r + 1) : end - 1;
      int first = 0, span = 1;
      gsize cell = row_cell_covering (pt, table, row_start, row_end, col, &first, &span);

      if (cell == (gsize) -1)
        continue;
      if (span > 1)
        {
          /* A merged cell loses one of its columns and keeps its text. */
          w42_pt_set_cell_span (pt, cell, span - 1);
          continue;
        }
      {
        /* The cell's own extent: to the next CELL mark or the row's end. */
        int nf = 0, nsp = 1;
        gsize next = (gsize) -1;

        for (int c = col + 1; c < props->n_cols && next == (gsize) -1; c++)
          next = row_cell_covering (pt, table, row_start, row_end, c, &nf, &nsp);
        if (next == (gsize) -1)
          next = row_end;
        /* Renumbering knows a row by its cell in column 0: when that is
         * the one going, the next cell takes column 0 first. */
        if (first == 0 && next != row_end)
          {
            gsize off = 0;
            W42Piece *q = pt_find (pt, next, &off);

            if (q != NULL && piece_is_strux (q, W42_STRUX_CELL))
              {
                /* As a recorded edit, so that undo puts the old mark back. */
                gsize payload = CELL_PAYLOAD (CELL_ROW (q->offset), 0, CELL_SPAN (q->offset));
                W42ApIdx qap = q->ap;

                pt_push (pt, pt_do_delete (pt, next, 1));
                pt_insert_strux_at (pt, next, W42_STRUX_CELL, payload, qap);
                pt_push (pt, cr_new (CR_INSERT, next, 1));
              }
          }
        pt->coalescing = FALSE;
        pt_push (pt, pt_do_delete (pt, cell, next - cell));
      }
    }
  pt_table_renumber (pt, table);
  pt->coalescing = FALSE;
  pt_push (pt, pt_do_set_widths (pt, table, widths));
  g_array_free (widths, TRUE);
  w42_pt_end_group (pt);

  g_array_free (rows, TRUE);
  pt_coalesce (pt);
}

/* ---- Fragments ---------------------------------------------------------- */

/* Copies what is at [from, from + n) of `src` into `dst` at `at`, one
 * position at a time: text with its formatting re-interned in `dst`,
 * paragraph marks as paragraph marks, pictures as pictures.  Returns how
 * many positions went in. */
static gsize
copy_range (W42PieceTable *dst, gsize at, W42PieceTable *src, gsize from, gsize n)
{
  gsize put = 0;
  GString *run = g_string_new (NULL);
  W42ApIdx run_ap = 0;
  gboolean have_run = FALSE;

  for (gsize pos = from; pos < from + n && pos < src->length; pos++)
    {
      gsize offset = 0;
      W42Piece *p = pt_find (src, pos, &offset);
      W42ApIdx ap;
      const W42Fmt *fmt;

      if (p == NULL)
        break;
      fmt = w42_ap_table_get (src->aps, p->ap);
      ap = w42_ap_table_intern (dst->aps, fmt);

      if (p->type == W42_PIECE_TEXT)
        {
          const gunichar *chars = pt_buffer (src, p->in_change) + p->offset + offset;

          if (have_run && ap != run_ap)
            {
              w42_pt_insert_text (dst, at + put, run->str, run_ap);
              put += g_utf8_strlen (run->str, -1);
              g_string_truncate (run, 0);
            }
          g_string_append_unichar (run, chars[0]);
          run_ap = ap;
          have_run = TRUE;
          continue;
        }

      if (have_run)
        {
          w42_pt_insert_text (dst, at + put, run->str, run_ap);
          put += g_utf8_strlen (run->str, -1);
          g_string_truncate (run, 0);
          have_run = FALSE;
        }

      if (p->type == W42_PIECE_OBJECT)
        {
          const W42Object *object = w42_object_table_get (src->objects, (W42ObjectIdx) p->offset);

          if (object != NULL)
            {
              W42ObjectIdx idx = w42_object_table_add (dst->objects, object->data, object->format,
                                                       object->pixel_w, object->pixel_h,
                                                       object->width, object->height);
              w42_pt_insert_object (dst, at + put, idx, ap);
              put += 1;
            }
        }
      else if (piece_is_strux (p, W42_STRUX_BLOCK))
        {
          w42_pt_insert_block (dst, at + put, ap);
          put += 1;
        }
      /* Other marks -- sections, tables, cells, notes -- are not copied;
       * a cell's end still ends its paragraph. */
      else if (piece_is_strux (p, W42_STRUX_CELL) || piece_is_strux (p, W42_STRUX_ENDTABLE))
        {
          if (put > 0)
            {
              w42_pt_insert_block (dst, at + put, ap);
              put += 1;
            }
        }
    }
  if (have_run)
    {
      w42_pt_insert_text (dst, at + put, run->str, run_ap);
      put += g_utf8_strlen (run->str, -1);
    }
  g_string_free (run, TRUE);
  return put;
}

W42PieceTable *
w42_pt_extract (W42PieceTable *pt, gsize start, gsize n)
{
  W42PieceTable *frag;

  g_return_val_if_fail (pt != NULL, NULL);

  frag = w42_pt_new ();
  w42_pt_load_text (frag, "");
  copy_range (frag, w42_pt_first_caret_pos (frag), pt, start, n);
  w42_pt_clear_undo (frag);
  return frag;
}

gsize
w42_pt_insert_fragment (W42PieceTable *pt, gsize pos, W42PieceTable *frag)
{
  gsize first;

  g_return_val_if_fail (pt != NULL && frag != NULL, 0);

  first = w42_pt_first_caret_pos (frag);
  if (frag->length <= first)
    return 0;
  return copy_range (pt, pos, frag, first, frag->length - first);
}

void
w42_pt_restyle_tree (W42PieceTable *pt, const char *name)
{
  const char **kids;

  g_return_if_fail (pt != NULL);
  g_return_if_fail (name != NULL);

  w42_pt_begin_group (pt);
  w42_pt_restyle (pt, name);
  kids = w42_stylesheet_descendants (pt->styles, name);
  for (guint i = 0; kids != NULL && kids[i] != NULL; i++)
    w42_pt_restyle (pt, kids[i]);
  g_free (kids);
  w42_pt_end_group (pt);
}

void
w42_pt_set_author (W42PieceTable *pt, const char *name)
{
  g_return_if_fail (pt != NULL);
  pt->author = name != NULL && *name != '\0' ? g_intern_string (name) : NULL;
}

const char *
w42_pt_get_author (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return pt->author;
}

gboolean
w42_pt_table_bounds (W42PieceTable *pt, int table, gsize *start, gsize *end)
{
  gsize s = 0, e = 0;

  g_return_val_if_fail (pt != NULL, FALSE);
  if (!pt_table_span (pt, table, &s, &e, NULL))
    return FALSE;
  if (start) *start = s;
  if (end)   *end = e;
  return TRUE;
}

gboolean
w42_pt_row_bounds (W42PieceTable *pt, int table, int row, gsize *start, gsize *end)
{
  gsize s = 0, e = 0;
  GArray *rows;
  gboolean ok = FALSE;

  g_return_val_if_fail (pt != NULL, FALSE);
  rows = g_array_new (FALSE, FALSE, sizeof (gsize));
  if (pt_table_span (pt, table, &s, &e, rows) && row >= 0 && row < (int) rows->len)
    {
      if (start) *start = g_array_index (rows, gsize, row);
      /* The next row's first cell, or the ENDTABLE mark. */
      if (end)   *end = (row + 1 < (int) rows->len)
                        ? g_array_index (rows, gsize, row + 1) : e - 1;
      ok = TRUE;
    }
  g_array_free (rows, TRUE);
  return ok;
}

void
w42_pt_table_split (W42PieceTable *pt, int table, int row)
{
  const W42TableProps *props;
  gsize start = 0, end = 0, at;
  GArray *rows;
  int *widths;
  int n_cols, new_table;
  gboolean borders;

  g_return_if_fail (pt != NULL);

  props = w42_pt_table_props (pt, table);
  rows = g_array_new (FALSE, FALSE, sizeof (gsize));
  if (props == NULL || !pt_table_span (pt, table, &start, &end, rows) ||
      row <= 0 || row >= (int) rows->len)
    {
      g_array_free (rows, TRUE);
      return;                         /* the first row cannot start a split */
    }

  at = g_array_index (rows, gsize, row);
  n_cols = props->n_cols;
  borders = props->borders;
  widths = g_new0 (int, MAX (n_cols, 1));
  for (int c = 0; c < n_cols; c++)
    widths[c] = c < (int) props->widths->len ? g_array_index (props->widths, int, c) : 0;
  g_array_free (rows, TRUE);

  w42_pt_begin_group (pt);
  /* The first table ends before the row, a paragraph comes between, and a
   * table of the same shape starts again at the row. */
  w42_pt_insert_table_end (pt, at, w42_ap_table_default (pt->aps));
  new_table = w42_pt_insert_table_start (pt, at + 2, n_cols, widths);
  w42_pt_table_set_borders (pt, new_table, borders);
  pt_table_renumber (pt, table);
  pt_table_renumber (pt, new_table);
  w42_pt_end_group (pt);
  pt->coalescing = FALSE;

  g_free (widths);
}

gboolean
w42_pt_cell_range (W42PieceTable *pt, int table, int row, int col,
                   gsize *start, gsize *end)
{
  const W42TableProps *props;
  gsize s, row_end = 0;

  g_return_val_if_fail (pt != NULL, FALSE);
  props = w42_pt_table_props (pt, table);
  if (props == NULL || !w42_pt_row_bounds (pt, table, row, NULL, &row_end))
    return FALSE;
  s = w42_pt_cell_start (pt, table, row, col);
  if (s == (gsize) -1)
    return FALSE;

  if (start) *start = s;
  if (end)
    {
      /* Up to the next cell's marks, or to the end of the row. */
      *end = row_end;
      for (int c = col + 1; c < props->n_cols; c++)
        {
          gsize next = w42_pt_cell_start (pt, table, row, c);

          if (next != (gsize) -1 && next >= s + 2)
            {
              *end = next - 2;
              break;
            }
        }
    }
  return TRUE;
}

void
w42_pt_table_sort (W42PieceTable *pt, int table, gboolean descending)
{
  const W42TableProps *props;
  int n_rows, n_cols, first;
  GPtrArray *frags;
  GArray *order;
  char **keys;

  g_return_if_fail (pt != NULL);

  props = w42_pt_table_props (pt, table);
  n_rows = w42_pt_table_rows (pt, table);
  n_cols = props != NULL ? props->n_cols : 0;
  first = props != NULL ? props->header_rows : 0;
  if (n_rows - first < 2 || n_cols < 1)
    return;                           /* nothing to sort */

  /* Every cell's content, and the key its row sorts on. */
  frags = g_ptr_array_new ();
  keys = g_new0 (char *, n_rows);
  for (int r = 0; r < n_rows; r++)
    {
      for (int c = 0; c < n_cols; c++)
        {
          gsize s = 0, e = 0;
          W42PieceTable *frag = NULL;

          if (w42_pt_cell_range (pt, table, r, c, &s, &e) && e > s)
            frag = w42_pt_extract (pt, s, e - s);
          g_ptr_array_add (frags, frag);
        }
      {
        gsize s = 0, e = 0;
        char *text = w42_pt_cell_range (pt, table, r, 0, &s, &e) && e > s
                     ? w42_pt_get_text (pt, s, e - s) : g_strdup ("");
        char *folded = g_utf8_casefold (text != NULL ? text : "", -1);

        keys[r] = g_utf8_collate_key (folded, -1);
        g_free (folded);
        g_free (text);
      }
    }

  order = g_array_new (FALSE, FALSE, sizeof (int));
  for (int r = first; r < n_rows; r++)
    g_array_append_val (order, r);
  /* An insertion sort: the tables people sort have tens of rows. */
  for (guint i = 1; i < order->len; i++)
    {
      int v = g_array_index (order, int, i);
      int j = (int) i - 1;

      while (j >= 0)
        {
          int u = g_array_index (order, int, j);
          int cmp = strcmp (keys[u], keys[v]);

          if (descending ? cmp >= 0 : cmp <= 0)
            break;
          g_array_index (order, int, j + 1) = u;
          j--;
        }
      g_array_index (order, int, j + 1) = v;
    }

  /* The cells written back last first, so the places still to be written
   * are not moved by the ones already done. */
  w42_pt_begin_group (pt);
  for (int i = (int) order->len - 1; i >= 0; i--)
    {
      int target = first + i;
      int source = g_array_index (order, int, i);

      if (source == target)
        continue;
      for (int c = n_cols - 1; c >= 0; c--)
        {
          gsize s = 0, e = 0;
          W42PieceTable *frag = g_ptr_array_index (frags, source * n_cols + c);

          if (!w42_pt_cell_range (pt, table, target, c, &s, &e))
            continue;
          if (e > s)
            w42_pt_delete (pt, s, e - s);
          if (frag != NULL)
            w42_pt_insert_fragment (pt, s, frag);
        }
    }
  w42_pt_end_group (pt);
  pt->coalescing = FALSE;

  for (guint i = 0; i < frags->len; i++)
    if (g_ptr_array_index (frags, i) != NULL)
      w42_pt_free (g_ptr_array_index (frags, i));
  g_ptr_array_free (frags, TRUE);
  for (int r = 0; r < n_rows; r++)
    g_free (keys[r]);
  g_free (keys);
  g_array_free (order, TRUE);
}

gsize
w42_pt_table_to_text (W42PieceTable *pt, int table)
{
  const W42TableProps *props;
  int n_rows, n_cols;
  gsize start = 0, end = 0, at;
  GString *text;

  g_return_val_if_fail (pt != NULL, (gsize) -1);

  props = w42_pt_table_props (pt, table);
  n_rows = w42_pt_table_rows (pt, table);
  n_cols = props != NULL ? props->n_cols : 0;
  if (!w42_pt_table_bounds (pt, table, &start, &end) || n_rows < 1 || n_cols < 1)
    return (gsize) -1;

  /* A tab between the cells of a row, a paragraph for each row. */
  text = g_string_new (NULL);
  for (int r = 0; r < n_rows; r++)
    {
      for (int c = 0; c < n_cols; c++)
        {
          gsize s = 0, e = 0;
          char *cell = w42_pt_cell_range (pt, table, r, c, &s, &e) && e > s
                       ? w42_pt_get_text (pt, s, e - s) : NULL;

          if (cell != NULL)
            {
              g_strdelimit (cell, "\n\t", ' ');
              g_string_append (text, g_strstrip (cell));
              g_free (cell);
            }
          if (c + 1 < n_cols)
            g_string_append_c (text, '\t');
        }
      if (r + 1 < n_rows)
        g_string_append_c (text, '\n');
    }

  w42_pt_begin_group (pt);
  /* A table's marks may only go when the whole table goes: its rows are
   * taken out one by one, the last taking the table with it. */
  while (w42_pt_table_rows (pt, table) > 0)
    w42_pt_table_delete_row (pt, table, w42_pt_table_rows (pt, table) - 1);

  /* Where the table was, a paragraph for each of its rows. */
  at = MIN (start, pt->length);
  {
    char **lines = g_strsplit (text->str, "\n", -1);

    for (guint i = 0; lines[i] != NULL; i++)
      {
        w42_pt_insert_block (pt, at, w42_pt_ap_at (pt, at));
        at += 1;
        if (*lines[i] != '\0')
          {
            w42_pt_insert_text (pt, at, lines[i], w42_pt_ap_at (pt, at));
            at += g_utf8_strlen (lines[i], -1);
          }
      }
    g_strfreev (lines);
  }

  /* The paragraph mark that followed the table went with it: what comes
   * after needs one of its own, or it would join the last row. */
  {
    gsize offset = 0;
    W42Piece *next = at < pt->length ? pt_find (pt, at, &offset) : NULL;

    if (next != NULL && next->type != W42_PIECE_STRUX)
      w42_pt_insert_block (pt, at, w42_pt_ap_at (pt, at));
  }
  w42_pt_end_group (pt);
  pt->coalescing = FALSE;
  g_string_free (text, TRUE);

  return start;
}

gsize
w42_pt_text_to_table (W42PieceTable *pt, gsize start, gsize end)
{
  GPtrArray *blocks, *lines;
  gsize at;
  int n_cols = 1, table = -1, r0 = 0, c0 = 0;

  g_return_val_if_fail (pt != NULL, (gsize) -1);

  start = pt_block_start (pt, start);
  end = MIN (MAX (end, start + 1), pt->length);
  blocks = w42_pt_snapshot_blocks (pt);
  lines = g_ptr_array_new ();
  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);

      if (block->start_pos < start || block->start_pos >= end)
        continue;
      if (block->table >= 0 || block->note >= 0)
        {
          g_ptr_array_free (blocks, TRUE);
          g_ptr_array_free (lines, TRUE);
          return (gsize) -1;          /* a table already, or a note */
        }
      {
        char **fields = g_strsplit (block->text->str, "\t", -1);

        n_cols = MAX (n_cols, (int) g_strv_length (fields));
        g_ptr_array_add (lines, fields);
      }
    }
  g_ptr_array_free (blocks, TRUE);

  if (lines->len == 0)
    {
      g_ptr_array_free (lines, TRUE);
      return (gsize) -1;
    }
  n_cols = MIN (n_cols, 63);

  w42_pt_begin_group (pt);
  /* The table goes in after the paragraphs, is filled, and then they go:
   * a table cannot be put where its own paragraphs still are. */
  at = MIN (end, pt->length);
  w42_pt_insert_table (pt, at, (int) lines->len, n_cols, w42_pt_ap_at (pt, at));
  if (w42_pt_cell_at (pt, w42_pt_clamp_pos (pt, at + 3), &table, &r0, &c0))
    for (int r = (int) lines->len - 1; r >= 0; r--)
      {
        char **fields = g_ptr_array_index (lines, r);

        for (int c = MIN ((int) g_strv_length (fields), n_cols) - 1; c >= 0; c--)
          {
            gsize s2 = w42_pt_cell_start (pt, table, r, c);

            if (s2 != (gsize) -1 && *fields[c] != '\0')
              w42_pt_insert_text (pt, s2, fields[c], w42_pt_ap_at (pt, s2));
          }
      }
  if (end > start)
    w42_pt_delete (pt, start, end - start);
  at = start;
  w42_pt_end_group (pt);
  pt->coalescing = FALSE;

  for (guint i = 0; i < lines->len; i++)
    g_strfreev (g_ptr_array_index (lines, i));
  g_ptr_array_free (lines, TRUE);

  return at;
}

const W42DocInfo *
w42_pt_get_info (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return &pt->info;
}

void
w42_pt_set_info (W42PieceTable *pt, const W42DocInfo *info)
{
  g_return_if_fail (pt != NULL);

  if (info == NULL)
    {
      memset (&pt->info, 0, sizeof pt->info);
      return;
    }
  /* Interned: the strings outlive whoever handed them over. */
  pt->info.title    = info->title    != NULL && *info->title    ? g_intern_string (info->title) : NULL;
  pt->info.subject  = info->subject  != NULL && *info->subject  ? g_intern_string (info->subject) : NULL;
  pt->info.author   = info->author   != NULL && *info->author   ? g_intern_string (info->author) : NULL;
  pt->info.keywords = info->keywords != NULL && *info->keywords ? g_intern_string (info->keywords) : NULL;
  pt->info.comments = info->comments != NULL && *info->comments ? g_intern_string (info->comments) : NULL;
}

static W42PageText *
page_text_slot (W42PieceTable *pt, gboolean header, W42PageTextKind kind)
{
  switch (kind)
    {
    case W42_PAGE_TEXT_FIRST: return header ? &pt->header_first : &pt->footer_first;
    case W42_PAGE_TEXT_EVEN:  return header ? &pt->header_even : &pt->footer_even;
    default:                  return header ? &pt->header : &pt->footer;
    }
}

const W42PageText *
w42_pt_get_header_kind (W42PieceTable *pt, W42PageTextKind kind)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return page_text_slot (pt, TRUE, kind);
}

const W42PageText *
w42_pt_get_footer_kind (W42PieceTable *pt, W42PageTextKind kind)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return page_text_slot (pt, FALSE, kind);
}

void
w42_pt_set_header_kind (W42PieceTable *pt, W42PageTextKind kind,
                        const char *text, W42Align align)
{
  g_return_if_fail (pt != NULL);
  page_text_set (page_text_slot (pt, TRUE, kind), text, align);
}

void
w42_pt_set_footer_kind (W42PieceTable *pt, W42PageTextKind kind,
                        const char *text, W42Align align)
{
  g_return_if_fail (pt != NULL);
  page_text_set (page_text_slot (pt, FALSE, kind), text, align);
}

gboolean
w42_pt_get_title_page (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, FALSE);
  return pt->title_page != 0;
}

void
w42_pt_set_title_page (W42PieceTable *pt, gboolean on)
{
  g_return_if_fail (pt != NULL);
  pt->title_page = on ? 1 : 0;
}

gboolean
w42_pt_get_facing_pages (W42PieceTable *pt)
{
  g_return_val_if_fail (pt != NULL, FALSE);
  return pt->facing_pages != 0;
}

void
w42_pt_set_facing_pages (W42PieceTable *pt, gboolean on)
{
  g_return_if_fail (pt != NULL);
  pt->facing_pages = on ? 1 : 0;
}

/* Which of the three a page uses: the first page's own if there is one,
 * then even pages' own, else the ordinary one. */
static const W42PageText *
page_text_for (W42PieceTable *pt, gboolean header, int page)
{
  if (page == 0 && pt->title_page)
    return page_text_slot (pt, header, W42_PAGE_TEXT_FIRST);
  if (pt->facing_pages && (page + 1) % 2 == 0)
    return page_text_slot (pt, header, W42_PAGE_TEXT_EVEN);
  return page_text_slot (pt, header, W42_PAGE_TEXT_DEFAULT);
}

const W42PageText *
w42_pt_page_header (W42PieceTable *pt, int page)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return page_text_for (pt, TRUE, page);
}

const W42PageText *
w42_pt_page_footer (W42PieceTable *pt, int page)
{
  g_return_val_if_fail (pt != NULL, NULL);
  return page_text_for (pt, FALSE, page);
}
