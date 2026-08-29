/* w42-merge.c - see w42-merge.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-merge.h"

#include <glib/gstdio.h>
#include <string.h>

#include "w42-rtf.h"
#include "w42-search.h"

/* ---------------------------------------------------------------------- */
/* CSV                                                                     */
/* ---------------------------------------------------------------------- */

/* One row of CSV from `p`, quotes honoured, returning the position after
 * its line ending.  The separator is a comma, or a semicolon when the
 * header used one, which spreadsheets in some countries write. */
static const char *
csv_row (const char *p, const char *end, char sep, GPtrArray *out)
{
  GString *field = g_string_new (NULL);
  gboolean quoted = FALSE;

  while (p < end)
    {
      char c = *p;

      if (quoted)
        {
          if (c == '"')
            {
              if (p + 1 < end && p[1] == '"')
                {
                  g_string_append_c (field, '"');
                  p += 2;
                  continue;
                }
              quoted = FALSE;
              p++;
              continue;
            }
          g_string_append_c (field, c);
          p++;
          continue;
        }

      if (c == '"' && field->len == 0)
        {
          quoted = TRUE;
          p++;
        }
      else if (c == sep)
        {
          g_ptr_array_add (out, g_strdup (field->str));
          g_string_truncate (field, 0);
          p++;
        }
      else if (c == '\r' || c == '\n')
        {
          p++;
          if (c == '\r' && p < end && *p == '\n')
            p++;
          break;
        }
      else
        {
          g_string_append_c (field, c);
          p++;
        }
    }

  g_ptr_array_add (out, g_strdup (field->str));
  g_string_free (field, TRUE);
  return p;
}

W42MergeSource *
w42_merge_source_load (GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  const char *p, *end;
  char sep = ',';
  W42MergeSource *source;
  GPtrArray *header;

  g_return_val_if_fail (G_IS_FILE (file), NULL);

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return NULL;

  if (!g_utf8_validate (contents, length, NULL))
    {
      char *conv = g_convert (contents, length, "UTF-8", "WINDOWS-1252", NULL, &length, NULL);
      g_free (contents);
      contents = conv != NULL ? conv : g_strdup ("");
      length = strlen (contents);
    }

  p = contents;
  end = contents + length;
  if (length >= 3 && (guchar) p[0] == 0xEF && (guchar) p[1] == 0xBB && (guchar) p[2] == 0xBF)
    p += 3;                       /* a byte order mark */

  /* The header decides the separator: whichever it has more of. */
  {
    const char *nl = memchr (p, '\n', end - p);
    gsize first = nl != NULL ? (gsize) (nl - p) : (gsize) (end - p);
    int commas = 0, semis = 0;

    for (gsize i = 0; i < first; i++)
      {
        if (p[i] == ',') commas++;
        if (p[i] == ';') semis++;
      }
    if (semis > commas)
      sep = ';';
  }

  source = g_new0 (W42MergeSource, 1);
  source->rows = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);

  header = g_ptr_array_new ();
  p = csv_row (p, end, sep, header);
  for (guint i = 0; i < header->len; i++)
    g_strstrip (g_ptr_array_index (header, i));
  g_ptr_array_add (header, NULL);
  source->fields = (char **) g_ptr_array_free (header, FALSE);

  while (p < end)
    {
      GPtrArray *row = g_ptr_array_new ();
      guint n_fields = g_strv_length (source->fields);

      p = csv_row (p, end, sep, row);

      /* A blank line is not a row. */
      if (row->len == 1 && *(char *) g_ptr_array_index (row, 0) == '\0')
        {
          g_free (g_ptr_array_index (row, 0));
          g_ptr_array_free (row, TRUE);
          continue;
        }
      while (row->len < n_fields)
        g_ptr_array_add (row, g_strdup (""));
      g_ptr_array_add (row, NULL);
      g_ptr_array_add (source->rows, g_ptr_array_free (row, FALSE));
    }

  g_free (contents);

  if (source->fields[0] == NULL || *source->fields[0] == '\0')
    {
      w42_merge_source_free (source);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "The data source has no field names in its first row.");
      return NULL;
    }
  return source;
}

void
w42_merge_source_free (W42MergeSource *source)
{
  if (source == NULL)
    return;
  g_strfreev (source->fields);
  g_ptr_array_free (source->rows, TRUE);
  g_free (source);
}

char *
w42_merge_field_text (const char *name)
{
  return g_strdup_printf ("\302\253%s\302\273", name);
}

/* ---------------------------------------------------------------------- */
/* Merging                                                                 */
/* ---------------------------------------------------------------------- */

gboolean
w42_merge_to_file (W42PieceTable *pt, const W42PageSetup *page,
                   const W42MergeSource *source, GFile *out, GError **error)
{
  char *tmp_path, *copy_path;
  GFile *tmp, *copy_file;
  char *master = NULL;
  gsize master_len = 0;
  GString *result;
  const char *body;
  gboolean ok;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (source != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (out), FALSE);

  /* The main document as RTF, once; each copy is the same RTF with the
   * fields replaced, and the copies share one header since they share
   * one document. */
  tmp_path = g_build_filename (g_get_tmp_dir (), "word42-merge-XXXXXX.rtf", NULL);
  {
    int fd = g_mkstemp (tmp_path);
    if (fd < 0)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not make a temporary file.");
        g_free (tmp_path);
        return FALSE;
      }
    g_close (fd, NULL);
  }
  tmp = g_file_new_for_path (tmp_path);
  copy_path = g_strconcat (tmp_path, ".copy.rtf", NULL);
  copy_file = g_file_new_for_path (copy_path);

  if (!w42_rtf_save (pt, page, tmp, error) ||
      !g_file_get_contents (tmp_path, &master, &master_len, error))
    {
      g_object_unref (tmp);
      g_object_unref (copy_file);
      g_unlink (tmp_path);
      g_free (tmp_path);
      g_free (copy_path);
      return FALSE;
    }

  /* The first paragraph at the start of a line: the header and footer
   * groups before it have \pard of their own on their lines. */
  body = strstr (master, "\n\\pard");
  body = body != NULL ? body + 1 : master + master_len - 1;

  result = g_string_new_len (master, body - master);

  for (guint r = 0; r < source->rows->len; r++)
    {
      char **row = g_ptr_array_index (source->rows, r);
      W42PieceTable *copy = w42_pt_new ();
      W42PageSetup pg = { 0 };
      char *copy_rtf = NULL;
      gsize copy_len = 0;
      const char *copy_body;
      W42SearchOptions options = { TRUE, FALSE, FALSE, TRUE };

      if (!w42_rtf_load (copy, &pg, tmp, NULL))
        {
          w42_pt_free (copy);
          continue;
        }

      for (guint f = 0; source->fields[f] != NULL; f++)
        {
          char *needle = w42_merge_field_text (source->fields[f]);
          const char *value = f < g_strv_length (row) ? row[f] : "";

          w42_search_replace_all (copy, needle, value, &options);
          g_free (needle);
        }

      if (w42_rtf_save (copy, page, copy_file, NULL) &&
          g_file_get_contents (copy_path, &copy_rtf, &copy_len, NULL) &&
          (copy_body = strstr (copy_rtf, "\n\\pard")) != NULL && (copy_body += 1) != NULL)
        {
          gsize n = copy_len - (copy_body - copy_rtf);

          /* Up to, not including, the file's closing brace -- one brace,
           * whatever groups the last paragraph ends in. */
          while (n > 0 && g_ascii_isspace (copy_body[n - 1]))
            n--;
          if (n > 0 && copy_body[n - 1] == '}')
            n--;
          if (r > 0)
            g_string_append (result, "\\page\n");
          g_string_append_len (result, copy_body, n);
          g_string_append_c (result, '\n');
        }
      g_free (copy_rtf);
      w42_pt_free (copy);
    }

  g_string_append (result, "}\n");

  ok = g_file_replace_contents (out, result->str, result->len, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);

  g_string_free (result, TRUE);
  g_free (master);
  g_object_unref (tmp);
  g_object_unref (copy_file);
  g_unlink (tmp_path);
  g_unlink (copy_path);
  g_free (tmp_path);
  g_free (copy_path);
  return ok;
}
