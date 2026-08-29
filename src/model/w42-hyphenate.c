/* w42-hyphenate.c - see w42-hyphenate.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-hyphenate.h"

#include <string.h>

#ifdef HAVE_HYPHEN
#include <hyphen.h>
#endif

#define SOFT_HYPHEN 0x00AD

struct _W42Hyphenator {
#ifdef HAVE_HYPHEN
  HyphenDict *dict;
#endif
  char *language;
};

#ifdef HAVE_HYPHEN

/* Where the hyph_*.dic files may be: next to the program (the Windows
 * bundle), where this build was told they are, and the usual places. */
static char *
find_dictionary (const char *language)
{
  char *name = g_strdup_printf ("hyph_%s.dic", language);
  const char *env = g_getenv ("W42_HYPHEN_DIR");
  GPtrArray *dirs = g_ptr_array_new_with_free_func (g_free);
  char *found = NULL;

  if (env != NULL && *env != '\0')
    g_ptr_array_add (dirs, g_strdup (env));
#ifdef G_OS_WIN32
  {
    char *base = g_win32_get_package_installation_directory_of_module (NULL);
    if (base != NULL)
      g_ptr_array_add (dirs, g_build_filename (base, "share", "hyphen", NULL));
    g_free (base);
  }
#endif
#ifdef W42_HYPHEN_DIR
  g_ptr_array_add (dirs, g_strdup (W42_HYPHEN_DIR));
#endif
  g_ptr_array_add (dirs, g_strdup ("/usr/share/hyphen"));
  g_ptr_array_add (dirs, g_strdup ("/usr/local/share/hyphen"));
  g_ptr_array_add (dirs, g_strdup ("/app/share/hyphen"));
  g_ptr_array_add (dirs, g_strdup ("/opt/homebrew/share/hyphen"));

  for (guint i = 0; i < dirs->len && found == NULL; i++)
    {
      char *path = g_build_filename (g_ptr_array_index (dirs, i), name, NULL);

      if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        found = path;
      else
        g_free (path);
    }

  g_ptr_array_free (dirs, TRUE);
  g_free (name);
  return found;
}

#endif

W42Hyphenator *
w42_hyphenator_new (void)
{
#ifdef HAVE_HYPHEN
  const char * const *langs = g_get_language_names ();
  GPtrArray *candidates = g_ptr_array_new_with_free_func (g_free);
  W42Hyphenator *hyph = NULL;

  /* The user's language, its country-less form with the country the
   * dictionaries are named by, then English. */
  for (guint i = 0; langs != NULL && langs[i] != NULL; i++)
    {
      char *lang = g_strdup (langs[i]);
      char *dot = strchr (lang, '.');

      if (dot != NULL)
        *dot = '\0';
      if (strlen (lang) >= 2 && !g_str_equal (lang, "C"))
        g_ptr_array_add (candidates, lang);
      else
        g_free (lang);
    }
  g_ptr_array_add (candidates, g_strdup ("en_US"));
  g_ptr_array_add (candidates, g_strdup ("en_GB"));

  for (guint i = 0; i < candidates->len && hyph == NULL; i++)
    {
      const char *language = g_ptr_array_index (candidates, i);
      char *path = find_dictionary (language);
      HyphenDict *dict;

      if (path == NULL)
        continue;
      dict = hnj_hyphen_load (path);
      g_free (path);
      if (dict == NULL)
        continue;

      hyph = g_new0 (W42Hyphenator, 1);
      hyph->dict = dict;
      hyph->language = g_strdup (language);
    }

  g_ptr_array_free (candidates, TRUE);
  return hyph;
#else
  return NULL;
#endif
}

void
w42_hyphenator_free (W42Hyphenator *hyph)
{
  if (hyph == NULL)
    return;
#ifdef HAVE_HYPHEN
  hnj_hyphen_free (hyph->dict);
#endif
  g_free (hyph->language);
  g_free (hyph);
}

const char *
w42_hyphenator_language (W42Hyphenator *hyph)
{
  return hyph != NULL ? hyph->language : NULL;
}

/* The break points of `word` (UTF-8, letters only) as character indexes:
 * a break after character k means index k is set.  Returns the number of
 * characters, or -1 when the word cannot be hyphenated. */
static int
break_points (W42Hyphenator *hyph, const char *word, GArray *out)
{
#ifdef HAVE_HYPHEN
  gsize bytes = strlen (word);
  glong n_chars = g_utf8_strlen (word, -1);
  char *hyphens, *hyphword;
  char **rep = NULL;
  int *pos = NULL, *cut = NULL;
  char *lower;
  int rc;

  if (n_chars < 5 || bytes == 0)
    return -1;

  /* The patterns are for lower case; a word whose lower case has a
   * different number of characters cannot be mapped back and is left. */
  lower = g_utf8_strdown (word, -1);
  if (g_utf8_strlen (lower, -1) != n_chars)
    {
      g_free (lower);
      return -1;
    }
  bytes = strlen (lower);
  hyphens = g_malloc0 (bytes + 5);
  hyphword = g_malloc0 (bytes * 2 + 5);

  rc = hnj_hyphen_hyphenate2 (hyph->dict, lower, (int) bytes, hyphens, hyphword,
                              &rep, &pos, &cut);

  if (rc == 0)
    {
      /* hyphens[] is per byte of the word; odd means a break after that
       * byte.  Count characters as we go so the index is a character
       * index, and keep Word's two-before, three-after minimum. */
      const char *p = lower;
      int ci = 0;

      while (*p != '\0')
        {
          const char *next = g_utf8_next_char (p);
          /* libhyphen reports by character, not by byte. */
          gboolean odd = (hyphens[ci] & 1) != 0;
          gboolean standard = rep == NULL || rep[ci] == NULL;

          if (odd && standard && ci + 1 >= 2 && n_chars - (ci + 1) >= 3)
            {
              int after = ci + 1;
              g_array_append_val (out, after);
            }
          ci++;
          p = next;
        }
    }

  if (rep != NULL)
    {
      for (gsize i = 0; i < bytes; i++)
        g_free (rep[i]);
      free (rep);
    }
  free (pos);
  free (cut);
  g_free (hyphens);
  g_free (hyphword);
  g_free (lower);
  return rc == 0 ? (int) n_chars : -1;
#else
  (void) hyph; (void) word; (void) out;
  return -1;
#endif
}

char *
w42_hyphenator_word (W42Hyphenator *hyph, const char *word)
{
  GArray *breaks;
  GString *out;
  const char *p;
  int ci = 0;
  guint next = 0;

  g_return_val_if_fail (word != NULL, NULL);
  if (hyph == NULL)
    return NULL;

  breaks = g_array_new (FALSE, FALSE, sizeof (int));
  if (break_points (hyph, word, breaks) < 0 || breaks->len == 0)
    {
      g_array_free (breaks, TRUE);
      return NULL;
    }

  out = g_string_new (NULL);
  for (p = word; *p != '\0'; p = g_utf8_next_char (p))
    {
      g_string_append_unichar (out, g_utf8_get_char (p));
      ci++;
      if (next < breaks->len && g_array_index (breaks, int, next) == ci)
        {
          g_string_append_unichar (out, SOFT_HYPHEN);
          next++;
        }
    }
  g_array_free (breaks, TRUE);
  return g_string_free (out, FALSE);
}

/* ---------------------------------------------------------------------- */
/* The document                                                            */
/* ---------------------------------------------------------------------- */

static gboolean
is_letter (gunichar c)
{
  return g_unichar_isalpha (c);
}

int
w42_pt_hyphenate (W42PieceTable *pt, W42Hyphenator *hyph)
{
  gsize len, pos;
  GArray *inserts;          /* document positions, ascending */
  int n = 0;

  g_return_val_if_fail (pt != NULL, 0);
  if (hyph == NULL)
    return 0;

  len = w42_pt_length (pt);
  inserts = g_array_new (FALSE, FALSE, sizeof (gsize));

  /* Words are runs of letters; the text has one character per document
   * position except the struxes, which get_text gives as "" or "\n"
   * (a section as nothing, a paragraph as a newline), so positions are
   * tracked by walking the piece table's own characters. */
  pos = 0;
  {
    while (pos < len)
      {
        char *one = w42_pt_get_text (pt, pos, 1);
        gunichar c = (*one != '\0') ? g_utf8_get_char (one) : 0;

        g_free (one);
        if (c != 0 && is_letter (c))
          {
            gsize start = pos;
            GString *word = g_string_new (NULL);
            gboolean already = FALSE;

            while (pos < len)
              {
                char *ch = w42_pt_get_text (pt, pos, 1);
                gunichar d = (*ch != '\0') ? g_utf8_get_char (ch) : 0;

                g_free (ch);
                if (d == SOFT_HYPHEN)
                  {
                    already = TRUE;
                    pos++;
                    continue;
                  }
                if (d == 0 || !is_letter (d))
                  break;
                g_string_append_unichar (word, d);
                pos++;
              }

            if (!already)
              {
                GArray *breaks = g_array_new (FALSE, FALSE, sizeof (int));

                if (break_points (hyph, word->str, breaks) > 0)
                  for (guint i = 0; i < breaks->len; i++)
                    {
                      gsize at = start + (gsize) g_array_index (breaks, int, i);
                      g_array_append_val (inserts, at);
                    }
                g_array_free (breaks, TRUE);
              }
            g_string_free (word, TRUE);
          }
        else
          pos++;
      }
  }

  if (inserts->len > 0)
    {
      char shy[8];
      int bytes = g_unichar_to_utf8 (SOFT_HYPHEN, shy);

      shy[bytes] = '\0';
      w42_pt_begin_group (pt);
      for (guint i = inserts->len; i > 0; i--)
        {
          gsize at = g_array_index (inserts, gsize, i - 1);

          w42_pt_insert_text (pt, at, shy, w42_pt_ap_at (pt, at));
          n++;
        }
      w42_pt_end_group (pt);
    }

  g_array_free (inserts, TRUE);
  return n;
}

int
w42_pt_unhyphenate (W42PieceTable *pt)
{
  gsize len, pos;
  GArray *found;
  int n = 0;

  g_return_val_if_fail (pt != NULL, 0);

  len = w42_pt_length (pt);
  found = g_array_new (FALSE, FALSE, sizeof (gsize));
  for (pos = 0; pos < len; pos++)
    {
      char *one = w42_pt_get_text (pt, pos, 1);

      if (*one != '\0' && g_utf8_get_char (one) == SOFT_HYPHEN)
        g_array_append_val (found, pos);
      g_free (one);
    }

  if (found->len > 0)
    {
      w42_pt_begin_group (pt);
      for (guint i = found->len; i > 0; i--)
        {
          w42_pt_delete (pt, g_array_index (found, gsize, i - 1), 1);
          n++;
        }
      w42_pt_end_group (pt);
    }
  g_array_free (found, TRUE);
  return n;
}
