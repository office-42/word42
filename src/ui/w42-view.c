/* w42-view.c - see w42-view.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-view.h"

#include "w42-autocorrect.h"
#include "w42-autotext.h"
#include "w42-index.h"
#include "w42-autoformat.h"
#include "w42-hyphenate.h"
#include <glib/gstdio.h>
#include "w42-rtf.h"

#include <math.h>
#include <string.h>

/* Space around and between the pages, in device pixels. */
#define PAGE_GAP     16.0
#define CARET_PERIOD 530

struct _W42View {
  GtkWidget      parent_instance;
  GtkWidget *context_menu;   /* the right-click menu, a popover */
  gboolean   autocorrect;    /* Tools > Options: correct as you type */

  W42Document   *doc;
  W42Layout     *layout;
  gulong         doc_changed_id;

  double         zoom;

  gsize          caret;
  gsize          anchor;      /* equal to caret when there is no selection */
  double         want_x;      /* sticky column for up/down, or -1 */

  /* Formatting toggled with no selection, applied to the next thing typed. */
  W42CharMask    pending_mask;
  W42CharFmt     pending;

  W42ViewMode    mode;
  W42Spell      *spell;       /* not owned */
  gboolean       track_changes;
  GtkIMContext  *im;
  gboolean       dragging;

  /* Dragging a handle of the selected picture.  The picture is not resized
   * until the button goes up; while it is down a dotted outline shows the
   * size it will be, as Word 6 did. */
  int            handle;        /* -1, or the handle being dragged */
  double         drag_x0, drag_y0;
  double         pic_x, pic_y, pic_w, pic_h;   /* page px at the start */
  double         new_w, new_h;                 /* page px, proposed */

  /* Dragging a column edge of a table: a dotted line shows where the
   * edge will go, and the widths change when the button goes up. */
  int            col_table;     /* -1, or the table whose edge it is */
  int            col_col;       /* the column whose right edge */
  int            col_page;
  double         col_x0;        /* page px, where the edge was */
  double         col_x;         /* page px, where it is going */

  /* Dragging the selection itself.  Pressing inside the selection arms
   * the drag; moving beyond a click's worth starts it, a grey drop caret
   * follows the pointer, and the button going up moves the text there —
   * or copies it, with Ctrl held.  A press that never moves is a click,
   * and places the caret when the button goes up. */
  gboolean       text_drag_armed;
  gboolean       text_dragging;
  gsize          drop_pos;

  /* Edit > Repeat: the last action Repeat can do again, stamped with the
   * undo history's state when it was recorded.  Any edit since is one the
   * record does not cover, and the stamp no longer matches, so Repeat
   * offers nothing rather than repeating an older action. */
  guint8         repeat_kind;       /* W42_REPEAT_* */
  GString       *repeat_text;       /* the last run of typing */
  W42CharMask    repeat_char_mask;
  W42CharFmt     repeat_char_fmt;   /* its pointers are interned */
  W42ParaMask    repeat_para_mask;
  W42ParaFmt     repeat_para_fmt;
  const char    *repeat_style;      /* interned */
  guint8         repeat_case;       /* W42CaseKind */
  gsize          repeat_undo_pos;
  guint64        repeat_serial;
  guint          blink_id;
  gboolean       blink_on;
};

G_DEFINE_FINAL_TYPE (W42View, w42_view, GTK_TYPE_WIDGET)

enum {
  SIGNAL_STATE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void view_relayout (W42View *self);
static void view_scroll_to_caret (W42View *self);
static void view_popup_at_caret (W42View *self);
static void view_insert_paragraph (W42View *self);

/* ---------------------------------------------------------------------- */
/* Small helpers                                                           */
/* ---------------------------------------------------------------------- */

static W42PieceTable *
view_pt (W42View *self)
{
  return self->doc != NULL ? w42_document_pt (self->doc) : NULL;
}

/* The caret at the end of a paragraph sits just before the next
 * paragraph's mark; paragraph formatting there means this paragraph's. */
static gsize
para_pos (W42View *self, gsize pos)
{
  W42PieceTable *pt = view_pt (self);

  if (pt != NULL && pos > 0 && w42_pt_is_block_mark (pt, pos))
    return pos - 1;
  return pos;
}

/* Where the formatting of the selection is read: its first character,
 * rather than the one before it. */
static gsize
fmt_probe (W42View *self);

static gsize
sel_start (W42View *self)
{
  return MIN (self->caret, self->anchor);
}

static gsize
sel_end (W42View *self)
{
  return MAX (self->caret, self->anchor);
}

gboolean
w42_view_has_selection (W42View *self)
{
  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  return self->caret != self->anchor;
}

static void
view_state_changed (W42View *self)
{
  g_signal_emit (self, signals[SIGNAL_STATE_CHANGED], 0);
}

/* ---------------------------------------------------------------------- */
/* Edit > Repeat                                                           */
/* ---------------------------------------------------------------------- */

enum {
  W42_REPEAT_NONE = 0,
  W42_REPEAT_TYPING,
  W42_REPEAT_CHAR_FMT,
  W42_REPEAT_PARA_FMT,
  W42_REPEAT_STYLE,
  W42_REPEAT_CASE
};

/* Stamps the record with where the undo history stands now: Repeat is
 * offered only while the history still stands there, so it repeats the
 * last action or nothing. */
static void
view_record_repeat (W42View *self, int kind)
{
  W42PieceTable *pt = view_pt (self);

  self->repeat_kind = kind;
  if (pt != NULL)
    w42_pt_undo_state (pt, &self->repeat_undo_pos, &self->repeat_serial);

  /* The edit's own state-changed went out before the record was made,
   * with Repeat looking stale; say again, so the menu enables it. */
  view_state_changed (self);
}

static gboolean
view_repeat_stamp_current (W42View *self)
{
  W42PieceTable *pt = view_pt (self);
  gsize pos = 0;
  guint64 serial = 0;

  if (pt == NULL || self->repeat_kind == W42_REPEAT_NONE)
    return FALSE;
  w42_pt_undo_state (pt, &pos, &serial);
  return pos == self->repeat_undo_pos && serial == self->repeat_serial;
}

static void
view_reset_blink (W42View *self)
{
  self->blink_on = TRUE;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Moving the caret ends a run of typing for undo, drops any sticky column and
 * forgets formatting that was toggled but never used. */
static void
view_caret_moved (W42View *self, gboolean keep_want_x)
{
  W42PieceTable *pt = view_pt (self);

  if (pt != NULL)
    w42_pt_break_undo_coalesce (pt);

  if (!keep_want_x)
    self->want_x = -1.0;

  self->pending_mask = 0;

  view_reset_blink (self);
  view_scroll_to_caret (self);
  view_state_changed (self);
}

static void
view_set_caret (W42View *self, gsize pos, gboolean extend)
{
  W42PieceTable *pt = view_pt (self);

  if (pt == NULL)
    return;

  self->caret = w42_pt_clamp_pos (pt, pos);
  if (!extend)
    self->anchor = self->caret;

  view_caret_moved (self, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Geometry                                                                */
/* ---------------------------------------------------------------------- */

static double
view_page_origin_x (W42View *self)
{
  double page_w = w42_layout_page_width (self->layout) * self->zoom;
  double width  = gtk_widget_get_width (GTK_WIDGET (self));

  /* Normal view has no sheet to centre: the text starts at the left, as it
   * did in Word 6, and the window is simply a window on to the galley. */
  if (self->mode == W42_VIEW_NORMAL)
    return 0.0;

  if (width > page_w + 2 * PAGE_GAP)
    return (width - page_w) / 2.0;

  return PAGE_GAP;
}

static double
view_page_origin_y (W42View *self, int page)
{
  double page_h = w42_layout_page_height (self->layout) * self->zoom;

  if (self->mode == W42_VIEW_NORMAL)
    return 0.0;

  return PAGE_GAP + page * (page_h + PAGE_GAP);
}

/* Widget coordinates to a page number plus a point in that page's own
 * unzoomed coordinate system. */
static void
view_widget_to_page (W42View *self,
                     double   wx,
                     double   wy,
                     int     *page,
                     double  *px,
                     double  *py)
{
  double page_h = w42_layout_page_height (self->layout) * self->zoom;
  int n_pages = w42_layout_n_pages (self->layout);
  int index = 0;

  /* Normal view is one continuous page starting at the origin; Page Layout
   * stacks sheets with a gap above and between them. */
  if (self->mode == W42_VIEW_PAGE_LAYOUT)
    {
      index = (int) floor ((wy - PAGE_GAP) / (page_h + PAGE_GAP));
      index = CLAMP (index, 0, n_pages - 1);
    }

  *page = index;
  *px = (wx - view_page_origin_x (self)) / self->zoom;
  *py = (wy - view_page_origin_y (self, index)) / self->zoom;
}

/* ---------------------------------------------------------------------- */
/* Layout plumbing                                                         */
/* ---------------------------------------------------------------------- */

/* The bookmark a table of contents carries, so it can be rebuilt. */
#define TOC_BOOKMARK "_Toc"

static int view_insert_toc_entries (W42View *self);
static GBytes *view_selection_as_rtf (W42View *self);

static void
view_relayout (W42View *self)
{
  if (self->doc == NULL)
    return;

  w42_layout_set_spell_caret (self->layout, self->caret);
  w42_layout_build (self->layout, self->doc);

  self->caret  = w42_pt_clamp_pos (view_pt (self), self->caret);
  self->anchor = w42_pt_clamp_pos (view_pt (self), self->anchor);

  gtk_widget_queue_resize (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_document_changed (W42Document *doc, gpointer data)
{
  W42View *self = data;

  (void) doc;
  view_relayout (self);
  view_state_changed (self);
}

/* A change to the text: reformat, mark the document dirty, tell the window. */
static void
view_edited (W42View *self)
{
  w42_document_set_modified (self->doc, TRUE);

  /* The document announces the change, and every view on it -- this one
   * and any other window's -- lays itself out again on hearing it. */
  w42_document_touch (self->doc);

  view_reset_blink (self);
  view_scroll_to_caret (self);
  view_state_changed (self);
}

static void
view_scroll_to_caret (W42View *self)
{
  GtkWidget *sw;
  GtkAdjustment *vadj;
  int page = 0;
  double x = 0, y = 0, h = 0;
  double top, bottom, value, page_size;

  if (self->doc == NULL)
    return;

  sw = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_SCROLLED_WINDOW);
  if (sw == NULL)
    return;

  if (!w42_layout_pos_to_caret (self->layout, self->caret, &page, &x, &y, &h))
    return;

  vadj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (sw));
  value = gtk_adjustment_get_value (vadj);
  page_size = gtk_adjustment_get_page_size (vadj);

  if (page_size <= 0)
    return;

  top    = view_page_origin_y (self, page) + y * self->zoom;
  bottom = top + h * self->zoom;

  if (top < value)
    gtk_adjustment_set_value (vadj, MAX (0.0, top - PAGE_GAP));
  else if (bottom > value + page_size)
    gtk_adjustment_set_value (vadj, bottom - page_size + PAGE_GAP);
}

/* ---------------------------------------------------------------------- */
/* Formatting                                                              */
/* ---------------------------------------------------------------------- */

void
w42_view_get_char_fmt (W42View *self, W42CharFmt *out)
{
  W42PieceTable *pt;
  const W42Fmt *fmt;
  gsize pos;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (out != NULL);

  pt = view_pt (self);
  if (pt == NULL)
    {
      W42Fmt def;
      w42_fmt_init_default (&def);
      *out = def.ch;
      return;
    }

  pos = fmt_probe (self);
  fmt = w42_ap_table_get (w42_pt_ap_table (pt), w42_pt_ap_at (pt, pos));
  *out = fmt->ch;

  if (!w42_view_has_selection (self) && self->pending_mask != 0)
    {
      const W42CharMask m = self->pending_mask;

      if (m & W42_CHAR_FAMILY)    out->family    = self->pending.family;
      if (m & W42_CHAR_SIZE)      out->size      = self->pending.size;
      if (m & W42_CHAR_BOLD)      out->bold      = self->pending.bold;
      if (m & W42_CHAR_ITALIC)    out->italic    = self->pending.italic;
      if (m & W42_CHAR_UNDERLINE) out->underline = self->pending.underline;
      if (m & W42_CHAR_STRIKEOUT) out->strikeout = self->pending.strikeout;
      if (m & W42_CHAR_COLOR)     out->color     = self->pending.color;
      if (m & W42_CHAR_LANG)      out->lang      = self->pending.lang;
    }
}

W42Align
w42_view_get_align (W42View *self)
{
  W42PieceTable *pt;

  g_return_val_if_fail (W42_IS_VIEW (self), W42_ALIGN_LEFT);

  pt = view_pt (self);
  if (pt == NULL)
    return W42_ALIGN_LEFT;

  return w42_ap_table_get (w42_pt_ap_table (pt),
                           w42_pt_block_ap_at (pt, para_pos (self, self->caret)))->pa.align;
}

void
w42_view_get_para_fmt (W42View *self, W42ParaFmt *out)
{
  W42PieceTable *pt;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (out != NULL);

  pt = view_pt (self);
  if (pt == NULL)
    {
      W42Fmt def;
      w42_fmt_init_default (&def);
      *out = def.pa;
      return;
    }

  *out = w42_ap_table_get (w42_pt_ap_table (pt),
                           w42_pt_block_ap_at (pt, para_pos (self, self->caret)))->pa;
}

void
w42_view_apply_para_fmt (W42View *self, W42ParaMask mask,
                         const W42ParaFmt *value)
{
  /* see para_pos */
  W42PieceTable *pt;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (value != NULL);

  pt = view_pt (self);
  if (pt == NULL)
    return;

  w42_pt_apply_para_fmt (pt, w42_view_has_selection (self) ? sel_start (self) : para_pos (self, sel_start (self)),
                         sel_end (self) - sel_start (self), mask, value);
  view_edited (self);

  self->repeat_para_mask = mask;
  self->repeat_para_fmt  = *value;
  view_record_repeat (self, W42_REPEAT_PARA_FMT);
}

W42ListKind
w42_view_get_list (W42View *self)
{
  W42PieceTable *pt;

  g_return_val_if_fail (W42_IS_VIEW (self), W42_LIST_NONE);

  pt = view_pt (self);
  if (pt == NULL)
    return W42_LIST_NONE;

  return (W42ListKind) w42_ap_table_get (w42_pt_ap_table (pt),
                                         w42_pt_block_ap_at (pt, para_pos (self, self->caret)))->pa.list;
}

void
w42_view_table_merge_cells (W42View *self)
{
  W42PieceTable *pt;
  int t1, r1, c1, t2, r2, c2;
  gsize last;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, sel_start (self), &t1, &r1, &c1))
    return;

  last = MAX (sel_start (self), sel_end (self) - 1);
  if (!w42_pt_cell_at (pt, last, &t2, &r2, &c2) || t2 != t1)
    return;

  /* Down a column: the cells become one tall cell. */
  if (c2 == c1 && r2 > r1)
    {
      if (w42_pt_merge_cells_down (pt, t1, r1, c1, r2 - r1 + 1))
        {
          self->anchor = self->caret = w42_pt_cell_start (pt, t1, r1, c1);
          view_edited (self);
        }
      return;
    }
  if (r2 != r1 || c2 <= c1)
    return;

  w42_pt_table_merge_cells (pt, t1, r1, c1, c2);
  self->anchor = self->caret = w42_pt_cell_start (pt, t1, r1, c1);
  view_edited (self);
}

/* ---------------------------------------------------------------------- */
/* Table of contents                                                       */
/* ---------------------------------------------------------------------- */

static gboolean view_delete_selection (W42View *self);

int
w42_view_insert_toc (W42View *self)
{
  W42PieceTable *pt;
  int made;

  g_return_val_if_fail (W42_IS_VIEW (self), 0);

  pt = view_pt (self);
  if (pt == NULL)
    return 0;

  w42_pt_begin_group (pt);
  view_delete_selection (self);
  made = view_insert_toc_entries (self);
  w42_pt_end_group (pt);
  return made;
}

/* Insert > Update Table of Contents: the table put in earlier is found by
 * the bookmark it carries, taken out, and made again from the headings
 * and page numbers as they now are. */
gboolean
w42_view_update_toc (W42View *self)
{
  W42PieceTable *pt;
  gsize start, end;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_find_bookmark (pt, TOC_BOOKMARK, &start, &end))
    return FALSE;

  w42_pt_begin_group (pt);
  w42_pt_delete (pt, start, end - start);
  self->caret = self->anchor = w42_pt_clamp_pos (pt, start);
  /* The page numbers come from the layout, which must first forget the
   * old table. */
  view_relayout (self);
  view_insert_toc_entries (self);
  w42_pt_end_group (pt);
  return TRUE;
}

static int
view_insert_toc_entries (W42View *self)
{
  W42PieceTable *pt = view_pt (self);
  W42StyleSheet *styles;
  GPtrArray *blocks;
  const GArray *lines;
  const W42PageSetup *page;
  int made = 0;
  gsize at;
  int text_twips;

  styles = w42_pt_stylesheet (pt);
  page = w42_document_page_setup (self->doc);
  text_twips = page->width - page->margin_left - page->margin_right;
  if (w42_page_columns (page) > 1)
    text_twips = (text_twips - (w42_page_columns (page) - 1) * w42_page_column_gap (page))
                 / w42_page_columns (page);

  /* Pages come from the layout as it stands; in Normal view everything is
   * on page 1, so the numbers are what Page Layout would show only when
   * that is the view.  Word 6 had the same limitation in reverse. */
  blocks = w42_pt_snapshot_blocks (pt);
  lines = w42_layout_lines (self->layout);

  /* The entries go in at the start of the caret's paragraph, each as a
   * paragraph of its own before it. */
  at = w42_pt_paragraph_start (pt, self->caret) + 1;

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42Fmt *fmt = w42_ap_table_get (w42_pt_ap_table (pt), block->ap);
      int level = w42_stylesheet_outline (styles, fmt->pa.style);
      int page_no = 1;
      GString *entry;
      W42Fmt efmt;
      W42ApIdx ap;
      gsize n;

      if (level <= 0 || block->note >= 0 || block->table >= 0 || block->text->len == 0)
        continue;
      if (block->start_pos >= at - 1 && block->start_pos < at + 1)
        continue;       /* the paragraph the table goes into */

      for (guint i = 0; i < lines->len; i++)
        if (g_array_index (lines, W42LineBox, i).block == (int) b)
          {
            page_no = g_array_index (lines, W42LineBox, i).page + 1;
            break;
          }

      entry = g_string_new (NULL);
      for (const char *p = block->text->str; *p; p = g_utf8_next_char (p))
        {
          gunichar c = g_utf8_get_char (p);
          if (c != 0xFFFC && c != '\t')
            g_string_append_unichar (entry, c);
        }
      g_string_append_printf (entry, "\t%d", page_no);

      w42_fmt_init_default (&efmt);
      efmt.pa.indent_left = (level - 1) * 360;
      /* The page number sits at a right stop at the margin, with the
       * dots running out to it, as a table of contents has always been
       * set. */
      w42_para_fmt_set_tab_leader (&efmt.pa, text_twips, W42_TAB_RIGHT,
                                   W42_TAB_LEAD_DOT);
      /* The whole table carries one bookmark, so Update can find it. */
      efmt.ch.bookmark = g_intern_static_string (TOC_BOOKMARK);
      ap = w42_ap_table_intern (w42_pt_ap_table (pt), &efmt);

      n = g_utf8_strlen (entry->str, -1);
      w42_pt_insert_text (pt, at, entry->str, ap);
      w42_pt_insert_block (pt, at + n, ap);
      /* the entry's own paragraph mark takes the entry's formatting */
      w42_pt_apply_para_fmt (pt, at, 0, W42_PARA_INDENT_LEFT | W42_PARA_TABS, &efmt.pa);
      at += n + 1;
      made++;
      g_string_free (entry, TRUE);
    }

  g_ptr_array_free (blocks, TRUE);

  if (made > 0)
    {
      self->caret = self->anchor = w42_pt_clamp_pos (pt, at);
      view_edited (self);
    }
  return made;
}

int
w42_view_autoformat (W42View *self, const W42AutoFormat *what)
{
  W42PieceTable *pt;
  int changed;

  g_return_val_if_fail (W42_IS_VIEW (self), 0);
  g_return_val_if_fail (what != NULL, 0);

  pt = view_pt (self);
  if (pt == NULL)
    return 0;

  changed = w42_pt_autoformat (pt, what);
  if (changed > 0)
    {
      self->caret = self->anchor = w42_pt_clamp_pos (pt, self->caret);
      view_edited (self);
    }
  return changed;
}

/* ---------------------------------------------------------------------- */
/* Insert > Index                                                          */
/* ---------------------------------------------------------------------- */

/* An index entry is a run of text marked as a field whose code is XE, or
 * XE:term when the entry is filed under something other than the words
 * on the page.  The text stays where it is and reads as it did; the
 * index gathers the marked runs and says which page each is on. */
gboolean
w42_view_mark_index_entry (W42View *self, const char *term)
{
  W42PieceTable *pt;
  W42CharFmt want;
  char *code;
  char *marked;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);

  pt = view_pt (self);
  if (pt == NULL || !w42_view_has_selection (self))
    return FALSE;

  marked = w42_view_get_selected_text (self);
  if (marked == NULL || *marked == '\0')
    {
      g_free (marked);
      return FALSE;
    }

  /* The term is kept in the code only when it differs from the words
   * that are marked, so the common case costs nothing. */
  if (term != NULL && *term != '\0' && !g_str_equal (term, marked))
    code = g_strconcat (W42_INDEX_FIELD ":", term, NULL);
  else
    code = g_strdup (W42_INDEX_FIELD);

  memset (&want, 0, sizeof want);
  want.field = g_intern_string (code);
  w42_pt_apply_char_fmt (pt, sel_start (self), sel_end (self) - sel_start (self),
                         W42_CHAR_FIELD, &want);
  view_edited (self);

  g_free (code);
  g_free (marked);
  return TRUE;
}

int
w42_view_insert_index (W42View *self)
{
  W42PieceTable *pt;
  gsize at, start, end, after = 0;
  int made;

  g_return_val_if_fail (W42_IS_VIEW (self), 0);

  pt = view_pt (self);
  if (pt == NULL)
    return 0;

  w42_pt_begin_group (pt);

  /* An index already there is replaced where it stands, as Update Table
   * of Contents replaces the table. */
  if (w42_pt_find_bookmark (pt, W42_INDEX_BOOKMARK, &start, &end))
    {
      w42_pt_delete (pt, start, end - start);
      self->caret = self->anchor = w42_pt_clamp_pos (pt, start);
      view_relayout (self);
    }
  else
    {
      view_delete_selection (self);
    }

  at = w42_pt_paragraph_start (pt, self->caret) + 1;
  made = w42_index_build (pt, self->layout, w42_document_page_setup (self->doc),
                          at, &after);
  w42_pt_end_group (pt);

  if (made > 0)
    {
      self->caret = self->anchor = w42_pt_clamp_pos (pt, after);
      view_edited (self);
    }
  return made;
}

/* ---------------------------------------------------------------------- */
/* Insert > Cross-reference                                                */
/* ---------------------------------------------------------------------- */

gboolean
w42_view_insert_cross_reference (W42View *self, const char *bookmark,
                                 gboolean page_number)
{
  W42PieceTable *pt;
  gsize start, end;
  char *text;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  g_return_val_if_fail (bookmark != NULL, FALSE);

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_find_bookmark (pt, bookmark, &start, &end))
    return FALSE;

  if (page_number)
    {
      /* The page of the line holding the bookmark's first character, as
       * the layout stands: in Normal view that is page 1 for everything. */
      GPtrArray *blocks = w42_pt_snapshot_blocks (pt);
      const GArray *lines = w42_layout_lines (self->layout);
      int page = 1, block_index = -1;

      for (guint b = 0; b < blocks->len; b++)
        {
          const W42Block *block = g_ptr_array_index (blocks, b);

          if (block->start_pos <= start)
            block_index = (int) b;
          else
            break;
        }
      if (block_index >= 0)
        {
          const W42Block *block = g_ptr_array_index (blocks, block_index);
          gsize chars = start > block->start_pos ? start - block->start_pos - 1 : 0;
          guint byte = 0;

          if (chars < (gsize) g_utf8_strlen (block->text->str, -1))
            byte = (guint) (g_utf8_offset_to_pointer (block->text->str, (glong) chars)
                            - block->text->str);

          for (guint i = 0; i < lines->len; i++)
            {
              const W42LineBox *line = &g_array_index (lines, W42LineBox, i);

              if (line->block != block_index)
                continue;
              page = line->page + 1;
              if (byte < line->start_index + line->length)
                break;
            }
        }
      g_ptr_array_free (blocks, TRUE);
      text = g_strdup_printf ("%d", page);
    }
  else
    {
      GString *plain = g_string_new (NULL);
      char *raw = w42_pt_get_text (pt, start, end - start);

      /* Paragraph marks and pictures do not belong in a reference. */
      for (const char *p = raw; *p; p = g_utf8_next_char (p))
        {
          gunichar c = g_utf8_get_char (p);

          if (c == '\n')
            g_string_append_c (plain, ' ');
          else if (c != 0xFFFC)
            g_string_append_unichar (plain, c);
        }
      g_free (raw);
      text = g_string_free (plain, FALSE);
    }

  if (*text != '\0')
    w42_view_insert_text (self, text);
  g_free (text);
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Format > Change Case                                                    */
/* ---------------------------------------------------------------------- */

/* Each character of the selection is replaced by its new case in turn,
 * keeping its formatting, so that a mixed selection stays mixed. */
void
w42_view_change_case (W42View *self, W42CaseKind kind)
{
  W42PieceTable *pt;
  gsize start, end, pos;
  gboolean sentence_start = TRUE, word_start = TRUE;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_view_has_selection (self))
    return;

  start = sel_start (self);
  end = sel_end (self);

  w42_pt_begin_group (pt);
  for (pos = start; pos < end; pos++)
    {
      char *old = w42_pt_get_text (pt, pos, 1);
      gunichar c = (old != NULL && *old != '\0') ? g_utf8_get_char (old) : 0;
      gunichar out = c;
      char buf[8];
      int n;

      if (c == 0 || c == '\n' || c == 0xFFFC)
        {
          sentence_start = word_start = TRUE;
          g_free (old);
          continue;
        }

      switch (kind)
        {
        case W42_CASE_LOWER:
          out = g_unichar_tolower (c);
          break;
        case W42_CASE_UPPER:
          out = g_unichar_toupper (c);
          break;
        case W42_CASE_TITLE:
          out = word_start ? g_unichar_totitle (c) : g_unichar_tolower (c);
          break;
        case W42_CASE_SENTENCE:
          out = sentence_start ? g_unichar_toupper (c) : g_unichar_tolower (c);
          break;
        case W42_CASE_TOGGLE:
          out = g_unichar_isupper (c) ? g_unichar_tolower (c)
              : g_unichar_islower (c) ? g_unichar_toupper (c) : c;
          break;
        }

      if (g_unichar_isalnum (c))
        sentence_start = FALSE;
      else if (c == '.' || c == '!' || c == '?')
        sentence_start = TRUE;
      word_start = !g_unichar_isalnum (c) && c != '\'';

      if (out != c)
        {
          W42ApIdx ap = w42_pt_ap_at (pt, pos + 1);

          n = g_unichar_to_utf8 (out, buf);
          buf[n] = '\0';
          w42_pt_delete (pt, pos, 1);
          w42_pt_insert_text (pt, pos, buf, ap);
        }
      g_free (old);
    }
  w42_pt_end_group (pt);

  self->anchor = start;
  self->caret = end;
  view_edited (self);

  self->repeat_case = (guint8) kind;
  view_record_repeat (self, W42_REPEAT_CASE);
}

/* ---------------------------------------------------------------------- */
/* Hyperlinks and bookmarks                                                */
/* ---------------------------------------------------------------------- */

void
w42_view_set_link (W42View *self, const char *url, const char *text)
{
  W42PieceTable *pt;
  W42CharFmt want;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;

  memset (&want, 0, sizeof want);
  want.link = (url != NULL && *url != '\0') ? g_intern_string (url) : NULL;

  if (!w42_view_has_selection (self))
    {
      /* Nothing selected: the address itself, or the text given, becomes
       * the link. */
      const char *body = (text != NULL && *text != '\0') ? text : url;
      W42Fmt fmt;
      gsize n;

      if (body == NULL || *body == '\0')
        return;
      fmt = *w42_ap_table_get (w42_pt_ap_table (pt), w42_pt_ap_at (pt, self->caret));
      fmt.ch.link = want.link;
      n = g_utf8_strlen (body, -1);
      w42_pt_begin_group (pt);
      w42_pt_insert_text (pt, self->caret, body,
                          w42_ap_table_intern (w42_pt_ap_table (pt), &fmt));
      w42_pt_end_group (pt);
      self->caret += n;
      self->anchor = self->caret;
      view_edited (self);
      return;
    }

  w42_pt_apply_char_fmt (pt, sel_start (self), sel_end (self) - sel_start (self),
                         W42_CHAR_LINK, &want);
  view_edited (self);
}

const char *
w42_view_get_link (W42View *self)
{
  W42PieceTable *pt;
  const W42Fmt *fmt;

  g_return_val_if_fail (W42_IS_VIEW (self), NULL);

  pt = view_pt (self);
  if (pt == NULL)
    return NULL;

  fmt = w42_ap_table_get (w42_pt_ap_table (pt), w42_pt_ap_at (pt, fmt_probe (self)));
  return fmt->ch.link;
}

typedef struct {
  GtkWindow *window;   /* or NULL, both refs owned here */
  char      *url;
} FollowLink;

static void
follow_link_response (GObject *source, GAsyncResult *result, gpointer data)
{
  FollowLink *fl = data;

  if (gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), result, NULL) == 1)
    {
      GtkUriLauncher *launcher = gtk_uri_launcher_new (fl->url);

      gtk_uri_launcher_launch (launcher, fl->window, NULL, NULL, NULL);
      g_object_unref (launcher);
    }
  g_clear_object (&fl->window);
  g_free (fl->url);
  g_free (fl);
}

gboolean
w42_view_follow_link (W42View *self, const char *url)
{
  GtkRoot *root;
  GtkWindow *window;
  const char *scheme;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);

  if (url == NULL || *url == '\0')
    return FALSE;

  root = gtk_widget_get_root (GTK_WIDGET (self));
  window = GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL;

  /* The address is the document's, not the user's.  The web and mail are
   * what a link in a document means; anything else -- file:, a UNC path,
   * whatever scheme has a handler here -- is shown in full and asked
   * about first, because the text the user clicked need not look like
   * the place it goes. */
  scheme = g_uri_peek_scheme (url);
  if (scheme != NULL && (g_str_equal (scheme, "http") || g_str_equal (scheme, "https") ||
                         g_str_equal (scheme, "mailto")))
    {
      GtkUriLauncher *launcher = gtk_uri_launcher_new (url);

      gtk_uri_launcher_launch (launcher, window, NULL, NULL, NULL);
      g_object_unref (launcher);
    }
  else
    {
      const char *buttons[] = { "Cancel", "Open", NULL };
      GtkAlertDialog *dialog = gtk_alert_dialog_new ("Open this link?");
      FollowLink *fl = g_new0 (FollowLink, 1);

      fl->window = window != NULL ? g_object_ref (window) : NULL;
      fl->url = g_strdup (url);
      gtk_alert_dialog_set_detail (dialog, url);
      gtk_alert_dialog_set_buttons (dialog, buttons);
      gtk_alert_dialog_set_cancel_button (dialog, 0);
      gtk_alert_dialog_set_default_button (dialog, 0);
      gtk_alert_dialog_choose (dialog, window, NULL, follow_link_response, fl);
      g_object_unref (dialog);
    }
  return TRUE;
}

void
w42_view_set_comment (W42View *self, const char *text)
{
  W42PieceTable *pt;
  W42CharFmt want;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_view_has_selection (self))
    return;

  memset (&want, 0, sizeof want);
  want.comment = (text != NULL && *text != '\0') ? g_intern_string (text) : NULL;
  w42_pt_apply_char_fmt (pt, sel_start (self), sel_end (self) - sel_start (self),
                         W42_CHAR_COMMENT, &want);
  view_edited (self);
}

const char *
w42_view_get_comment (W42View *self)
{
  W42PieceTable *pt;
  const W42Fmt *fmt;

  g_return_val_if_fail (W42_IS_VIEW (self), NULL);

  pt = view_pt (self);
  if (pt == NULL)
    return NULL;

  fmt = w42_ap_table_get (w42_pt_ap_table (pt), w42_pt_ap_at (pt, sel_start (self) + 1));
  if (fmt->ch.comment == NULL)
    fmt = w42_ap_table_get (w42_pt_ap_table (pt), w42_pt_ap_at (pt, fmt_probe (self)));
  return fmt->ch.comment;
}

void
w42_view_set_show_marks (W42View *self, gboolean show)
{
  g_return_if_fail (W42_IS_VIEW (self));
  w42_layout_set_show_marks (self->layout, show);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
w42_view_set_track_changes (W42View *self, gboolean on)
{
  g_return_if_fail (W42_IS_VIEW (self));
  self->track_changes = on;
}

gboolean
w42_view_get_track_changes (W42View *self)
{
  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  return self->track_changes;
}

gboolean
w42_view_resolve_revisions (W42View *self, gboolean accept)
{
  W42PieceTable *pt;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_resolve_revisions (pt, accept))
    return FALSE;
  self->caret = w42_pt_clamp_pos (pt, self->caret);
  self->anchor = self->caret;
  view_edited (self);
  return TRUE;
}

void
w42_view_set_bookmark (W42View *self, const char *name)
{
  W42PieceTable *pt;
  W42CharFmt want;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_view_has_selection (self))
    return;

  memset (&want, 0, sizeof want);
  want.bookmark = (name != NULL && *name != '\0') ? g_intern_string (name) : NULL;
  w42_pt_apply_char_fmt (pt, sel_start (self), sel_end (self) - sel_start (self),
                         W42_CHAR_BOOKMARK, &want);
  view_edited (self);
}

gboolean
w42_view_go_to_bookmark (W42View *self, const char *name)
{
  W42PieceTable *pt;
  gsize start = 0, end = 0;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);

  pt = view_pt (self);
  if (pt == NULL || name == NULL || !w42_pt_find_bookmark (pt, name, &start, &end))
    return FALSE;

  w42_view_select_range (self, start, end);
  return TRUE;
}

void
w42_view_insert_footnote (W42View *self)
{
  W42PieceTable *pt;
  W42ApIdx ap;
  gsize body;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;

  w42_pt_begin_group (pt);
  view_delete_selection (self);
  ap = w42_pt_block_ap_at (pt, para_pos (self, self->caret));
  body = w42_pt_insert_footnote (pt, self->caret, ap);
  w42_pt_apply_style (pt, body, 0, "Normal");
  w42_pt_end_group (pt);

  self->caret = self->anchor = w42_pt_clamp_pos (pt, body);
  view_edited (self);
}

void
w42_view_insert_endnote (W42View *self)
{
  W42PieceTable *pt;
  W42ApIdx ap;
  gsize body;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;

  w42_pt_begin_group (pt);
  view_delete_selection (self);
  ap = w42_pt_block_ap_at (pt, para_pos (self, self->caret));
  body = w42_pt_insert_endnote (pt, self->caret, ap);
  w42_pt_apply_style (pt, body, 0, "Normal");
  w42_pt_end_group (pt);

  self->caret = self->anchor = w42_pt_clamp_pos (pt, body);
  view_edited (self);
}

gboolean
w42_view_go_to_note (W42View *self)
{
  W42PieceTable *pt;
  int id;
  gsize target = (gsize) -1;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);

  pt = view_pt (self);
  if (pt == NULL)
    return FALSE;

  /* On a mark, or just after one: to the note.  In a note: to its mark. */
  id = w42_pt_footnote_at (pt, self->caret);
  if (id < 0 && self->caret > 0)
    id = w42_pt_footnote_at (pt, self->caret - 1);
  if (id >= 0)
    target = w42_pt_note_body (pt, id);
  else
    {
      GPtrArray *blocks = w42_pt_snapshot_blocks (pt);
      int b = w42_layout_block_at_pos (self->layout, self->caret);

      if (b >= 0 && (guint) b < blocks->len)
        {
          const W42Block *block = g_ptr_array_index (blocks, b);

          if (block->note >= 0)
            target = w42_pt_note_reference (pt, block->note);
          if (target != (gsize) -1)
            target += 1;      /* just after the mark */
        }
      g_ptr_array_free (blocks, TRUE);
    }

  if (target == (gsize) -1)
    return FALSE;
  view_set_caret (self, target, FALSE);
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Go To                                                                   */
/* ---------------------------------------------------------------------- */

/* The document position at which a laid-out line begins. */
static gsize
line_box_pos (W42Layout *layout, const W42LineBox *box)
{
  const W42Block *block = g_ptr_array_index (w42_layout_blocks (layout),
                                             box->block);
  const char *text = block->text->str;

  return block->start_pos + 1 +
         (gsize) g_utf8_pointer_to_offset (text, text + box->start_index);
}

void
w42_view_go_to_page (W42View *self, int page)
{
  const GArray *lines;
  const W42LineBox *hit = NULL;

  g_return_if_fail (W42_IS_VIEW (self));

  if (view_pt (self) == NULL)
    return;

  lines = w42_layout_lines (self->layout);
  for (guint i = 0; i < lines->len; i++)
    {
      const W42LineBox *box = &g_array_index (lines, W42LineBox, i);

      if (box->page >= page - 1)
        {
          hit = box;
          break;
        }
      hit = box;    /* past the end: the last line there is */
    }

  if (hit != NULL)
    view_set_caret (self, line_box_pos (self->layout, hit), FALSE);
}

void
w42_view_go_to_line (W42View *self, int line)
{
  const GArray *lines;
  guint index;

  g_return_if_fail (W42_IS_VIEW (self));

  if (view_pt (self) == NULL)
    return;

  lines = w42_layout_lines (self->layout);
  if (lines->len == 0)
    return;

  index = (guint) CLAMP (line - 1, 0, (int) lines->len - 1);
  view_set_caret (self,
                  line_box_pos (self->layout,
                                &g_array_index (lines, W42LineBox, index)),
                  FALSE);
}

int
w42_view_line_count (W42View *self)
{
  g_return_val_if_fail (W42_IS_VIEW (self), 0);
  return (int) w42_layout_lines (self->layout)->len;
}

/* ---------------------------------------------------------------------- */
/* Spelling                                                                */
/* ---------------------------------------------------------------------- */

/* Tools > Options: correct as you type, or leave what is typed alone. */
void
w42_view_set_autocorrect (W42View *self, gboolean on)
{
  g_return_if_fail (W42_IS_VIEW (self));
  self->autocorrect = on;
}

gboolean
w42_view_get_autocorrect (W42View *self)
{
  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  return self->autocorrect;
}

void
w42_view_set_spell (W42View *self, W42Spell *spell)
{
  g_return_if_fail (W42_IS_VIEW (self));

  self->spell = spell;
  w42_layout_set_spell (self->layout, spell);
  view_relayout (self);
}

void
w42_view_spell_refresh (W42View *self)
{
  g_return_if_fail (W42_IS_VIEW (self));
  view_relayout (self);
}

/* The language marked on the run the byte at `at` is in. */
static const char *
block_run_lang (W42PieceTable *pt, const W42Block *block, gsize at)
{
  W42ApTable *aps = w42_pt_ap_table (pt);

  for (guint i = 0; i < block->runs->len; i++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, i);

      if (at >= run->byte_offset && at < run->byte_offset + run->n_bytes)
        return w42_ap_table_get (aps, run->ap)->ch.lang;
    }
  return NULL;
}

gboolean
w42_view_find_misspelling (W42View *self, W42Spell *spell, gsize from,
                           gsize *start, gsize *end)
{
  W42PieceTable *pt;
  GPtrArray *blocks;
  gboolean found = FALSE;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  g_return_val_if_fail (spell != NULL, FALSE);

  pt = view_pt (self);
  if (pt == NULL)
    return FALSE;

  blocks = w42_pt_snapshot_blocks (pt);

  for (guint b = 0; b < blocks->len && !found; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const char *text = block->text->str;
      gsize base = block->start_pos + 1;
      gsize s = 0, e = 0;

      if (base + g_utf8_strlen (text, -1) < from)
        continue;

      while (w42_spell_next_word (text, block->text->len, &s, &e))
        {
          gsize ws = base + (gsize) g_utf8_pointer_to_offset (text, text + s);

          if (ws < from)
            continue;
          if (w42_spell_check_lang (spell, block_run_lang (pt, block, s),
                                    text + s, (gssize) (e - s)))
            continue;

          *start = ws;
          *end = base + (gsize) g_utf8_pointer_to_offset (text, text + e);
          found = TRUE;
          break;
        }
    }

  g_ptr_array_free (blocks, TRUE);
  return found;
}

#define LIST_INDENT 360   /* a quarter inch, Word's own */

gboolean
w42_view_list_level_by (W42View *self, int delta)
{
  W42PieceTable *pt;
  W42ParaFmt want;
  int level;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);

  pt = view_pt (self);
  if (pt == NULL)
    return FALSE;

  want = w42_ap_table_get (w42_pt_ap_table (pt),
                           w42_pt_block_ap_at (pt, para_pos (self, sel_start (self))))->pa;
  if (want.list == W42_LIST_NONE)
    return FALSE;
  level = CLAMP ((int) want.list_level + delta, 0, 8);
  if (level == (int) want.list_level)
    return TRUE;

  /* The indent follows the level, a quarter inch a step, as Word's did. */
  want.indent_left = MAX (want.indent_left + (level - (int) want.list_level) * LIST_INDENT, LIST_INDENT);
  want.list_level = (guint8) level;
  w42_pt_apply_para_fmt (pt, w42_view_has_selection (self) ? sel_start (self) : para_pos (self, sel_start (self)), sel_end (self) - sel_start (self),
                         W42_PARA_LIST | W42_PARA_INDENT_LEFT, &want);
  view_edited (self);
  return TRUE;
}

void
w42_view_set_list_start (W42View *self, int start)
{
  W42PieceTable *pt;
  W42ParaFmt want;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;

  want = w42_ap_table_get (w42_pt_ap_table (pt),
                           w42_pt_block_ap_at (pt, para_pos (self, sel_start (self))))->pa;
  want.list_start = (guint8) CLAMP (start, 0, 255);
  w42_pt_apply_para_fmt (pt, w42_view_has_selection (self) ? sel_start (self) : para_pos (self, sel_start (self)), sel_end (self) - sel_start (self),
                         W42_PARA_LIST, &want);
  view_edited (self);
}

void
w42_view_set_list (W42View *self, W42ListKind kind)
{
  W42PieceTable *pt;
  W42ParaFmt cur, want;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;

  cur = w42_ap_table_get (w42_pt_ap_table (pt),
                          w42_pt_block_ap_at (pt, para_pos (self, sel_start (self))))->pa;

  memset (&want, 0, sizeof want);
  want.list         = (guint8) kind;
  want.indent_left  = cur.indent_left;
  want.indent_first = cur.indent_first;

  if (kind != W42_LIST_NONE && cur.list == W42_LIST_NONE)
    {
      want.indent_left  = cur.indent_left + LIST_INDENT;
      want.indent_first = -LIST_INDENT;
    }
  else if (kind == W42_LIST_NONE && cur.list != W42_LIST_NONE)
    {
      want.indent_left  = MAX (0, cur.indent_left - LIST_INDENT);
      want.indent_first = 0;
    }

  w42_pt_apply_para_fmt (pt, w42_view_has_selection (self) ? sel_start (self) : para_pos (self, sel_start (self)),
                         sel_end (self) - sel_start (self),
                         W42_PARA_LIST | W42_PARA_INDENT_LEFT |
                         W42_PARA_INDENT_FIRST, &want);
  view_edited (self);
}

/* Apply to the selection, or remember for the next keystroke. */
static void
view_apply_char_fmt (W42View *self, W42CharMask mask, const W42CharFmt *value)
{
  W42PieceTable *pt = view_pt (self);

  if (pt == NULL)
    return;

  if (w42_view_has_selection (self))
    {
      w42_pt_apply_char_fmt (pt, sel_start (self),
                             sel_end (self) - sel_start (self), mask, value);
      view_edited (self);
      return;
    }

  self->pending_mask |= mask;

  if (mask & W42_CHAR_FAMILY)    self->pending.family    = value->family;
  if (mask & W42_CHAR_SIZE)      self->pending.size      = value->size;
  if (mask & W42_CHAR_BOLD)      self->pending.bold      = value->bold;
  if (mask & W42_CHAR_ITALIC)    self->pending.italic    = value->italic;
  if (mask & W42_CHAR_UNDERLINE) self->pending.underline = value->underline;
  if (mask & W42_CHAR_STRIKEOUT) self->pending.strikeout = value->strikeout;
  if (mask & W42_CHAR_COLOR)     self->pending.color     = value->color;
  if (mask & W42_CHAR_LANG)      self->pending.lang      = value->lang;

  view_state_changed (self);
}

static void
view_toggle (W42View *self, W42CharMask mask)
{
  W42CharFmt now;
  W42CharFmt want;

  w42_view_get_char_fmt (self, &now);

  memset (&want, 0, sizeof want);
  switch (mask)
    {
    case W42_CHAR_BOLD:      want.bold      = !now.bold;      break;
    case W42_CHAR_ITALIC:    want.italic    = !now.italic;    break;
    case W42_CHAR_UNDERLINE: want.underline = !now.underline; break;
    default:                 return;
    }

  view_apply_char_fmt (self, mask, &want);
}

void
w42_view_apply_char_fmt (W42View *self, W42CharMask mask, const W42CharFmt *value)
{
  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (value != NULL);

  {
    /* Formatting a selection is repeatable; toggling formatting for the
     * text yet to be typed is not an edit, and is not recorded. */
    gboolean edited = w42_view_has_selection (self);

    view_apply_char_fmt (self, mask, value);
    if (edited)
      {
        self->repeat_char_mask = mask;
        self->repeat_char_fmt  = *value;
        view_record_repeat (self, W42_REPEAT_CHAR_FMT);
      }
  }
}

void
w42_view_toggle_bold (W42View *self)
{
  g_return_if_fail (W42_IS_VIEW (self));
  view_toggle (self, W42_CHAR_BOLD);
}

void
w42_view_toggle_italic (W42View *self)
{
  g_return_if_fail (W42_IS_VIEW (self));
  view_toggle (self, W42_CHAR_ITALIC);
}

void
w42_view_toggle_underline (W42View *self)
{
  g_return_if_fail (W42_IS_VIEW (self));
  view_toggle (self, W42_CHAR_UNDERLINE);
}

void
w42_view_set_font_family (W42View *self, const char *family)
{
  W42CharFmt want;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (family != NULL);

  memset (&want, 0, sizeof want);
  want.family = g_intern_string (family);
  view_apply_char_fmt (self, W42_CHAR_FAMILY, &want);
}

void
w42_view_set_font_size (W42View *self, int half_points)
{
  W42CharFmt want;

  g_return_if_fail (W42_IS_VIEW (self));

  memset (&want, 0, sizeof want);
  want.size = half_points;
  view_apply_char_fmt (self, W42_CHAR_SIZE, &want);
}

void
w42_view_set_align (W42View *self, W42Align align)
{
  W42PieceTable *pt;
  W42ParaFmt want;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;

  memset (&want, 0, sizeof want);
  want.align = align;

  w42_pt_apply_para_fmt (pt, w42_view_has_selection (self) ? sel_start (self) : para_pos (self, sel_start (self)),
                         sel_end (self) - sel_start (self),
                         W42_PARA_ALIGN, &want);
  view_edited (self);
}

const char *
w42_view_get_style (W42View *self)
{
  W42PieceTable *pt;
  const char *style;

  g_return_val_if_fail (W42_IS_VIEW (self), "Normal");

  pt = view_pt (self);
  if (pt == NULL)
    return "Normal";

  style = w42_ap_table_get (w42_pt_ap_table (pt),
                            w42_pt_block_ap_at (pt, para_pos (self, self->caret)))->pa.style;
  return style != NULL ? style : "Normal";
}

void
w42_view_apply_style (W42View *self, const char *name)
{
  W42PieceTable *pt;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (name != NULL);

  pt = view_pt (self);
  if (pt == NULL)
    return;

  {
    /* A character style goes on the selection, or on what is typed
     * next, the way Bold does. */
    const W42Style *style = w42_stylesheet_find (w42_pt_stylesheet (pt), name);

    if (style != NULL && style->character)
      {
        w42_view_apply_char_fmt (self,
                                 W42_CHAR_FAMILY | W42_CHAR_SIZE | W42_CHAR_BOLD |
                                 W42_CHAR_ITALIC | W42_CHAR_UNDERLINE | W42_CHAR_STRIKEOUT |
                                 W42_CHAR_COLOR | W42_CHAR_SMALLCAPS | W42_CHAR_ALLCAPS |
                                 W42_CHAR_SCRIPT,
                                 &style->ch);
        return;
      }
  }

  w42_pt_apply_style (pt, sel_start (self),
                      sel_end (self) - sel_start (self), name);
  view_edited (self);

  self->repeat_style = g_intern_string (name);
  view_record_repeat (self, W42_REPEAT_STYLE);
}

/* ---------------------------------------------------------------------- */
/* Editing                                                                 */
/* ---------------------------------------------------------------------- */

static W42ApIdx
view_effective_ap (W42View *self)
{
  W42PieceTable *pt = view_pt (self);
  W42Fmt fmt = *w42_ap_table_get (w42_pt_ap_table (pt),
                                  w42_pt_ap_at (pt, self->caret));
  W42CharFmt want;

  if (self->pending_mask == 0)
    return w42_pt_ap_at (pt, self->caret);

  w42_view_get_char_fmt (self, &want);
  fmt.ch = want;

  return w42_ap_table_intern (w42_pt_ap_table (pt), &fmt);
}

/* With revisions being marked, deleting strikes the text through and
 * leaves it, unless it is text inserted under the same marking, which
 * simply goes.  Returns whether the range was marked rather than removed. */
static gboolean
view_tracked_delete (W42View *self, gsize start, gsize end)
{
  W42PieceTable *pt = view_pt (self);
  const W42Fmt *fmt;
  W42CharFmt want;

  if (!self->track_changes)
    return FALSE;

  fmt = w42_ap_table_get (w42_pt_ap_table (pt), w42_pt_ap_at (pt, start + 1));
  if (fmt->ch.revision == 1)
    return FALSE;

  memset (&want, 0, sizeof want);
  want.revision = 2;
  w42_pt_apply_char_fmt (pt, start, end - start, W42_CHAR_REVISION, &want);
  return TRUE;
}

static gboolean
view_delete_selection (W42View *self)
{
  W42PieceTable *pt = view_pt (self);
  gsize start, end;

  if (!w42_view_has_selection (self))
    return FALSE;

  start = sel_start (self);
  end   = sel_end (self);

  if (view_tracked_delete (self, start, end))
    {
      self->caret = self->anchor = end;
      return TRUE;
    }

  w42_pt_delete (pt, start, end - start);
  self->caret = self->anchor = w42_pt_clamp_pos (pt, start);

  return TRUE;
}

/* Drops the dragged selection at target: the text moves there, or a copy
 * of it goes there with Ctrl held, formatting and all, as one undo step.
 * A drop back on the selection itself is the drag cancelled.  With
 * revisions being marked, the move marks the old text deleted and the
 * new inserted, as Word did. */
static void
view_drop_text (W42View *self, gsize target, gboolean copy)
{
  W42PieceTable *pt = view_pt (self);
  W42PieceTable *frag;
  gsize start, end, n;

  if (pt == NULL || !w42_view_has_selection (self))
    return;

  start = sel_start (self);
  end   = sel_end (self);
  if (target >= start && target <= end)
    return;

  frag = w42_pt_extract (pt, start, end - start);

  w42_pt_begin_group (pt);
  if (!copy && !view_tracked_delete (self, start, end))
    {
      w42_pt_delete (pt, start, end - start);
      if (target > end)
        target -= end - start;
    }
  target = w42_pt_clamp_pos (pt, target);
  n = w42_pt_insert_fragment (pt, target, frag);
  if (self->track_changes && n > 0)
    {
      W42CharFmt want;

      memset (&want, 0, sizeof want);
      want.revision = 1;
      w42_pt_apply_char_fmt (pt, target, n, W42_CHAR_REVISION, &want);
    }
  w42_pt_end_group (pt);
  w42_pt_free (frag);

  /* The dropped text stays selected, as Word left it. */
  self->anchor = w42_pt_clamp_pos (pt, target);
  self->caret  = w42_pt_clamp_pos (pt, target + n);
  view_edited (self);
}

void
w42_view_insert_text (W42View *self, const char *utf8)
{
  W42PieceTable *pt;
  W42ApIdx ap;
  gboolean grouped;
  gboolean repeat_continues;
  gsize repeat_pos0 = 0;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (utf8 != NULL);

  pt = view_pt (self);
  if (pt == NULL || *utf8 == '\0')
    return;

  /* For Edit > Repeat: whether this insert carries on the recorded run
   * of typing, or begins one.  It carries on when nothing else has been
   * done since (the stamp still matches) and, checked at the end, the
   * insert coalesced into the same undo record. */
  repeat_continues = self->repeat_kind == W42_REPEAT_TYPING &&
                     view_repeat_stamp_current (self);
  w42_pt_undo_state (pt, &repeat_pos0, NULL);

  ap = view_effective_ap (self);

  grouped = w42_view_has_selection (self);
  if (grouped)
    w42_pt_begin_group (pt);

  view_delete_selection (self);

  w42_pt_insert_text (pt, self->caret, utf8, ap);
  if (self->track_changes)
    {
      W42CharFmt want;

      memset (&want, 0, sizeof want);
      want.revision = 1;
      w42_pt_apply_char_fmt (pt, self->caret, g_utf8_strlen (utf8, -1),
                             W42_CHAR_REVISION, &want);
    }
  self->caret += g_utf8_strlen (utf8, -1);
  self->anchor = self->caret;

  if (grouped)
    w42_pt_end_group (pt);

  self->pending_mask = 0;
  view_edited (self);

  {
    gsize pos1 = 0;

    w42_pt_undo_state (pt, &pos1, NULL);
    if (self->repeat_text == NULL)
      self->repeat_text = g_string_new (NULL);
    if (repeat_continues && pos1 == repeat_pos0)
      g_string_append (self->repeat_text, utf8);
    else
      g_string_assign (self->repeat_text, utf8);
    view_record_repeat (self, W42_REPEAT_TYPING);
  }
}

/* Insert > Caption: a paragraph of its own under the caret's, in the
 * Caption style, with the label already in it.  All of it is one undo
 * step, and it goes below the paragraph the caret is in wherever in it
 * the caret happens to be. */
/* Whether the caret is in the text of a footnote or endnote rather than
 * in the body: some commands have nothing to do down there. */
gboolean
w42_view_caret_in_note (W42View *self)
{
  GPtrArray *blocks;
  int b;
  gboolean note = FALSE;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  if (view_pt (self) == NULL)
    return FALSE;

  blocks = w42_pt_snapshot_blocks (view_pt (self));
  b = w42_layout_block_at_pos (self->layout, self->caret);
  if (b >= 0 && (guint) b < blocks->len)
    note = ((const W42Block *) g_ptr_array_index (blocks, b))->note >= 0;
  g_ptr_array_free (blocks, TRUE);
  return note;
}

void
w42_view_insert_caption (W42View *self, const char *label)
{
  W42PieceTable *pt;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (label != NULL);

  pt = view_pt (self);
  if (pt == NULL)
    return;

  w42_pt_begin_group (pt);
  view_set_caret (self, w42_pt_paragraph_end (pt, self->caret), FALSE);
  view_insert_paragraph (self);
  w42_view_apply_style (self, "Caption");
  w42_view_insert_text (self, label);
  w42_pt_end_group (pt);
}

void
w42_view_insert_picture (W42View    *self,
                         GBytes     *data,
                         const char *format,
                         int         pixel_w,
                         int         pixel_h)
{
  W42PieceTable *pt;
  W42ObjectIdx idx;
  W42ApIdx ap;
  gboolean grouped;
  int width, height;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (data != NULL);

  pt = view_pt (self);
  if (pt == NULL || pixel_w <= 0 || pixel_h <= 0)
    return;

  /* Shown at its pixel size taken as 96 dpi -- fifteen twips a pixel -- which
   * is what a screenshot or a web image means by its size.  Anything wider
   * than the column is scaled to fit by the layout engine. */
  width  = pixel_w * 15;
  height = pixel_h * 15;

  idx = w42_object_table_add (w42_pt_object_table (pt), data, format,
                              pixel_w, pixel_h, width, height);
  ap = w42_pt_ap_at (pt, self->caret);

  grouped = w42_view_has_selection (self);
  if (grouped)
    w42_pt_begin_group (pt);

  view_delete_selection (self);
  w42_pt_insert_object (pt, self->caret, idx, ap);
  self->caret += 1;
  self->anchor = self->caret;

  if (grouped)
    w42_pt_end_group (pt);

  self->pending_mask = 0;
  view_edited (self);
}

void
w42_view_insert_shape (W42View *self, W42ShapeKind kind, int width, int height,
                       double line_pt, guint32 line_rgb,
                       gboolean filled, guint32 fill_rgb, const char *text)
{
  W42PieceTable *pt;
  W42ObjectIdx idx;
  W42ApIdx ap;
  gboolean grouped;
  int w_px, h_px;
  GBytes *png;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;
  width = MAX (width, 15);
  height = MAX (height, 15);
  w_px = MAX (width / 15, 2);
  h_px = MAX (height / 15, 2);

  /* A PNG of it goes with it, for the formats that cannot say a shape. */
  png = w42_shape_render (kind, w_px, h_px, line_pt, line_rgb, filled, fill_rgb, text);
  if (png == NULL)
    return;
  idx = w42_object_table_add (w42_pt_object_table (pt), png, g_intern_static_string ("png"),
                              w_px, h_px, width, height);
  g_bytes_unref (png);
  w42_object_table_set_shape (w42_pt_object_table (pt), idx, kind, line_pt, line_rgb,
                              filled, fill_rgb, text);
  ap = w42_pt_ap_at (pt, self->caret);

  grouped = w42_view_has_selection (self);
  if (grouped)
    w42_pt_begin_group (pt);
  view_delete_selection (self);
  w42_pt_insert_object (pt, self->caret, idx, ap);
  self->caret += 1;
  self->anchor = self->caret;
  if (grouped)
    w42_pt_end_group (pt);

  self->pending_mask = 0;
  view_edited (self);
}

const W42Object *
w42_view_get_object (W42View *self)
{
  W42PieceTable *pt;
  gsize start, end;
  W42ObjectIdx idx;

  g_return_val_if_fail (W42_IS_VIEW (self), NULL);
  pt = view_pt (self);
  if (pt == NULL || !w42_view_has_selection (self))
    return NULL;
  start = sel_start (self);
  end = sel_end (self);
  if (end != start + 1)
    return NULL;
  idx = w42_pt_object_at (pt, start);
  if (idx == W42_OBJECT_NONE)
    return NULL;
  return w42_object_table_get (w42_pt_object_table (pt), idx);
}

void
w42_view_set_shape (W42View *self, W42ShapeKind kind, double line_pt, guint32 line_rgb,
                    gboolean filled, guint32 fill_rgb, const char *text)
{
  W42PieceTable *pt;
  W42ObjectTable *objects;
  const W42Object *object;
  W42ObjectIdx old, fresh;
  gsize pos;
  W42ApIdx ap;
  GBytes *png;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  object = w42_view_get_object (self);
  if (pt == NULL || object == NULL)
    return;
  pos = sel_start (self);
  old = w42_pt_object_at (pt, pos);
  objects = w42_pt_object_table (pt);

  /* A fresh object, so that undo brings the old one back; its PNG is
   * drawn again, since the formats that use it show it as it is now. */
  png = w42_shape_render (kind, MAX (object->width / 15, 2), MAX (object->height / 15, 2),
                          line_pt, line_rgb, filled, fill_rgb, text);
  if (png == NULL)
    return;
  fresh = w42_object_table_add (objects, png, g_intern_static_string ("png"),
                                MAX (object->width / 15, 2), MAX (object->height / 15, 2),
                                object->width, object->height);
  g_bytes_unref (png);
  w42_object_table_set_wrap (objects, fresh, object->wrap);
  w42_object_table_set_position (objects, fresh, object->positioned, object->pos_x, object->pos_y);
  w42_object_table_set_shape (objects, fresh, kind, line_pt, line_rgb, filled, fill_rgb, text);
  (void) old;

  ap = w42_pt_ap_at (pt, pos);
  w42_pt_begin_group (pt);
  w42_pt_delete (pt, pos, 1);
  w42_pt_insert_object (pt, pos, fresh, ap);
  w42_pt_end_group (pt);
  self->anchor = pos;
  self->caret = pos + 1;
  view_edited (self);
}

static void view_insert_paragraph (W42View *self);

void
w42_view_insert_table (W42View *self, int rows, int cols)
{
  W42PieceTable *pt;
  gsize at, cell;
  int table = -1;
  W42ApIdx ap;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || rows < 1 || cols < 1)
    return;

  /* No tables inside tables. */
  if (w42_pt_cell_at (pt, self->caret, NULL, NULL, NULL))
    return;

  ap = w42_pt_block_ap_at (pt, para_pos (self, self->caret));
  at = w42_pt_paragraph_end (pt, self->caret);

  w42_pt_insert_table (pt, at, rows, cols, ap);

  /* The new table is the last one made. */
  {
    int t = 0;
    while (w42_pt_table_props (pt, t + 1) != NULL)
      t++;
    table = t;
  }

  cell = w42_pt_cell_start (pt, table, 0, 0);
  self->caret = self->anchor = (cell != (gsize) -1) ? cell : at;
  view_edited (self);
}

gboolean
w42_view_in_table (W42View *self)
{
  W42PieceTable *pt;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);

  pt = view_pt (self);
  return pt != NULL && w42_pt_cell_at (pt, self->caret, NULL, NULL, NULL);
}

/* Tab in a cell goes to the next cell, and past the last cell adds a row,
 * which is how every table in every word processor has grown since 1989. */
static gboolean
view_table_tab (W42View *self, gboolean backwards)
{
  W42PieceTable *pt = view_pt (self);
  const W42TableProps *props;
  int table, row, col, rows;
  gsize target;

  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return FALSE;

  props = w42_pt_table_props (pt, table);
  rows = w42_pt_table_rows (pt, table);
  if (props == NULL)
    return FALSE;

  /* A column a merged cell covers has no cell of its own to land in, so
   * both directions step until they find one. */
  if (backwards)
    {
      do
        {
          if (col > 0)
            col--;
          else if (row > 0)
            {
              row--;
              col = props->n_cols - 1;
            }
          else
            return TRUE;
        }
      while (w42_pt_cell_start (pt, table, row, col) == (gsize) -1);
    }
  else
    {
      col += MAX (w42_pt_cell_span (pt, table, row, col), 1);
      while (col < props->n_cols &&
             w42_pt_cell_start (pt, table, row, col) == (gsize) -1)
        col++;

      if (col >= props->n_cols)
        {
          if (row + 1 >= rows)
            {
              w42_pt_table_insert_row (pt, table, row);
              w42_document_set_modified (self->doc, TRUE);
            }
          row++;
          col = 0;
        }
    }

  target = w42_pt_cell_start (pt, table, row, col);
  if (target == (gsize) -1)
    return TRUE;

  /* Select the cell's text, as Word does on Tab. */
  self->anchor = target;
  self->caret = w42_pt_paragraph_end (pt, target);
  {
    /* ...but not the paragraph mark that ends the last paragraph of the
     * cell, which paragraph_end sits before already. */
    gsize offset = 0;
    (void) offset;
  }
  view_relayout (self);
  view_caret_moved (self, FALSE);
  return TRUE;
}

void
w42_view_table_insert_row (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;

  w42_pt_table_insert_row (pt, table, row);
  view_edited (self);
}

/* The text column's width in twips: the page less its margins, shared
 * between newspaper columns when there are any. */
static int
view_text_twips (W42View *self)
{
  const W42PageSetup *page = w42_document_page_setup (self->doc);
  int text_twips = page->width - page->margin_left - page->margin_right;

  if (w42_page_columns (page) > 1)
    text_twips = (text_twips - (w42_page_columns (page) - 1) * w42_page_column_gap (page))
                 / w42_page_columns (page);
  return MAX (text_twips, 1440);
}

void
w42_view_table_insert_row_above (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  gsize landing;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  w42_pt_table_insert_row_before (pt, table, row);
  /* The caret stays in the cell it was in, now a row further down. */
  landing = w42_pt_cell_start (pt, table, row + 1, col);
  if (landing != (gsize) -1)
    self->caret = self->anchor = w42_pt_clamp_pos (pt, landing);
  view_edited (self);
}

void
w42_view_table_insert_column_left (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  gsize landing;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  w42_pt_table_insert_column_before (pt, table, col);
  landing = w42_pt_cell_start (pt, table, row, col + 1);
  if (landing != (gsize) -1)
    self->caret = self->anchor = w42_pt_clamp_pos (pt, landing);
  view_edited (self);
}

void
w42_view_table_delete_table (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  gsize start = 0, end = 0;
  int guard = 0;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  if (!w42_pt_table_bounds (pt, table, &start, &end))
    return;
  /* Row by row from the top; the last row takes the table with it. */
  w42_pt_begin_group (pt);
  while (w42_pt_table_bounds (pt, table, &start, &end) && guard++ < 5000)
    w42_pt_table_delete_row (pt, table, 0);
  w42_pt_end_group (pt);
  self->caret = self->anchor = w42_pt_clamp_pos (pt, start);
  view_edited (self);
}

void
w42_view_table_select_column (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  gsize first = (gsize) -1, last_end = 0;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  /* The cells of the column, top to bottom: a range from the first's
   * start to the last's end.  What lies between belongs to other
   * columns too, which is as much as a linear selection can say. */
  for (int r = 0; r < 4096; r++)
    {
      gsize s, e;

      if (!w42_pt_cell_range (pt, table, r, col, &s, &e))
        break;
      if (first == (gsize) -1)
        first = s;
      last_end = e;
    }
  if (first == (gsize) -1)
    return;
  w42_view_select_range (self, first, last_end > first ? last_end - 1 : last_end);
}

void
w42_view_table_select_cell (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  gsize s, e;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  if (!w42_pt_cell_range (pt, table, row, col, &s, &e))
    return;
  w42_view_select_range (self, s, e > s ? e - 1 : e);
}

void
w42_view_table_distribute_columns (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  const W42TableProps *props;
  int *widths;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  props = w42_pt_table_props (pt, table);
  if (props == NULL || props->n_cols <= 0)
    return;
  /* An equal share of what the table spans now. */
  {
    int total = 0;

    for (int c = 0; c < props->n_cols; c++)
      total += c < (int) props->widths->len ? g_array_index (props->widths, int, c) : 0;
    if (total <= 0)
      total = view_text_twips (self);
    widths = g_new0 (int, props->n_cols);
    for (int c = 0; c < props->n_cols; c++)
      widths[c] = total / props->n_cols;
  }
  w42_pt_table_set_widths (pt, table, widths, props->n_cols);
  g_free (widths);
  view_edited (self);
}

void
w42_view_table_distribute_rows (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  const W42TableProps *props;
  int tallest = 0;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  props = w42_pt_table_props (pt, table);
  if (props == NULL)
    return;
  /* Every row at least as tall as the tallest set height; the text
   * decides the rest, as it always does. */
  for (guint r = 0; r < props->row_heights->len; r++)
    tallest = MAX (tallest, g_array_index (props->row_heights, int, r));
  w42_pt_begin_group (pt);
  for (int r = 0; r < 4096; r++)
    {
      gsize s, e;

      if (!w42_pt_row_bounds (pt, table, r, &s, &e))
        break;
      w42_pt_table_set_row_height (pt, table, r, tallest);
    }
  w42_pt_end_group (pt);
  view_edited (self);
}

void
w42_view_table_autofit_window (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  const W42TableProps *props;
  int *widths, total = 0, want;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  props = w42_pt_table_props (pt, table);
  if (props == NULL || props->n_cols <= 0)
    return;
  want = view_text_twips (self);
  for (int c = 0; c < props->n_cols; c++)
    total += c < (int) props->widths->len ? g_array_index (props->widths, int, c) : 0;
  widths = g_new0 (int, props->n_cols);
  for (int c = 0; c < props->n_cols; c++)
    {
      int w = c < (int) props->widths->len ? g_array_index (props->widths, int, c) : 0;

      /* Scaled to the column's width, keeping their proportions. */
      widths[c] = total > 0 ? (int) ((gint64) w * want / total) : want / props->n_cols;
    }
  w42_pt_table_set_widths (pt, table, widths, props->n_cols);
  g_free (widths);
  view_edited (self);
}

gboolean
w42_view_table_cell_text (W42View *self, int row, int col, char **out)
{
  W42PieceTable *pt;
  int table, r, c;
  gsize s, e;

  g_return_val_if_fail (W42_IS_VIEW (self) && out != NULL, FALSE);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &r, &c))
    return FALSE;
  if (!w42_pt_cell_range (pt, table, row, col, &s, &e) || e <= s)
    return FALSE;
  *out = w42_pt_get_text (pt, s, e - s);
  return *out != NULL;
}

/* The number a cell's text holds, if it holds one: "1,234.50" is one,
 * "$12" is one, "twelve" is not. */
static gboolean
cell_number (const char *text, double *out)
{
  GString *digits = g_string_new (NULL);
  gboolean any = FALSE, ok;
  char *end = NULL;

  for (const char *c = text; *c != '\0'; c++)
    {
      if (g_ascii_isdigit (*c) || *c == '.' || *c == '-')
        {
          g_string_append_c (digits, *c);
          any = any || g_ascii_isdigit (*c);
        }
      else if (*c == ',' || *c == ' ' || *c == '$' || (guchar) *c == 0xC2 || (guchar) *c == 0xA0)
        continue;
      else if (any)
        break;
    }
  *out = g_ascii_strtod (digits->str, &end);
  ok = any && end != NULL && *end == '\0';
  g_string_free (digits, TRUE);
  return ok;
}

gboolean
w42_view_table_formula (W42View *self, const char *formula)
{
  W42PieceTable *pt;
  int table, row, col;
  char *upper, *open, *close;
  const char *fn, *arg;
  double acc = 0.0, n = 0.0;
  gboolean have = FALSE;
  char result[64];
  W42CharFmt ch;

  g_return_val_if_fail (W42_IS_VIEW (self) && formula != NULL, FALSE);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return FALSE;

  /* "=SUM(ABOVE)", "=AVERAGE(LEFT)", "=COUNT(BELOW)", "=MAX(RIGHT)",
   * "=MIN(...)", "=PRODUCT(...)": Word XP's functions over the cells in
   * one direction, up to the first that holds no number. */
  upper = g_ascii_strup (formula, -1);
  g_strstrip (upper);
  fn = upper[0] == '=' ? upper + 1 : upper;
  open = strchr (fn, '(');
  close = open != NULL ? strchr (open, ')') : NULL;
  if (open == NULL || close == NULL)
    {
      g_free (upper);
      return FALSE;
    }
  *open = '\0';
  *close = '\0';
  arg = open + 1;
  while (*arg == ' ')
    arg++;
  {
    int dr = 0, dc = 0;

    if (g_str_has_prefix (arg, "ABOVE"))      dr = -1;
    else if (g_str_has_prefix (arg, "BELOW")) dr = 1;
    else if (g_str_has_prefix (arg, "LEFT"))  dc = -1;
    else if (g_str_has_prefix (arg, "RIGHT")) dc = 1;
    else
      {
        g_free (upper);
        return FALSE;
      }
    if (g_str_equal (fn, "PRODUCT"))
      acc = 1.0;
    for (int r = row + dr, c = col + dc; r >= 0 && c >= 0 && r < 4096 && c < 1024; r += dr, c += dc)
      {
        char *text = NULL;
        double v;

        if (!w42_view_table_cell_text (self, r, c, &text))
          break;
        if (!cell_number (text, &v))
          {
            g_free (text);
            if (have)
              break;
            continue;                /* a heading above the numbers */
          }
        g_free (text);
        if (g_str_equal (fn, "MAX"))          acc = have ? MAX (acc, v) : v;
        else if (g_str_equal (fn, "MIN"))     acc = have ? MIN (acc, v) : v;
        else if (g_str_equal (fn, "PRODUCT")) acc *= v;
        else                                  acc += v;   /* SUM, AVERAGE, COUNT */
        n += 1.0;
        have = TRUE;
      }
    if (g_str_equal (fn, "AVERAGE") && n > 0.0)
      acc /= n;
    else if (g_str_equal (fn, "COUNT"))
      acc = n;
  }
  g_free (upper);

  if (fabs (acc - floor (acc + 0.5)) < 1e-9)
    g_snprintf (result, sizeof result, "%.0f", acc);
  else
    g_snprintf (result, sizeof result, "%.2f", acc);

  /* The result goes in as a field, so Update Fields can work it out
   * again; its code is the formula as Word spelt it. */
  w42_view_get_char_fmt (self, &ch);
  ch.field = g_intern_string (formula[0] == '=' ? formula : g_strconcat ("=", formula, NULL));
  w42_view_insert_text (self, result);
  {
    gsize end = self->caret, start = end - strlen (result);
    W42CharFmt want;

    memset (&want, 0, sizeof want);
    want.field = ch.field;
    w42_pt_apply_char_fmt (pt, start, end - start, W42_CHAR_FIELD, &want);
  }
  view_edited (self);
  return TRUE;
}

void
w42_view_table_insert_column (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  w42_pt_table_insert_column (pt, table, col);
  {
    gsize landing = w42_pt_cell_start (pt, table, row, col + 1);

    if (landing != (gsize) -1)
      self->caret = self->anchor = w42_pt_clamp_pos (pt, landing);
  }
  view_edited (self);
}

void
w42_view_table_delete_column (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  gsize landing;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  w42_pt_table_delete_column (pt, table, col);
  {
    const W42TableProps *props = w42_pt_table_props (pt, table);
    int n = props != NULL ? props->n_cols : 1;

    landing = w42_pt_cell_start (pt, table, row, MIN (col, n - 1));
    if (landing == (gsize) -1)
      landing = w42_pt_cell_start (pt, table, row, 0);
  }
  if (landing != (gsize) -1)
    self->caret = self->anchor = w42_pt_clamp_pos (pt, MIN (landing, w42_pt_length (pt) - 1));
  view_edited (self);
}

gboolean
w42_view_table_get_borders (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  const W42TableProps *props;

  g_return_val_if_fail (W42_IS_VIEW (self), TRUE);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return TRUE;
  props = w42_pt_table_props (pt, table);
  return props == NULL || props->borders;
}

void
w42_view_table_set_borders (W42View *self, gboolean borders)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  w42_pt_table_set_borders (pt, table, borders);
  view_edited (self);
}

void
w42_view_cell_set_shading (W42View *self, int percent)
{
  W42PieceTable *pt;
  int table, row, col, t2, r2, c2;
  gsize start, end;
  W42ParaFmt want;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;

  /* The grey is the cell's own, on its mark, so that it fills the whole
   * cell and not just the lines of text; the paragraphs' shading, if
   * any, is left as it is. */
  (void) start; (void) end; (void) t2; (void) r2; (void) c2;
  if (w42_pt_cell_get_fmt (pt, table, row, col) == NULL)
    return;
  want = *w42_pt_cell_get_fmt (pt, table, row, col);
  want.shading = (guint8) CLAMP (percent, 0, 100);
  if (want.shading > 0)
    {
      want.has_shading_color = 0;
      want.shading_color = 0;
    }
  w42_pt_cell_set_fmt (pt, table, row, col, &want);
  view_edited (self);
}

void
w42_view_table_delete_row (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  gsize landing;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;

  landing = w42_pt_cell_start (pt, table, 0, 0);
  w42_pt_table_delete_row (pt, table, row);

  /* Land on the row that took the deleted one's place, or where the table
   * was if it went. */
  {
    gsize again = w42_pt_cell_start (pt, table, MIN (row, MAX (w42_pt_table_rows (pt, table) - 1, 0)), 0);
    self->caret = self->anchor = w42_pt_clamp_pos (pt, again != (gsize) -1 ? again : landing);
  }
  view_edited (self);
}

void
w42_view_insert_page_break (W42View *self)
{
  W42PieceTable *pt;
  W42ParaFmt want;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;

  memset (&want, 0, sizeof want);
  want.page_break_before = 1;

  w42_pt_begin_group (pt);
  view_insert_paragraph (self);
  w42_pt_apply_para_fmt (pt, self->caret, 0, W42_PARA_PAGE_BREAK, &want);
  w42_pt_end_group (pt);

  view_edited (self);
}

/* The paragraph that starts the caret's section: the nearest one at or
 * before `pos` marked as a section break, or (gsize) -1 for the first
 * section, which the page setup governs. */
static gsize
view_section_start (W42View *self, gsize pos, W42ParaFmt *out)
{
  W42PieceTable *pt = view_pt (self);
  GPtrArray *blocks = w42_pt_snapshot_blocks (pt);
  gsize found = (gsize) -1;

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42Fmt *fmt;

      if (block->start_pos > pos)
        break;
      fmt = w42_ap_table_get (w42_pt_ap_table (pt), block->ap);
      if (fmt->pa.section_break && b > 0)
        {
          found = block->start_pos;
          if (out != NULL)
            *out = fmt->pa;
        }
    }
  g_ptr_array_free (blocks, TRUE);
  return found;
}

void
w42_view_get_columns (W42View *self, int *columns, int *gap)
{
  W42ParaFmt pa;
  const W42PageSetup *page;

  g_return_if_fail (W42_IS_VIEW (self));

  page = w42_document_page_setup (self->doc);
  if (view_pt (self) != NULL && view_section_start (self, self->caret, &pa) != (gsize) -1)
    {
      *columns = MAX (pa.columns, 1);
      *gap = pa.column_gap > 0 ? pa.column_gap : 720;
    }
  else
    {
      *columns = w42_page_columns (page);
      *gap = w42_page_column_gap (page);
    }
}

void
w42_view_set_columns (W42View *self, int columns, int gap, W42ColumnsScope scope)
{
  W42PieceTable *pt;
  W42ParaFmt want;
  gsize start;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;

  memset (&want, 0, sizeof want);
  want.section_break = 1;
  want.columns = (guint8) CLAMP (columns, 1, 9);
  want.column_gap = gap;

  if (scope == W42_COLUMNS_FORWARD)
    {
      w42_pt_begin_group (pt);
      view_insert_paragraph (self);
      w42_pt_apply_para_fmt (pt, self->caret, 0, W42_PARA_SECTION, &want);
      w42_pt_end_group (pt);
      view_edited (self);
      return;
    }

  start = scope == W42_COLUMNS_SECTION ? view_section_start (self, self->caret, NULL) : (gsize) -1;
  if (start != (gsize) -1)
    {
      w42_pt_apply_para_fmt (pt, start + 1, 0, W42_PARA_SECTION, &want);
      view_edited (self);
    }
  else
    {
      W42PageSetup page = *w42_document_page_setup (self->doc);

      page.columns = columns;
      page.column_gap = gap;
      w42_document_set_page_setup (self->doc, &page);
      w42_document_set_modified (self->doc, TRUE);
      w42_document_touch (self->doc);
    }
}

/* The page a position is on, as the layout stands. */
static int
view_page_of (W42View *self, gsize pos)
{
  W42PieceTable *pt = view_pt (self);
  GPtrArray *blocks = w42_pt_snapshot_blocks (pt);
  const GArray *lines = w42_layout_lines (self->layout);
  int page = 1, block_index = -1;

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);

      if (block->start_pos <= pos)
        block_index = (int) b;
      else
        break;
    }
  if (block_index >= 0)
    {
      const W42Block *block = g_ptr_array_index (blocks, block_index);
      gsize chars = pos > block->start_pos ? pos - block->start_pos - 1 : 0;
      guint byte = 0;

      if (chars < (gsize) g_utf8_strlen (block->text->str, -1))
        byte = (guint) (g_utf8_offset_to_pointer (block->text->str, (glong) chars) - block->text->str);
      for (guint i = 0; i < lines->len; i++)
        {
          const W42LineBox *line = &g_array_index (lines, W42LineBox, i);

          if (line->block != block_index)
            continue;
          page = line->page + 1;
          if (byte < line->start_index + line->length)
            break;
        }
    }
  g_ptr_array_free (blocks, TRUE);
  return page;
}

/* What a field says now. */
static char *
view_field_value (W42View *self, const char *code, gsize pos)
{
  W42PieceTable *pt = view_pt (self);

  if (g_str_equal (code, "PAGE"))
    return g_strdup_printf ("%d", view_page_of (self, pos));
  if (g_str_equal (code, "NUMPAGES"))
    return g_strdup_printf ("%d", w42_layout_n_pages (self->layout));
  if (g_str_equal (code, "DATE") || g_str_equal (code, "TIME"))
    {
      GDateTime *now = g_date_time_new_now_local ();
      char *text = g_date_time_format (now, g_str_equal (code, "DATE") ? "%x" : "%X");

      g_date_time_unref (now);
      return text;
    }
  if (g_str_has_prefix (code, W42_INDEX_FIELD))
    return NULL;                 /* an index entry: its text is the text */
  if (g_str_equal (code, "FILENAME"))
    return w42_document_get_title (self->doc);
  if (g_str_equal (code, "NUMWORDS"))
    {
      char *text = w42_pt_get_text (pt, 0, w42_pt_length (pt));
      int words = 0;
      gboolean in_word = FALSE;

      for (const char *p = text; *p; p = g_utf8_next_char (p))
        {
          gunichar c = g_utf8_get_char (p);

          if (g_unichar_isspace (c) || c == 0xFFFC)
            in_word = FALSE;
          else if (!in_word)
            {
              in_word = TRUE;
              words++;
            }
        }
      g_free (text);
      return g_strdup_printf ("%d", words);
    }
  return g_strdup ("?");
}

int
w42_view_update_fields (W42View *self)
{
  W42PieceTable *pt;
  GPtrArray *blocks;
  GArray *ranges;              /* gsize start, end; const char *code -- packed */
  int changed = 0;

  g_return_val_if_fail (W42_IS_VIEW (self), 0);

  pt = view_pt (self);
  if (pt == NULL)
    return 0;

  /* Every field's range, from the runs; then back to front, so earlier
   * positions hold while later text changes length. */
  blocks = w42_pt_snapshot_blocks (pt);
  ranges = g_array_new (FALSE, FALSE, sizeof (gsize) * 3);
  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);

      for (guint r = 0; r < block->runs->len; r++)
        {
          const W42Run *run = &g_array_index (block->runs, W42Run, r);
          const W42CharFmt *ch = &w42_ap_table_get (w42_pt_ap_table (pt), run->ap)->ch;

          if (ch->field != NULL && run->object == W42_OBJECT_NONE && run->footnote == 0)
            {
              gsize entry[3] = { run->doc_pos, run->doc_pos + run->n_chars, (gsize) (gintptr) ch->field };

              /* Adjacent runs of the same field are one field. */
              if (ranges->len > 0)
                {
                  gsize *last = &g_array_index (ranges, gsize, (ranges->len - 1) * 3);

                  if (last[1] == entry[0] && last[2] == entry[2])
                    {
                      last[1] = entry[1];
                      continue;
                    }
                }
              g_array_append_val (ranges, entry);
            }
        }
    }
  g_ptr_array_free (blocks, TRUE);

  if (ranges->len > 0)
    {
      w42_pt_begin_group (pt);
      for (guint i = ranges->len; i > 0; i--)
        {
          gsize *entry = &g_array_index (ranges, gsize, (i - 1) * 3);
          const char *code = (const char *) (gintptr) entry[2];
          char *value = view_field_value (self, code, entry[0]);
          char *old = value != NULL ? w42_pt_get_text (pt, entry[0], entry[1] - entry[0]) : NULL;

          if (value != NULL && !g_str_equal (value, old))
            {
              W42ApIdx ap = w42_pt_ap_at (pt, entry[0] + 1);
              gsize n = g_utf8_strlen (value, -1);

              w42_pt_delete (pt, entry[0], entry[1] - entry[0]);
              w42_pt_insert_text (pt, entry[0], value, ap);
              if (self->caret > entry[1])
                self->caret += n - (entry[1] - entry[0]);
              else if (self->caret > entry[0])
                self->caret = entry[0] + n;
              changed++;
            }
          g_free (value);
          g_free (old);
        }
      w42_pt_end_group (pt);
      if (changed > 0)
        {
          self->caret = w42_pt_clamp_pos (pt, self->caret);
          self->anchor = self->caret;
          view_edited (self);
        }
    }
  g_array_free (ranges, TRUE);
  return changed;
}

void
w42_view_insert_field (W42View *self, const char *code)
{
  W42PieceTable *pt;
  W42CharFmt want;
  char *value;
  gsize n;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (code != NULL);

  pt = view_pt (self);
  if (pt == NULL)
    return;

  value = view_field_value (self, code, self->caret);
  n = g_utf8_strlen (value, -1);
  memset (&want, 0, sizeof want);
  want.field = g_intern_string (code);

  w42_pt_begin_group (pt);
  view_delete_selection (self);
  w42_pt_insert_text (pt, self->caret, value, view_effective_ap (self));
  w42_pt_apply_char_fmt (pt, self->caret, n, W42_CHAR_FIELD, &want);
  self->caret += n;
  self->anchor = self->caret;
  w42_pt_end_group (pt);
  g_free (value);
  view_edited (self);
}

int
w42_view_hyphenate (W42View *self, gboolean remove)
{
  W42PieceTable *pt;
  int n;

  g_return_val_if_fail (W42_IS_VIEW (self), 0);

  pt = view_pt (self);
  if (pt == NULL)
    return 0;

  if (remove)
    n = w42_pt_unhyphenate (pt);
  else
    {
      W42Hyphenator *hyph = w42_hyphenator_new ();

      if (hyph == NULL)
        return -1;
      n = w42_pt_hyphenate (pt, hyph);
      w42_hyphenator_free (hyph);
    }

  if (n > 0)
    {
      self->caret = w42_pt_clamp_pos (pt, self->caret);
      self->anchor = self->caret;
      view_edited (self);
    }
  return n;
}

void
w42_view_insert_section_break (W42View *self)
{
  int columns = 1, gap = 720;

  g_return_if_fail (W42_IS_VIEW (self));

  if (view_pt (self) == NULL)
    return;
  w42_view_get_columns (self, &columns, &gap);
  w42_view_set_columns (self, columns, gap, W42_COLUMNS_FORWARD);
}

static void
view_insert_paragraph (W42View *self)
{
  W42PieceTable *pt = view_pt (self);
  W42ApIdx ap;

  if (pt == NULL)
    return;

  /* A new paragraph inherits the current one's formatting, as Word does --
   * except after a heading, whose "next style" is Normal.  Nobody wants two
   * Heading 1s in a row; they want the heading and then the text under it. */
  ap = w42_pt_block_ap_at (pt, para_pos (self, self->caret));

  /* Enter on an empty list item ends the list, as it does in Word: the
   * item becomes an ordinary paragraph instead of a new item being made. */
  if (!w42_view_has_selection (self) &&
      w42_ap_table_get (w42_pt_ap_table (pt), ap)->pa.list != W42_LIST_NONE &&
      w42_pt_paragraph_end (pt, self->caret) ==
        w42_pt_paragraph_start (pt, self->caret) + 1)
    {
      w42_view_set_list (self, W42_LIST_NONE);
      return;
    }

  w42_pt_begin_group (pt);

  view_delete_selection (self);

  w42_pt_insert_block (pt, self->caret, ap);
  self->caret += 1;
  self->anchor = self->caret;

  {
    const W42Fmt *fmt = w42_ap_table_get (w42_pt_ap_table (pt), ap);
    W42StyleSheet *styles = w42_pt_stylesheet (pt);

    if (w42_stylesheet_outline (styles, fmt->pa.style) > 0)
      w42_pt_apply_style (pt, self->caret, 0, "Normal");
  }

  w42_pt_end_group (pt);

  view_edited (self);
}

/* The position a word to the left or right of `pos`: past the spaces and
 * punctuation, then past the letters, as Ctrl+Left and Ctrl+Right walk.
 * A paragraph mark counts as a word of its own. */
static gboolean
view_char_is_word (W42PieceTable *pt, gsize pos, gboolean *is_mark)
{
  char *t = w42_pt_get_text (pt, pos, 1);
  gunichar c = t != NULL && *t != '\0' ? g_utf8_get_char (t) : 0;

  *is_mark = (c == '\n');
  g_free (t);
  return g_unichar_isalnum (c) || c == '_' || c == '\'';
}

static gsize
view_word_move (W42View *self, gsize pos, int dir)
{
  W42PieceTable *pt = view_pt (self);
  gsize p = pos;
  gboolean mark;

  if (dir > 0)
    {
      gsize end = w42_pt_length (pt);

      if (p < end && view_char_is_word (pt, p, &mark))
        while (p < end && view_char_is_word (pt, p, &mark))
          p = w42_pt_next_pos (pt, p);
      else if (p < end)
        {
          view_char_is_word (pt, p, &mark);
          p = w42_pt_next_pos (pt, p);
          if (mark)
            return p;
        }
      while (p < end && !view_char_is_word (pt, p, &mark) && !mark)
        p = w42_pt_next_pos (pt, p);
      return p;
    }
  else
    {
      gsize first = w42_pt_first_caret_pos (pt);

      while (p > first)
        {
          gsize q = w42_pt_prev_pos (pt, p);

          if (view_char_is_word (pt, q, &mark) || mark)
            break;
          p = q;
        }
      if (p > first)
        {
          gsize q = w42_pt_prev_pos (pt, p);

          if (!view_char_is_word (pt, q, &mark) && mark)
            return q;                 /* the paragraph mark before */
        }
      while (p > first)
        {
          gsize q = w42_pt_prev_pos (pt, p);

          if (!view_char_is_word (pt, q, &mark))
            break;
          p = q;
        }
      return p;
    }
}

static void
view_delete (W42View *self, int dir)
{
  W42PieceTable *pt = view_pt (self);
  gsize from, to;

  if (pt == NULL)
    return;

  if (view_delete_selection (self))
    {
      view_edited (self);
      return;
    }

  if (dir < 0)
    {
      from = w42_pt_prev_pos (pt, self->caret);
      to   = self->caret;
      if (from >= to)
        return;
      if (view_tracked_delete (self, from, to))
        {
          self->caret = self->anchor = from;
          view_edited (self);
          return;
        }
      w42_pt_delete (pt, from, to - from);
      self->caret = self->anchor = w42_pt_clamp_pos (pt, from);
    }
  else
    {
      from = self->caret;
      to   = w42_pt_next_pos (pt, self->caret);
      if (to <= from)
        return;
      if (view_tracked_delete (self, from, to))
        {
          self->caret = self->anchor = to;
          view_edited (self);
          return;
        }
      w42_pt_delete (pt, from, to - from);
      self->caret = self->anchor = w42_pt_clamp_pos (pt, from);
    }

  view_edited (self);
}

void
w42_view_undo (W42View *self)
{
  W42PieceTable *pt;
  gsize caret;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_can_undo (pt))
    return;

  caret = w42_pt_undo (pt);
  if (caret != (gsize) -1)
    self->caret = self->anchor = w42_pt_clamp_pos (pt, caret);

  view_edited (self);
  /* Back at what was saved: nothing to save.  The window reads the flag
   * when the document says it changed, so say so again. */
  if (w42_document_at_saved_state (self->doc))
    {
      w42_document_set_modified (self->doc, FALSE);
      w42_document_touch (self->doc);
    }
}

void
w42_view_redo (W42View *self)
{
  W42PieceTable *pt;
  gsize caret;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_can_redo (pt))
    return;

  caret = w42_pt_redo (pt);
  if (caret != (gsize) -1)
    self->caret = self->anchor = w42_pt_clamp_pos (pt, caret);
  view_edited (self);
  if (w42_document_at_saved_state (self->doc))
    {
      w42_document_set_modified (self->doc, FALSE);
      w42_document_touch (self->doc);
    }
}

gboolean
w42_view_can_repeat (W42View *self)
{
  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  return view_repeat_stamp_current (self);
}

void
w42_view_repeat (W42View *self)
{
  g_return_if_fail (W42_IS_VIEW (self));

  if (!view_repeat_stamp_current (self))
    return;

  switch (self->repeat_kind)
    {
    case W42_REPEAT_TYPING:
      {
        /* Inserting re-records the run; keep it as it was, so Repeat
         * again puts in the same text rather than twice as much. */
        char *text = g_strdup (self->repeat_text->str);

        w42_view_insert_text (self, text);
        g_string_assign (self->repeat_text, text);
        g_free (text);
      }
      break;
    case W42_REPEAT_CHAR_FMT:
      view_apply_char_fmt (self, self->repeat_char_mask, &self->repeat_char_fmt);
      view_record_repeat (self, W42_REPEAT_CHAR_FMT);
      break;
    case W42_REPEAT_PARA_FMT:
      w42_view_apply_para_fmt (self, self->repeat_para_mask, &self->repeat_para_fmt);
      break;
    case W42_REPEAT_STYLE:
      w42_view_apply_style (self, self->repeat_style);
      break;
    case W42_REPEAT_CASE:
      w42_view_change_case (self, (W42CaseKind) self->repeat_case);
      break;
    default:
      break;
    }
}

void
w42_view_copy (W42View *self)
{
  W42PieceTable *pt;
  char *text;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !w42_view_has_selection (self))
    return;

  text = w42_pt_get_text (pt, sel_start (self),
                          sel_end (self) - sel_start (self));

  /* Text for everything that takes text, and RTF for what takes RTF, so
   * formatting survives a round trip through the clipboard. */
  {
    GdkClipboard *clipboard = gtk_widget_get_clipboard (GTK_WIDGET (self));
    GBytes *rtf = view_selection_as_rtf (self);
    GdkContentProvider *providers[3];
    guint n = 0;
    GValue value = G_VALUE_INIT;
    GdkContentProvider *all;

    if (rtf != NULL)
      {
        providers[n++] = gdk_content_provider_new_for_bytes ("text/rtf", rtf);
        providers[n++] = gdk_content_provider_new_for_bytes ("application/rtf", rtf);
      }
    g_value_init (&value, G_TYPE_STRING);
    g_value_set_string (&value, text);
    providers[n++] = gdk_content_provider_new_for_value (&value);
    g_value_unset (&value);
    all = gdk_content_provider_new_union (providers, n);
    gdk_clipboard_set_content (clipboard, all);
    g_object_unref (all);
    if (rtf != NULL)
      g_bytes_unref (rtf);
  }
  g_free (text);
}

void
w42_view_cut (W42View *self)
{
  g_return_if_fail (W42_IS_VIEW (self));

  if (!w42_view_has_selection (self))
    return;

  w42_view_copy (self);

  if (view_delete_selection (self))
    view_edited (self);
}

/* The selection as an RTF file's bytes, through a temporary file, since
 * the RTF writer writes files. */
static void on_clipboard_text (GObject *source, GAsyncResult *result, gpointer data);

static GBytes *
view_selection_as_rtf (W42View *self)
{
  W42PieceTable *pt = view_pt (self);
  W42PieceTable *frag;
  char *path;
  int fd;
  GFile *file;
  GBytes *bytes = NULL;

  if (pt == NULL || !w42_view_has_selection (self))
    return NULL;
  frag = w42_pt_extract (pt, sel_start (self), sel_end (self) - sel_start (self));
  path = g_build_filename (g_get_tmp_dir (), "word42-clip-XXXXXX.rtf", NULL);
  fd = g_mkstemp (path);
  if (fd >= 0)
    {
      g_close (fd, NULL);
      file = g_file_new_for_path (path);
      if (w42_rtf_save (frag, w42_document_page_setup (self->doc), file, NULL))
        bytes = g_file_load_bytes (file, NULL, NULL, NULL);
      g_object_unref (file);
      g_unlink (path);
    }
  g_free (path);
  w42_pt_free (frag);
  return bytes;
}

/* Puts an RTF fragment in at the caret, replacing the selection. */
static void
view_paste_rtf (W42View *self, GBytes *rtf)
{
  W42PieceTable *pt = view_pt (self);
  W42PieceTable *frag = w42_pt_new ();
  W42PageSetup page = { 0 };
  char *path;
  int fd;
  gboolean ok = FALSE;

  if (pt == NULL)
    {
      w42_pt_free (frag);
      return;
    }
  path = g_build_filename (g_get_tmp_dir (), "word42-paste-XXXXXX.rtf", NULL);
  fd = g_mkstemp (path);
  if (fd >= 0)
    {
      GFile *file;

      g_close (fd, NULL);
      file = g_file_new_for_path (path);
      ok = g_file_replace_contents (file, g_bytes_get_data (rtf, NULL), g_bytes_get_size (rtf),
                                    NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, NULL) &&
           w42_rtf_load (frag, &page, file, NULL);
      g_object_unref (file);
      g_unlink (path);
    }
  g_free (path);

  if (ok)
    {
      w42_pt_begin_group (pt);
      view_delete_selection (self);
      self->caret += w42_pt_insert_fragment (pt, self->caret, frag);
      self->caret = w42_pt_clamp_pos (pt, self->caret);
      self->anchor = self->caret;
      w42_pt_end_group (pt);
      view_edited (self);
    }
  w42_pt_free (frag);
}

static void
on_clipboard_rtf_read (GObject *source, GAsyncResult *result, gpointer data)
{
  W42View *self = data;
  GOutputStream *mem = G_OUTPUT_STREAM (source);

  if (g_output_stream_splice_finish (mem, result, NULL) >= 0)
    {
      GBytes *bytes = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem));

      view_paste_rtf (self, bytes);
      g_bytes_unref (bytes);
    }
  g_object_unref (mem);
  g_object_unref (self);
}

static void
on_clipboard_rtf (GObject *source, GAsyncResult *result, gpointer data)
{
  W42View *self = data;
  const char *mime = NULL;
  GInputStream *in = gdk_clipboard_read_finish (GDK_CLIPBOARD (source), result, &mime, NULL);

  if (in != NULL)
    {
      /* Read on the main loop, never blocking it: when the clipboard is
       * word42's own, the bytes are produced on that same loop. */
      GOutputStream *mem = g_memory_output_stream_new_resizable ();

      g_object_set_data_full (G_OBJECT (mem), "w42-source", in, g_object_unref);
      g_output_stream_splice_async (mem, in,
                                    G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                    G_PRIORITY_DEFAULT, NULL, on_clipboard_rtf_read, self);
      return;                                   /* self's reference passes on */
    }

  /* No RTF after all: the text, then. */
  gdk_clipboard_read_text_async (gtk_widget_get_clipboard (GTK_WIDGET (self)),
                                 NULL, on_clipboard_text, g_object_ref (self));
  g_object_unref (self);
}

static void
on_clipboard_text (GObject *source, GAsyncResult *result, gpointer data)
{
  W42View *self = data;
  char *text;

  text = gdk_clipboard_read_text_finish (GDK_CLIPBOARD (source), result, NULL);

  if (text != NULL)
    {
      /* Paste arrives as plain text; paragraph breaks have to become struxes
       * rather than literal newlines in a run. */
      W42PieceTable *pt = view_pt (self);
      char **lines = g_strsplit_set (text, "\n", -1);

      if (pt != NULL)
        {
          w42_pt_begin_group (pt);
          view_delete_selection (self);

          for (guint i = 0; lines[i] != NULL; i++)
            {
              char *line = g_strdup (lines[i]);

              /* Windows text ends its lines with CR LF: the CR goes. */
              {
                char *w = line;

                for (const char *r = line; *r != '\0'; r++)
                  if (*r != '\r')
                    *w++ = *r;
                *w = '\0';
              }

              if (i > 0)
                {
                  w42_pt_insert_block (pt, self->caret,
                                       w42_pt_block_ap_at (pt, para_pos (self, self->caret)));
                  self->caret += 1;
                }

              if (*line != '\0')
                {
                  w42_pt_insert_text (pt, self->caret, line,
                                      w42_pt_ap_at (pt, self->caret));
                  self->caret += g_utf8_strlen (line, -1);
                }

              g_free (line);
            }

          self->anchor = self->caret;
          w42_pt_end_group (pt);
          view_edited (self);
        }

      g_strfreev (lines);
      g_free (text);
    }

  g_object_unref (self);
}

void
w42_view_paste (W42View *self)
{
  g_return_if_fail (W42_IS_VIEW (self));

  {
    GdkClipboard *clipboard = gtk_widget_get_clipboard (GTK_WIDGET (self));
    GdkContentFormats *formats = gdk_clipboard_get_formats (clipboard);
    static const char *rtf_mimes[] = { "text/rtf", "application/rtf", NULL };

    if (gdk_content_formats_contain_mime_type (formats, "text/rtf") ||
        gdk_content_formats_contain_mime_type (formats, "application/rtf"))
      {
        gdk_clipboard_read_async (clipboard, rtf_mimes, G_PRIORITY_DEFAULT, NULL,
                                  on_clipboard_rtf, g_object_ref (self));
        return;
      }
  }
  gdk_clipboard_read_text_async (gtk_widget_get_clipboard (GTK_WIDGET (self)),
                                 NULL, on_clipboard_text,
                                 g_object_ref (self));
}

void
w42_view_get_selection_bounds (W42View *self, gsize *start, gsize *end)
{
  g_return_if_fail (W42_IS_VIEW (self));
  if (start != NULL) *start = sel_start (self);
  if (end != NULL) *end = sel_end (self);
}

void
w42_view_select_range (W42View *self, gsize start, gsize end)
{
  W42PieceTable *pt;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;

  self->anchor = w42_pt_clamp_pos (pt, start);
  self->caret  = w42_pt_clamp_pos (pt, end);

  view_caret_moved (self, FALSE);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

char *
w42_view_get_selected_text (W42View *self)
{
  W42PieceTable *pt;

  g_return_val_if_fail (W42_IS_VIEW (self), NULL);

  pt = view_pt (self);
  if (pt == NULL || !w42_view_has_selection (self))
    return NULL;

  return w42_pt_get_text (pt, sel_start (self),
                          sel_end (self) - sel_start (self));
}

void
w42_view_select_all (W42View *self)
{
  W42PieceTable *pt;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL)
    return;

  self->anchor = w42_pt_first_caret_pos (pt);
  self->caret  = w42_pt_length (pt);

  view_caret_moved (self, FALSE);
}

/* ---------------------------------------------------------------------- */
/* Input                                                                   */
/* ---------------------------------------------------------------------- */

/* The text of the caret's paragraph up to the caret: what AutoCorrect
 * looks at.  A long paragraph is read from the end, since a correction
 * never reaches further back than a word or two. */
static char *
text_before_caret (W42View *self)
{
  W42PieceTable *pt = view_pt (self);
  gsize start;

  if (pt == NULL)
    return NULL;
  start = w42_pt_paragraph_start (pt, self->caret) + 1;
  if (self->caret <= start)
    return NULL;
  if (self->caret - start > 64)
    start = self->caret - 64;
  return w42_pt_get_text (pt, start, self->caret - start);
}

gboolean
w42_view_expand_autotext (W42View *self)
{
  W42PieceTable *pt;
  char *before, *entry = NULL;
  gsize back = 0;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);

  pt = view_pt (self);
  if (pt == NULL)
    return FALSE;
  before = text_before_caret (self);
  if (before == NULL)
    return FALSE;

  /* The name is what was typed since the last space -- or, since a name
   * may have spaces in it, the longest ending of the line that names an
   * entry, so that "Yours sincerely" expands as well as "ys". */
  for (const char *p = before; *p != '\0'; p = g_utf8_next_char (p))
    {
      if (p != before && !g_unichar_isspace (g_utf8_get_char (g_utf8_prev_char (p))))
        continue;                 /* start at a word, not inside one */
      entry = w42_autotext_get (p);
      if (entry != NULL)
        {
          back = (gsize) g_utf8_strlen (p, -1);
          break;
        }
    }
  g_free (before);

  if (entry == NULL || back == 0 || back > self->caret)
    return FALSE;

  w42_pt_begin_group (pt);
  w42_pt_delete (pt, self->caret - back, back);
  self->caret -= back;
  w42_pt_insert_text (pt, self->caret, entry, view_effective_ap (self));
  self->caret += g_utf8_strlen (entry, -1);
  self->anchor = self->caret;
  w42_pt_end_group (pt);
  view_edited (self);

  g_free (entry);
  return TRUE;
}

/* Word 6 corrected as you typed, and so does this: the correction is
 * worked out from the text behind the caret and put in as one undo step
 * with the character that prompted it. */
static void
autocorrect_after_typing (W42View *self, const char *typed)
{
  W42PieceTable *pt = view_pt (self);
  W42Correction fix;
  char *before;
  gunichar c;

  if (pt == NULL || !self->autocorrect || typed == NULL || *typed == '\0')
    return;
  if (g_utf8_strlen (typed, -1) != 1)
    return;                       /* pasted or composed text is left alone */

  c = g_utf8_get_char (typed);
  before = text_before_caret (self);
  if (before == NULL)
    return;

  fix = w42_autocorrect (before, c);
  g_free (before);

  if (fix.back == 0 || fix.text == NULL || fix.back > self->caret)
    return;

  w42_pt_begin_group (pt);
  w42_pt_delete (pt, self->caret - fix.back, fix.back);
  self->caret -= fix.back;
  w42_pt_insert_text (pt, self->caret, fix.text, view_effective_ap (self));
  self->caret += g_utf8_strlen (fix.text, -1);
  self->anchor = self->caret;
  w42_pt_end_group (pt);
  view_edited (self);
}

static void
on_im_commit (GtkIMContext *im, const char *text, gpointer data)
{
  W42View *self = W42_VIEW (data);

  (void) im;
  w42_view_insert_text (self, text);
  autocorrect_after_typing (self, text);
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                gpointer               data)
{
  W42View *self = data;
  W42PieceTable *pt = view_pt (self);
  gboolean extend = (state & GDK_SHIFT_MASK) != 0;
  gboolean ctrl   = (state & GDK_CONTROL_MASK) != 0;
  GdkEvent *event;

  (void) keycode;

  if (pt == NULL)
    return GDK_EVENT_PROPAGATE;

  if (keyval == GDK_KEY_Menu ||
      (keyval == GDK_KEY_F10 && (state & GDK_SHIFT_MASK) != 0))
    {
      view_popup_at_caret (self);
      return GDK_EVENT_STOP;
    }

  if (ctrl)
    {
      switch (keyval)
        {
        case GDK_KEY_b: case GDK_KEY_B:
          w42_view_toggle_bold (self);      return GDK_EVENT_STOP;
        case GDK_KEY_i: case GDK_KEY_I:
          w42_view_toggle_italic (self);    return GDK_EVENT_STOP;
        case GDK_KEY_u: case GDK_KEY_U:
          w42_view_toggle_underline (self); return GDK_EVENT_STOP;
        case GDK_KEY_Home:
          view_set_caret (self, w42_pt_first_caret_pos (pt), extend);
          return GDK_EVENT_STOP;
        case GDK_KEY_End:
          view_set_caret (self, w42_pt_length (pt), extend);
          return GDK_EVENT_STOP;
        case GDK_KEY_Left: case GDK_KEY_KP_Left:
          view_set_caret (self, view_word_move (self, self->caret, -1), extend);
          return GDK_EVENT_STOP;
        case GDK_KEY_Right: case GDK_KEY_KP_Right:
          view_set_caret (self, view_word_move (self, self->caret, +1), extend);
          return GDK_EVENT_STOP;
        case GDK_KEY_BackSpace:
        case GDK_KEY_Delete: case GDK_KEY_KP_Delete:
          /* Ctrl+Backspace and Ctrl+Delete take out a word. */
          if (!w42_view_has_selection (self))
            {
              gsize to = view_word_move (self, self->caret, keyval == GDK_KEY_BackSpace ? -1 : +1);

              if (to != self->caret)
                {
                  self->anchor = to;
                  view_delete (self, keyval == GDK_KEY_BackSpace ? -1 : +1);
                }
              return GDK_EVENT_STOP;
            }
          break;
        default:
          break;
        }
    }

  switch (keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      if (ctrl)
        w42_view_insert_page_break (self);
      else if (extend)
        w42_view_insert_text (self, "\342\200\250");   /* a line break, U+2028 */
      else
        view_insert_paragraph (self);
      return GDK_EVENT_STOP;

    case GDK_KEY_BackSpace:
      view_delete (self, -1);
      return GDK_EVENT_STOP;

    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
      view_delete (self, +1);
      return GDK_EVENT_STOP;

    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab:
      if (view_table_tab (self, keyval == GDK_KEY_ISO_Left_Tab || extend))
        return GDK_EVENT_STOP;
      /* At the start of a list item, Tab moves it a level in and
       * Shift+Tab a level out, as Word does. */
      if (self->caret == w42_pt_paragraph_start (pt, self->caret) + 1 &&
          w42_view_list_level_by (self, (keyval == GDK_KEY_ISO_Left_Tab || extend) ? -1 : +1))
        return GDK_EVENT_STOP;
      if (keyval == GDK_KEY_ISO_Left_Tab)
        return GDK_EVENT_STOP;
      w42_view_insert_text (self, "\t");
      return GDK_EVENT_STOP;

    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      view_set_caret (self, w42_pt_prev_pos (pt, self->caret), extend);
      return GDK_EVENT_STOP;

    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      view_set_caret (self, w42_pt_next_pos (pt, self->caret), extend);
      return GDK_EVENT_STOP;

    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      {
        int dir = (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) ? -1 : 1;
        double want = self->want_x;
        gsize target = w42_layout_move_line (self->layout, self->caret, dir,
                                             &want);

        self->caret = w42_pt_clamp_pos (pt, target);
        if (!extend)
          self->anchor = self->caret;

        view_caret_moved (self, TRUE);
        self->want_x = want;
        return GDK_EVENT_STOP;
      }

    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
      view_set_caret (self, w42_layout_line_start (self->layout, self->caret),
                      extend);
      return GDK_EVENT_STOP;

    case GDK_KEY_End:
    case GDK_KEY_KP_End:
      view_set_caret (self, w42_layout_line_end (self->layout, self->caret),
                      extend);
      return GDK_EVENT_STOP;

    case GDK_KEY_Page_Up:
    case GDK_KEY_Page_Down:
      {
        int dir = (keyval == GDK_KEY_Page_Up) ? -1 : 1;
        double want = self->want_x;
        gsize target = self->caret;

        /* Roughly a screenful; the exact count matters less than landing in
         * a sensible place. */
        for (int i = 0; i < 24; i++)
          target = w42_layout_move_line (self->layout, target, dir, &want);

        self->caret = w42_pt_clamp_pos (pt, target);
        if (!extend)
          self->anchor = self->caret;

        view_caret_moved (self, TRUE);
        self->want_x = want;
        return GDK_EVENT_STOP;
      }

    default:
      break;
    }

  event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller));
  if (event != NULL && gtk_im_context_filter_keypress (self->im, event))
    return GDK_EVENT_STOP;

  return GDK_EVENT_PROPAGATE;
}

/* Word's double-click-selects-a-word, triple-click-selects-a-paragraph. */
static void
view_select_word_at (W42View *self, gsize pos)
{
  W42Layout *layout = self->layout;
  int block = w42_layout_block_at_pos (layout, pos);
  const W42Block *blk;
  gsize byte;
  const char *text;
  const char *p, *start, *end;

  if (block < 0)
    return;

  blk  = g_ptr_array_index (w42_layout_blocks (layout), block);
  text = blk->text->str;
  byte = w42_block_pos_to_byte (blk, pos);

  if (blk->text->len == 0)
    return;

  if (byte >= blk->text->len)
    byte = blk->text->len - 1;

  p = text + byte;
  start = p;
  while (start > text)
    {
      const char *prev = g_utf8_find_prev_char (text, start);
      if (prev == NULL || !g_unichar_isalnum (g_utf8_get_char (prev)))
        break;
      start = prev;
    }

  end = p;
  while (*end != '\0' && g_unichar_isalnum (g_utf8_get_char (end)))
    end = g_utf8_next_char (end);

  self->anchor = w42_block_byte_to_pos (blk, (gsize) (start - text));
  self->caret  = w42_block_byte_to_pos (blk, (gsize) (end - text));
}

static void
view_select_block_at (W42View *self, gsize pos)
{
  W42Layout *layout = self->layout;
  int block = w42_layout_block_at_pos (layout, pos);
  const W42Block *blk;

  if (block < 0)
    return;

  blk = g_ptr_array_index (w42_layout_blocks (layout), block);
  self->anchor = blk->start_pos + 1;
  self->caret  = blk->start_pos + 1 + g_utf8_strlen (blk->text->str, -1);
}

/* ---------------------------------------------------------------------- */
/* Picture handles                                                         */
/* ---------------------------------------------------------------------- */

#define HANDLE_SIZE 6.0

/* The selected picture, if the selection is exactly one picture. */
static gboolean
view_selected_picture (W42View *self, gsize *pos)
{
  W42PieceTable *pt = view_pt (self);

  if (pt == NULL || sel_end (self) - sel_start (self) != 1)
    return FALSE;
  if (w42_pt_object_at (pt, sel_start (self)) == W42_OBJECT_NONE)
    return FALSE;

  *pos = sel_start (self);
  return TRUE;
}

/* The eight handles, in widget coordinates: corners 0-3 (TL, TR, BL, BR),
 * then sides 4-7 (top, right, bottom, left). */
static gboolean
view_picture_frame (W42View *self, double *fx, double *fy, double *fw, double *fh)
{
  gsize pos;
  int page = 0;
  double x = 0, y = 0, w = 0, h = 0;

  if (!view_selected_picture (self, &pos))
    return FALSE;
  if (!w42_layout_object_rect (self->layout, pos, &page, &x, &y, &w, &h))
    return FALSE;

  *fx = view_page_origin_x (self) + x * self->zoom;
  *fy = view_page_origin_y (self, page) + y * self->zoom;
  *fw = w * self->zoom;
  *fh = h * self->zoom;
  return TRUE;
}

static void
handle_centre (int handle, double fx, double fy, double fw, double fh,
               double *hx, double *hy)
{
  static const double SX[8] = { 0, 1, 0, 1, 0.5, 1, 0.5, 0 };
  static const double SY[8] = { 0, 0, 1, 1, 0, 0.5, 1, 0.5 };

  *hx = fx + fw * SX[handle];
  *hy = fy + fh * SY[handle];
}

static int
view_handle_at (W42View *self, double x, double y)
{
  double fx, fy, fw, fh;

  if (!view_picture_frame (self, &fx, &fy, &fw, &fh))
    return -1;

  for (int i = 0; i < 8; i++)
    {
      double hx, hy;

      handle_centre (i, fx, fy, fw, fh, &hx, &hy);
      if (fabs (x - hx) <= HANDLE_SIZE && fabs (y - hy) <= HANDLE_SIZE)
        return i;
    }

  return -1;
}

static const char *
handle_cursor (int handle)
{
  switch (handle)
    {
    case 0: case 3: return "nwse-resize";
    case 1: case 2: return "nesw-resize";
    case 4: case 6: return "ns-resize";
    case 5: case 7: return "ew-resize";
    default:        return "text";
    }
}

/* A click on the picture itself selects it, so the handles appear. */
static gboolean
view_click_selects_picture (W42View *self, int page, double px, double py, gsize near)
{
  W42PieceTable *pt = view_pt (self);
  gsize candidates[2] = { near, near > 0 ? near - 1 : near };
  const GArray *floats = w42_layout_floats (self->layout);

  /* A wrapped picture sits away from its anchor: hit-test it directly. */
  for (guint i = 0; floats != NULL && i < floats->len; i++)
    {
      const W42FloatBox *f = &g_array_index (floats, W42FloatBox, i);

      if (f->page == page && px >= f->x && px <= f->x + f->w && py >= f->y && py <= f->y + f->h)
        {
          self->anchor = f->pos;
          self->caret = f->pos + 1;
          return TRUE;
        }
    }

  for (int i = 0; i < 2; i++)
    {
      gsize pos = candidates[i];
      int opage = 0;
      double x = 0, y = 0, w = 0, h = 0;

      if (w42_pt_object_at (pt, pos) == W42_OBJECT_NONE)
        continue;
      if (!w42_layout_object_rect (self->layout, pos, &opage, &x, &y, &w, &h))
        continue;
      if (opage == page && px >= x && px <= x + w && py >= y && py <= y + h)
        {
          self->anchor = pos;
          self->caret = pos + 1;
          return TRUE;
        }
    }

  return FALSE;
}

static void
view_drag_handle (W42View *self, double x, double y)
{
  double dx = (x - self->drag_x0) / self->zoom;
  double dy = (y - self->drag_y0) / self->zoom;
  double w = self->pic_w, h = self->pic_h;
  int handle = self->handle;

  /* Which way each handle pulls. */
  if (handle == 1 || handle == 3 || handle == 5) w = self->pic_w + dx;
  if (handle == 0 || handle == 2 || handle == 7) w = self->pic_w - dx;
  if (handle == 2 || handle == 3 || handle == 6) h = self->pic_h + dy;
  if (handle == 0 || handle == 1 || handle == 4) h = self->pic_h - dy;

  /* Corners keep the picture's shape; sides stretch it. */
  if (handle < 4 && self->pic_w > 0 && self->pic_h > 0)
    {
      double scale = MAX (w / self->pic_w, h / self->pic_h);
      w = self->pic_w * scale;
      h = self->pic_h * scale;
    }

  self->new_w = MAX (w, 8.0);
  self->new_h = MAX (h, 8.0);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
view_finish_handle (W42View *self)
{
  W42PieceTable *pt = view_pt (self);
  gsize pos;

  if (self->handle < 0)
    return;
  self->handle = -1;
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "text");

  if (pt == NULL || !view_selected_picture (self, &pos))
    return;

  w42_pt_resize_object (pt, pos,
                        (int) lround (self->new_w * W42_TWIPS_PER_PX),
                        (int) lround (self->new_h * W42_TWIPS_PER_PX));
  self->anchor = pos;
  self->caret = pos + 1;
  view_edited (self);
}

/* ---------------------------------------------------------------------- */
/* Column edges                                                            */
/* ---------------------------------------------------------------------- */

#define EDGE_GRAB 4.0   /* page px either side of a cell edge */

/* The column edge under a point, in widget coordinates. */
static gboolean
view_column_edge_at (W42View *self, double wx, double wy,
                     int *table, int *col, int *page, double *edge_x)
{
  const GArray *rects;
  int p = 0;
  double px = 0, py = 0;

  if (self->doc == NULL)
    return FALSE;

  view_widget_to_page (self, wx, wy, &p, &px, &py);
  rects = w42_layout_cell_rects (self->layout);

  for (guint i = 0; i < rects->len; i++)
    {
      const W42CellRect *r = &g_array_index (rects, W42CellRect, i);
      double edge = r->x + r->w;

      if (r->page != p || py < r->y || py > r->y + r->h)
        continue;
      if (fabs (px - edge) * self->zoom > EDGE_GRAB)
        continue;

      *table = r->table;
      *col = r->col;
      *page = p;
      *edge_x = edge;
      return TRUE;
    }

  return FALSE;
}

static void
view_finish_column_drag (W42View *self)
{
  W42PieceTable *pt = view_pt (self);
  const W42TableProps *props;
  const W42PageSetup *page;
  int table = self->col_table;
  int *widths;
  int text_w, delta;

  if (table < 0)
    return;
  self->col_table = -1;
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "text");

  if (pt == NULL)
    return;

  props = w42_pt_table_props (pt, table);
  page = w42_document_page_setup (self->doc);
  if (props == NULL || page == NULL || props->n_cols < 1)
    return;

  delta = (int) lround ((self->col_x - self->col_x0) * W42_TWIPS_PER_PX);
  if (delta == 0)
    return;

  /* Widths of 0 mean an equal share of the column; make them real before
   * moving one edge, since the columns either side of it must know what
   * they are to give and take. */
  text_w = page->width - page->margin_left - page->margin_right;
  widths = g_new0 (int, props->n_cols);
  for (int c = 0; c < props->n_cols; c++)
    {
      int w = c < (int) props->widths->len ? g_array_index (props->widths, int, c) : 0;
      widths[c] = w > 0 ? w : text_w / props->n_cols;
    }

  {
    int c = CLAMP (self->col_col, 0, props->n_cols - 1);
    int min = 360;

    if (c + 1 < props->n_cols)
      {
        /* An inner edge moves between two columns: one grows by what the
         * other gives up, and the table stays as wide as it was. */
        delta = CLAMP (delta, min - widths[c], widths[c + 1] - min);
        widths[c] += delta;
        widths[c + 1] -= delta;
      }
    else
      {
        widths[c] = MAX (widths[c] + delta, min);
      }
  }

  w42_pt_table_set_widths (pt, table, widths, props->n_cols);
  g_free (widths);
  view_edited (self);
}

static void
view_draw_column_drag (W42View *self, cairo_t *cr)
{
  const GArray *rects = w42_layout_cell_rects (self->layout);
  double top = G_MAXDOUBLE, bottom = -G_MAXDOUBLE;
  double x, oy;

  if (self->col_table < 0)
    return;

  for (guint i = 0; i < rects->len; i++)
    {
      const W42CellRect *r = &g_array_index (rects, W42CellRect, i);

      if (r->table != self->col_table || r->page != self->col_page)
        continue;
      top = MIN (top, r->y);
      bottom = MAX (bottom, r->y + r->h);
    }
  if (top > bottom)
    return;

  x  = view_page_origin_x (self) + self->col_x * self->zoom;
  oy = view_page_origin_y (self, self->col_page);

  cairo_save (cr);
  cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
  cairo_set_line_width (cr, 1.0);
  {
    double dash[2] = { 3.0, 3.0 };
    cairo_set_dash (cr, dash, 2, 0.0);
  }
  cairo_move_to (cr, floor (x) + 0.5, oy + top * self->zoom);
  cairo_line_to (cr, floor (x) + 0.5, oy + bottom * self->zoom);
  cairo_stroke (cr);
  cairo_restore (cr);
}

static void
view_draw_handles (W42View *self, cairo_t *cr)
{
  double fx, fy, fw, fh;

  if (!view_picture_frame (self, &fx, &fy, &fw, &fh))
    return;

  if (self->handle >= 0)
    {
      /* The outline of the size it will be, anchored at the corner or side
       * opposite the one being dragged. */
      double ox = fx, oy = fy;
      double w = self->new_w * self->zoom, h = self->new_h * self->zoom;

      if (self->handle == 0 || self->handle == 2 || self->handle == 7)
        ox = fx + fw - w;
      if (self->handle == 0 || self->handle == 1 || self->handle == 4)
        oy = fy + fh - h;
      if (self->handle == 5 || self->handle == 7)
        oy = fy;
      if (self->handle == 4 || self->handle == 6)
        ox = fx;

      cairo_save (cr);
      cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
      cairo_set_line_width (cr, 1.0);
      {
        double dash[2] = { 3.0, 3.0 };
        cairo_set_dash (cr, dash, 2, 0.0);
      }
      cairo_rectangle (cr, floor (ox) + 0.5, floor (oy) + 0.5,
                       floor (w), floor (h));
      cairo_stroke (cr);
      cairo_restore (cr);
    }

  cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
  for (int i = 0; i < 8; i++)
    {
      double hx, hy;

      handle_centre (i, fx, fy, fw, fh, &hx, &hy);
      cairo_rectangle (cr, floor (hx - HANDLE_SIZE / 2), floor (hy - HANDLE_SIZE / 2),
                       HANDLE_SIZE, HANDLE_SIZE);
    }
  cairo_fill (cr);
}

/* Whether a press at widget coordinates (x, y) lands inside the
 * selection — the press that begins a drag of the selected text. */
static gboolean
view_press_in_selection (W42View *self, double x, double y)
{
  int page = 0;
  double px = 0, py = 0;
  gsize pos;

  if (view_pt (self) == NULL || !w42_view_has_selection (self))
    return FALSE;

  view_widget_to_page (self, x, y, &page, &px, &py);
  pos = w42_pt_clamp_pos (view_pt (self),
                          w42_layout_point_to_pos (self->layout, page, px, py));
  return pos >= sel_start (self) && pos < sel_end (self);
}

static void
on_click_pressed (GtkGestureClick *gesture,
                  int              n_press,
                  double           x,
                  double           y,
                  gpointer         data)
{
  W42View *self = data;
  int page = 0;
  double px = 0, py = 0;
  gsize pos;

  (void) gesture;

  gtk_widget_grab_focus (GTK_WIDGET (self));

  if (self->doc == NULL)
    return;

  /* A handle of the selected picture, or the edge of a table column,
   * belongs to the drag gesture. */
  if (n_press == 1)
    {
      int t, c, pg;
      double ex;

      if (view_handle_at (self, x, y) >= 0 ||
          view_column_edge_at (self, x, y, &t, &c, &pg, &ex))
        {
          self->dragging = FALSE;
          return;
        }
    }

  view_widget_to_page (self, x, y, &page, &px, &py);
  pos = w42_layout_point_to_pos (self->layout, page, px, py);
  pos = w42_pt_clamp_pos (view_pt (self), pos);

  /* Ctrl+click on a link follows it, as Word did. */
  if (n_press == 1 &&
      (gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture)) &
       GDK_CONTROL_MASK))
    {
      /* The character either side of the position: ap_at prefers the left
       * one, and a click on a link's first letter lands on its left. */
      const W42Fmt *fmt = w42_ap_table_get (w42_pt_ap_table (view_pt (self)),
                                            w42_pt_ap_at (view_pt (self), pos));

      if (fmt->ch.link == NULL)
        fmt = w42_ap_table_get (w42_pt_ap_table (view_pt (self)),
                                w42_pt_ap_at (view_pt (self), pos + 1));

      if (fmt->ch.link != NULL && w42_view_follow_link (self, fmt->ch.link))
        {
          self->dragging = FALSE;
          return;
        }
    }

  if (n_press == 1 && view_click_selects_picture (self, page, px, py, pos))
    {
      self->dragging = FALSE;
      view_caret_moved (self, FALSE);
      return;
    }

  if (n_press >= 3)
    {
      view_select_block_at (self, pos);
      self->dragging = FALSE;
    }
  else if (n_press == 2)
    {
      view_select_word_at (self, pos);
      self->dragging = FALSE;
    }
  else
    {
      /* Shift+click extends the selection to the click, as in Word. */
      GdkModifierType mods = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));

      /* A press inside the selection arms a drag of the selected text;
       * the selection stands until the button goes up or the drag moves. */
      if (!(mods & GDK_SHIFT_MASK) &&
          w42_view_has_selection (self) &&
          pos >= sel_start (self) && pos < sel_end (self))
        {
          self->text_drag_armed = TRUE;
          self->dragging = FALSE;
          return;
        }

      self->caret = pos;
      if (!(mods & GDK_SHIFT_MASK))
        self->anchor = pos;
      self->dragging = TRUE;
    }

  view_caret_moved (self, FALSE);
}

static void
on_click_released (GtkGestureClick *gesture,
                   int              n_press,
                   double           x,
                   double           y,
                   gpointer         data)
{
  W42View *self = data;

  (void) gesture; (void) n_press;

  /* The press was inside the selection but never became a drag: a plain
   * click, so the caret goes where it pointed. */
  if (self->text_drag_armed && !self->text_dragging && self->doc != NULL)
    {
      int page = 0;
      double px = 0, py = 0;

      view_widget_to_page (self, x, y, &page, &px, &py);
      self->caret = self->anchor =
        w42_pt_clamp_pos (view_pt (self),
                          w42_layout_point_to_pos (self->layout, page, px, py));
      view_caret_moved (self, FALSE);
    }

  self->text_drag_armed = FALSE;
  self->dragging = FALSE;
}

/* Dragging a handle of the selected picture. */
static void
on_drag_begin (GtkGestureDrag *gesture, double x, double y, gpointer data)
{
  W42View *self = data;
  int handle;
  double fx, fy, fw, fh;

  if (self->doc == NULL)
    return;

  handle = view_handle_at (self, x, y);
  if (handle < 0)
    {
      int table, col, page;
      double edge_x;

      if (view_column_edge_at (self, x, y, &table, &col, &page, &edge_x))
        {
          self->col_table = table;
          self->col_col   = col;
          self->col_page  = page;
          self->col_x0    = edge_x;
          self->col_x     = edge_x;
          self->drag_x0   = x;
          self->drag_y0   = y;
          self->dragging  = FALSE;
          gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
          return;
        }

      /* Pressed inside the selection: the drag, if it becomes one, moves
       * the text.  Undecided until it moves, so a plain click can still
       * place the caret when the button goes up.  This runs before the
       * click gesture's own press handler, so it looks at the press
       * itself rather than at the flag that handler sets. */
      if (view_press_in_selection (self, x, y) &&
          !(gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture)) &
            GDK_SHIFT_MASK))
        {
          self->text_drag_armed = TRUE;
          self->drag_x0 = x;
          self->drag_y0 = y;
          self->dragging = FALSE;
          return;
        }

      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
    }

  view_picture_frame (self, &fx, &fy, &fw, &fh);
  self->handle  = handle;
  self->drag_x0 = x;
  self->drag_y0 = y;
  self->pic_w   = fw / self->zoom;
  self->pic_h   = fh / self->zoom;
  self->new_w   = self->pic_w;
  self->new_h   = self->pic_h;
  self->dragging = FALSE;

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_drag_update (GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
  W42View *self = data;

  (void) gesture;

  if (self->handle >= 0)
    view_drag_handle (self, self->drag_x0 + dx, self->drag_y0 + dy);
  else if (self->col_table >= 0)
    {
      self->col_x = self->col_x0 + dx / self->zoom;
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
  else if (self->text_drag_armed)
    {
      int page = 0;
      double px = 0, py = 0;

      if (!self->text_dragging)
        {
          /* Still a click until it moves a click's worth. */
          if (ABS (dx) < 4 && ABS (dy) < 4)
            return;
          self->text_dragging = TRUE;
          gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
          gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "move");
        }

      view_widget_to_page (self, self->drag_x0 + dx, self->drag_y0 + dy,
                           &page, &px, &py);
      self->drop_pos = w42_pt_clamp_pos (view_pt (self),
                                         w42_layout_point_to_pos (self->layout,
                                                                  page, px, py));
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static void
on_drag_end (GtkGestureDrag *gesture, double dx, double dy, gpointer data)
{
  W42View *self = data;

  (void) gesture;

  if (self->handle >= 0)
    {
      view_drag_handle (self, self->drag_x0 + dx, self->drag_y0 + dy);
      view_finish_handle (self);
    }
  else if (self->col_table >= 0)
    {
      self->col_x = self->col_x0 + dx / self->zoom;
      view_finish_column_drag (self);
    }
  else if (self->text_dragging)
    {
      GdkModifierType mods = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (gesture));
      int page = 0;
      double px = 0, py = 0;

      view_widget_to_page (self, self->drag_x0 + dx, self->drag_y0 + dy,
                           &page, &px, &py);
      view_drop_text (self,
                      w42_pt_clamp_pos (view_pt (self),
                                        w42_layout_point_to_pos (self->layout,
                                                                 page, px, py)),
                      (mods & GDK_CONTROL_MASK) != 0);
      self->text_dragging = FALSE;
      self->text_drag_armed = FALSE;
      gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "text");
      gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static void
on_motion (GtkEventControllerMotion *controller,
           double                    x,
           double                    y,
           gpointer                  data)
{
  W42View *self = data;
  int page = 0;
  double px = 0, py = 0;

  (void) controller;

  if (self->doc == NULL)
    return;

  if (self->handle >= 0 || self->col_table >= 0 || self->text_dragging)
    return;

  if (!self->dragging)
    {
      /* Over a handle or a column edge the pointer says what dragging it
       * would do. */
      int handle = view_handle_at (self, x, y);
      int t, c, pg;
      double ex;
      const char *cursor = handle_cursor (handle);

      if (handle < 0 && view_column_edge_at (self, x, y, &t, &c, &pg, &ex))
        cursor = "col-resize";
      else if (handle < 0 && view_pt (self) != NULL)
        {
          int lpage = 0;
          double lx = 0, ly = 0;
          gsize pos;
          const W42Fmt *fmt;

          view_widget_to_page (self, x, y, &lpage, &lx, &ly);
          pos = w42_pt_clamp_pos (view_pt (self),
                                  w42_layout_point_to_pos (self->layout, lpage, lx, ly));
          fmt = w42_ap_table_get (w42_pt_ap_table (view_pt (self)),
                                  w42_pt_ap_at (view_pt (self), pos));
          if (fmt->ch.link == NULL)
            fmt = w42_ap_table_get (w42_pt_ap_table (view_pt (self)),
                                    w42_pt_ap_at (view_pt (self), pos + 1));
          if (fmt->ch.link != NULL)
            cursor = "pointer";
        }

      gtk_widget_set_cursor_from_name (GTK_WIDGET (self), cursor);
      return;
    }

  view_widget_to_page (self, x, y, &page, &px, &py);
  self->caret = w42_pt_clamp_pos (view_pt (self),
                                  w42_layout_point_to_pos (self->layout,
                                                           page, px, py));

  gtk_widget_queue_draw (GTK_WIDGET (self));
  view_state_changed (self);
}

static void
on_focus_enter (GtkEventControllerFocus *controller, gpointer data)
{
  W42View *self = data;

  (void) controller;
  gtk_im_context_focus_in (self->im);
  view_reset_blink (self);
}

static void
on_focus_leave (GtkEventControllerFocus *controller, gpointer data)
{
  W42View *self = data;

  (void) controller;
  gtk_im_context_focus_out (self->im);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_view_realize (GtkWidget *widget, gpointer data)
{
  (void) data;
  gtk_im_context_set_client_widget (W42_VIEW (widget)->im, widget);
}

static void
on_view_unrealize (GtkWidget *widget, gpointer data)
{
  (void) data;
  gtk_im_context_set_client_widget (W42_VIEW (widget)->im, NULL);
}

static gboolean
on_blink (gpointer data)
{
  W42View *self = data;

  self->blink_on = !self->blink_on;
  gtk_widget_queue_draw (GTK_WIDGET (self));

  return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------- */
/* Painting                                                                */
/* ---------------------------------------------------------------------- */

/* Highlight bytes [start_byte, end_byte) of one line.  Pango reports the
 * visual ranges relative to the paragraph's text column, which is exactly
 * what W42LineBox.origin_x records. */
static void
draw_selection_on_line (cairo_t          *cr,
                        const W42LineBox *box,
                        gsize             start_byte,
                        gsize             end_byte)
{
  int *ranges = NULL;
  int n_ranges = 0;

  if (end_byte <= start_byte)
    return;

  pango_layout_line_get_x_ranges (box->line, (int) start_byte, (int) end_byte,
                                  &ranges, &n_ranges);

  for (int i = 0; i < n_ranges; i++)
    {
      double x0 = (double) ranges[2 * i] / PANGO_SCALE;
      double x1 = (double) ranges[2 * i + 1] / PANGO_SCALE;

      cairo_rectangle (cr, box->origin_x + x0, box->y, x1 - x0, box->height);
    }

  cairo_fill (cr);
  g_free (ranges);
}

/* The dotted rectangle Word calls "text boundaries". */
static void
draw_text_boundaries (W42View *self, cairo_t *cr)
{
  const W42PageSetup *page = w42_document_page_setup (self->doc);
  double dashes[2] = { 3.0, 3.0 };
  double l = w42_twips_to_px (page->margin_left);
  double t = w42_twips_to_px (page->margin_top);
  double w = w42_twips_to_px (page->width - page->margin_left -
                              page->margin_right);
  double h = w42_twips_to_px (page->height - page->margin_top -
                              page->margin_bottom);

  cairo_save (cr);
  cairo_set_source_rgb (cr, 0.72, 0.72, 0.72);
  cairo_set_line_width (cr, 1.0 / self->zoom);
  cairo_set_dash (cr, dashes, 2, 0.0);
  cairo_rectangle (cr, l, t, w, h);
  cairo_stroke (cr);
  cairo_restore (cr);
}

static void
view_draw (W42View *self, cairo_t *cr, int width, int height)
{
  W42Layout *layout = self->layout;
  const GArray *lines = w42_layout_lines (layout);
  GPtrArray *blocks = w42_layout_blocks (layout);
  double page_w = w42_layout_page_width (layout);
  double page_h = w42_layout_page_height (layout);
  double zoom = self->zoom;
  double ox = view_page_origin_x (self);
  gboolean focused = gtk_widget_has_focus (GTK_WIDGET (self));
  gboolean has_sel = w42_view_has_selection (self);
  gsize sel_a = sel_start (self);
  gsize sel_b = sel_end (self);
  int n_pages = w42_layout_n_pages (layout);

  gboolean paged = (self->mode == W42_VIEW_PAGE_LAYOUT);
  double paper_r = 1.0, paper_g = 1.0, paper_b = 1.0;

  {
    /* Format > Background: the colour the paper is, white unless the
     * document says otherwise. */
    const W42PageSetup *setup = self->doc != NULL
                                  ? w42_document_page_setup (self->doc) : NULL;
    double pr = 1.0, pg = 1.0, pb = 1.0;

    if (setup != NULL && setup->has_background)
      {
        pr = ((setup->background >> 16) & 0xFF) / 255.0;
        pg = ((setup->background >> 8) & 0xFF) / 255.0;
        pb = (setup->background & 0xFF) / 255.0;
      }
    paper_r = pr; paper_g = pg; paper_b = pb;
  }

  if (paged)
    {
      /* The light grey desk the sheets sit on. */
      cairo_set_source_rgb (cr, 0.86, 0.86, 0.86);
    }
  else
    {
      /* Normal view is paper all the way out to the window frame. */
      cairo_set_source_rgb (cr, paper_r, paper_g, paper_b);
    }
  cairo_paint (cr);

  if (blocks == NULL)
    return;

  for (int p = 0; p < n_pages; p++)
    {
      double oy = view_page_origin_y (self, p);
      double pw = page_w * zoom;
      double ph = page_h * zoom;

      if (oy > height || oy + ph < 0)
        continue;

      if (paged)
        {
          /* The sheet, centred, with a hairline round it. */
          cairo_set_source_rgb (cr, paper_r, paper_g, paper_b);
          cairo_rectangle (cr, ox, oy, pw, ph);
          cairo_fill (cr);
          cairo_set_source_rgb (cr, 0.62, 0.62, 0.62);
          cairo_set_line_width (cr, 1.0);
          cairo_rectangle (cr, floor (ox) + 0.5, floor (oy) + 0.5, floor (pw), floor (ph));
          cairo_stroke (cr);
        }

      cairo_save (cr);
      if (paged)
        {
          cairo_rectangle (cr, ox, oy, pw, ph);
          cairo_clip (cr);
        }
      cairo_translate (cr, ox, oy);
      cairo_scale (cr, zoom, zoom);

      if (paged)
        draw_text_boundaries (self, cr);

      w42_layout_draw_backdrop (layout, cr, p);

      for (guint i = 0; i < lines->len; i++)
        {
          const W42LineBox *box = &g_array_index (lines, W42LineBox, i);
          const W42Block *blk;
          gsize line_first, line_last;

          if (box->page != p)
            continue;

          blk = g_ptr_array_index (blocks, box->block);
          line_first = w42_block_byte_to_pos (blk, box->start_index);
          line_last  = w42_block_byte_to_pos (blk,
                                              box->start_index + box->length);

          if (has_sel && sel_b > line_first && sel_a < line_last + 1)
            {
              gsize a = MAX (sel_a, line_first);
              gsize b = MIN (sel_b, line_last);

              cairo_set_source_rgb (cr, 0.0, 0.0, 0.50);

              if (b > a)
                draw_selection_on_line (cr, box,
                                        w42_block_pos_to_byte (blk, a),
                                        w42_block_pos_to_byte (blk, b));

              /* A selection that runs on into the next paragraph shows the
               * paragraph mark itself as a short highlighted stub. */
              if (sel_b > line_last &&
                  box->start_index + box->length >= blk->text->len)
                {
                  cairo_rectangle (cr, box->x + box->width, box->y,
                                   6.0, box->height);
                  cairo_fill (cr);
                }
            }

          cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
          w42_layout_draw_line (layout, cr, box);
        }

      cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
      w42_layout_draw_furniture (layout, cr, p);

      cairo_restore (cr);
    }

  if (focused)
    {
      view_draw_handles (self, cr);
      view_draw_column_drag (self, cr);
    }

  if (focused && self->blink_on)
    {
      int cpage = 0;
      double cx = 0, cy = 0, ch = 0;

      if (w42_layout_pos_to_caret (layout, self->caret, &cpage, &cx, &cy, &ch))
        {
          double x = ox + cx * zoom;
          double y = view_page_origin_y (self, cpage) + cy * zoom;

          cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
          cairo_rectangle (cr, floor (x), y, 1.0, ch * zoom);
          cairo_fill (cr);
        }
    }

  /* Dragging the selection: a grey caret marks where the text will drop. */
  if (self->text_dragging)
    {
      int cpage = 0;
      double cx = 0, cy = 0, ch = 0;

      if (w42_layout_pos_to_caret (layout, self->drop_pos, &cpage, &cx, &cy, &ch))
        {
          double x = ox + cx * zoom;
          double y = view_page_origin_y (self, cpage) + cy * zoom;

          cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
          cairo_rectangle (cr, floor (x), y, 2.0, ch * zoom);
          cairo_fill (cr);
        }
    }
}

static void
w42_view_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  W42View *self = W42_VIEW (widget);
  int width  = gtk_widget_get_width (widget);
  int height = gtk_widget_get_height (widget);
  cairo_t *cr;

  if (self->doc == NULL || width <= 0 || height <= 0)
    return;

  cr = gtk_snapshot_append_cairo (snapshot,
                                  &GRAPHENE_RECT_INIT (0, 0, width, height));

  view_draw (self, cr, width, height);

  cairo_destroy (cr);
}

static void
w42_view_measure (GtkWidget      *widget,
                  GtkOrientation  orientation,
                  int             for_size,
                  int            *minimum,
                  int            *natural,
                  int            *minimum_baseline,
                  int            *natural_baseline)
{
  W42View *self = W42_VIEW (widget);
  double size;

  (void) for_size;

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    {
      size = w42_layout_page_width (self->layout) * self->zoom;
      if (self->mode == W42_VIEW_PAGE_LAYOUT)
        size += 2 * PAGE_GAP;
    }
  else
    {
      double page_h = w42_layout_page_height (self->layout) * self->zoom;
      int n = w42_layout_n_pages (self->layout);

      size = (self->mode == W42_VIEW_NORMAL)
               ? page_h
               : PAGE_GAP + n * (page_h + PAGE_GAP);
    }

  *minimum = *natural = (int) ceil (size);
  *minimum_baseline = *natural_baseline = -1;
}

/* ---------------------------------------------------------------------- */
/* Object plumbing                                                         */
/* ---------------------------------------------------------------------- */

/* A right click outside the selection moves the caret there first, so
 * the menu acts on what was clicked. */
static void
on_secondary_pressed (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  W42View *self = data;
  W42PieceTable *pt = view_pt (self);
  GdkRectangle at = { (int) x, (int) y, 1, 1 };

  (void) gesture; (void) n_press;
  if (pt == NULL)
    return;
  {
    int page;
    double px, py;
    gsize pos;

    view_widget_to_page (self, x, y, &page, &px, &py);
    pos = w42_pt_clamp_pos (pt, w42_layout_point_to_pos (self->layout, page, px, py));
    if (!w42_view_has_selection (self) || pos < sel_start (self) || pos > sel_end (self))
      {
        self->caret = self->anchor = pos;
        view_caret_moved (self, FALSE);
      }
  }
  gtk_widget_grab_focus (GTK_WIDGET (self));
  gtk_popover_set_pointing_to (GTK_POPOVER (self->context_menu), &at);
  gtk_popover_popup (GTK_POPOVER (self->context_menu));
}

/* Shift+F10 and the Menu key put the context menu where the caret is,
 * which is where the keyboard is looking. */
static void
view_popup_at_caret (W42View *self)
{
  GdkRectangle at = { 0, 0, 1, 1 };
  int page = 0;
  double x = 0, y = 0, h = 0;

  if (self->context_menu == NULL)
    return;
  if (w42_layout_pos_to_caret (self->layout, self->caret, &page, &x, &y, &h))
    {
      at.x = (int) (view_page_origin_x (self) + x * self->zoom);
      at.y = (int) (view_page_origin_y (self, page) + (y + h) * self->zoom);
    }
  gtk_popover_set_pointing_to (GTK_POPOVER (self->context_menu), &at);
  gtk_popover_popup (GTK_POPOVER (self->context_menu));
}

static void
w42_view_dispose (GObject *object)
{
  W42View *self = W42_VIEW (object);

  g_clear_pointer (&self->context_menu, gtk_widget_unparent);

  if (self->blink_id != 0)
    {
      g_source_remove (self->blink_id);
      self->blink_id = 0;
    }

  if (self->doc != NULL && self->doc_changed_id != 0)
    {
      g_signal_handler_disconnect (self->doc, self->doc_changed_id);
      self->doc_changed_id = 0;
    }

  g_clear_object (&self->doc);
  g_clear_object (&self->im);
  g_clear_pointer (&self->layout, w42_layout_free);

  if (self->repeat_text != NULL)
    {
      g_string_free (self->repeat_text, TRUE);
      self->repeat_text = NULL;
    }

  G_OBJECT_CLASS (w42_view_parent_class)->dispose (object);
}

static void
w42_view_class_init (W42ViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = w42_view_dispose;

  widget_class->snapshot = w42_view_snapshot;
  widget_class->measure  = w42_view_measure;

  gtk_widget_class_set_css_name (widget_class, "w42view");

  signals[SIGNAL_STATE_CHANGED] =
    g_signal_new ("state-changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
w42_view_init (W42View *self)
{
  GtkEventController *key, *motion, *focus;
  GtkGesture *click;

  self->layout = w42_layout_new ();
  self->mode   = W42_VIEW_NORMAL;
  self->zoom   = 1.0;

  w42_layout_set_galley (self->layout, TRUE);
  self->caret  = 2;
  self->anchor = 2;
  self->want_x = -1.0;
  self->blink_on = TRUE;

  gtk_widget_set_focusable (GTK_WIDGET (self), TRUE);
  gtk_widget_set_can_focus (GTK_WIDGET (self), TRUE);
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "text");
  self->handle = -1;
  self->col_table = -1;

  self->im = gtk_im_multicontext_new ();
  g_signal_connect (self->im, "commit", G_CALLBACK (on_im_commit), self);

  /* The input method is given the widget only once the widget has a surface
   * to give it; the Windows IME backend asks for one immediately. */
  g_signal_connect (self, "realize", G_CALLBACK (on_view_realize), NULL);
  g_signal_connect (self, "unrealize", G_CALLBACK (on_view_unrealize), NULL);

  key = gtk_event_controller_key_new ();
  g_signal_connect (key, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (GTK_WIDGET (self), key);

  click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_click_pressed), self);
  g_signal_connect (click, "released", G_CALLBACK (on_click_released), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

  {
    /* The right button: the usual short menu, as Word 6 had from 6.0. */
    GtkGesture *secondary = gtk_gesture_click_new ();
    GMenu *menu = g_menu_new ();
    GMenu *section;

    section = g_menu_new ();
    g_menu_append (section, "Cu_t", "win.cut");
    g_menu_append (section, "_Copy", "win.copy");
    g_menu_append (section, "_Paste", "win.paste");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
    g_object_unref (section);
    section = g_menu_new ();
    g_menu_append (section, "_Font...", "win.font");
    g_menu_append (section, "P_aragraph...", "win.paragraph");
    g_menu_append (section, "_Bullets and Numbering...", "win.bullets-numbering");
    g_menu_append (section, "_Hyperlink...", "win.hyperlink");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
    g_object_unref (section);
    section = g_menu_new ();
    g_menu_append (section, "Table P_roperties...", "win.table-properties");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
    g_object_unref (section);

    self->context_menu = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
    g_object_unref (menu);
    gtk_popover_set_has_arrow (GTK_POPOVER (self->context_menu), FALSE);
    gtk_widget_set_halign (self->context_menu, GTK_ALIGN_START);
    gtk_widget_set_parent (self->context_menu, GTK_WIDGET (self));

    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (secondary), GDK_BUTTON_SECONDARY);
    g_signal_connect (secondary, "pressed", G_CALLBACK (on_secondary_pressed), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (secondary));
  }

  {
    GtkGesture *drag = gtk_gesture_drag_new ();

    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
    g_signal_connect (drag, "drag-begin", G_CALLBACK (on_drag_begin), self);
    g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
    g_signal_connect (drag, "drag-end", G_CALLBACK (on_drag_end), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));
  }

  motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
  gtk_widget_add_controller (GTK_WIDGET (self), motion);

  focus = gtk_event_controller_focus_new ();
  g_signal_connect (focus, "enter", G_CALLBACK (on_focus_enter), self);
  g_signal_connect (focus, "leave", G_CALLBACK (on_focus_leave), self);
  gtk_widget_add_controller (GTK_WIDGET (self), focus);

  self->blink_id = g_timeout_add (CARET_PERIOD, on_blink, self);
}

GtkWidget *
w42_view_new (void)
{
  return g_object_new (W42_TYPE_VIEW, NULL);
}

void
w42_view_set_document (W42View *self, W42Document *doc)
{
  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (doc == NULL || W42_IS_DOCUMENT (doc));

  if (self->doc == doc)
    return;

  if (self->doc != NULL && self->doc_changed_id != 0)
    {
      g_signal_handler_disconnect (self->doc, self->doc_changed_id);
      self->doc_changed_id = 0;
    }

  g_set_object (&self->doc, doc);

  /* The shaped paragraphs the layout keeps belong to the document it
   * had: another document's formatting is numbered its own way. */
  w42_layout_forget_shaping (self->layout);

  if (doc != NULL)
    {
      self->doc_changed_id = g_signal_connect (doc, "changed",
                                               G_CALLBACK (on_document_changed),
                                               self);
      self->caret = self->anchor = w42_pt_first_caret_pos (w42_document_pt (doc));
      view_relayout (self);
    }

  view_state_changed (self);
}

W42Document *
w42_view_get_document (W42View *self)
{
  g_return_val_if_fail (W42_IS_VIEW (self), NULL);
  return self->doc;
}

W42Layout *
w42_view_get_layout (W42View *self)
{
  g_return_val_if_fail (W42_IS_VIEW (self), NULL);
  return self->layout;
}

void
w42_view_get_text_column (W42View *self,
                          double  *left,
                          double  *width,
                          double  *page_left,
                          double  *page_width)
{
  const W42PageSetup *page;
  double ox, zoom, margin_l, margin_r, pw;

  g_return_if_fail (W42_IS_VIEW (self));

  if (self->doc == NULL)
    return;

  page = w42_document_page_setup (self->doc);
  zoom = self->zoom;
  ox = view_page_origin_x (self);
  pw = w42_layout_page_width (self->layout) * zoom;

  /* Normal view drops the page margins for a narrow inset, so the numbers
   * come from the layout rather than from the page setup. */
  if (self->mode == W42_VIEW_NORMAL)
    {
      margin_l = w42_twips_to_px (360) * zoom;
      margin_r = margin_l;
    }
  else
    {
      margin_l = w42_twips_to_px (page->margin_left) * zoom;
      margin_r = w42_twips_to_px (page->margin_right) * zoom;
    }

  if (page_left)  *page_left  = ox;
  if (page_width) *page_width = pw;
  if (left)       *left       = ox + margin_l;
  if (width)
    {
      double text_w = pw - margin_l - margin_r;

      /* With columns, the ruler measures the first one. */
      if (self->mode == W42_VIEW_PAGE_LAYOUT && w42_page_columns (page) > 1)
        {
          int n = w42_page_columns (page);
          double gap = w42_twips_to_px (w42_page_column_gap (page)) * zoom;

          text_w = (text_w - (n - 1) * gap) / n;
        }
      *width = text_w;
    }
}

void
w42_view_set_mode (W42View *self, W42ViewMode mode)
{
  g_return_if_fail (W42_IS_VIEW (self));

  if (self->mode == mode)
    return;

  self->mode = mode;
  w42_layout_set_galley (self->layout, mode == W42_VIEW_NORMAL);

  view_relayout (self);
  view_scroll_to_caret (self);
  view_state_changed (self);
}

W42ViewMode
w42_view_get_mode (W42View *self)
{
  g_return_val_if_fail (W42_IS_VIEW (self), W42_VIEW_NORMAL);
  return self->mode;
}

void
w42_view_set_zoom (W42View *self, double zoom)
{
  g_return_if_fail (W42_IS_VIEW (self));

  self->zoom = CLAMP (zoom, 0.25, 5.0);
  gtk_widget_queue_resize (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
  view_state_changed (self);
}

double
w42_view_get_zoom (W42View *self)
{
  g_return_val_if_fail (W42_IS_VIEW (self), 1.0);
  return self->zoom;
}

gsize
w42_view_get_caret (W42View *self)
{
  g_return_val_if_fail (W42_IS_VIEW (self), 0);
  return self->caret;
}

gboolean
w42_view_get_picture (W42View *self, int *width, int *height, W42Wrap *wrap)
{
  W42PieceTable *pt;
  const W42Object *object;
  gsize pos;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);

  pt = view_pt (self);
  if (pt == NULL || !view_selected_picture (self, &pos))
    return FALSE;
  object = w42_object_table_get (w42_pt_object_table (pt), w42_pt_object_at (pt, pos));
  if (object == NULL)
    return FALSE;
  if (width)  *width  = object->width;
  if (height) *height = object->height;
  if (wrap)   *wrap   = object->wrap;
  return TRUE;
}

void
w42_view_set_picture (W42View *self, int width, int height, W42Wrap wrap)
{
  W42PieceTable *pt;
  gsize pos;

  g_return_if_fail (W42_IS_VIEW (self));

  pt = view_pt (self);
  if (pt == NULL || !view_selected_picture (self, &pos))
    return;

  w42_pt_begin_group (pt);
  w42_pt_resize_object (pt, pos, width, height);
  w42_pt_set_object_wrap (pt, pos, wrap);
  w42_pt_end_group (pt);
  self->anchor = pos;
  self->caret = pos + 1;
  view_edited (self);
}

void
w42_view_table_autoformat (W42View *self, const W42TableFormat *fmt,
                           gboolean heading, gboolean first_column)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (fmt != NULL);

  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  if (w42_pt_table_autoformat (pt, table, fmt, heading, first_column))
    view_edited (self);
}

void
w42_view_table_split_cell (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  /* A cell merged downwards is given its rows back first; only then is a
   * cell merged sideways split, as Word's Split Cells does. */
  if (w42_pt_split_cells_down (pt, table, row, col))
    {
      self->anchor = self->caret = w42_pt_cell_start (pt, table, row, col);
      view_edited (self);
      return;
    }
  w42_pt_table_split_cell (pt, table, row, col);
  self->anchor = self->caret = w42_pt_cell_start (pt, table, row, col);
  view_edited (self);
}

int
w42_view_table_get_row_height (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_val_if_fail (W42_IS_VIEW (self), 0);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return 0;
  return w42_pt_table_get_row_height (pt, table, row);
}

void
w42_view_table_set_row_height (W42View *self, int twips)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  if (w42_pt_table_get_row_height (pt, table, row) == twips)
    return;
  w42_pt_table_set_row_height (pt, table, row, twips);
  view_edited (self);
}

int
w42_view_table_get_header_rows (W42View *self)
{
  W42PieceTable *pt;
  const W42TableProps *props;
  int table, row, col;

  g_return_val_if_fail (W42_IS_VIEW (self), 0);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return 0;
  props = w42_pt_table_props (pt, table);
  return props != NULL ? props->header_rows : 0;
}

void
w42_view_table_set_header_rows (W42View *self, int n)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  if (w42_view_table_get_header_rows (self) == n)
    return;
  w42_pt_table_set_header_rows (pt, table, n);
  view_edited (self);
}

void
w42_view_set_drop_cap (W42View *self, int lines)
{
  W42ParaFmt pa;

  g_return_if_fail (W42_IS_VIEW (self));
  w42_view_get_para_fmt (self, &pa);
  pa.drop_cap = (guint8) CLAMP (lines, 0, 10);
  w42_view_apply_para_fmt (self, W42_PARA_FRAME, &pa);
}

void
w42_view_set_frame (W42View *self, int side, int width)
{
  W42ParaFmt pa;

  g_return_if_fail (W42_IS_VIEW (self));
  w42_view_get_para_fmt (self, &pa);
  pa.frame_side = (guint8) CLAMP (side, 0, 2);
  pa.frame_width = CLAMP (width, 0, 31680);
  w42_view_apply_para_fmt (self, W42_PARA_FRAME, &pa);
}

static gsize
fmt_probe (W42View *self)
{
  return w42_view_has_selection (self) ? sel_start (self) + 1 : sel_start (self);
}

int
w42_view_cell_get_borders (W42View *self)
{
  W42PieceTable *pt;
  const W42TableProps *props;
  int table, row, col, own;

  g_return_val_if_fail (W42_IS_VIEW (self), 0);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return 0;
  own = w42_pt_cell_get_borders (pt, table, row, col);
  if (own >= 0)
    return own;
  props = w42_pt_table_props (pt, table);
  return props == NULL || props->borders ? W42_BORDER_BOX : 0;
}

/* The colour behind the caret's cell, which the cell's own mark carries and
 * every format that has a way to say it keeps. */
void
w42_view_cell_set_fill (W42View *self, gboolean has, guint32 rgb)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  w42_pt_cell_set_fill (pt, table, row, col, has, rgb);
  view_edited (self);
}

gboolean
w42_view_cell_get_fill (W42View *self, guint32 *rgb)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return FALSE;
  return w42_pt_cell_get_fill (pt, table, row, col, rgb);
}

gboolean
w42_view_cell_get_fmt (W42View *self, W42ParaFmt *out)
{
  W42PieceTable *pt;
  int table, row, col;
  const W42ParaFmt *pa;

  g_return_val_if_fail (W42_IS_VIEW (self) && out != NULL, FALSE);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return FALSE;
  pa = w42_pt_cell_get_fmt (pt, table, row, col);
  if (pa == NULL)
    return FALSE;
  *out = *pa;
  return TRUE;
}

void
w42_view_cell_set_fmt (W42View *self, const W42ParaFmt *pa)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self) && pa != NULL);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  w42_pt_cell_set_fmt (pt, table, row, col, pa);
  view_edited (self);
}

gboolean
w42_view_table_get_edges (W42View *self, W42BorderEdge *out)
{
  W42PieceTable *pt;
  int table, row, col;
  const W42TableProps *props;

  g_return_val_if_fail (W42_IS_VIEW (self) && out != NULL, FALSE);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return FALSE;
  props = w42_pt_table_props (pt, table);
  if (props == NULL)
    return FALSE;
  memcpy (out, props->edge, sizeof props->edge);
  return TRUE;
}

void
w42_view_table_set_edges (W42View *self, const W42BorderEdge *outer, const W42BorderEdge *inside)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  w42_pt_table_set_edges (pt, table, outer, inside);
  view_edited (self);
}

void
w42_view_cell_set_borders (W42View *self, int sides)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  w42_pt_cell_set_borders (pt, table, row, col, sides);
  view_edited (self);
}

void
w42_view_select_word (W42View *self)
{
  g_return_if_fail (W42_IS_VIEW (self));
  if (view_pt (self) == NULL)
    return;
  view_select_word_at (self, self->caret);
  view_caret_moved (self, FALSE);
}

void
w42_view_insert_paragraph (W42View *self)
{
  g_return_if_fail (W42_IS_VIEW (self));
  if (view_pt (self) == NULL)
    return;
  view_insert_paragraph (self);
}

void
w42_view_table_select_row (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  gsize start = 0, end = 0;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  if (!w42_pt_row_bounds (pt, table, row, &start, &end))
    return;
  w42_view_select_range (self, start + 2, end > 0 ? end - 1 : end);
}

void
w42_view_table_select_table (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  gsize start = 0, end = 0;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  if (!w42_pt_table_bounds (pt, table, &start, &end))
    return;
  w42_view_select_range (self, start + 3, end > 1 ? end - 1 : end);
}

gboolean
w42_view_table_split_table (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return FALSE;
  /* The first row has nothing above it to leave behind. */
  if (row <= 0)
    return FALSE;
  w42_pt_table_split (pt, table, row);
  view_edited (self);
  return TRUE;
}

void
w42_view_set_gridlines (W42View *self, gboolean show)
{
  g_return_if_fail (W42_IS_VIEW (self));
  w42_layout_set_gridlines (self->layout, show);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
w42_view_table_sort (W42View *self, gboolean descending)
{
  W42PieceTable *pt;
  int table, row, col;

  g_return_if_fail (W42_IS_VIEW (self));
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return;
  w42_pt_table_sort (pt, table, descending);
  {
    const W42TableProps *props = w42_pt_table_props (pt, table);

    self->anchor = self->caret =
      w42_pt_cell_start (pt, table, props != NULL ? props->header_rows : 0, 0);
  }
  view_edited (self);
}

gboolean
w42_view_table_to_text (W42View *self)
{
  W42PieceTable *pt;
  int table, row, col;
  gsize at;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  pt = view_pt (self);
  if (pt == NULL || !w42_pt_cell_at (pt, self->caret, &table, &row, &col))
    return FALSE;
  at = w42_pt_table_to_text (pt, table);
  if (at == (gsize) -1)
    return FALSE;
  self->anchor = self->caret = w42_pt_clamp_pos (pt, at);
  view_edited (self);
  return TRUE;
}

gboolean
w42_view_text_to_table (W42View *self)
{
  W42PieceTable *pt;
  gsize start, end, at;

  g_return_val_if_fail (W42_IS_VIEW (self), FALSE);
  pt = view_pt (self);
  if (pt == NULL)
    return FALSE;

  /* The paragraphs the selection touches, or the caret's own. */
  start = w42_pt_paragraph_start (pt, sel_start (self));
  end = w42_pt_paragraph_end (pt, w42_view_has_selection (self) ? sel_end (self) - 1 : self->caret);
  at = w42_pt_text_to_table (pt, start, end);
  if (at == (gsize) -1)
    return FALSE;
  self->anchor = self->caret = w42_pt_clamp_pos (pt, at + 3);
  view_edited (self);
  return TRUE;
}

void
w42_view_paste_text (W42View *self)
{
  GdkClipboard *clipboard;

  g_return_if_fail (W42_IS_VIEW (self));

  /* Whatever else the clipboard holds, its text is what goes in. */
  clipboard = gtk_widget_get_clipboard (GTK_WIDGET (self));
  gdk_clipboard_read_text_async (clipboard, NULL, on_clipboard_text, g_object_ref (self));
}

void
w42_view_clear (W42View *self)
{
  g_return_if_fail (W42_IS_VIEW (self));
  if (view_pt (self) == NULL || !w42_view_has_selection (self))
    return;
  if (view_delete_selection (self))
    view_edited (self);
}

void
w42_view_insert_fragment (W42View *self, W42PieceTable *frag)
{
  W42PieceTable *pt;

  g_return_if_fail (W42_IS_VIEW (self));
  g_return_if_fail (frag != NULL);

  pt = view_pt (self);
  if (pt == NULL)
    return;

  w42_pt_begin_group (pt);
  view_delete_selection (self);
  self->caret += w42_pt_insert_fragment (pt, self->caret, frag);
  self->anchor = self->caret;
  w42_pt_end_group (pt);
  view_edited (self);
}
