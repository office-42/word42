/* w42-latex-run.c - typesetting a document with LaTeX
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The document is written as LaTeX into a folder of its own, the engine
 * runs there in the background, and its PDF goes where it was asked for.
 * Tectonic fetches the packages it needs the first time, which takes a
 * minute; the others have them installed.  pdfLaTeX, XeLaTeX and
 * LuaLaTeX run again for as long as they say the cross-references have
 * changed -- the page count, the table of contents, the long tables'
 * widths -- as latexmk would run them; Tectonic runs itself as often as
 * it needs.
 */

#include "w42-latex-run.h"

#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include "w42-dialogs.h"
#include "w42-latex.h"
#include "w42-window.h"

/* Runs enough for the references of any document to settle: a table of
 * contents that moves the pages it lists takes a third. */
#define MAX_RUNS 4

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

/* ---- reading what the engine said ------------------------------------- */

/* What TeX says to someone at a terminal, which a person reading the
 * error in a box can do nothing with. */
static gboolean
terminal_talk (const char *line)
{
  static const char * const TALK[] = {
    "Type X to quit", "Type  H <return>", "Enter file name:", "! Emergency stop",
    "*** (job aborted", "No pages of output", "!  ==> Fatal error",
  };

  if (*line == '\0')
    return TRUE;
  for (guint i = 0; i < G_N_ELEMENTS (TALK); i++)
    if (g_str_has_prefix (line, TALK[i]))
      return TRUE;
  return FALSE;
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
    if ((lines[i][0] == '!' || g_str_has_prefix (lines[i], "error:")) && !terminal_talk (lines[i]))
      for (guint k = i; k < n && k < i + 4; k++)
        if (!terminal_talk (lines[k]))
          g_string_append_printf (s, "%s\n", lines[k]);
  if (s->len == 0)
    for (guint i = n > 12 ? n - 12 : 0; i < n; i++)
      g_string_append_printf (s, "%s\n", lines[i]);
  g_strfreev (lines);
  return g_string_free (s, FALSE);
}

/* The source line of the first error: TeX's "l.12" under its '!' line,
 * or Tectonic's "document.tex:12:". */
static int
latex_error_line (const char *output)
{
  const char *bang = output;
  const char *p;

  if (output == NULL)
    return 0;
  p = strstr (output, "document.tex:");
  if (p != NULL && g_ascii_isdigit (p[13]))
    return atoi (p + 13);
  while (bang != NULL && *bang != '!')
    {
      bang = strchr (bang, '\n');
      if (bang != NULL)
        bang++;
    }
  for (p = bang; p != NULL; )
    {
      p = strstr (p, "\nl.");
      if (p == NULL)
        break;
      p += 3;
      if (g_ascii_isdigit (*p))
        return atoi (p);
    }
  return 0;
}

/* LaTeX asks to be run again when what it wrote down for the next run --
 * page numbers, labels, the outline -- differs from what it read. */
static gboolean
asks_rerun (const char *output)
{
  return output != NULL &&
         (strstr (output, "Rerun to get") != NULL ||
          strstr (output, "Label(s) may have changed") != NULL ||
          strstr (output, "Rerun LaTeX") != NULL ||
          strstr (output, "Please rerun LaTeX") != NULL);
}

/* ---- compiling -------------------------------------------------------- */

typedef struct {
  char         *engine;
  char         *name;          /* "pdflatex", without a folder or .exe */
  char         *dir;
  gboolean      tectonic;
  gboolean      synctex;
  int           runs;          /* made so far */
  GCancellable *cancellable;
  W42LatexDone  done;
  gpointer      data;
} Compile;

static void compile_run (Compile *c);

static void
compile_free (Compile *c)
{
  g_free (c->engine);
  g_free (c->name);
  g_free (c->dir);
  g_clear_object (&c->cancellable);
  g_free (c);
}

static void
compile_finish (Compile *c, gboolean ok, const char *output)
{
  W42LatexResult result = { 0 };
  char *pdf = g_build_filename (c->dir, "document.pdf", NULL);
  char *synctex = g_build_filename (c->dir, "document.synctex.gz", NULL);
  char *errors = NULL;

  if (!g_file_test (synctex, G_FILE_TEST_EXISTS))
    {
      g_free (synctex);
      synctex = g_build_filename (c->dir, "document.synctex", NULL);
      if (!g_file_test (synctex, G_FILE_TEST_EXISTS))
        g_clear_pointer (&synctex, g_free);
    }
  result.ok = ok && g_file_test (pdf, G_FILE_TEST_EXISTS);
  result.engine = c->name;
  if (result.ok)
    {
      result.pdf = pdf;
      result.synctex = synctex;
    }
  else
    {
      errors = latex_errors (output);
      result.errors = errors;
      result.line = latex_error_line (output);
    }
  if (!g_cancellable_is_cancelled (c->cancellable))
    c->done (&result, c->data);
  g_free (errors);
  g_free (synctex);
  g_free (pdf);
  compile_free (c);
}

static void
on_engine_done (GObject *source, GAsyncResult *result, gpointer data)
{
  Compile *c = data;
  GSubprocess *proc = G_SUBPROCESS (source);
  GBytes *out = NULL;
  char *output = NULL;
  GError *error = NULL;
  gboolean ok;

  g_subprocess_communicate_finish (proc, result, &out, NULL, &error);
  if (g_cancellable_is_cancelled (c->cancellable))
    {
      /* Nobody is waiting for it any more: a preview closed, a window gone. */
      g_subprocess_force_exit (proc);
      g_clear_error (&error);
      if (out != NULL)
        g_bytes_unref (out);
      g_object_unref (proc);
      compile_free (c);
      return;
    }

  /* TeX writes what it has to say in the bytes of the input it was
   * reading, which need not be UTF-8. */
  if (out != NULL)
    output = g_utf8_make_valid (g_bytes_get_data (out, NULL), (gssize) g_bytes_get_size (out));
  ok = error == NULL && g_subprocess_get_if_exited (proc) && g_subprocess_get_exit_status (proc) == 0;
  if (error != NULL)
    {
      g_free (output);
      output = g_strdup (error->message);
    }

  if (ok && !c->tectonic && c->runs < MAX_RUNS && asks_rerun (output))
    compile_run (c);
  else
    compile_finish (c, ok, output);

  g_clear_error (&error);
  if (out != NULL)
    g_bytes_unref (out);
  g_free (output);
  g_object_unref (proc);
}

static void
compile_run (Compile *c)
{
  GSubprocessLauncher *launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                             G_SUBPROCESS_FLAGS_STDERR_MERGE |
                                                             G_SUBPROCESS_FLAGS_STDIN_PIPE);
  GSubprocess *proc;
  GError *error = NULL;

  c->runs++;
  g_subprocess_launcher_set_cwd (launcher, c->dir);
  if (c->tectonic)
    proc = g_subprocess_launcher_spawn (launcher, &error, c->engine,
                                        c->synctex ? "--synctex" : "document.tex",
                                        c->synctex ? "document.tex" : NULL, NULL);
  else
    proc = g_subprocess_launcher_spawn (launcher, &error, c->engine, "-interaction=nonstopmode",
                                        "-halt-on-error",
                                        c->synctex ? "-synctex=1" : "document.tex",
                                        c->synctex ? "document.tex" : NULL, NULL);
  g_object_unref (launcher);
  if (proc == NULL)
    {
      char *message = g_strdup_printf (_("Word42 could not start LaTeX: %s"), error->message);

      g_error_free (error);
      compile_finish (c, FALSE, message);
      g_free (message);
      return;
    }
  /* An empty standard input: a TeX that stops to ask about an error
   * reads its end and gives up, rather than waiting for ever. */
  {
    GBytes *nothing = g_bytes_new_static ("", 0);

    g_subprocess_communicate_async (proc, nothing, c->cancellable, on_engine_done, c);
    g_bytes_unref (nothing);
  }
}

void
w42_latex_compile (const char *engine, const char *dir, gboolean synctex,
                   GCancellable *cancellable, W42LatexDone done, gpointer data)
{
  Compile *c;
  char *base, *dot;

  g_return_if_fail (engine != NULL && dir != NULL && done != NULL);

  c = g_new0 (Compile, 1);
  c->engine = g_strdup (engine);
  c->dir = g_strdup (dir);
  c->synctex = synctex;
  c->cancellable = cancellable != NULL ? g_object_ref (cancellable) : g_cancellable_new ();
  c->done = done;
  c->data = data;
  base = g_path_get_basename (engine);
  dot = strrchr (base, '.');
  if (dot != NULL && g_ascii_strcasecmp (dot, ".exe") == 0)
    *dot = '\0';
  c->name = base;
  c->tectonic = g_ascii_strncasecmp (base, "tectonic", 8) == 0;
  compile_run (c);
}

void
w42_latex_remove_dir (const char *dir)
{
  GDir *d;
  const char *name;

  if (dir == NULL)
    return;
  d = g_dir_open (dir, 0, NULL);
  while (d != NULL && (name = g_dir_read_name (d)) != NULL)
    {
      char *path = g_build_filename (dir, name, NULL);

      if (g_file_test (path, G_FILE_TEST_IS_DIR) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
        w42_latex_remove_dir (path);
      else
        g_remove (path);
      g_free (path);
    }
  if (d != NULL)
    g_dir_close (d);
  g_rmdir (dir);
}

/* ---- File > Export as PDF and LaTeX Preview in LaTeX mode -------------- */

typedef struct {
  GtkWindow *parent;         /* weak */
  char      *dir;            /* the folder the typesetting happens in */
  GFile     *target;         /* where the PDF goes, or NULL to open it */
} Job;

static void
job_free (Job *job)
{
  if (job->parent != NULL)
    g_object_remove_weak_pointer (G_OBJECT (job->parent), (gpointer *) &job->parent);
  g_clear_object (&job->target);
  g_free (job->dir);
  g_free (job);
}

static void
say (Job *job, const char *text)
{
  if (job->parent != NULL && W42_IS_WINDOW (job->parent))
    w42_window_flash_status (W42_WINDOW (job->parent), text);
}

static void
on_typeset (const W42LatexResult *result, gpointer data)
{
  Job *job = data;
  GError *error = NULL;

  if (!result->ok)
    {
      if (job->parent != NULL)
        w42_message_show (job->parent, _("LaTeX could not typeset the document."), result->errors);
      say (job, _("LaTeX stopped with an error."));
      w42_latex_remove_dir (job->dir);
    }
  else if (job->target != NULL)
    {
      GFile *pdf = g_file_new_for_path (result->pdf);

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
      g_object_unref (pdf);
      /* The PDF is where it was asked for; the rest is scaffolding. */
      w42_latex_remove_dir (job->dir);
    }
  else if (job->parent != NULL)
    {
      /* A preview: the PDF opened in the desktop's viewer, which reads it
       * from where it is, so the folder stays. */
      GFile *pdf = g_file_new_for_path (result->pdf);
      GtkFileLauncher *launcher = gtk_file_launcher_new (pdf);

      gtk_file_launcher_launch (launcher, job->parent, NULL, NULL, NULL);
      g_object_unref (launcher);
      g_object_unref (pdf);
      say (job, _("Typeset with LaTeX."));
    }
  job_free (job);
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
  job->dir = tmp;
  job->target = target != NULL ? g_object_ref (target) : NULL;

  tex = g_file_new_build_filename (tmp, "document.tex", NULL);
  if (!w42_latex_export (w42_document_pt (doc), w42_document_page_setup (doc), tex, &error))
    {
      w42_message_show (parent, _("Word42 could not write the LaTeX source."), error->message);
      g_error_free (error);
      g_object_unref (tex);
      w42_latex_remove_dir (job->dir);
      job_free (job);
      g_free (engine);
      return;
    }
  g_object_unref (tex);

  say (job, _("Typesetting with LaTeX..."));
  w42_latex_compile (engine, job->dir, FALSE, NULL, on_typeset, job);
  g_free (engine);
}
