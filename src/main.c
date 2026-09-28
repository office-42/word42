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

#include <glib/gi18n.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

/* The translations are looked for where W42_LOCALE_DIR says (the macOS
 * app's launcher sets it), then beside the program (the Windows bundle),
 * then where this build installs them.  Only the messages follow the
 * user's locale here; GTK takes the rest of it when it starts, and the
 * converter below does without. */
static void
init_translations (void)
{
  const char *env = g_getenv ("W42_LOCALE_DIR");
  char *dir;

  if (env != NULL && *env != '\0')
    dir = g_strdup (env);
  else
    {
#ifdef G_OS_WIN32
      char *base = g_win32_get_package_installation_directory_of_module (NULL);

      dir = g_build_filename (base, "share", "locale", NULL);
      g_free (base);
#else
      dir = g_strdup (W42_LOCALE_DIR);
#endif
    }

#ifdef LC_MESSAGES
  setlocale (LC_MESSAGES, "");
#endif
#if defined (G_OS_WIN32) && defined (LIBINTL_VERSION) && LIBINTL_VERSION >= 0x001500
  {
    /* The folder may be under a user name with an Ø in it, which the
     * narrow call would read in the local code page. */
    gunichar2 *wdir = g_utf8_to_utf16 (dir, -1, NULL, NULL, NULL);

    if (wdir != NULL)
      wbindtextdomain (GETTEXT_PACKAGE, (const wchar_t *) wdir);
    g_free (wdir);
  }
#else
  bindtextdomain (GETTEXT_PACKAGE, dir);
#endif
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
  g_free (dir);
}

/* word42 --convert-to=pdf roman.odt: the document read and written again
 * in another format, with no window, no display and no questions -- what
 * an author's script does to make the proofs, the e-book and the copy
 * for the publisher out of one manuscript.  Handled before GTK is
 * started, since GTK wants a display even to say what it would do.
 * A PDF made so is an export, as File > Export as PDF makes one: the
 * pages, compressed, with a password, a signature or the document inside
 * only when the --pdf- options ask for them.
 *
 * Returns -1 when the command line asks for no conversion. */
/* The value of --name=VALUE or --name VALUE at argv[*i], moving *i past
 * a value given as the next argument; NULL when argv[*i] is not --name. */
static const char *
option_value (int argc, char *argv[], int *i, const char *name)
{
  const char *arg = argv[*i];
  gsize n = strlen (name);

  if (strncmp (arg, name, n) != 0)
    return NULL;
  if (arg[n] == '=')
    return arg + n + 1;
  if (arg[n] == '\0' && *i + 1 < argc)
    return argv[++*i];
  return NULL;
}

/* A password as OpenSSL takes one: env:NAME is the environment variable
 * NAME, file:PATH the first line of the file, and anything else the
 * password itself -- which every user of the machine can see in the list
 * of running programs, where the other two are not. */
static char *
secret_value (const char *value)
{
  if (g_str_has_prefix (value, "env:"))
    return g_strdup (g_getenv (value + 4));
  if (g_str_has_prefix (value, "file:"))
    {
      char *contents = NULL;

      if (!g_file_get_contents (value + 5, &contents, NULL, NULL))
        return NULL;
      contents[strcspn (contents, "\r\n")] = '\0';
      return contents;
    }
  return g_strdup (value);
}

/* Sets `field` to the password `value` names. */
static void
set_secret (char **field, const char *value)
{
  char *secret = secret_value (value);

  w42_pdf_options_set (field, secret);
  if (secret != NULL)
    memset (secret, 0, strlen (secret));
  g_free (secret);
}

/* --pdf-pictures: a resolution, or the name Word gave it. */
static int
picture_ppi (const char *value)
{
  if (g_ascii_strcasecmp (value, "print") == 0)
    return W42_PDF_PPI_PRINT;
  if (g_ascii_strcasecmp (value, "screen") == 0)
    return W42_PDF_PPI_SCREEN;
  if (g_ascii_strcasecmp (value, "email") == 0 || g_ascii_strcasecmp (value, "e-mail") == 0)
    return W42_PDF_PPI_EMAIL;
  return (int) CLAMP (g_ascii_strtoll (value, NULL, 10), 0, 2400);
}

static int
convert_main (int argc, char *argv[])
{
  const char *format = NULL, *outdir = NULL;
  char *password = NULL;
  GPtrArray *files = g_ptr_array_new ();
  W42PdfOptions *pdf = w42_pdf_options_new ();
  int failed = 0;

  for (int i = 1; i < argc; i++)
    {
      const char *arg = argv[i];
      const char *v;

      if ((v = option_value (argc, argv, &i, "--convert-to")) != NULL)
        format = v;
      else if ((v = option_value (argc, argv, &i, "--outdir")) != NULL)
        outdir = v;
      else if ((v = option_value (argc, argv, &i, "--password")) != NULL)
        set_secret (&password, v);
      else if ((v = option_value (argc, argv, &i, "--pdf-password")) != NULL)
        set_secret (&pdf->open_password, v);
      else if ((v = option_value (argc, argv, &i, "--pdf-modify-password")) != NULL)
        set_secret (&pdf->modify_password, v);
      else if ((v = option_value (argc, argv, &i, "--pdf-sign")) != NULL)
        {
          w42_pdf_options_set (&pdf->certificate, v);
          pdf->sign = TRUE;
        }
      else if ((v = option_value (argc, argv, &i, "--pdf-sign-password")) != NULL)
        set_secret (&pdf->certificate_password, v);
      else if ((v = option_value (argc, argv, &i, "--pdf-reason")) != NULL)
        w42_pdf_options_set (&pdf->reason, v);
      else if ((v = option_value (argc, argv, &i, "--pdf-location")) != NULL)
        w42_pdf_options_set (&pdf->location, v);
      else if ((v = option_value (argc, argv, &i, "--pdf-contact")) != NULL)
        w42_pdf_options_set (&pdf->contact, v);
      else if ((v = option_value (argc, argv, &i, "--pdf-pictures")) != NULL)
        pdf->picture_ppi = picture_ppi (v);
      else if (g_str_equal (arg, "--pdf-uncompressed"))
        pdf->compress = FALSE;
      else if (g_str_equal (arg, "--pdf-keep-document"))
        pdf->keep_document = TRUE;
      else if (arg[0] != '-')
        g_ptr_array_add (files, (gpointer) arg);
    }

  if (format == NULL)
    {
      g_ptr_array_free (files, TRUE);
      w42_pdf_options_free (pdf);
      w42_pdf_options_set (&password, NULL);
      return -1;
    }
  if (*format == '.')
    format++;
  if (*format == '\0' || files->len == 0)
    {
      fprintf (stderr, "%s\n%s\n",
               _("Usage: word42 --convert-to=FORMAT [--outdir=DIR] FILE..."),
               _("FORMAT is the extension to write: pdf, epub, odt, docx, "
                 "rtf, html, txt, abw or tex."));
      g_ptr_array_free (files, TRUE);
      w42_pdf_options_free (pdf);
      w42_pdf_options_set (&password, NULL);
      return 2;
    }

  /* The folder asked for is made if it is not there, as a build script
   * expects of an output folder. */
  if (outdir != NULL && g_mkdir_with_parents (outdir, 0755) != 0)
    {
      fprintf (stderr, "word42: %s: %s\n", outdir, _("the folder could not be made"));
      g_ptr_array_free (files, TRUE);
      w42_pdf_options_free (pdf);
      w42_pdf_options_set (&password, NULL);
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
          fprintf (stderr, "word42: %s: %s\n", out_path, _("already in that format"));
          failed++;
        }
      else if (!w42_document_load_with (doc, in, password, NULL, &error) ||
               !w42_io_save_with (w42_document_pt (doc), w42_document_page_setup (doc),
                                  out, pdf, &error))
        {
          char *shown = g_file_get_parse_name (in);

          fprintf (stderr, "word42: %s: %s\n", shown,
                   error != NULL ? error->message : _("could not be converted"));
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
  w42_pdf_options_free (pdf);
  w42_pdf_options_set (&password, NULL);
  return failed > 0 ? 1 : 0;
}

int
main (int argc, char *argv[])
{
  W42Application *app;
  int status;

  init_translations ();

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

#ifdef G_OS_WIN32
  /* The title bars are the system's own, with its snapping to the edges
   * of the screens.  GTK draws its own on Windows, on every window and
   * dialog, unless told not to; a GTK_CSD the user has set is kept. */
  g_setenv ("GTK_CSD", "0", FALSE);
#endif

  app = w42_application_new ();
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);

  return status;
}
