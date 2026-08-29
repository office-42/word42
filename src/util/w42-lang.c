/* w42-lang.c - see w42-lang.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-lang.h"

#include <stdlib.h>
#include <string.h>

/* The languages a word processor of this size is likely to be asked
 * for, with the numbers Word's formats use for them.  The numbers are
 * the well-known Windows language identifiers, published by Microsoft
 * and used by every program that reads or writes RTF. */
static const W42Language LANGUAGES[] = {
  { W42_LANG_NONE, "(no proofing)",              1024 },
  { "en-US", "English (United States)",          1033 },
  { "en-GB", "English (United Kingdom)",         2057 },
  { "en-AU", "English (Australia)",              3081 },
  { "en-CA", "English (Canada)",                 4105 },
  { "nb-NO", "Norwegian (Bokmal)",               1044 },
  { "nn-NO", "Norwegian (Nynorsk)",              2068 },
  { "da-DK", "Danish",                           1030 },
  { "sv-SE", "Swedish",                          1053 },
  { "fi-FI", "Finnish",                          1035 },
  { "is-IS", "Icelandic",                        1039 },
  { "de-DE", "German (Germany)",                 1031 },
  { "de-AT", "German (Austria)",                 3079 },
  { "de-CH", "German (Switzerland)",             2055 },
  { "fr-FR", "French (France)",                  1036 },
  { "fr-CA", "French (Canada)",                  3084 },
  { "es-ES", "Spanish (Spain)",                  3082 },
  { "es-MX", "Spanish (Mexico)",                 2058 },
  { "pt-PT", "Portuguese (Portugal)",            2070 },
  { "pt-BR", "Portuguese (Brazil)",              1046 },
  { "it-IT", "Italian",                          1040 },
  { "nl-NL", "Dutch (Netherlands)",              1043 },
  { "nl-BE", "Dutch (Belgium)",                  2067 },
  { "pl-PL", "Polish",                           1045 },
  { "cs-CZ", "Czech",                            1029 },
  { "sk-SK", "Slovak",                           1051 },
  { "hu-HU", "Hungarian",                        1038 },
  { "ro-RO", "Romanian",                         1048 },
  { "bg-BG", "Bulgarian",                        1026 },
  { "el-GR", "Greek",                            1032 },
  { "ru-RU", "Russian",                          1049 },
  { "uk-UA", "Ukrainian",                        1058 },
  { "tr-TR", "Turkish",                          1055 },
  { "he-IL", "Hebrew",                           1037 },
  { "ar-SA", "Arabic (Saudi Arabia)",            1025 },
  { "fa-IR", "Persian",                          1065 },
  { "hi-IN", "Hindi",                            1081 },
  { "th-TH", "Thai",                             1054 },
  { "vi-VN", "Vietnamese",                       1066 },
  { "id-ID", "Indonesian",                       1057 },
  { "ja-JP", "Japanese",                         1041 },
  { "ko-KR", "Korean",                           1042 },
  { "zh-CN", "Chinese (Simplified)",             2052 },
  { "zh-TW", "Chinese (Traditional)",            1028 },
  { "ca-ES", "Catalan",                          1027 },
  { "et-EE", "Estonian",                         1061 },
  { "lv-LV", "Latvian",                          1062 },
  { "lt-LT", "Lithuanian",                       1063 },
  { "sl-SI", "Slovenian",                        1060 },
  { "hr-HR", "Croatian",                         1050 },
  { "sr-RS", "Serbian",                          2074 },
  { "af-ZA", "Afrikaans",                        1078 },
};

const W42Language *
w42_languages (int *n)
{
  if (n != NULL)
    *n = (int) G_N_ELEMENTS (LANGUAGES);
  return LANGUAGES;
}

const char *
w42_lang_from_lcid (int lcid)
{
  for (guint i = 0; i < G_N_ELEMENTS (LANGUAGES); i++)
    if (LANGUAGES[i].lcid == lcid)
      return g_intern_static_string (LANGUAGES[i].tag);
  return NULL;
}

int
w42_lang_to_lcid (const char *tag)
{
  gsize n;

  if (tag == NULL || *tag == '\0')
    return 0;

  for (guint i = 0; i < G_N_ELEMENTS (LANGUAGES); i++)
    if (g_ascii_strcasecmp (LANGUAGES[i].tag, tag) == 0)
      return LANGUAGES[i].lcid;

  /* "en" on its own: the first English there is. */
  n = strlen (tag);
  for (guint i = 0; i < G_N_ELEMENTS (LANGUAGES); i++)
    if (g_ascii_strncasecmp (LANGUAGES[i].tag, tag, n) == 0 &&
        LANGUAGES[i].tag[n] == '-')
      return LANGUAGES[i].lcid;

  return 0;
}

const char *
w42_lang_normalise (const char *tag)
{
  int lcid = w42_lang_to_lcid (tag);

  return lcid != 0 ? w42_lang_from_lcid (lcid) : NULL;
}

const char *
w42_lang_name (const char *tag)
{
  if (tag == NULL)
    return "(document default)";

  for (guint i = 0; i < G_N_ELEMENTS (LANGUAGES); i++)
    if (g_ascii_strcasecmp (LANGUAGES[i].tag, tag) == 0)
      return LANGUAGES[i].name;
  return tag;
}

const char *
w42_lang_default (void)
{
  static const char *cached;

  if (cached == NULL)
    {
      const char * const *names = g_get_language_names ();
      const char *pick = "en-US";

      /* g_get_language_names() gives "nb_NO.UTF-8", "nb_NO", "nb", "C":
       * the first that is one of ours, spelt the way we spell it. */
      for (int i = 0; names != NULL && names[i] != NULL; i++)
        {
          char tag[16];
          gsize j = 0;

          for (const char *p = names[i]; *p != '\0' && j + 1 < sizeof tag; p++)
            {
              if (*p == '.' || *p == '@')
                break;
              tag[j++] = *p == '_' ? '-' : *p;
            }
          tag[j] = '\0';
          if (j == 0 || g_ascii_strcasecmp (tag, "C") == 0)
            continue;
          if (w42_lang_to_lcid (tag) != 0)
            {
              /* The table's own spelling, so that tags compare equal. */
              for (guint k = 0; k < G_N_ELEMENTS (LANGUAGES); k++)
                if (LANGUAGES[k].lcid == w42_lang_to_lcid (tag))
                  {
                    pick = LANGUAGES[k].tag;
                    break;
                  }
              break;
            }
        }
      cached = g_intern_string (pick);
    }
  return cached;
}
