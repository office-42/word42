/* convert-main.c - word42-convert, Word42's engine without a window
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 *     word42-convert [--pages] [--text] IN [OUT]
 *
 * reads IN and writes OUT, each in the format its name says, through the
 * same readers and writers File > Open and File > Save As use.  --pages
 * lays the document out as Page Layout does and prints how many pages it
 * comes to; --text prints its text.  It links libw42core alone, so it
 * runs where there is no display -- which is what the smoke test the CI
 * runs needs -- and anything it gets wrong the window gets wrong too.
 */

#include "w42-io.h"
#include "w42-layout.h"

#include <stdio.h>
#include <string.h>

static void
usage (FILE *to)
{
  fputs ("usage: word42-convert [--pages] [--text] IN [OUT]\n"
         "\n"
         "Reads IN and writes OUT, each in the format its extension names:\n"
         ".doc (read only), .docx, .odt, .rtf, .html, .abw, .pptx, .pdf\n"
         "(written; read when built with poppler) or plain text.\n"
         "\n"
         "  --pages    print the number of pages IN lays out to\n"
         "  --text     print the text of IN\n"
         "  --version  print the version\n"
         "  --help     print this\n", to);
}

int
main (int argc, char *argv[])
{
  gboolean pages = FALSE, text = FALSE;
  const char *in = NULL, *out = NULL;
  W42PieceTable *pt;
  W42PageSetup page;
  GFile *file;
  GError *error = NULL;
  int status = 0;

  for (int i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--pages") == 0)
        pages = TRUE;
      else if (strcmp (argv[i], "--text") == 0)
        text = TRUE;
      else if (strcmp (argv[i], "--version") == 0)
        {
          printf ("word42-convert %s\n", W42_VERSION);
          return 0;
        }
      else if (strcmp (argv[i], "--help") == 0)
        {
          usage (stdout);
          return 0;
        }
      else if (argv[i][0] == '-' && argv[i][1] != '\0')
        {
          fprintf (stderr, "word42-convert: unknown option %s\n", argv[i]);
          usage (stderr);
          return 2;
        }
      else if (in == NULL)
        in = argv[i];
      else if (out == NULL)
        out = argv[i];
      else
        {
          usage (stderr);
          return 2;
        }
    }
  if (in == NULL || (out == NULL && !pages && !text))
    {
      usage (stderr);
      return 2;
    }

  /* What a file does not say of its page is what a new document's is:
   * US Letter with one-inch margins, as W42Document starts. */
  memset (&page, 0, sizeof page);
  page.width = 12240;
  page.height = 15840;
  page.margin_left = page.margin_right = 1440;
  page.margin_top = page.margin_bottom = 1440;
  pt = w42_pt_new ();
  file = g_file_new_for_commandline_arg (in);
  if (!w42_io_load (pt, &page, file, &error))
    {
      fprintf (stderr, "word42-convert: %s: %s\n", in, error->message);
      g_clear_error (&error);
      status = 1;
      goto done;
    }

  if (text)
    {
      gsize first = w42_pt_first_caret_pos (pt);
      char *utf8 = w42_pt_get_text (pt, first, w42_pt_length (pt) - first);

      fputs (utf8, stdout);
      if (*utf8 != '\0' && utf8[strlen (utf8) - 1] != '\n')
        fputc ('\n', stdout);
      g_free (utf8);
    }

  if (pages)
    {
      W42Layout *layout = w42_layout_new ();

      w42_layout_set_galley (layout, FALSE);
      w42_layout_build_pt (layout, pt, &page);
      printf ("%d\n", w42_layout_n_pages (layout));
      w42_layout_free (layout);
    }

  if (out != NULL)
    {
      GFile *target = g_file_new_for_commandline_arg (out);

      if (!w42_io_save (pt, &page, target, &error))
        {
          fprintf (stderr, "word42-convert: %s: %s\n", out, error->message);
          g_clear_error (&error);
          status = 1;
        }
      g_object_unref (target);
    }

done:
  g_object_unref (file);
  w42_pt_free (pt);
  return status;
}
