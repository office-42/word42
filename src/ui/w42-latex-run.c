/* w42-latex-run.c - typesetting a document with LaTeX
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The document is written as LaTeX into a folder of its own, the engine
 * runs there in the background, and its PDF goes where it was asked for.
 * Tectonic fetches the packages it needs the first time, which takes a
 * minute; the others have them installed.  pdfLaTeX, XeLaTeX and
 * LuaLaTeX run twice, so that the page count and the long tables' widths
 * are right; Tectonic runs itself as often as it needs.
 */

#include "w42-latex-run.h"

#include <string.h>
#include <glib/gi18n.h>

#include "w42-dialogs.h"
#include "w42-latex.h"
#include "w42-window.h"

typedef struct {
  GtkWindow *parent;         /* weak */
  GFile     *dir;            /* the folder the typesetting happens in */
  GFile     *target;         /* where the PDF goes, or NULL to open it */
  char      *engine;
  gboolean   tectonic;
  int        passes;         /* runs still to make */
} Job;

static void job_run (Job *job);

/* ---- finding an engine ------------------------------------------------ */

static char *
existing (const char *path)
{
  return path != NULL && g_file_test (path, G_FILE_TEST_IS_EXECUTABLE) ? g_strdup (path) : NULL;
}

/* An engine in one of the folders its installer uses, when the PATH does
 * not say: Tectonic in the user's programs, MiKTeX for one user or all,
 * the newest TeX Live, MacTeX. */
static char *
installed (const char *name)
{
  char *found = NULL;
#ifdef G_OS_WIN32
  const char *local = g_getenv ("LOCALAPPDATA");
  const char *programs = g_getenv ("ProgramFiles");
  char *exe = g_strconcat (name, ".exe", NULL);
  char *candidates[4] = { NULL };

  if (local != NULL)
    {
      candidates[0] = g_build_filename (local, "Programs", "tectonic", exe, NULL);
      candidates[1] = g_build_filename (local, "Programs", "MiKTeX", "miktex", "bin", "x64", exe, NULL);
    }
  if (programs != NULL)
    candidates[2] = g_build_filename (programs, "MiKTeX", "miktex", "bin", "x64", exe, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (candidates) && found == NULL; i++)
    found = existing (candidates[i]);
  for (guint i = 0; i < G_N_ELEMENTS (candidates); i++)
    g_free (candidates[i]);

  if (found == NULL)
    {
      /* C:\texlive\<year>\bin\windows, the newest year first. */
      GDir *d = g_dir_open ("C:\\texlive", 0, NULL);
      const char *year;
      char *best = NULL;

      while (d != NULL && (year = g_dir_read_name (d)) != NULL)
        if (g_ascii_isdigit (year[0]) && (best == NULL || strcmp (year, best) > 0))
          {
            g_free (best);
            best = g_strdup (year);
          }
      if (d != NULL)
        g_dir_close (d);
      if (best != NULL)
        {
          char *path = g_build_filename ("C:\\texlive", best, "bin", "windows", exe, NULL);

          found = existing (path);
          g_free (path);
          g_free (best);
        }
    }
  g_free (exe);
#else
  static const char * const DIRS[] = { "/Library/TeX/texbin", "/opt/homebrew/bin", "/usr/local/bin", "/usr/bin" };

  for (guint i = 0; i < G_N_ELEMENTS (DIRS) && found == NULL; i++)
    {
      char *path = g_build_filename (DIRS[i], name, NULL);

      found = existing (path);
      g_free (path);
    }
#endif
  return found;
}

char *
w42_latex_find_engine (void)
{
  static const char * const ENGINES[] = { "tectonic", "lualatex", "xelatex", "pdflatex" };

  for (guint i = 0; i < G_N_ELEMENTS (ENGINES); i++)
    {
      char *path = g_find_program_in_path (ENGINES[i]);

      if (path == NULL)
        path = installed (ENGINES[i]);
      if (path != NULL)
        return path;
    }
  return NULL;
}

/* ---- the job ---------------------------------------------------------- */

static void
job_free (Job *job)
{
  if (job->parent != NULL)
    g_object_remove_weak_pointer (G_OBJECT (job->parent), (gpointer *) &job->parent);
  g_clear_object (&job->dir);
  g_clear_object (&job->target);
  g_free (job->engine);
  g_free (job);
}

static void
say (Job *job, const char *text)
{
  if (job->parent != NULL && W42_IS_WINDOW (job->parent))
    w42_window_flash_status (W42_WINDOW (job->parent), text);
}

/* What LaTeX said went wrong: its lines that begin with '!', and the few
 * after each, or the output's end when it has none. */
static char *
latex_errors (const char *output)
{
  char **lines = g_strsplit (output != NULL ? output : "", "\n", -1);
  GString *s = g_string_new (NULL);
  guint n = g_strv_length (lines);

  for (guint i = 0; i < n && s->len < 1200; i++)
    if (lines[i][0] == '!' || g_str_has_prefix (lines[i], "error:"))
      for (guint k = i; k < n && k < i + 4; k++)
        g_string_append_printf (s, "%s\n", lines[k]);
  if (s->len == 0)
    for (guint i = n > 12 ? n - 12 : 0; i < n; i++)
      g_string_append_printf (s, "%s\n", lines[i]);
  g_strfreev (lines);
  return g_string_free (s, FALSE);
}

static void
job_finished (Job *job)
{
  GFile *pdf = g_file_get_child (job->dir, "document.pdf");
  GError *error = NULL;

  if (job->target != NULL)
    {
      if (g_file_copy (pdf, job->target, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error))
        {
          char *base = g_file_get_basename (job->target);
          /* Translators: %s is the name of the PDF made. */
          char *text = g_strdup_printf (_("Typeset with LaTeX as %s."), base);

          say (job, text);
          g_free (text);
          g_free (base);
        }
      else if (job->parent != NULL)
        w42_message_show (job->parent, _("Word42 could not save the PDF LaTeX made."),
                          error->message);
      g_clear_error (&error);
    }
  else if (job->parent != NULL)
    {
      /* A preview: the PDF opened in the desktop's viewer. */
      GtkFileLauncher *launcher = gtk_file_launcher_new (pdf);

      gtk_file_launcher_launch (launcher, job->parent, NULL, NULL, NULL);
      g_object_unref (launcher);
      say (job, _("Typeset with LaTeX."));
    }
  g_object_unref (pdf);
  job_free (job);
}

static void
on_engine_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Job *job = data;
  GSubprocess *proc = G_SUBPROCESS (source);
  char *output = NULL;
  GError *error = NULL;

  g_subprocess_communicate_utf8_finish (proc, result, &output, NULL, &error);
  if (error == NULL && g_subprocess_get_if_exited (proc) && g_subprocess_get_exit_status (proc) == 0)
    {
      if (--job->passes > 0)
        job_run (job);
      else
        job_finished (job);
    }
  else
    {
      if (job->parent != NULL)
        {
          char *detail = error != NULL ? g_strdup (error->message) : latex_errors (output);

          w42_message_show (job->parent, _("LaTeX could not typeset the document."), detail);
          g_free (detail);
        }
      say (job, _("LaTeX stopped with an error."));
      job_free (job);
    }
  g_clear_error (&error);
  g_free (output);
  g_object_unref (proc);
}

static void
job_run (Job *job)
{
  GSubprocessLauncher *launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                             G_SUBPROCESS_FLAGS_STDERR_MERGE |
                                                             G_SUBPROCESS_FLAGS_STDIN_PIPE);
  char *cwd = g_file_get_path (job->dir);
  GSubprocess *proc;
  GError *error = NULL;

  g_subprocess_launcher_set_cwd (launcher, cwd);
  if (job->tectonic)
    proc = g_subprocess_launcher_spawn (launcher, &error, job->engine, "document.tex", NULL);
  else
    proc = g_subprocess_launcher_spawn (launcher, &error, job->engine, "-interaction=nonstopmode",
                                        "-halt-on-error", "document.tex", NULL);
  if (proc == NULL)
    {
      if (job->parent != NULL)
        w42_message_show (job->parent, _("Word42 could not start LaTeX."), error->message);
      g_clear_error (&error);
      job_free (job);
    }
  else
    g_subprocess_communicate_utf8_async (proc, "", NULL, on_engine_done, job);
  g_object_unref (launcher);
  g_free (cwd);
}

void
w42_latex_typeset (GtkWindow *parent, W42Document *doc, GFile *target)
{
  char *engine = w42_latex_find_engine ();
  char *tmp;
  Job *job;
  GFile *tex;
  GError *error = NULL;

  g_return_if_fail (W42_IS_DOCUMENT (doc));

  if (engine == NULL)
    {
      w42_message_show (parent, _("LaTeX mode needs a TeX engine, and none is installed."),
                        _("Install Tectonic (tectonic-typesetting.github.io), which fetches what "
                          "it needs by itself, or MiKTeX (miktex.org) or TeX Live (tug.org/texlive), "
                          "and try again. Word42 looks for them on the PATH and where their "
                          "installers put them."));
      return;
    }

  tmp = g_dir_make_tmp ("word42-latex-XXXXXX", &error);
  if (tmp == NULL)
    {
      w42_message_show (parent, _("Word42 could not make a folder to typeset in."), error->message);
      g_error_free (error);
      g_free (engine);
      return;
    }

  job = g_new0 (Job, 1);
  job->parent = parent;
  if (parent != NULL)
    g_object_add_weak_pointer (G_OBJECT (parent), (gpointer *) &job->parent);
  job->dir = g_file_new_for_path (tmp);
  job->target = target != NULL ? g_object_ref (target) : NULL;
  job->engine = engine;
  {
    char *base = g_path_get_basename (engine);

    job->tectonic = g_ascii_strncasecmp (base, "tectonic", 8) == 0;
    g_free (base);
  }
  job->passes = job->tectonic ? 1 : 2;
  g_free (tmp);

  tex = g_file_get_child (job->dir, "document.tex");
  if (!w42_latex_export (w42_document_pt (doc), w42_document_page_setup (doc), tex, &error))
    {
      w42_message_show (parent, _("Word42 could not write the LaTeX source."), error->message);
      g_error_free (error);
      g_object_unref (tex);
      job_free (job);
      return;
    }
  g_object_unref (tex);

  say (job, _("Typesetting with LaTeX..."));
  job_run (job);
}
