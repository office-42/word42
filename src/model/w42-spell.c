/* w42-spell.c - see w42-spell.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-spell.h"

#include <string.h>

#ifdef HAVE_ENCHANT
#include <enchant.h>
#endif

struct _W42Spell {
#ifdef HAVE_ENCHANT
  EnchantBroker *broker;
  EnchantDict   *dict;
#endif
  char          *language;
  GHashTable    *ignored;    /* words ignored this session */
};

/* ---------------------------------------------------------------------- */
/* Words                                                                   */
/* ---------------------------------------------------------------------- */

static gboolean
is_word_char (gunichar c)
{
  return g_unichar_isalpha (c) || g_unichar_ismark (c) || c == 0x00AD;
}

gboolean
w42_spell_next_word (const char *text, gsize len, gsize *start, gsize *end)
{
  const char *stop = text + len;
  const char *p = text + *end;

  while (p < stop)
    {
      const char *word_start;
      gboolean has_digit = FALSE;

      /* Skip to the next letter or digit.  Digits start a token too, so
       * that "3rd" is one token to throw away rather than a "rd" to look
       * up. */
      while (p < stop && !is_word_char (g_utf8_get_char (p)) &&
             !g_unichar_isdigit (g_utf8_get_char (p)))
        p = g_utf8_next_char (p);
      if (p >= stop)
        break;

      word_start = p;

      /* Take letters, and an apostrophe that has a letter on each side, so
       * "don't" is one word and a closing quote is not part of one. */
      while (p < stop)
        {
          gunichar c = g_utf8_get_char (p);

          if (is_word_char (c))
            {
              p = g_utf8_next_char (p);
            }
          else if (g_unichar_isdigit (c))
            {
              has_digit = TRUE;
              p = g_utf8_next_char (p);
            }
          else if ((c == '\'' || c == 0x2019) &&
                   p > word_start && g_utf8_next_char (p) < stop &&
                   is_word_char (g_utf8_get_char (g_utf8_next_char (p))))
            {
              p = g_utf8_next_char (p);
            }
          else
            break;
        }

      if (!has_digit)
        {
          *start = (gsize) (word_start - text);
          *end   = (gsize) (p - text);
          return TRUE;
        }
    }

  *start = *end = len;
  return FALSE;
}

/* ---------------------------------------------------------------------- */
/* The dictionary                                                          */
/* ---------------------------------------------------------------------- */

#ifdef HAVE_ENCHANT

/* The user's language if there is a dictionary for it, else the language
 * without its country, else English. */
static EnchantDict *
open_dictionary (EnchantBroker *broker, char **language)
{
  const char * const *names = g_get_language_names ();
  static const char *fallbacks[] = { "en_US", "en_GB", "en", NULL };

  for (int pass = 0; pass < 2; pass++)
    {
      const char * const *list = pass == 0 ? names : fallbacks;

      for (guint i = 0; list != NULL && list[i] != NULL; i++)
        {
          char *tag = g_strdup (list[i]);
          char *dot = strchr (tag, '.');
          char *at = strchr (tag, '@');

          if (dot != NULL) *dot = '\0';
          if (at != NULL) *at = '\0';

          if (*tag != '\0' && !g_str_equal (tag, "C") &&
              !g_str_equal (tag, "POSIX") &&
              enchant_broker_dict_exists (broker, tag))
            {
              EnchantDict *dict = enchant_broker_request_dict (broker, tag);

              if (dict != NULL)
                {
                  *language = tag;
                  return dict;
                }
            }

          g_free (tag);
        }
    }

  return NULL;
}

#endif

W42Spell *
w42_spell_new (void)
{
#ifdef HAVE_ENCHANT
  W42Spell *spell = g_new0 (W42Spell, 1);

  spell->broker = enchant_broker_init ();
  if (spell->broker == NULL)
    {
      g_free (spell);
      return NULL;
    }

  spell->dict = open_dictionary (spell->broker, &spell->language);
  if (spell->dict == NULL)
    {
      enchant_broker_free (spell->broker);
      g_free (spell);
      return NULL;
    }

  spell->ignored = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  return spell;
#else
  return NULL;
#endif
}

void
w42_spell_free (W42Spell *spell)
{
  if (spell == NULL)
    return;

#ifdef HAVE_ENCHANT
  if (spell->dict != NULL)
    enchant_broker_free_dict (spell->broker, spell->dict);
  if (spell->broker != NULL)
    enchant_broker_free (spell->broker);
#endif

  g_hash_table_destroy (spell->ignored);
  g_free (spell->language);
  g_free (spell);
}

const char *
w42_spell_language (W42Spell *spell)
{
  g_return_val_if_fail (spell != NULL, NULL);
  return spell->language;
}

/* The script the dictionary is written for.  A word in another script is
 * one this dictionary cannot judge: an English dictionary knows nothing
 * about Chinese, Russian or Arabic, and marking every such word wrong
 * would put a red line under a whole page of it. */
static GUnicodeScript
dictionary_script (const char *language)
{
  static const struct { const char *prefix; GUnicodeScript script; } LANGS[] = {
    { "ru", G_UNICODE_SCRIPT_CYRILLIC },  { "uk", G_UNICODE_SCRIPT_CYRILLIC },
    { "be", G_UNICODE_SCRIPT_CYRILLIC },  { "bg", G_UNICODE_SCRIPT_CYRILLIC },
    { "sr", G_UNICODE_SCRIPT_CYRILLIC },  { "mk", G_UNICODE_SCRIPT_CYRILLIC },
    { "el", G_UNICODE_SCRIPT_GREEK },
    { "ar", G_UNICODE_SCRIPT_ARABIC },    { "fa", G_UNICODE_SCRIPT_ARABIC },
    { "ur", G_UNICODE_SCRIPT_ARABIC },    { "ps", G_UNICODE_SCRIPT_ARABIC },
    { "he", G_UNICODE_SCRIPT_HEBREW },    { "yi", G_UNICODE_SCRIPT_HEBREW },
    { "th", G_UNICODE_SCRIPT_THAI },      { "lo", G_UNICODE_SCRIPT_LAO },
    { "hi", G_UNICODE_SCRIPT_DEVANAGARI },{ "mr", G_UNICODE_SCRIPT_DEVANAGARI },
    { "ne", G_UNICODE_SCRIPT_DEVANAGARI },{ "sa", G_UNICODE_SCRIPT_DEVANAGARI },
    { "bn", G_UNICODE_SCRIPT_BENGALI },   { "ta", G_UNICODE_SCRIPT_TAMIL },
    { "te", G_UNICODE_SCRIPT_TELUGU },    { "kn", G_UNICODE_SCRIPT_KANNADA },
    { "gu", G_UNICODE_SCRIPT_GUJARATI },  { "pa", G_UNICODE_SCRIPT_GURMUKHI },
    { "ml", G_UNICODE_SCRIPT_MALAYALAM }, { "si", G_UNICODE_SCRIPT_SINHALA },
    { "am", G_UNICODE_SCRIPT_ETHIOPIC },  { "ti", G_UNICODE_SCRIPT_ETHIOPIC },
    { "ka", G_UNICODE_SCRIPT_GEORGIAN },  { "hy", G_UNICODE_SCRIPT_ARMENIAN },
    { "km", G_UNICODE_SCRIPT_KHMER },     { "my", G_UNICODE_SCRIPT_MYANMAR },
    { "zh", G_UNICODE_SCRIPT_HAN },       { "ja", G_UNICODE_SCRIPT_HIRAGANA },
    { "ko", G_UNICODE_SCRIPT_HANGUL }
  };

  if (language != NULL)
    for (guint i = 0; i < G_N_ELEMENTS (LANGS); i++)
      if (g_ascii_strncasecmp (language, LANGS[i].prefix, 2) == 0)
        return LANGS[i].script;
  return G_UNICODE_SCRIPT_LATIN;
}

/* The script a word is written in: the first character that belongs to a
 * script of its own.  Marks and punctuation take the word's script, so
 * "don't" and an accented letter do not count as a script change. */
static GUnicodeScript
word_script (const char *word, gsize len)
{
  const char *stop = word + len;

  for (const char *q = word; q < stop; q = g_utf8_next_char (q))
    {
      GUnicodeScript script = g_unichar_get_script (g_utf8_get_char (q));

      if (script != G_UNICODE_SCRIPT_COMMON && script != G_UNICODE_SCRIPT_INHERITED &&
          script != G_UNICODE_SCRIPT_UNKNOWN)
        return script;
    }
  return G_UNICODE_SCRIPT_COMMON;
}

/* Japanese is written in three scripts at once, and Chinese shares one
 * of them, so a dictionary for either takes all three. */
static gboolean
script_fits (GUnicodeScript word, GUnicodeScript dict)
{
  if (word == dict || word == G_UNICODE_SCRIPT_COMMON)
    return TRUE;
  if (dict == G_UNICODE_SCRIPT_HIRAGANA || dict == G_UNICODE_SCRIPT_HAN)
    return word == G_UNICODE_SCRIPT_HIRAGANA || word == G_UNICODE_SCRIPT_KATAKANA ||
           word == G_UNICODE_SCRIPT_HAN;
  if (dict == G_UNICODE_SCRIPT_HANGUL)
    return word == G_UNICODE_SCRIPT_HANGUL || word == G_UNICODE_SCRIPT_HAN;
  return FALSE;
}

gboolean
w42_spell_check (W42Spell *spell, const char *word, gssize len)
{
  char *copy;
  gboolean ok = TRUE;

  g_return_val_if_fail (spell != NULL, TRUE);
  g_return_val_if_fail (word != NULL, TRUE);

  if (len < 0)
    len = (gssize) strlen (word);
  if (len == 0)
    return TRUE;

  copy = g_strndup (word, (gsize) len);

  /* Soft hyphens from Tools > Hyphenation are not letters. */
  if (strstr (copy, "\302\255") != NULL)
    {
      GString *plain = g_string_new (NULL);

      for (const char *p = copy; *p != '\0'; p = g_utf8_next_char (p))
        if (g_utf8_get_char (p) != 0x00AD)
          g_string_append_unichar (plain, g_utf8_get_char (p));
      g_free (copy);
      copy = g_string_free (plain, FALSE);
      len = (gssize) strlen (copy);
    }

  if (g_hash_table_contains (spell->ignored, copy))
    ok = TRUE;
  else if (!script_fits (word_script (copy, (gsize) len), dictionary_script (spell->language)))
    ok = TRUE;   /* another script: not this dictionary's to judge */
#ifdef HAVE_ENCHANT
  else
    ok = enchant_dict_check (spell->dict, copy, len) == 0;
#endif

  g_free (copy);
  return ok;
}

char **
w42_spell_suggest (W42Spell *spell, const char *word, gssize len)
{
#ifdef HAVE_ENCHANT
  size_t n = 0;
  char **found;
  char **out;

  g_return_val_if_fail (spell != NULL, NULL);
  g_return_val_if_fail (word != NULL, NULL);

  if (len < 0)
    len = (gssize) strlen (word);

  found = enchant_dict_suggest (spell->dict, word, len, &n);
  if (found == NULL || n == 0)
    {
      if (found != NULL)
        enchant_dict_free_string_list (spell->dict, found);
      return NULL;
    }

  out = g_new0 (char *, n + 1);
  for (size_t i = 0; i < n; i++)
    out[i] = g_strdup (found[i]);

  enchant_dict_free_string_list (spell->dict, found);
  return out;
#else
  (void) spell; (void) word; (void) len;
  return NULL;
#endif
}

/* The word without its soft hyphens, which the checker looks past. */
static char *
plain_word (const char *word)
{
  GString *plain = g_string_new (NULL);

  for (const char *p = word; *p != '\0'; p = g_utf8_next_char (p))
    if (g_utf8_get_char (p) != 0x00AD)
      g_string_append_unichar (plain, g_utf8_get_char (p));
  return g_string_free (plain, FALSE);
}

void
w42_spell_ignore (W42Spell *spell, const char *word)
{
  g_return_if_fail (spell != NULL);
  g_return_if_fail (word != NULL);

  g_hash_table_add (spell->ignored, plain_word (word));
}

void
w42_spell_add (W42Spell *spell, const char *word)
{
  g_return_if_fail (spell != NULL);
  g_return_if_fail (word != NULL);

  {
    char *plain = plain_word (word);

#ifdef HAVE_ENCHANT
    enchant_dict_add (spell->dict, plain, -1);
#endif
    /* Belt and braces: Enchant's personal list is read back on the next
     * check, but the session list costs nothing. */
    g_hash_table_add (spell->ignored, plain);
  }
}
