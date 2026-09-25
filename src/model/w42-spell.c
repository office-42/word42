/* w42-spell.c - see w42-spell.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-spell.h"
#include "w42-lang.h"

#include <string.h>

#ifdef HAVE_ENCHANT
#include <enchant.h>
#endif

struct _W42Spell {
#ifdef HAVE_ENCHANT
  EnchantBroker *broker;
  EnchantDict   *dict;       /* the default, which w42_spell_check uses */
#endif
  char          *language;   /* the default's BCP-47 tag */
  GHashTable    *ignored;    /* words ignored this session */
  GHashTable    *by_lang;    /* BCP-47 tag -> EnchantDict*, or NULL when
                              * there is no dictionary for it; each holds
                              * a reference of its own */
  guint          serial;     /* bumped when a word is ignored or added,
                              * or the default dictionary changes */
  /* What the dictionary said of each word asked about: a long document
   * asks about the same few thousand words hundreds of thousands of
   * times, and asking Hunspell each time doubled the time the Bible
   * sample took to lay out.  A word in another language is keyed with
   * its tag in front.  Emptied when a word is ignored or added, and
   * when it has grown past any document's vocabulary. */
  GHashTable    *checked;    /* char* -> 1 right, 2 wrong */
};

#define CHECKED_LIMIT 200000

static gboolean
checked_lookup (W42Spell *spell, const char *key, gboolean *ok)
{
  gpointer v = g_hash_table_lookup (spell->checked, key);

  if (v == NULL)
    return FALSE;
  *ok = GPOINTER_TO_INT (v) == 1;
  return TRUE;
}

/* Takes `key`. */
static void
checked_store (W42Spell *spell, char *key, gboolean ok)
{
  if (g_hash_table_size (spell->checked) >= CHECKED_LIMIT)
    g_hash_table_remove_all (spell->checked);
  g_hash_table_insert (spell->checked, key, GINT_TO_POINTER (ok ? 1 : 2));
}

/* ---------------------------------------------------------------------- */
/* Words                                                                   */
/* ---------------------------------------------------------------------- */

static gboolean
is_word_char (gunichar c)
{
  return g_unichar_isalpha (c) || g_unichar_ismark (c) || c == 0x00AD;
}

/* The first `len` bytes of `word` without the soft hyphens Tools >
 * Hyphenation put in, which are not letters and which no dictionary
 * knows: every path to a dictionary goes through here. */
static char *
plain_copy (const char *word, gsize len)
{
  GString *plain;

  if (g_strstr_len (word, (gssize) len, "\302\255") == NULL)
    return g_strndup (word, len);

  plain = g_string_sized_new (len);
  for (const char *p = word; p < word + len; p = g_utf8_next_char (p))
    if (g_utf8_get_char (p) != 0x00AD)
      g_string_append_unichar (plain, g_utf8_get_char (p));
  return g_string_free (plain, FALSE);
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

static void
add_try (GPtrArray *tries, char *name)
{
  for (guint i = 0; i < tries->len; i++)
    if (g_ascii_strcasecmp (g_ptr_array_index (tries, i), name) == 0)
      {
        g_free (name);
        return;
      }
  g_ptr_array_add (tries, name);
}

/* A tag as Enchant spells it: "nb_NO". */
static char *
enchant_spelling (const char *tag)
{
  char *name = g_strdup (tag);

  g_strdelimit (name, "-", '_');
  return name;
}

/* The name Enchant has the dictionary for `lang` under, or NULL.  The tag
 * is tried as it is; then without its country; then with the country
 * its language is usually written in, as the language table has it,
 * which is how "nb" finds nb_NO, and how "no" -- Norwegian, which in
 * practice means Bokmål -- does too; and last with the language's own
 * code for the country, "fr" as fr_FR, for a language the table does
 * not know. */
static char *
dictionary_name (EnchantBroker *broker, const char *lang)
{
  GPtrArray *tries = g_ptr_array_new_with_free_func (g_free);
  char *base = enchant_spelling (lang);
  char *sep = strchr (base, '_');
  char *found = NULL;
  const char *usual;

  add_try (tries, g_strdup (base));
  if (sep != NULL)
    {
      *sep = '\0';
      add_try (tries, g_strdup (base));
    }
  if ((usual = w42_lang_normalise (lang)) != NULL)
    add_try (tries, enchant_spelling (usual));
  if ((usual = w42_lang_normalise (base)) != NULL)
    add_try (tries, enchant_spelling (usual));
  if (strlen (base) >= 2)
    {
      char *upper = g_ascii_strup (base, -1);

      add_try (tries, g_strdup_printf ("%s_%s", base, upper));
      g_free (upper);
    }

  for (guint i = 0; i < tries->len && found == NULL; i++)
    if (enchant_broker_dict_exists (broker, g_ptr_array_index (tries, i)))
      found = g_strdup (g_ptr_array_index (tries, i));

  g_free (base);
  g_ptr_array_free (tries, TRUE);
  return found;
}

/* The desktop's language if there is a dictionary for it, found the way
 * dictionary_name finds one, else English.  `language` receives its tag,
 * spelt the BCP-47 way like every other tag in the program. */
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
          char *name = NULL;

          if (dot != NULL) *dot = '\0';
          if (at != NULL) *at = '\0';

          if (*tag != '\0' && !g_str_equal (tag, "C") &&
              !g_str_equal (tag, "POSIX"))
            name = dictionary_name (broker, tag);
          g_free (tag);

          if (name != NULL)
            {
              EnchantDict *dict = enchant_broker_request_dict (broker, name);

              if (dict != NULL)
                {
                  *language = g_strdelimit (name, "_", '-');
                  return dict;
                }
              g_free (name);
            }
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
  spell->by_lang = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  spell->checked = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
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
  /* The dictionaries other languages opened go back first: they are the
   * broker's, and the broker outlives them by a line.  Enchant hands out
   * one dictionary per name and counts who holds it, so one that is the
   * default's too, or another tag's, is given back once per holder. */
  if (spell->by_lang != NULL)
    {
      GHashTableIter it;
      gpointer key, value;

      g_hash_table_iter_init (&it, spell->by_lang);
      while (g_hash_table_iter_next (&it, &key, &value))
        if (value != NULL)
          enchant_broker_free_dict (spell->broker, value);
    }
  if (spell->dict != NULL)
    enchant_broker_free_dict (spell->broker, spell->dict);
  if (spell->broker != NULL)
    enchant_broker_free (spell->broker);
#endif

  if (spell->by_lang != NULL)
    g_hash_table_destroy (spell->by_lang);
  g_hash_table_destroy (spell->ignored);
  g_hash_table_destroy (spell->checked);
  g_free (spell->language);
  g_free (spell);
}

const char *
w42_spell_language (W42Spell *spell)
{
  g_return_val_if_fail (spell != NULL, NULL);
  return spell->language;
}

gboolean
w42_spell_set_language (W42Spell *spell, const char *lang)
{
  g_return_val_if_fail (spell != NULL, FALSE);

#ifdef HAVE_ENCHANT
  {
    EnchantDict *dict = NULL;
    char *language = NULL;

    if (lang == NULL || *lang == '\0')
      dict = open_dictionary (spell->broker, &language);
    else if (g_strcmp0 (lang, W42_LANG_NONE) != 0)
      {
        char *name = dictionary_name (spell->broker, lang);

        if (name != NULL)
          dict = enchant_broker_request_dict (spell->broker, name);
        g_free (name);
        language = g_strdup (lang);
      }
    if (dict == NULL)
      {
        g_free (language);
        return FALSE;
      }

    /* Enchant gives the same dictionary back for the same name, with one
     * more reference on it. */
    if (dict == spell->dict && g_strcmp0 (language, spell->language) == 0)
      {
        enchant_broker_free_dict (spell->broker, dict);
        g_free (language);
        return TRUE;
      }

    enchant_broker_free_dict (spell->broker, spell->dict);
    spell->dict = dict;
    g_free (spell->language);
    spell->language = language;
    /* The default's verdicts are kept under the bare word, so they are
     * the old dictionary's; and everything already underlined was
     * underlined by it. */
    g_hash_table_remove_all (spell->checked);
    spell->serial++;
    return TRUE;
  }
#else
  (void) lang;
  return FALSE;
#endif
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

/* The dictionary for a tag, opened once and kept.  NULL when there is
 * none, which is remembered too so that the broker is asked once. */
#ifdef HAVE_ENCHANT
static EnchantDict *
dict_for_lang (W42Spell *spell, const char *lang)
{
  gpointer found;
  EnchantDict *dict = NULL;
  char *name;

  if (lang == NULL || *lang == '\0')
    return spell->dict;
  if (g_strcmp0 (lang, spell->language) == 0)
    return spell->dict;

  if (g_hash_table_lookup_extended (spell->by_lang, lang, NULL, &found))
    return found;

  name = dictionary_name (spell->broker, lang);
  if (name != NULL)
    dict = enchant_broker_request_dict (spell->broker, name);
  g_free (name);

  g_hash_table_insert (spell->by_lang, g_strdup (lang), dict);
  return dict;
}
#endif

gboolean
w42_spell_has_language (W42Spell *spell, const char *lang)
{
#ifdef HAVE_ENCHANT
  g_return_val_if_fail (spell != NULL, FALSE);
  if (lang == NULL || g_strcmp0 (lang, W42_LANG_NONE) == 0)
    return FALSE;
  return dict_for_lang (spell, lang) != NULL;
#else
  (void) spell; (void) lang;
  return FALSE;
#endif
}

gboolean
w42_spell_check_lang (W42Spell *spell, const char *lang,
                      const char *word, gssize len)
{
  g_return_val_if_fail (spell != NULL, TRUE);

  /* Not language at all: nothing in it can be misspelt. */
  if (lang != NULL && g_strcmp0 (lang, W42_LANG_NONE) == 0)
    return TRUE;
#ifdef HAVE_ENCHANT
  {
    EnchantDict *dict = dict_for_lang (spell, lang);
    char *copy;
    gboolean ok;

    if (dict == NULL)
      return TRUE;               /* no dictionary for it: not ours to judge */
    if (dict == spell->dict)
      return w42_spell_check (spell, word, len);

    g_return_val_if_fail (word != NULL, TRUE);
    if (len < 0)
      len = (gssize) strlen (word);
    copy = plain_copy (word, (gsize) len);
    len = (gssize) strlen (copy);
    if (len == 0)
      {
        g_free (copy);
        return TRUE;
      }
    {
      char *key = g_strconcat (lang, "\037", copy, NULL);

      if (checked_lookup (spell, key, &ok))
        {
          g_free (key);
          g_free (copy);
          return ok;
        }
      if (g_hash_table_contains (spell->ignored, copy))
        ok = TRUE;
      else if (!script_fits (word_script (copy, (gsize) len), dictionary_script (lang)))
        ok = TRUE;
      else
        ok = enchant_dict_check (dict, copy, len) == 0;
      checked_store (spell, key, ok);
    }
    g_free (copy);
    return ok;
  }
#else
  return w42_spell_check (spell, word, len);
#endif
}

char **
w42_spell_suggest_lang (W42Spell *spell, const char *lang,
                        const char *word, gssize len)
{
#ifdef HAVE_ENCHANT
  EnchantDict *dict;
  size_t n = 0;
  char **found, **out;
  char *plain;

  g_return_val_if_fail (spell != NULL, NULL);
  g_return_val_if_fail (word != NULL, NULL);

  if (lang != NULL && g_strcmp0 (lang, W42_LANG_NONE) == 0)
    return NULL;
  dict = dict_for_lang (spell, lang);
  if (dict == NULL || dict == spell->dict)
    return w42_spell_suggest (spell, word, len);

  if (len < 0)
    len = (gssize) strlen (word);
  plain = plain_copy (word, (gsize) len);
  found = *plain != '\0' ? enchant_dict_suggest (dict, plain, -1, &n) : NULL;
  g_free (plain);
  if (found == NULL || n == 0)
    {
      if (found != NULL)
        enchant_dict_free_string_list (dict, found);
      return NULL;
    }
  out = g_new0 (char *, n + 1);
  for (size_t i = 0; i < n; i++)
    out[i] = g_strdup (found[i]);
  enchant_dict_free_string_list (dict, found);
  return out;
#else
  (void) lang;
  return w42_spell_suggest (spell, word, len);
#endif
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
  copy = plain_copy (word, (gsize) len);
  len = (gssize) strlen (copy);
  if (len == 0)
    {
      g_free (copy);
      return TRUE;
    }

  if (checked_lookup (spell, copy, &ok))
    {
      g_free (copy);
      return ok;
    }
  if (g_hash_table_contains (spell->ignored, copy))
    ok = TRUE;
  else if (!script_fits (word_script (copy, (gsize) len), dictionary_script (spell->language)))
    ok = TRUE;   /* another script: not this dictionary's to judge */
#ifdef HAVE_ENCHANT
  else
    ok = enchant_dict_check (spell->dict, copy, len) == 0;
#endif

  checked_store (spell, copy, ok);      /* takes the copy */
  return ok;
}

char **
w42_spell_suggest (W42Spell *spell, const char *word, gssize len)
{
#ifdef HAVE_ENCHANT
  size_t n = 0;
  char **found;
  char **out;
  char *plain;

  g_return_val_if_fail (spell != NULL, NULL);
  g_return_val_if_fail (word != NULL, NULL);

  if (len < 0)
    len = (gssize) strlen (word);

  plain = plain_copy (word, (gsize) len);
  found = *plain != '\0'
            ? enchant_dict_suggest (spell->dict, plain, -1, &n) : NULL;
  g_free (plain);
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

void
w42_spell_ignore (W42Spell *spell, const char *word)
{
  g_return_if_fail (spell != NULL);
  g_return_if_fail (word != NULL);

  g_hash_table_add (spell->ignored, plain_copy (word, strlen (word)));
  g_hash_table_remove_all (spell->checked);
  spell->serial++;
}

guint
w42_spell_serial (W42Spell *spell)
{
  g_return_val_if_fail (spell != NULL, 0);
  return spell->serial;
}

void
w42_spell_add (W42Spell *spell, const char *word)
{
  g_return_if_fail (spell != NULL);
  g_return_if_fail (word != NULL);

  {
    char *plain = plain_copy (word, strlen (word));

#ifdef HAVE_ENCHANT
    enchant_dict_add (spell->dict, plain, -1);
#endif
    /* Belt and braces: Enchant's personal list is read back on the next
     * check, but the session list costs nothing. */
    g_hash_table_add (spell->ignored, plain);
    g_hash_table_remove_all (spell->checked);
    spell->serial++;
  }
}
