/* w42-autocorrect.c - see w42-autocorrect.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-autocorrect.h"

#include <string.h>

/* The misspellings Word 6 shipped with, near enough: the ones a hand
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

      if (!g_unichar_isalpha (c) && c != '\'' && c != '(' && c != ')' && c != '.')
        break;
      p = prev;
      (*n_chars)++;
    }
  return p;
}

/* Whether the word at `word` begins a sentence: nothing but space, or a
 * full stop and space, comes before it. */
static gboolean
starts_sentence (const char *before, const char *word)
{
  const char *p = word;

  while (p > before)
    {
      const char *prev = g_utf8_prev_char (p);
      gunichar c = g_utf8_get_char (prev);

      if (g_unichar_isspace (c))
        {
          p = prev;
          continue;
        }
      return c == '.' || c == '!' || c == '?';
    }
  return TRUE;
}

W42Correction
w42_autocorrect (const char *before, gunichar typed)
{
  W42Correction none = { 0, NULL };
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
                         prev == 0x2014 || prev == 0x2013;

      none.back = 1;
      if (typed == '"')
        none.text = opening ? "\342\200\234" : "\342\200\235";
      else
        none.text = opening ? "\342\200\230" : "\342\200\231";
      return none;
    }

  /* --- an em dash out of two hyphens ---------------------------------- */
  if (typed == '-' && char_back (before, 1) == '-')
    {
      none.back = 2;
      none.text = "\342\200\223";        /* an en dash, as Word made */
      return none;
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
        const char *at = before + len - 1 - wrong_len;   /* the typed character is last */

        if (at < word || at < before)
          continue;
        if (g_ascii_strncasecmp (at, REPLACEMENTS[i], wrong_len) != 0)
          continue;
        /* It must be the whole word, not the end of a longer one. */
        if (at > before)
          {
            gunichar prev = g_utf8_get_char (g_utf8_prev_char (at));

            if (g_unichar_isalpha (prev))
              continue;
          }
        {
          /* A word that was typed with a capital keeps it. */
          GString *fixed = g_string_new (NULL);

          if ((g_ascii_isupper (*at) || starts_sentence (before, at)) &&
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
        if (starts_sentence (before, word))
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
