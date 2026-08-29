/* w42-template.c - see w42-template.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-template.h"

#include <string.h>

static const W42Template TEMPLATES[] = {
  { "Blank Document", "An empty page, as File > New gives you." },
  { "Letter",         "A letter: the date, an address, a greeting and a closing." },
  { "Memo",           "A memorandum: To, From, Date and Subject over a rule." },
  { "Fax Cover",      "A fax cover sheet: who it is for, how many pages, a message." },
  { "Report",         "A report: a title, headings and a place for the text." },
  { "Meeting Notes",  "Notes of a meeting: who was there, what was decided." },
};

const W42Template *
w42_templates (int *n)
{
  if (n != NULL)
    *n = (int) G_N_ELEMENTS (TEMPLATES);
  return TEMPLATES;
}

/* ---------------------------------------------------------------------- */
/* Building one                                                            */
/* ---------------------------------------------------------------------- */

typedef struct {
  W42PieceTable *pt;
  gsize          pos;
  gboolean       first;      /* the first paragraph is already there */
} Build;

/* A paragraph in `style`, with `text` in it.  A NULL style leaves the
 * paragraph as it is. */
static void
para (Build *b, const char *style, const char *text)
{
  gsize start;

  if (!b->first)
    {
      w42_pt_insert_block (b->pt, b->pos, w42_pt_ap_at (b->pt, b->pos));
      b->pos++;
    }
  b->first = FALSE;
  start = b->pos;

  if (text != NULL && *text != '\0')
    {
      w42_pt_insert_text (b->pt, b->pos, text, w42_pt_ap_at (b->pt, b->pos));
      b->pos += g_utf8_strlen (text, -1);
    }
  if (style != NULL)
    w42_pt_apply_style (b->pt, start, b->pos > start ? b->pos - start : 0, style);
}

/* Today, as a field: the text is what it says now, and Update Fields
 * says it again later. */
static void
date_field (Build *b)
{
  GDateTime *now = g_date_time_new_now_local ();
  char *text = g_date_time_format (now, "%e %B %Y");
  W42CharFmt want;
  gsize n;

  text = g_strstrip (text);
  n = g_utf8_strlen (text, -1);
  w42_pt_insert_text (b->pt, b->pos, text, w42_pt_ap_at (b->pt, b->pos));
  memset (&want, 0, sizeof want);
  want.field = g_intern_static_string ("DATE");
  w42_pt_apply_char_fmt (b->pt, b->pos, n, W42_CHAR_FIELD, &want);
  b->pos += n;

  g_free (text);
  g_date_time_unref (now);
}

/* The last paragraph made takes this formatting. */
static void
last_para_fmt (Build *b, W42ParaMask mask, const W42ParaFmt *pa)
{
  gsize start = w42_pt_paragraph_start (b->pt, b->pos);

  w42_pt_apply_para_fmt (b->pt, start + 1, 0, mask, pa);
}

static void
space_after (Build *b, int twips)
{
  W42ParaFmt pa;

  memset (&pa, 0, sizeof pa);
  pa.space_after = twips;
  last_para_fmt (b, W42_PARA_SPACE_AFTER, &pa);
}

static void
rule_under (Build *b)
{
  W42ParaFmt pa;

  memset (&pa, 0, sizeof pa);
  pa.border = W42_BORDER_BOTTOM;
  pa.border_width = 8;
  last_para_fmt (b, W42_PARA_BORDER, &pa);
}

static void
tabbed_line (Build *b, const char *label, const char *value)
{
  W42ParaFmt pa;
  char *line = g_strdup_printf ("%s\t%s", label, value);

  para (b, "Normal", line);
  memset (&pa, 0, sizeof pa);
  pa.n_tabs = 1;
  pa.tab_pos[0] = 1440;
  pa.tab_kind[0] = W42_TAB_BYTE (W42_TAB_LEFT, W42_TAB_LEAD_NONE);
  last_para_fmt (b, W42_PARA_TABS, &pa);
  g_free (line);
}

void
w42_template_make (W42PieceTable *pt, W42PageSetup *page, int which)
{
  Build b;

  g_return_if_fail (pt != NULL);

  which = CLAMP (which, 0, (int) G_N_ELEMENTS (TEMPLATES) - 1);

  w42_pt_load_text (pt, "");
  b.pt = pt;
  b.pos = w42_pt_first_caret_pos (pt);
  b.first = TRUE;

  if (page != NULL)
    {
      /* Every one of these is an ordinary page; the margins are Word 6's
       * defaults, which the rest of the program uses too. */
      page->columns = 0;
      page->column_gap = 0;
    }

  switch (which)
    {
    case 1:                                   /* Letter */
      para (&b, "Normal", "Your Name");
      para (&b, "Normal", "Your Address");
      para (&b, "Normal", "Town, Postcode");
      space_after (&b, 240);
      para (&b, "Normal", NULL);
      date_field (&b);
      space_after (&b, 240);
      para (&b, "Normal", "Recipient's Name");
      para (&b, "Normal", "Their Address");
      para (&b, "Normal", "Town, Postcode");
      space_after (&b, 240);
      para (&b, "Normal", "Dear Sir or Madam,");
      space_after (&b, 120);
      para (&b, "Normal", "The letter goes here.");
      space_after (&b, 240);
      para (&b, "Normal", "Yours faithfully,");
      para (&b, "Normal", NULL);
      para (&b, "Normal", NULL);
      para (&b, "Normal", "Your Name");
      break;

    case 2:                                   /* Memo */
      para (&b, "Title", "MEMORANDUM");
      space_after (&b, 240);
      tabbed_line (&b, "To:", "Everyone");
      tabbed_line (&b, "From:", "Your Name");
      {
        W42ParaFmt pa;
        gsize start;

        para (&b, "Normal", "Date:\t");
        date_field (&b);
        start = w42_pt_paragraph_start (pt, b.pos);
        memset (&pa, 0, sizeof pa);
        pa.n_tabs = 1;
        pa.tab_pos[0] = 1440;
        pa.tab_kind[0] = W42_TAB_BYTE (W42_TAB_LEFT, W42_TAB_LEAD_NONE);
        w42_pt_apply_para_fmt (pt, start + 1, 0, W42_PARA_TABS, &pa);
      }
      tabbed_line (&b, "Subject:", "What this is about");
      rule_under (&b);
      space_after (&b, 240);
      para (&b, "Normal", "The memorandum goes here.");
      break;

    case 3:                                   /* Fax cover */
      para (&b, "Title", "FACSIMILE");
      space_after (&b, 240);
      tabbed_line (&b, "To:", "Their Name");
      tabbed_line (&b, "Fax:", "Their Number");
      tabbed_line (&b, "From:", "Your Name");
      tabbed_line (&b, "Fax:", "Your Number");
      tabbed_line (&b, "Pages:", "1, including this one");
      rule_under (&b);
      space_after (&b, 240);
      para (&b, "Normal", "The message goes here.");
      break;

    case 4:                                   /* Report */
      para (&b, "Title", "The Title of the Report");
      para (&b, "Normal", "Your Name");
      para (&b, "Normal", NULL);
      date_field (&b);
      space_after (&b, 360);
      para (&b, "Heading 1", "Summary");
      para (&b, "Normal", "What the report says, in a paragraph.");
      para (&b, "Heading 1", "The Matter in Hand");
      para (&b, "Normal", "The body of the report goes here.");
      para (&b, "Heading 2", "A Point Worth Its Own Heading");
      para (&b, "Normal", "And what there is to say about it.");
      para (&b, "Heading 1", "What Follows From It");
      para (&b, "Normal", "The conclusion goes here.");
      break;

    case 5:                                   /* Meeting notes */
      para (&b, "Title", "Meeting Notes");
      para (&b, "Normal", NULL);
      date_field (&b);
      space_after (&b, 240);
      tabbed_line (&b, "Present:", "Who was there");
      tabbed_line (&b, "Apologies:", "Who was not");
      rule_under (&b);
      space_after (&b, 240);
      para (&b, "Heading 1", "Matters discussed");
      para (&b, "Normal", "The first matter.");
      para (&b, "Heading 1", "Decisions");
      para (&b, "Normal", "What was decided, and by whom it will be done.");
      break;

    default:                                  /* Blank */
      break;
    }

  w42_pt_clear_undo (pt);
}

char *
w42_template_folder (void)
{
  char *dir = g_build_filename (g_get_user_data_dir (), "word42", "templates", NULL);

  g_mkdir_with_parents (dir, 0700);
  return dir;
}

static int
compare_names (gconstpointer a, gconstpointer b)
{
  const char * const *x = a;
  const char * const *y = b;

  return g_utf8_collate (*x, *y);
}

char **
w42_template_files (void)
{
  char *dir = w42_template_folder ();
  GDir *d = g_dir_open (dir, 0, NULL);
  GPtrArray *names = g_ptr_array_new ();
  const char *name;

  if (d != NULL)
    {
      while ((name = g_dir_read_name (d)) != NULL)
        {
          /* Only what this program can open again. */
          if (g_str_has_suffix (name, ".rtf") || g_str_has_suffix (name, ".docx") ||
              g_str_has_suffix (name, ".odt") || g_str_has_suffix (name, ".abw") ||
              g_str_has_suffix (name, ".zabw") || g_str_has_suffix (name, ".doc") ||
              g_str_has_suffix (name, ".txt") || g_str_has_suffix (name, ".html"))
            g_ptr_array_add (names, g_strdup (name));
        }
      g_dir_close (d);
    }

  g_ptr_array_sort (names, compare_names);
  g_ptr_array_add (names, NULL);
  g_free (dir);
  return (char **) g_ptr_array_free (names, FALSE);
}
