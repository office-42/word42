/* main.c - word42, a word processor in the shape of Word 97
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
 */

#include "w42-application.h"
#include "w42-document.h"
#include "w42-io.h"

#include <stdio.h>
#include <string.h>

/* word42 --convert-to=pdf roman.odt: the document read and written again
 * in another format, with no window, no display and no questions -- what
 * an author's script does to make the proofs, the e-book and the copy
 * for the publisher out of one manuscript.  Handled before GTK is
 * started, since GTK wants a display even to say what it would do.
 *
 * Returns -1 when the command line asks for no conversion. */
static int
convert_main (int argc, char *argv[])
{
  const char *format = NULL, *outdir = NULL;
  GPtrArray *files = g_ptr_array_new ();
  int failed = 0;

  for (int i = 1; i < argc; i++)
    {
      const char *arg = argv[i];

      if (g_str_has_prefix (arg, "--convert-to="))
        format = arg + strlen ("--convert-to=");
      else if (g_str_equal (arg, "--convert-to") && i + 1 < argc)
        format = argv[++i];
      else if (g_str_has_prefix (arg, "--outdir="))
        outdir = arg + strlen ("--outdir=");
      else if (g_str_equal (arg, "--outdir") && i + 1 < argc)
        outdir = argv[++i];
      else if (arg[0] != '-')
        g_ptr_array_add (files, (gpointer) arg);
    }

  if (format == NULL)
    {
      g_ptr_array_free (files, TRUE);
      return -1;
    }
  if (*format == '.')
    format++;
  if (*format == '\0' || files->len == 0)
    {
      fprintf (stderr, "Usage: word42 --convert-to=FORMAT [--outdir=DIR] FILE...\n"
                       "FORMAT is the extension to write: pdf, epub, odt, docx, "
                       "rtf, html, txt or abw.\n");
      g_ptr_array_free (files, TRUE);
      return 2;
    }

  /* The folder asked for is made if it is not there, as a build script
   * expects of an output folder. */
  if (outdir != NULL && g_mkdir_with_parents (outdir, 0755) != 0)
    {
      fprintf (stderr, "word42: %s: the folder could not be made\n", outdir);
      g_ptr_array_free (files, TRUE);
      return 1;
    }

  for (guint i = 0; i < files->len; i++)
    {
      GFile *in = g_file_new_for_commandline_arg (g_ptr_array_index (files, i));
      W42Document *doc = w42_document_new ();
      GError *error = NULL;
      char *base = g_file_get_basename (in);
      char *dot = strrchr (base, '.');
      char *name, *out_path;
      GFile *out;

      if (dot != NULL && dot != base)
        *dot = '\0';
      name = g_strconcat (base, ".", format, NULL);
      if (outdir != NULL)
        out_path = g_build_filename (outdir, name, NULL);
      else
        {
          GFile *parent = g_file_get_parent (in);
          char *dir = parent != NULL ? g_file_get_path (parent) : g_strdup (".");

          out_path = g_build_filename (dir, name, NULL);
          g_free (dir);
          g_clear_object (&parent);
        }
      out = g_file_new_for_path (out_path);

      if (g_file_equal (in, out))
        {
          fprintf (stderr, "word42: %s: already in that format\n", out_path);
          failed++;
        }
      else if (!w42_document_load (doc, in, &error) ||
               !w42_io_save (w42_document_pt (doc), w42_document_page_setup (doc),
                             out, &error))
        {
          char *shown = g_file_get_parse_name (in);

          fprintf (stderr, "word42: %s: %s\n", shown,
                   error != NULL ? error->message : "could not be converted");
          g_free (shown);
          failed++;
        }
      else
        printf ("%s\n", out_path);

      g_clear_error (&error);
      g_object_unref (out);
      g_free (out_path);
      g_free (name);
      g_free (base);
      g_object_unref (doc);
      g_object_unref (in);
    }

  g_ptr_array_free (files, TRUE);
  return failed > 0 ? 1 : 0;
}

int
main (int argc, char *argv[])
{
  W42Application *app;
  int status;

#ifdef G_OS_WIN32
  {
    /* Windows gives main() its arguments in the local code page, where
     * Kapittel_Ørn.odt may not fit; GLib reads the command line again in
     * UTF-8, as GApplication does for itself below. */
    char **args = g_win32_get_command_line ();

    status = convert_main ((int) g_strv_length (args), args);
    g_strfreev (args);
  }
#else
  status = convert_main (argc, argv);
#endif
  if (status >= 0)
    return status;

  app = w42_application_new ();
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);

  return status;
}
