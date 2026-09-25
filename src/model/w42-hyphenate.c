/* w42-hyphenate.c - see w42-hyphenate.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-hyphenate.h"
#include "w42-lang.h"

#include <string.h>

#ifdef HAVE_HYPHEN
#include <hyphen.h>
#endif

#define SOFT_HYPHEN 0x00AD

struct _W42Hyphenator {
#ifdef HAVE_HYPHEN
  HyphenDict *dict;
  /* Many of the pattern files are not UTF-8 -- the Norwegian ones are
   * ISO8859-1 -- and libhyphen matches bytes, so a word goes to the
   * dictionary in the dictionary's own charset.  (GIConv) -1 when that
   * is UTF-8 already. */
  GIConv      to_dict;
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

/* The converter from UTF-8 to the charset the first line of the pattern
 * file names.  FALSE when the C library cannot convert to it, which makes
 * the dictionary no use to us. */
static gboolean
open_converter (HyphenDict *dict, GIConv *to_dict)
{
  char *cset;

  *to_dict = (GIConv) -1;
  if (dict->utf8)
    return TRUE;

  cset = g_strstrip (g_strdup (dict->cset));
  if (*cset != '\0')
    *to_dict = g_iconv_open (cset, "UTF-8");
  g_free (cset);
  return *to_dict != (GIConv) -1;
}

#endif

#ifdef HAVE_HYPHEN

/* Takes `name`, spelt the way the dictionaries' files are ("nb_NO"). */
static void
add_candidate (GPtrArray *candidates, char *name)
{
  g_strdelimit (name, "-", '_');
  for (guint i = 0; i < candidates->len; i++)
    if (g_str_equal (g_ptr_array_index (candidates, i), name))
      {
        g_free (name);
        return;
      }
  g_ptr_array_add (candidates, name);
}

/* The names a dictionary for `tag` -- "nb-NO", or the C library's
 * "nb_NO.UTF-8" -- may be filed under: the tag and its language on its
 * own, then the same for the tag the language table has for it, which
 * is what turns a bare "nb" into nb_NO, and "no" -- Norwegian, which in
 * practice means Bokmål -- into nb_NO and nb. */
static void
add_candidates (GPtrArray *candidates, const char *tag)
{
  char *name = g_strdup (tag);
  char *dot = strchr (name, '.');
  const char *usual;

  if (dot != NULL)
    *dot = '\0';
  usual = w42_lang_normalise (name);
  for (int pass = 0; pass < 2; pass++)
    {
      char *sep;

      if (strlen (name) >= 2 && !g_str_equal (name, "C") &&
          !g_str_equal (name, "POSIX"))
        {
          add_candidate (candidates, g_strdup (name));
          if ((sep = strpbrk (name, "-_")) != NULL)
            add_candidate (candidates, g_strndup (name, (gsize) (sep - name)));
        }
      g_free (name);
      if (pass == 1 || usual == NULL)
        return;
      name = g_strdup (usual);
    }
}

#endif

W42Hyphenator *
w42_hyphenator_new (void)
{
  return w42_hyphenator_new_for (NULL);
}

W42Hyphenator *
w42_hyphenator_new_for (const char *lang)
{
#ifdef HAVE_HYPHEN
  const char * const *langs = g_get_language_names ();
  GPtrArray *candidates = g_ptr_array_new_with_free_func (g_free);
  W42Hyphenator *hyph = NULL;

  /* The document's language; then the desktop's, which is all there was
   * before the document said; then English. */
  if (lang != NULL && g_strcmp0 (lang, W42_LANG_NONE) != 0)
    add_candidates (candidates, lang);
  for (guint i = 0; langs != NULL && langs[i] != NULL; i++)
    add_candidates (candidates, langs[i]);
  add_candidate (candidates, g_strdup ("en_US"));
  add_candidate (candidates, g_strdup ("en_GB"));

  for (guint i = 0; i < candidates->len && hyph == NULL; i++)
    {
      const char *language = g_ptr_array_index (candidates, i);
      char *path = find_dictionary (language);
      HyphenDict *dict;
      GIConv to_dict;

      if (path == NULL)
        continue;
      dict = hnj_hyphen_load (path);
      g_free (path);
      if (dict == NULL)
        continue;
      if (!open_converter (dict, &to_dict))
        {
          hnj_hyphen_free (dict);
          continue;
        }

      hyph = g_new0 (W42Hyphenator, 1);
      hyph->dict = dict;
      hyph->to_dict = to_dict;
      hyph->language = g_strdup (language);
    }

  g_ptr_array_free (candidates, TRUE);
  return hyph;
#else
  (void) lang;
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
  if (hyph->to_dict != (GIConv) -1)
    g_iconv_close (hyph->to_dict);
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
  glong n_chars = g_utf8_strlen (word, -1);
  gsize bytes;
  char *hyphens;
  char **rep = NULL;
  int *pos = NULL, *cut = NULL;
  char *lower;
  int rc;

  if (n_chars < 5)
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

  if (hyph->to_dict != (GIConv) -1)
    {
      /* A word with a letter the dictionary's charset has no byte for
       * cannot match its patterns, and is left alone.  The charsets the
       * pattern files use have one byte to a character, so the bytes
       * number the characters; one that does not is not ours to map. */
      gsize written = 0;
      char *converted = g_convert_with_iconv (lower, (gssize) bytes,
                                              hyph->to_dict, NULL, &written,
                                              NULL);

      g_free (lower);
      if (converted == NULL || written != (gsize) n_chars)
        {
          g_free (converted);
          return -1;
        }
      lower = converted;
      bytes = written;
    }
  hyphens = g_malloc0 (bytes + 5);

  rc = hnj_hyphen_hyphenate2 (hyph->dict, lower, (int) bytes, hyphens, NULL,
                              &rep, &pos, &cut);

  /* hyphens[] holds a digit per character, odd for a break after it:
   * libhyphen folds a UTF-8 dictionary's bytes into characters itself,
   * and in a single-byte charset they are the same thing.  Anything else
   * means the two have come apart, and no break is safer than a wrong
   * one. */
  if (rc == 0 && strlen (hyphens) == (gsize) n_chars)
    {
      /* Keep Word's two-before, three-after minimum.  A break that
       * changes the letters around it ("Schiff=fahrt") is not one a soft
       * hyphen can make. */
      for (int ci = 0; ci < (int) n_chars; ci++)
        {
          gboolean odd = (hyphens[ci] & 1) != 0;
          gboolean standard = rep == NULL || rep[ci] == NULL;

          if (odd && standard && ci + 1 >= 2 && n_chars - (ci + 1) >= 3)
            {
              int after = ci + 1;
              g_array_append_val (out, after);
            }
        }
    }

  if (rep != NULL)
    {
      for (gsize i = 0; i < bytes; i++)
        free (rep[i]);
      free (rep);
    }
  free (pos);
  free (cut);
  g_free (hyphens);
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
