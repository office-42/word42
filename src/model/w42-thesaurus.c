/* w42-thesaurus.c - Tools > Language > Thesaurus
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A MyThes thesaurus is two files.  The .idx is a line naming the
 * encoding, a line with the number of entries, and then "word|offset" for
 * every word, in order; the .dat is the encoding line again and then, at
 * each offset, "word|n" followed by n lines of "(part of speech)|synonym|
 * synonym|...".  The index is read into a table once; the .dat is mapped
 * and read at the offset asked for, since it runs to tens of megabytes.
 */

#include "w42-thesaurus.h"

#include <string.h>

struct _W42Thesaurus {
  char        *language;
  GMappedFile *dat;
  char        *encoding;      /* NULL for UTF-8 */
  GHashTable  *index;         /* lower-cased word -> offset into the .dat */
};

/* Where the th_*.dat files may be: next to the program (the Windows
 * bundle), where this build was told they are, and the usual places. */
static char *
find_thesaurus (const char *language)
{
  const char *env = g_getenv ("W42_THESAURUS_DIR");
  GPtrArray *dirs = g_ptr_array_new_with_free_func (g_free);
  char *found = NULL;

  if (env != NULL && *env != '\0')
    g_ptr_array_add (dirs, g_strdup (env));
#ifdef G_OS_WIN32
  {
    char *base = g_win32_get_package_installation_directory_of_module (NULL);
    if (base != NULL)
      g_ptr_array_add (dirs, g_build_filename (base, "share", "mythes", NULL));
    g_free (base);
  }
#endif
#ifdef W42_THESAURUS_DIR
  g_ptr_array_add (dirs, g_strdup (W42_THESAURUS_DIR));
#endif
  g_ptr_array_add (dirs, g_strdup ("/usr/share/mythes"));
  g_ptr_array_add (dirs, g_strdup ("/usr/local/share/mythes"));
  g_ptr_array_add (dirs, g_strdup ("/app/share/mythes"));
  g_ptr_array_add (dirs, g_strdup ("/opt/homebrew/share/mythes"));

  for (guint i = 0; i < dirs->len && found == NULL; i++)
    {
      /* LibreOffice's English files carry a "_v2"; the others do not. */
      const char *dir = g_ptr_array_index (dirs, i);
      char *v2 = g_strdup_printf ("th_%s_v2.dat", language);
      char *v1 = g_strdup_printf ("th_%s.dat", language);
      char *path = g_build_filename (dir, v2, NULL);

      if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        found = path;
      else
        {
          g_free (path);
          path = g_build_filename (dir, v1, NULL);
          if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
            found = path;
          else
            g_free (path);
        }
      g_free (v2);
      g_free (v1);
    }

  g_ptr_array_free (dirs, TRUE);
  return found;
}

/* The languages worth looking for a thesaurus in, the user's first. */
static GPtrArray *
candidate_languages (void)
{
  const char *const *names = g_get_language_names ();
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);

  for (guint i = 0; names != NULL && names[i] != NULL; i++)
    {
      char *lang = g_strdup (names[i]);
      char *dot = strchr (lang, '.');
      char *at = strchr (lang, '@');

      if (dot != NULL) *dot = '\0';
      if (at != NULL) *at = '\0';
      if (strchr (lang, '_') != NULL && !g_str_equal (lang, "C") &&
          !g_str_equal (lang, "POSIX"))
        g_ptr_array_add (out, lang);
      else
        g_free (lang);
    }
  g_ptr_array_add (out, g_strdup ("en_US"));
  g_ptr_array_add (out, g_strdup ("en_GB"));
  return out;
}

/* The line at `p`, up to and not including its end; `p` is moved past it. */
static char *
take_line (const char **p, const char *end)
{
  const char *nl = memchr (*p, '\n', end - *p);
  char *line;

  if (nl == NULL)
    nl = end;
  line = g_strndup (*p, nl - *p);
  if (nl > *p && line[nl - *p - 1] == '\r')
    line[nl - *p - 1] = '\0';
  *p = nl < end ? nl + 1 : end;
  return line;
}

static char *
to_utf8 (W42Thesaurus *self, const char *text)
{
  if (self->encoding == NULL || g_utf8_validate (text, -1, NULL))
    return g_strdup (text);
  return g_convert (text, -1, "UTF-8", self->encoding, NULL, NULL, NULL);
}

static gboolean
read_index (W42Thesaurus *self, const char *path)
{
  char *contents = NULL;
  gsize length = 0;
  const char *p, *end;
  char *line;

  if (!g_file_get_contents (path, &contents, &length, NULL))
    return FALSE;
  p = contents;
  end = contents + length;

  line = take_line (&p, end);           /* the encoding */
  if (g_ascii_strcasecmp (line, "UTF-8") != 0 && *line != '\0')
    self->encoding = g_strdup (line);
  else
    self->encoding = NULL;
  g_free (line);
  line = take_line (&p, end);           /* the count, not needed */
  g_free (line);

  self->index = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  while (p < end)
    {
      char *bar;

      line = take_line (&p, end);
      bar = strrchr (line, '|');
      if (bar != NULL && bar > line)
        {
          gsize offset = (gsize) g_ascii_strtoull (bar + 1, NULL, 10);
          char *word;

          *bar = '\0';
          word = to_utf8 (self, line);
          if (word != NULL)
            {
              char *key = g_utf8_strdown (word, -1);

              g_hash_table_replace (self->index, key, GSIZE_TO_POINTER (offset));
              g_free (word);
            }
        }
      g_free (line);
    }
  g_free (contents);
  return g_hash_table_size (self->index) > 0;
}

W42Thesaurus *
w42_thesaurus_new (void)
{
  return w42_thesaurus_new_for (NULL);
}

W42Thesaurus *
w42_thesaurus_new_for (const char *want)
{
  GPtrArray *langs = candidate_languages ();
  W42Thesaurus *self = NULL;

  /* The document's language first, spelt the way the files are named:
   * "nb-NO" is th_nb_NO; Norwegian written "no" is Bokmål. */
  if (want != NULL && *want != '\0')
    {
      char *tag = g_strdup (want);
      char *dash;

      for (char *p = tag; *p != '\0'; p++)
        if (*p == '-')
          *p = '_';
      if (g_ascii_strcasecmp (tag, "no") == 0 || g_ascii_strcasecmp (tag, "no_NO") == 0)
        {
          g_free (tag);
          tag = g_strdup ("nb_NO");
        }
      dash = strchr (tag, '_');
      if (dash == NULL)
        {
          /* A language without its country: its own country's file,
           * nb_NO for nb, de_DE for de. */
          char *upper = g_ascii_strup (tag, -1);
          char *full = g_strdup_printf ("%s_%s", tag, upper);

          g_free (upper);
          g_free (tag);
          tag = full;
        }
      g_ptr_array_insert (langs, 0, tag);
    }

  for (guint i = 0; i < langs->len && self == NULL; i++)
    {
      const char *lang = g_ptr_array_index (langs, i);
      char *dat = find_thesaurus (lang);
      char *idx;

      if (dat == NULL)
        continue;
      idx = g_strdup (dat);
      memcpy (idx + strlen (idx) - 3, "idx", 3);

      self = g_new0 (W42Thesaurus, 1);
      self->language = g_strdup (lang);
      self->dat = g_mapped_file_new (dat, FALSE, NULL);
      if (self->dat == NULL || !read_index (self, idx))
        {
          w42_thesaurus_free (self);
          self = NULL;
        }
      g_free (idx);
      g_free (dat);
    }
  g_ptr_array_free (langs, TRUE);
  return self;
}

void
w42_thesaurus_free (W42Thesaurus *self)
{
  if (self == NULL)
    return;
  g_clear_pointer (&self->index, g_hash_table_unref);
  g_clear_pointer (&self->dat, g_mapped_file_unref);
  g_free (self->encoding);
  g_free (self->language);
  g_free (self);
}

const char *
w42_thesaurus_language (W42Thesaurus *self)
{
  return self != NULL ? self->language : NULL;
}

static void
sense_free (gpointer data)
{
  W42Sense *sense = data;

  g_free (sense->meaning);
  g_strfreev (sense->synonyms);
  g_free (sense);
}

GPtrArray *
w42_thesaurus_lookup (W42Thesaurus *self, const char *word)
{
  char *key;
  gpointer value;
  gsize offset;
  const char *p, *end;
  char *line, *bar;
  int n;
  GPtrArray *senses;

  g_return_val_if_fail (self != NULL, NULL);
  if (word == NULL || *word == '\0')
    return NULL;

  key = g_utf8_strdown (word, -1);
  g_strstrip (key);
  if (!g_hash_table_lookup_extended (self->index, key, NULL, &value))
    {
      g_free (key);
      return NULL;
    }
  g_free (key);

  offset = GPOINTER_TO_SIZE (value);
  end = g_mapped_file_get_contents (self->dat) + g_mapped_file_get_length (self->dat);
  if (offset >= g_mapped_file_get_length (self->dat))
    return NULL;
  p = g_mapped_file_get_contents (self->dat) + offset;

  /* "word|n": how many senses follow. */
  line = take_line (&p, end);
  bar = strrchr (line, '|');
  n = bar != NULL ? atoi (bar + 1) : 0;
  g_free (line);
  if (n <= 0 || n > 200)
    return NULL;

  senses = g_ptr_array_new_with_free_func (sense_free);
  for (int i = 0; i < n && p < end; i++)
    {
      char *utf8;
      char **fields;
      W42Sense *sense;
      guint count = 0;

      line = take_line (&p, end);
      utf8 = to_utf8 (self, line);
      g_free (line);
      if (utf8 == NULL)
        continue;
      fields = g_strsplit (utf8, "|", -1);
      g_free (utf8);
      for (count = 0; fields[count] != NULL; count++)
        ;
      if (count < 2)
        {
          g_strfreev (fields);
          continue;
        }

      sense = g_new0 (W42Sense, 1);
      /* The file heads a sense with its part of speech alone; the first
       * synonym beside it says which meaning is meant, as Word's box
       * listed them -- without the file's "(generic term)" note on it. */
      {
        char *head = g_strdup (fields[1]);
        char *note = strstr (head, " (");

        if (note != NULL)
          *note = '\0';
        sense->meaning = g_strdup_printf ("%s %s", head, fields[0]);
        g_free (head);
      }
      sense->synonyms = g_new0 (char *, count);
      for (guint f = 1; f < count; f++)
        sense->synonyms[f - 1] = g_strdup (fields[f]);
      g_ptr_array_add (senses, sense);
      g_strfreev (fields);
    }

  if (senses->len == 0)
    {
      g_ptr_array_free (senses, TRUE);
      return NULL;
    }
  return senses;
}

void
w42_thesaurus_senses_free (GPtrArray *senses)
{
  if (senses != NULL)
    g_ptr_array_free (senses, TRUE);
}
