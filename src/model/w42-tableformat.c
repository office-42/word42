/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Table AutoFormat.  Word 6 offered a list of looks and a preview; this
 * offers the same kind of list, with looks of its own making, and puts
 * one on the caret's table in a single undo step. */

#include "w42-tableformat.h"

#include <string.h>

static const W42TableFormat FORMATS[] = {
  { "Plain",          "No rules and no shading.",
    W42_TF_RULES_NONE, 0,  0,  0, 0, 0 },
  { "Grid",           "Every cell ruled, the heading in bold.",
    W42_TF_RULES_GRID, 0,  0,  1, 0, 0 },
  { "Ruled",          "A rule round the table and under the heading.",
    W42_TF_RULES_BOX,  0,  0,  1, 0, 0 },
  { "Ruled Bands",    "Ruled, with every other row lightly shaded.",
    W42_TF_RULES_BOX,  0,  10, 1, 0, 0 },
  { "Shaded Heading", "A grid, with the heading row shaded.",
    W42_TF_RULES_GRID, 20, 0,  1, 0, 0 },
  { "Columns",        "A grid, heading and first column in bold.",
    W42_TF_RULES_GRID, 10, 0,  1, 0, 1 },
  { "Report",         "A rule round the table, the heading in bold italic.",
    W42_TF_RULES_BOX,  0,  0,  1, 1, 0 },
};

const W42TableFormat *
w42_table_formats (int *n)
{
  if (n != NULL)
    *n = (int) G_N_ELEMENTS (FORMATS);
  return FORMATS;
}

/* The sides a cell has under W42_TF_RULES_BOX. */
static int
box_sides (int row, int col, int rows, int cols, gboolean heading)
{
  int sides = 0;

  if (row == 0)
    sides |= W42_BORDER_TOP;
  if (row == rows - 1)
    sides |= W42_BORDER_BOTTOM;
  if (col == 0)
    sides |= W42_BORDER_LEFT;
  if (col == cols - 1)
    sides |= W42_BORDER_RIGHT;
  if (heading && row == 0 && rows > 1)
    sides |= W42_BORDER_BOTTOM;         /* the rule under the heading */
  return sides;
}

gboolean
w42_pt_table_autoformat (W42PieceTable        *pt,
                         int                   table,
                         const W42TableFormat *fmt,
                         gboolean              heading,
                         gboolean              first_column)
{
  const W42TableProps *props;
  int rows, cols;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (fmt != NULL, FALSE);

  props = w42_pt_table_props (pt, table);
  if (props == NULL)
    return FALSE;
  cols = props->n_cols;
  rows = w42_pt_table_rows (pt, table);
  if (rows <= 0 || cols <= 0)
    return FALSE;

  w42_pt_begin_group (pt);
  w42_pt_table_set_borders (pt, table, fmt->rules == W42_TF_RULES_GRID);

  for (int row = 0; row < rows; row++)
    {
      for (int col = 0; col < cols; col++)
        {
          gboolean head = heading && row == 0;
          gsize start, end;
          W42ParaFmt pa;
          W42CharFmt ch;
          int shading, sides;

          if (!w42_pt_cell_range (pt, table, row, col, &start, &end))
            continue;                   /* a column a merged cell covers */

          /* The rules first: a cell's own sides, or the table's. */
          sides = -1;
          if (fmt->rules == W42_TF_RULES_BOX)
            sides = box_sides (row, col, rows, cols, heading);
          else if (fmt->rules == W42_TF_RULES_NONE)
            sides = 0;
          if (w42_pt_cell_get_borders (pt, table, row, col) != sides)
            w42_pt_cell_set_borders (pt, table, row, col, sides);

          /* Then the shading: the heading's own, or the bands'. */
          shading = head ? fmt->head_shading
                         : (fmt->band_shading > 0 && (row % 2) == 1 ? fmt->band_shading : 0);
          memset (&pa, 0, sizeof pa);
          pa.shading = (guint8) shading;
          w42_pt_apply_para_fmt (pt, start, end > start ? end - start : 0,
                                 W42_PARA_SHADING, &pa);

          /* And what the text is set in. */
          memset (&ch, 0, sizeof ch);
          ch.bold = (head && fmt->head_bold) ||
                    (first_column && col == 0 && fmt->first_col_bold);
          ch.italic = head && fmt->head_italic;
          w42_pt_apply_char_fmt (pt, start, end > start ? end - start : 0,
                                 W42_CHAR_BOLD | W42_CHAR_ITALIC, &ch);
        }
    }

  w42_pt_end_group (pt);
  return TRUE;
}
