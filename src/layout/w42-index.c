/* w42-index.c - see w42-index.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-index.h"

#include <string.h>

static void
entry_free (gpointer data)
{
  W42IndexEntry *entry = data;

  g_free (entry->term);
  g_array_free (entry->pages, TRUE);
  g_free (entry);
}

static int
compare_entries (gconstpointer a, gconstpointer b)
{
  const W42IndexEntry * const *x = a;
  const W42IndexEntry * const *y = b;

  return g_utf8_collate ((*x)->term, (*y)->term);
}

static void
entry_add (GPtrArray *entries, const char *term, int page)
{
  W42IndexEntry *found = NULL;

  for (guint i = 0; i < entries->len; i++)
    {
      W42IndexEntry *entry = g_ptr_array_index (entries, i);

      if (g_utf8_collate (entry->term, term) == 0)
        {
          found = entry;
          break;
        }
    }
  if (found == NULL)
    {
      found = g_new0 (W42IndexEntry, 1);
      found->term = g_strdup (term);
      found->pages = g_array_new (FALSE, FALSE, sizeof (int));
      g_ptr_array_add (entries, found);
    }
  for (guint i = 0; i < found->pages->len; i++)
    if (g_array_index (found->pages, int, i) == page)
      return;
  g_array_append_val (found->pages, page);
}

GPtrArray *
w42_index_gather (W42PieceTable *pt, W42Layout *layout)
{
  GPtrArray *entries = g_ptr_array_new_with_free_func (entry_free);
  GPtrArray *blocks;
  const GArray *lines;

  g_return_val_if_fail (pt != NULL, entries);

  blocks = w42_pt_snapshot_blocks (pt);
  lines = layout != NULL ? w42_layout_lines (layout) : NULL;

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);

      for (guint r = 0; r < block->runs->len; r++)
        {
          const W42Run *run = &g_array_index (block->runs, W42Run, r);
          const W42CharFmt *ch = &w42_ap_table_get (w42_pt_ap_table (pt), run->ap)->ch;
          const char *colon;
          char *term;
          int page = 1;

          if (ch->field == NULL || !g_str_has_prefix (ch->field, W42_INDEX_FIELD))
            continue;
          if (g_strcmp0 (ch->bookmark, W42_INDEX_BOOKMARK) == 0)
            continue;                     /* the index itself */

          colon = strchr (ch->field, ':');
          if (colon != NULL && colon[1] != '\0')
            term = g_strdup (colon + 1);
          else
            term = g_strndup (block->text->str + run->byte_offset, run->n_bytes);
          g_strstrip (term);
          if (*term == '\0')
            {
              g_free (term);
              continue;
            }

          /* The page the run starts on, as the layout stands. */
          for (guint i = 0; lines != NULL && i < lines->len; i++)
            {
              const W42LineBox *line = &g_array_index (lines, W42LineBox, i);

              if (line->block != (int) b)
                continue;
              page = line->page + 1;
              if (run->byte_offset < line->start_index + line->length)
                break;
            }

          entry_add (entries, term, page);
          g_free (term);
        }
    }

  g_ptr_array_free (blocks, TRUE);
  g_ptr_array_sort (entries, compare_entries);
  return entries;
}

int
w42_index_build (W42PieceTable *pt, W42Layout *layout,
                 const W42PageSetup *page, gsize at, gsize *end)
{
  GPtrArray *entries;
  int text_twips = 9360;                  /* a letter page's text width */
  int made = 0;

  g_return_val_if_fail (pt != NULL, 0);

  if (page != NULL)
    {
      text_twips = page->width - page->margin_left - page->margin_right;
      if (w42_page_columns (page) > 1)
        text_twips = (text_twips - (w42_page_columns (page) - 1) * w42_page_column_gap (page))
                     / w42_page_columns (page);
    }

  entries = w42_index_gather (pt, layout);
  for (guint i = 0; i < entries->len; i++)
    {
      const W42IndexEntry *entry = g_ptr_array_index (entries, i);
      GString *line = g_string_new (entry->term);
      W42Fmt fmt;
      W42ApIdx ap;
      gsize n;

      /* "Term .......... 3, 7", the page numbers at a right stop at the
       * margin with dots running out to them. */
      g_string_append_c (line, '\t');
      for (guint p = 0; p < entry->pages->len; p++)
        g_string_append_printf (line, "%s%d", p > 0 ? ", " : "",
                                g_array_index (entry->pages, int, p));

      w42_fmt_init_default (&fmt);
      w42_para_fmt_set_tab_leader (&fmt.pa, text_twips, W42_TAB_RIGHT,
                                   W42_TAB_LEAD_DOT);
      /* The whole index carries one bookmark, so that asking for it
       * again replaces it where it stands. */
      fmt.ch.bookmark = g_intern_static_string (W42_INDEX_BOOKMARK);
      ap = w42_ap_table_intern (w42_pt_ap_table (pt), &fmt);

      n = g_utf8_strlen (line->str, -1);
      w42_pt_insert_text (pt, at, line->str, ap);
      w42_pt_insert_block (pt, at + n, ap);
      w42_pt_apply_para_fmt (pt, at, 0, W42_PARA_INDENT_LEFT | W42_PARA_TABS, &fmt.pa);
      at += n + 1;
      made++;
      g_string_free (line, TRUE);
    }

  g_ptr_array_free (entries, TRUE);
  if (end != NULL)
    *end = at;
  return made;
}

int
w42_figures_build (W42PieceTable *pt, W42Layout *layout,
                   const W42PageSetup *page, gsize at, gsize *end)
{
  GPtrArray *blocks;
  const GArray *lines;
  int text_twips = 9360;                  /* a letter page's text width */
  int made = 0;

  g_return_val_if_fail (pt != NULL, 0);

  if (page != NULL)
    {
      text_twips = page->width - page->margin_left - page->margin_right;
      if (w42_page_columns (page) > 1)
        text_twips = (text_twips - (w42_page_columns (page) - 1) * w42_page_column_gap (page))
                     / w42_page_columns (page);
    }

  blocks = w42_pt_snapshot_blocks (pt);
  lines = layout != NULL ? w42_layout_lines (layout) : NULL;

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42Fmt *fmt = w42_ap_table_get (w42_pt_ap_table (pt), block->ap);
      GString *line;
      W42Fmt efmt;
      W42ApIdx ap;
      gsize n;
      int page_no = 1;

      /* A caption is a paragraph in the Caption style, as Insert > Caption
       * makes one; the table's own lines are not captions, whatever they
       * say. */
      if (fmt->pa.style == NULL || g_ascii_strcasecmp (fmt->pa.style, "Caption") != 0)
        continue;
      if (block->note >= 0 || block->text->len == 0)
        continue;
      if (block->runs->len > 0)
        {
          const W42Run *run = &g_array_index (block->runs, W42Run, 0);
          const W42CharFmt *ch = &w42_ap_table_get (w42_pt_ap_table (pt), run->ap)->ch;

          if (g_strcmp0 (ch->bookmark, W42_FIGURES_BOOKMARK) == 0)
            continue;
        }

      line = g_string_new (NULL);
      for (const char *p = block->text->str; *p; p = g_utf8_next_char (p))
        {
          gunichar c = g_utf8_get_char (p);

          if (c != 0xFFFC && c != '\t')
            g_string_append_unichar (line, c);
        }
      g_strstrip (line->str);
      g_string_set_size (line, strlen (line->str));
      if (line->len == 0)
        {
          g_string_free (line, TRUE);
          continue;
        }

      /* The page the caption starts on, as the layout stands. */
      for (guint i = 0; lines != NULL && i < lines->len; i++)
        {
          const W42LineBox *lb = &g_array_index (lines, W42LineBox, i);

          if (lb->block == (int) b)
            {
              page_no = lb->page + 1;
              break;
            }
        }
      g_string_append_printf (line, "\t%d", page_no);

      w42_fmt_init_default (&efmt);
      w42_para_fmt_set_tab_leader (&efmt.pa, text_twips, W42_TAB_RIGHT,
                                   W42_TAB_LEAD_DOT);
      efmt.ch.bookmark = g_intern_static_string (W42_FIGURES_BOOKMARK);
      ap = w42_ap_table_intern (w42_pt_ap_table (pt), &efmt);

      n = g_utf8_strlen (line->str, -1);
      w42_pt_insert_text (pt, at, line->str, ap);
      w42_pt_insert_block (pt, at + n, ap);
      w42_pt_apply_para_fmt (pt, at, 0, W42_PARA_INDENT_LEFT | W42_PARA_TABS, &efmt.pa);
      at += n + 1;
      made++;
      g_string_free (line, TRUE);
    }

  g_ptr_array_free (blocks, TRUE);
  if (end != NULL)
    *end = at;
  return made;
}

