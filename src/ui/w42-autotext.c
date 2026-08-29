/* w42-autotext.c - see w42-autotext.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-autotext.h"

#include "w42-settings.h"

#include <string.h>

#define NAMES "autotext-names"
#define TEXTS "autotext-texts"

/* The two lists are parallel, and are kept that way: a name at index i
 * has its text at index i.  They are two lists rather than one key per
 * entry because a name is the user's to choose and a key file's keys
 * are not. */
static void
load (char ***names, char ***texts)
{
  guint n, t;

  *names = w42_settings_get_strv (NAMES);
  *texts = w42_settings_get_strv (TEXTS);
  if (*names == NULL)
    *names = g_new0 (char *, 1);
  if (*texts == NULL)
    *texts = g_new0 (char *, 1);

  /* A settings file edited by hand may have lost one of them. */
  n = g_strv_length (*names);
  t = g_strv_length (*texts);
  if (t < n)
    {
      char **grown = g_new0 (char *, n + 1);

      for (guint i = 0; i < n; i++)
        grown[i] = i < t ? (*texts)[i] : g_strdup ("");
      g_free (*texts);          /* the strings moved over; the array did not */
      *texts = grown;
    }
}

char **
w42_autotext_names (void)
{
  char **names, **texts;

  load (&names, &texts);
  g_strfreev (texts);
  return names;
}

char *
w42_autotext_get (const char *name)
{
  char **names, **texts;
  char *found = NULL;

  if (name == NULL || *name == '\0')
    return NULL;

  load (&names, &texts);
  for (guint i = 0; names[i] != NULL; i++)
    if (g_ascii_strcasecmp (names[i], name) == 0)
      {
        found = g_strdup (texts[i] != NULL ? texts[i] : "");
        break;
      }
  g_strfreev (names);
  g_strfreev (texts);
  return found;
}

void
w42_autotext_set (const char *name, const char *text)
{
  char **names, **texts;
  GPtrArray *keep_names, *keep_texts;
  gboolean replaced = FALSE;

  if (name == NULL || *name == '\0' || text == NULL || *text == '\0')
    return;

  load (&names, &texts);
  keep_names = g_ptr_array_new ();
  keep_texts = g_ptr_array_new ();
  for (guint i = 0; names[i] != NULL; i++)
    {
      gboolean same = g_ascii_strcasecmp (names[i], name) == 0;

      g_ptr_array_add (keep_names, same ? (char *) name : names[i]);
      g_ptr_array_add (keep_texts, same ? (char *) text
                                        : (texts[i] != NULL ? texts[i] : (char *) ""));
      if (same)
        replaced = TRUE;
    }
  if (!replaced)
    {
      g_ptr_array_add (keep_names, (char *) name);
      g_ptr_array_add (keep_texts, (char *) text);
    }
  g_ptr_array_add (keep_names, NULL);
  g_ptr_array_add (keep_texts, NULL);

  w42_settings_set_strv (NAMES, (const char * const *) keep_names->pdata);
  w42_settings_set_strv (TEXTS, (const char * const *) keep_texts->pdata);

  g_ptr_array_free (keep_names, TRUE);
  g_ptr_array_free (keep_texts, TRUE);
  g_strfreev (names);
  g_strfreev (texts);
}

void
w42_autotext_remove (const char *name)
{
  char **names, **texts;
  GPtrArray *keep_names, *keep_texts;

  if (name == NULL || *name == '\0')
    return;

  load (&names, &texts);
  keep_names = g_ptr_array_new ();
  keep_texts = g_ptr_array_new ();
  for (guint i = 0; names[i] != NULL; i++)
    {
      if (g_ascii_strcasecmp (names[i], name) == 0)
        continue;
      g_ptr_array_add (keep_names, names[i]);
      g_ptr_array_add (keep_texts, texts[i] != NULL ? texts[i] : (char *) "");
    }
  g_ptr_array_add (keep_names, NULL);
  g_ptr_array_add (keep_texts, NULL);

  w42_settings_set_strv (NAMES, (const char * const *) keep_names->pdata);
  w42_settings_set_strv (TEXTS, (const char * const *) keep_texts->pdata);

  g_ptr_array_free (keep_names, TRUE);
  g_ptr_array_free (keep_texts, TRUE);
  g_strfreev (names);
  g_strfreev (texts);
}

char *
w42_autotext_suggest_name (const char *text)
{
  GString *out;
  int words = 0;
  gboolean in_word = FALSE;

  if (text == NULL)
    return NULL;

  out = g_string_new (NULL);
  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isspace (c))
        {
          if (in_word && ++words >= 3)
            break;
          in_word = FALSE;
          g_string_append_c (out, ' ');
          continue;
        }
      in_word = TRUE;
      g_string_append_unichar (out, c);
      if (out->len >= 32)
        break;
    }

  g_strstrip (out->str);
  g_string_set_size (out, strlen (out->str));
  return g_string_free (out, out->len == 0);
}
