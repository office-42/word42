/* w42-template.c - see w42-template.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-template.h"

#include <glib/gi18n.h>
#include <string.h>

/* The names and hints are marked for translation: whatever shows them
 * passes them through _(). */
static const W42Template TEMPLATES[] = {
  { N_("Blank Document"), N_("An empty page, as File > New gives you.") },
  { N_("Letter"),         N_("A letter: the date, an address, a greeting and a closing.") },
  { N_("Memo"),           N_("A memorandum: To, From, Date and Subject over a rule.") },
  { N_("Fax Cover"),      N_("A fax cover sheet: who it is for, how many pages, a message.") },
  { N_("Report"),         N_("A report: a title, headings and a place for the text.") },
  { N_("Meeting Notes"),  N_("Notes of a meeting: who was there, what was decided.") },
  { N_("Novel"),          N_("A novel set as a book: small pages, justified text with "
                             "indented paragraphs, and every chapter on a page of its own.") },
  { N_("Manuscript"),     N_("A manuscript for an agent or a publisher: double-spaced, "
                             "with your name and the title at the top of every page.") },
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
  w42_para_fmt_set_edges (&pa, 8, 0, W42_BORDER_SINGLE);
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

/* A line set in the middle, without the first-line indent a novel's
 * paragraphs have: a title's byline, a scene break. */
static void
centred (Build *b, const char *style, const char *text)
{
  W42ParaFmt pa;

  para (b, style, text);
  memset (&pa, 0, sizeof pa);
  pa.align = W42_ALIGN_CENTER;
  pa.indent_first = 0;
  last_para_fmt (b, W42_PARA_ALIGN | W42_PARA_INDENT_FIRST, &pa);
}

/* The body text and chapter headings of a book, set before any text
 * goes in, so that every paragraph made is made in them. */
static void
book_styles (W42PieceTable *pt, const char *family, int size, W42Align align,
             int first_line, int spacing_pct, int heading_size,
             int heading_before)
{
  W42StyleSheet *sheet = w42_pt_stylesheet (pt);
  W42Style style;

  style = *w42_stylesheet_find (sheet, "Normal");
  style.ch.family = g_intern_static_string (family);
  style.ch.size = size;
  style.pa.align = align;
  style.pa.indent_first = first_line;
  style.pa.line_spacing = 0;
  style.pa.line_spacing_pct = spacing_pct;
  style.pa.widow_control = 1;
  w42_stylesheet_set (sheet, &style);

  /* A chapter: a page of its own, its title some way down it, and the
   * title kept with the text under it. */
  style = *w42_stylesheet_find (sheet, "Heading 1");
  style.ch.family = g_intern_static_string (family);
  style.ch.size = heading_size;
  style.ch.bold = 0;
  style.pa.align = W42_ALIGN_CENTER;
  style.pa.indent_first = 0;
  style.pa.space_before = heading_before;
  style.pa.space_after = 480;
  style.pa.page_break_before = 1;
  style.pa.keep_next = 1;
  style.pa.line_spacing = 0;
  style.pa.line_spacing_pct = spacing_pct;
  w42_stylesheet_set (sheet, &style);

  style = *w42_stylesheet_find (sheet, "Title");
  style.ch.family = g_intern_static_string (family);
  style.ch.size = MAX (heading_size, size) * 3 / 2;
  style.ch.bold = 0;
  style.pa.space_before = 2880;
  style.pa.space_after = 360;
  style.pa.indent_first = 0;
  w42_stylesheet_set (sheet, &style);

  w42_stylesheet_follow (sheet, "Normal");
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
      /* Every one of these is an ordinary page; the margins are Word 97's
       * defaults, which the rest of the program uses too. */
      page->columns = 0;
      page->column_gap = 0;
    }

  /* The text is the user's to replace, and is in their language; the
   * style names and field codes stay as they are, since they are keys. */
  switch (which)
    {
    case 1:                                   /* Letter */
      /* Translators: this and the lines that follow are the text of the
       * documents File > New from Template makes, for the user to type
       * over: write them as a template in your language would have them. */
      para (&b, "Normal", _("Your Name"));
      para (&b, "Normal", _("Your Address"));
      para (&b, "Normal", _("Town, Postcode"));
      space_after (&b, 240);
      para (&b, "Normal", NULL);
      date_field (&b);
      space_after (&b, 240);
      para (&b, "Normal", _("Recipient's Name"));
      para (&b, "Normal", _("Their Address"));
      para (&b, "Normal", _("Town, Postcode"));
      space_after (&b, 240);
      para (&b, "Normal", _("Dear Sir or Madam,"));
      space_after (&b, 120);
      para (&b, "Normal", _("The letter goes here."));
      space_after (&b, 240);
      para (&b, "Normal", _("Yours faithfully,"));
      para (&b, "Normal", NULL);
      para (&b, "Normal", NULL);
      para (&b, "Normal", _("Your Name"));
      break;

    case 2:                                   /* Memo */
      para (&b, "Title", _("MEMORANDUM"));
      space_after (&b, 240);
      /* Translators: a memo's first line: the heading, then who it is
       * for. */
      tabbed_line (&b, _("To:"), _("Everyone"));
      tabbed_line (&b, _("From:"), _("Your Name"));
      {
        W42ParaFmt pa;
        gsize start;
        /* Translators: a memo's heading, as "To:" and "From:" are. */
        char *line = g_strconcat (_("Date:"), "\t", NULL);

        para (&b, "Normal", line);
        g_free (line);
        date_field (&b);
        start = w42_pt_paragraph_start (pt, b.pos);
        memset (&pa, 0, sizeof pa);
        pa.n_tabs = 1;
        pa.tab_pos[0] = 1440;
        pa.tab_kind[0] = W42_TAB_BYTE (W42_TAB_LEFT, W42_TAB_LEAD_NONE);
        w42_pt_apply_para_fmt (pt, start + 1, 0, W42_PARA_TABS, &pa);
      }
      tabbed_line (&b, _("Subject:"), _("What this is about"));
      rule_under (&b);
      space_after (&b, 240);
      para (&b, "Normal", _("The memorandum goes here."));
      break;

    case 3:                                   /* Fax cover */
      para (&b, "Title", _("FACSIMILE"));
      space_after (&b, 240);
      tabbed_line (&b, _("To:"), _("Their Name"));
      /* Translators: on a fax cover sheet, the fax number. */
      tabbed_line (&b, _("Fax:"), _("Their Number"));
      tabbed_line (&b, _("From:"), _("Your Name"));
      tabbed_line (&b, _("Fax:"), _("Your Number"));
      /* Translators: on a fax cover sheet, how many pages are sent. */
      tabbed_line (&b, _("Pages:"), _("1, including this one"));
      rule_under (&b);
      space_after (&b, 240);
      para (&b, "Normal", _("The message goes here."));
      break;

    case 4:                                   /* Report */
      para (&b, "Title", _("The Title of the Report"));
      para (&b, "Normal", _("Your Name"));
      para (&b, "Normal", NULL);
      date_field (&b);
      space_after (&b, 360);
      para (&b, "Heading 1", _("Summary"));
      para (&b, "Normal", _("What the report says, in a paragraph."));
      para (&b, "Heading 1", _("The Matter in Hand"));
      para (&b, "Normal", _("The body of the report goes here."));
      para (&b, "Heading 2", _("A Point Worth Its Own Heading"));
      para (&b, "Normal", _("And what there is to say about it."));
      para (&b, "Heading 1", _("What Follows From It"));
      para (&b, "Normal", _("The conclusion goes here."));
      break;

    case 5:                                   /* Meeting notes */
      para (&b, "Title", _("Meeting Notes"));
      para (&b, "Normal", NULL);
      date_field (&b);
      space_after (&b, 240);
      /* Translators: in the notes of a meeting, who attended. */
      tabbed_line (&b, _("Present:"), _("Who was there"));
      /* Translators: in the notes of a meeting, who sent word that they
       * could not attend. */
      tabbed_line (&b, _("Apologies:"), _("Who was not"));
      rule_under (&b);
      space_after (&b, 240);
      para (&b, "Heading 1", _("Matters discussed"));
      para (&b, "Normal", _("The first matter."));
      para (&b, "Heading 1", _("Decisions"));
      para (&b, "Normal", _("What was decided, and by whom it will be done."));
      break;

    case 6:                                   /* Novel */
      if (page != NULL)
        {
          /* A5, the size of a hardback novel, with margins a book has. */
          page->width = 8391;
          page->height = 11906;
          page->margin_left = page->margin_right = 1021;
          page->margin_top = 1134;
          page->margin_bottom = 1247;
        }
      book_styles (pt, "Times New Roman", 22, W42_ALIGN_JUSTIFY, 340, 115,
                   36, 1440);
      w42_pt_set_footer (pt, "{PAGE}", W42_ALIGN_CENTER);
      w42_pt_set_title_page (pt, TRUE);
      w42_pt_set_footer_kind (pt, W42_PAGE_TEXT_FIRST, "", W42_ALIGN_CENTER);
      para (&b, "Title", _("The Title of the Novel"));
      /* Translators: under the title of a novel, what the book is. */
      centred (&b, "Normal", _("A Novel"));
      para (&b, "Heading 1", _("Chapter One"));
      /* Translators: Heading 1 is a style's name, which is not
       * translated. */
      para (&b, "Normal", _("The first lines of the book go here. The paragraphs "
                            "are justified and set in, as a printed novel's are, "
                            "and each chapter starts on a new page: give its title "
                            "the Heading 1 style."));
      /* Translators: Tools, Language, Set Language, Default and Word Count
       * Goal are the menus, menu items and button of those names: use the
       * words the translated menus and dialog use. */
      para (&b, "Normal", _("Tools \342\226\270 Language \342\226\270 Set "
                            "Language, Default makes the book's language the one "
                            "its spelling, quotation marks and hyphenation follow; "
                            "Tools \342\226\270 Word Count Goal sets how long it "
                            "is to be."));
      centred (&b, "Normal", "* * *");
      para (&b, "Normal", _("A new scene begins after a break like the one above."));
      para (&b, "Heading 1", _("Chapter Two"));
      para (&b, "Normal", _("And the story goes on."));
      break;

    case 7:                                   /* Manuscript */
      if (page != NULL)
        {
          page->width = 12240;
          page->height = 15840;
          page->margin_left = page->margin_right = 1440;
          page->margin_top = page->margin_bottom = 1440;
        }
      /* What agents and publishers ask for: a typewriter's face at
       * twelve points, double-spaced, ragged right, paragraphs set in
       * half an inch, and each chapter a third of the way down a new
       * page. */
      book_styles (pt, "Courier New", 24, W42_ALIGN_LEFT, 720, 200, 24, 2880);
      {
        /* Translators: the running head of a manuscript, the page number
         * after it; the writer puts their own surname and title in place
         * of these, as "The story begins here. Replace Surname and TITLE"
         * tells them, so use the same two words there. */
        char *head = g_strdup_printf ("%s / {PAGE}", _("Surname / TITLE"));

        w42_pt_set_header (pt, head, W42_ALIGN_RIGHT);
        g_free (head);
      }
      w42_pt_set_title_page (pt, TRUE);
      w42_pt_set_header_kind (pt, W42_PAGE_TEXT_FIRST, "", W42_ALIGN_RIGHT);
      {
        W42ParaFmt pa;

        memset (&pa, 0, sizeof pa);
        pa.indent_first = 0;
        pa.line_spacing_pct = 100;
        para (&b, "Normal", _("Your Name"));
        last_para_fmt (&b, W42_PARA_INDENT_FIRST | W42_PARA_LINE_SPACING_PCT, &pa);
        para (&b, "Normal", _("Your Address"));
        last_para_fmt (&b, W42_PARA_INDENT_FIRST | W42_PARA_LINE_SPACING_PCT, &pa);
        para (&b, "Normal", _("Your Email and Telephone"));
        last_para_fmt (&b, W42_PARA_INDENT_FIRST | W42_PARA_LINE_SPACING_PCT, &pa);
      }
      centred (&b, "Title", _("THE TITLE"));
      centred (&b, "Normal", _("by Your Name"));
      /* Translators: on a manuscript's title page, how long the book is. */
      centred (&b, "Normal", _("About 80,000 words"));
      para (&b, "Heading 1", _("Chapter One"));
      /* Translators: Surname and TITLE are the words of "Surname / TITLE";
       * View and Header and Footer are the menu items of that name. */
      para (&b, "Normal", _("The story begins here. Replace Surname and TITLE in "
                            "View \342\226\270 Header and Footer with your own; "
                            "the page number follows them on every page but the "
                            "first."));
      centred (&b, "Normal", "#");
      /* Translators: the "#" on the line above is the break meant. */
      para (&b, "Normal", _("A scene break is a hash mark on a line of its own."));
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
