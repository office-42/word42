/* w42-autocorrect.c - see w42-autocorrect.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-autocorrect.h"

#include <string.h>

/* The misspellings Word 97 shipped with, near enough: the ones a hand
 * makes rather than a head.  Pairs of wrong and right. */
static const char *const REPLACEMENTS[] = {
  "teh",      "the",
  "adn",      "and",
  "taht",     "that",
  "thier",    "their",
  "recieve",  "receive",
  "seperate", "separate",
  "occured",  "occurred",
  "definately", "definitely",
  "wich",     "which",
  "wiht",     "with",
  "dont",     "don't",
  "cant",     "can't",
  "isnt",     "isn't",
  "wasnt",    "wasn't",
  "youre",    "you're",
  "(c)",      "\302\251",
  "(r)",      "\302\256",
  "(tm)",     "\342\204\242",
  "...",      "\342\200\246",
  NULL, NULL
};

const char *const *
w42_autocorrect_replacements (void)
{
  return REPLACEMENTS;
}

/* What each language sets, by the language part of its tag; the first
 * entry is English's and the one a language not listed gets.  A novel in
 * the Scandinavian languages opens a line of dialogue with an en dash,
 * one in French, Spanish, Italian or Russian with an em dash. */
static const struct {
  const char   *lang;     /* "nb", or "de-CH" where a country differs */
  W42Typography t;
} TYPOGRAPHY[] = {
  { "en",    { 0x201C, 0x201D, 0x2018, 0x2019, 0,      FALSE } },
  { "nb",    { 0x00AB, 0x00BB, 0x2018, 0x2019, 0x2013, TRUE  } },
  { "nn",    { 0x00AB, 0x00BB, 0x2018, 0x2019, 0x2013, TRUE  } },
  { "no",    { 0x00AB, 0x00BB, 0x2018, 0x2019, 0x2013, TRUE  } },
  { "da",    { 0x00BB, 0x00AB, 0x203A, 0x2039, 0x2013, TRUE  } },
  { "sv",    { 0x201D, 0x201D, 0x2019, 0x2019, 0x2013, FALSE } },
  { "fi",    { 0x201D, 0x201D, 0x2019, 0x2019, 0x2013, TRUE  } },
  { "is",    { 0x201E, 0x201C, 0x201A, 0x2018, 0,      TRUE  } },
  { "de-CH", { 0x00AB, 0x00BB, 0x2039, 0x203A, 0,      TRUE  } },
  { "de",    { 0x201E, 0x201C, 0x201A, 0x2018, 0,      TRUE  } },
  { "fr",    { 0x00AB, 0x00BB, 0x201C, 0x201D, 0x2014, FALSE } },
  { "es",    { 0x00AB, 0x00BB, 0x201C, 0x201D, 0x2014, FALSE } },
  { "it",    { 0x00AB, 0x00BB, 0x201C, 0x201D, 0x2014, FALSE } },
  { "pt-BR", { 0x201C, 0x201D, 0x2018, 0x2019, 0x2014, FALSE } },
  { "pt",    { 0x00AB, 0x00BB, 0x201C, 0x201D, 0x2014, FALSE } },
  { "ru",    { 0x00AB, 0x00BB, 0x201E, 0x201C, 0x2014, FALSE } },
  { "uk",    { 0x00AB, 0x00BB, 0x201E, 0x201C, 0x2014, FALSE } },
  { "pl",    { 0x201E, 0x201D, 0x00AB, 0x00BB, 0x2014, TRUE  } },
  { "cs",    { 0x201E, 0x201C, 0x201A, 0x2018, 0,      TRUE  } },
  { "sk",    { 0x201E, 0x201C, 0x201A, 0x2018, 0,      TRUE  } },
  { "hu",    { 0x201E, 0x201D, 0x00BB, 0x00AB, 0x2013, TRUE  } },
  { "nl",    { 0x201C, 0x201D, 0x2018, 0x2019, 0,      FALSE } },
};

const W42Typography *
w42_typography_for (const char *lang)
{
  if (lang == NULL || *lang == '\0')
    return &TYPOGRAPHY[0].t;

  for (guint i = 0; i < G_N_ELEMENTS (TYPOGRAPHY); i++)
    {
      const char *want = TYPOGRAPHY[i].lang;
      gsize n = strlen (want);

      /* "de-CH" wants the whole tag; "de" any German. */
      if (g_ascii_strncasecmp (lang, want, n) == 0 &&
          (lang[n] == '\0' || (lang[n] == '-' && strchr (want, '-') == NULL) ||
           lang[n] == '_'))
        return &TYPOGRAPHY[i].t;
    }
  return &TYPOGRAPHY[0].t;
}

/* The abbreviations whose full stop does not end a sentence, without
 * it, by language: "ca. kl. 8" is one sentence.  An initial -- a single
 * letter -- never ends one either. */
static const char *const ABBREV_NORDIC[] = {
  "bl.a", "ca", "dvs", "d.v.s", "el", "e.l", "evt", "ev", "f.eks", "kl",
  "mht", "nr", "osv", "pga", "st", "jf", "jfr", "mv", "m.m", "o.l",
  "t.o.m", "f.o.m", "hhv", "iflg", "inkl", "ekskl", "mill", "mrd", "tlf",
  "ang", "vedr", "adr", "avd", "red", "sml", "gl", "m.a.o", "o.a", "div",
  "dr", "prof", "kap", "fig", "tab", "t.ex", "s.k", "bl", "resp", "etc",
  "vs", NULL
};

static const char *const ABBREV_GERMAN[] = {
  "z.b", "bzw", "usw", "d.h", "ca", "nr", "vgl", "evtl", "ggf", "u.a",
  "str", "dr", "prof", "hr", "fr", "etc", "vs", "s.o", "s.u", "z.t", NULL
};

static const char *const ABBREV_ENGLISH[] = {
  "e.g", "i.e", "mr", "mrs", "ms", "dr", "st", "approx", "cf", "pp",
  "vol", "fig", "jr", "sr", "prof", "inc", "ltd", "etc", "vs", "ca", NULL
};

static const char *const *
abbreviations_for (const char *lang)
{
  if (lang == NULL || g_ascii_strncasecmp (lang, "en", 2) == 0)
    return ABBREV_ENGLISH;
  if (g_ascii_strncasecmp (lang, "de", 2) == 0)
    return ABBREV_GERMAN;
  if (g_ascii_strncasecmp (lang, "nb", 2) == 0 ||
      g_ascii_strncasecmp (lang, "nn", 2) == 0 ||
      g_ascii_strncasecmp (lang, "no", 2) == 0 ||
      g_ascii_strncasecmp (lang, "da", 2) == 0 ||
      g_ascii_strncasecmp (lang, "sv", 2) == 0)
    return ABBREV_NORDIC;
  return NULL;
}

/* Whether the misspellings of REPLACEMENTS -- English words -- apply;
 * the symbols at the end of the list apply everywhere. */
static gboolean
language_is_english (const char *lang)
{
  return lang == NULL || g_ascii_strncasecmp (lang, "en", 2) == 0;
}

/* The character `n` back from the end, or 0. */
static gunichar
char_back (const char *text, gsize n)
{
  const char *p = text + strlen (text);

  for (gsize i = 0; i <= n; i++)
    {
      if (p == text)
        return 0;
      p = g_utf8_prev_char (p);
    }
  return g_utf8_get_char (p);
}

static gboolean
ends_word (gunichar c)
{
  return g_unichar_isspace (c) || c == '.' || c == ',' || c == ';' || c == ':' ||
         c == '!' || c == '?' || c == ')' || c == ']' || c == '"' ||
         c == 0x201D || c == 0x2019;
}

/* The word that ends just before the character just typed: its start in
 * `before` and its length in characters. */
static const char *
word_before (const char *before, gsize typed_len, gsize *n_chars)
{
  const char *end = before + strlen (before);
  const char *p;

  for (gsize i = 0; i < typed_len && end > before; i++)
    end = g_utf8_prev_char (end);

  p = end;
  *n_chars = 0;
  while (p > before)
    {
      const char *prev = g_utf8_prev_char (p);
      gunichar c = g_utf8_get_char (prev);

      /* The typed apostrophe is a curly one by now: "it’s" is a word. */
      if (!g_unichar_isalpha (c) && c != '\'' && c != 0x2019 &&
          c != '(' && c != ')' && c != '.')
        break;
      p = prev;
      (*n_chars)++;
    }
  return p;
}

/* An opening quote or bracket.  English's opening quotes are German's
 * closing ones, so a mark the language closes with never counts. */
static gboolean
is_opening_mark (gunichar c, const W42Typography *t)
{
  if (c == t->open || c == t->open2 || c == '(' || c == '[')
    return TRUE;
  if (c == t->close || c == t->close2)
    return FALSE;
  return c == 0x201C || c == 0x00AB || c == 0x201E || c == 0x2018 ||
         c == 0x201A;
}

/* Whether the full stop at `stop` ends a sentence: not after an ordinal
 * number where the language writes them so ("3. september"), not after
 * an initial, and not after one of the language's abbreviations. */
static gboolean
stop_ends_sentence (const char *before, const char *stop,
                    const W42Typography *t, const char *lang)
{
  const char *start = stop;
  const char *const *abbrev = abbreviations_for (lang);
  gboolean digits = TRUE;
  glong n;
  char *token;
  gboolean ends = TRUE;

  while (start > before)
    {
      const char *prev = g_utf8_prev_char (start);
      gunichar c = g_utf8_get_char (prev);

      if (g_unichar_isspace (c) || is_opening_mark (c, t))
        break;
      if (!g_unichar_isdigit (c))
        digits = FALSE;
      start = prev;
    }
  n = g_utf8_pointer_to_offset (start, stop);
  if (n == 0)
    return TRUE;
  if (digits)
    return !t->ordinal_dot;
  if (n == 1 && g_unichar_isalpha (g_utf8_get_char (start)))
    return FALSE;
  token = g_utf8_strdown (start, stop - start);
  for (guint i = 0; abbrev != NULL && abbrev[i] != NULL && ends; i++)
    if (g_str_equal (token, abbrev[i]))
      ends = FALSE;
  g_free (token);
  return ends;
}

/* Whether the word at `word` begins a sentence: nothing but space, an
 * opening quote or a dialogue dash comes before it, or the end of the
 * sentence before. */
static gboolean
starts_sentence (const char *before, const char *word,
                 const W42Typography *t, const char *lang)
{
  const char *p = word;

  while (p > before)
    {
      const char *prev = g_utf8_prev_char (p);
      gunichar c = g_utf8_get_char (prev);

      if (g_unichar_isspace (c) || is_opening_mark (c, t) ||
          c == 0x2013 || c == 0x2014)
        {
          /* A dash in the middle of a line is a pause, not a start. */
          if ((c == 0x2013 || c == 0x2014) && prev > before)
            {
              const char *q = prev;

              while (q > before && g_unichar_isspace (g_utf8_get_char (g_utf8_prev_char (q))))
                q = g_utf8_prev_char (q);
              if (q > before && !is_opening_mark (g_utf8_get_char (g_utf8_prev_char (q)), t))
                return FALSE;
            }
          p = prev;
          continue;
        }
      if (c == '.')
        return stop_ends_sentence (before, prev, t, lang);
      return c == '!' || c == '?';
    }
  return TRUE;
}

/* Whether an inner quotation is open in `text`: an opening inner mark
 * with no closing one after it.  A single quote after a letter is
 * otherwise an apostrophe, which in German or Danish is not the same
 * character as the closing inner quote. */
static gboolean
inner_quote_open (const char *text, const W42Typography *t)
{
  int depth = 0;

  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == t->open2 && t->open2 != t->close2)
        depth++;
      else if (c == t->close2 && depth > 0)
        depth--;
    }
  return depth > 0;
}

static const char *
intern_unichar (gunichar c, const char *then)
{
  char utf8[8];
  int n = g_unichar_to_utf8 (c, utf8);
  char *both;
  const char *out;

  utf8[n] = '\0';
  both = g_strconcat (utf8, then, NULL);
  out = g_intern_string (both);
  g_free (both);
  return out;
}

W42Correction
w42_autocorrect (const char *before, gunichar typed, const char *lang)
{
  W42Correction none = { 0, NULL };
  const W42Typography *t = w42_typography_for (lang);
  gsize len;
  const char *word;
  gsize n_chars;

  if (before == NULL || *before == '\0')
    return none;
  len = strlen (before);

  /* --- quotes: the one that fits where it stands ---------------------- */
  if (typed == '"' || typed == '\'')
    {
      gunichar prev = char_back (before, 1);
      gboolean opening = prev == 0 || g_unichar_isspace (prev) ||
                         prev == '(' || prev == '[' || prev == '{' ||
                         prev == 0x2014 || prev == 0x2013 ||
                         prev == t->open || prev == t->open2;

      none.back = 1;
      if (typed == '"')
        none.text = intern_unichar (opening ? t->open : t->close, "");
      else if (opening)
        none.text = intern_unichar (t->open2, "");
      else if (t->close2 != 0x2019 && g_unichar_isalpha (prev) &&
               !inner_quote_open (before, t))
        none.text = intern_unichar (0x2019, "");    /* an apostrophe */
      else
        none.text = intern_unichar (t->close2, "");
      return none;
    }

  /* --- an em dash out of two hyphens ---------------------------------- */
  if (typed == '-' && char_back (before, 1) == '-')
    {
      none.back = 2;
      none.text = "\342\200\223";        /* an en dash, as Word made */
      return none;
    }

  /* --- dashes out of hyphens standing alone -------------------------- */
  if (typed == ' ' && char_back (before, 1) == '-')
    {
      gunichar prev = char_back (before, 2);

      /* "- " opening a paragraph: a line of dialogue, where the language
       * writes dialogue so.  Elsewhere it is a list's marker, and left
       * for AutoFormat. */
      if (prev == 0 && t->dialogue != 0)
        {
          none.back = 2;
          none.text = intern_unichar (t->dialogue, " ");
          return none;
        }
      /* "ord - ord": a hyphen between spaces is a dash, as Word made it. */
      if (prev == ' ' && char_back (before, 3) != 0 &&
          !g_unichar_isspace (char_back (before, 3)))
        {
          none.back = 2;
          none.text = "\342\200\223 ";
          return none;
        }
    }

  /* Everything below happens when a word has just ended. */
  if (!ends_word (typed))
    return none;

  word = word_before (before, 1, &n_chars);
  if (n_chars == 0)
    return none;

  /* --- the replacements ------------------------------------------------ */
  {
    for (guint i = 0; REPLACEMENTS[i] != NULL; i += 2)
      {
        gsize wrong_len = strlen (REPLACEMENTS[i]);
        gboolean symbol = !g_ascii_isalpha (REPLACEMENTS[i][0]);

        /* The misspellings are English words; the symbols are anyone's. */
        if (!symbol && !language_is_english (lang))
          continue;
        /* The typed character is last, and may be more than a byte: a
         * closing quote is three. */
        const char *typed_at = g_utf8_prev_char (before + len);
        const char *at;

        if ((gsize) (typed_at - before) < wrong_len)
          continue;
        at = typed_at - wrong_len;
        if (at < word)
          continue;
        if (g_ascii_strncasecmp (at, REPLACEMENTS[i], wrong_len) != 0)
          continue;
        /* It must be the whole word, not the end of a longer one --
         * except a symbol's, since "Han ventet..." ends in dots all the
         * same. */
        if (at > before && (!symbol || REPLACEMENTS[i][0] == '('))
          {
            gunichar prev = g_utf8_get_char (g_utf8_prev_char (at));

            if (g_unichar_isalpha (prev))
              continue;
          }
        {
          /* A word that was typed with a capital keeps it. */
          GString *fixed = g_string_new (NULL);

          if ((g_ascii_isupper (*at) || starts_sentence (before, at, t, lang)) &&
              g_ascii_islower (REPLACEMENTS[i + 1][0]))
            {
              g_string_append_c (fixed, g_ascii_toupper (REPLACEMENTS[i + 1][0]));
              g_string_append (fixed, REPLACEMENTS[i + 1] + 1);
            }
          else
            g_string_append (fixed, REPLACEMENTS[i + 1]);
          g_string_append_unichar (fixed, typed);

          none.back = g_utf8_strlen (REPLACEMENTS[i], -1) + 1;
          none.text = g_intern_string (fixed->str);
          g_string_free (fixed, TRUE);
        }
        return none;
      }
  }

  /* --- TWo INitial CApitals -------------------------------------------- */
  if (n_chars >= 3)
    {
      gunichar a = g_utf8_get_char (word);
      gunichar b = g_utf8_get_char (g_utf8_next_char (word));
      gunichar c = g_utf8_get_char (g_utf8_next_char (g_utf8_next_char (word)));

      if (g_unichar_isupper (a) && g_unichar_isupper (b) && g_unichar_islower (c))
        {
          GString *fixed = g_string_new (NULL);
          const char *p = g_utf8_next_char (word);

          g_string_append_unichar (fixed, a);
          g_string_append_unichar (fixed, g_unichar_tolower (b));
          p = g_utf8_next_char (p);
          g_string_append (fixed, p);          /* the rest, and the typed character */

          none.back = n_chars + 1;
          none.text = g_intern_string (fixed->str);
          g_string_free (fixed, TRUE);
          return none;
        }
    }

  /* --- the first letter of a sentence ---------------------------------- */
  {
    gunichar first = g_utf8_get_char (word);

    if (g_unichar_islower (first))
      {
        if (starts_sentence (before, word, t, lang))
          {
            GString *fixed = g_string_new (NULL);

            g_string_append_unichar (fixed, g_unichar_toupper (first));
            g_string_append (fixed, g_utf8_next_char (word));
            none.back = n_chars + 1;
            none.text = g_intern_string (fixed->str);
            g_string_free (fixed, TRUE);
            return none;
          }
      }
  }

  return none;
}
