/* w42-lang.c - see w42-lang.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-lang.h"

#include <glib/gi18n.h>
#include <stdlib.h>
#include <string.h>

/* The languages a word processor of this size is likely to be asked
 * for, with the numbers Word's formats use for them.  The numbers are
 * the well-known Windows language identifiers, published by Microsoft
 * and used by every program that reads or writes RTF.  The names are
 * marked for translation; w42_lang_name() gives them translated. */
static const W42Language LANGUAGES[] = {
  /* Translators: the language of text that is not to be spell-checked
   * at all, as Tools > Language lists it. */
  { W42_LANG_NONE, N_("(no proofing)"),          1024 },
  { "en-US", N_("English (United States)"),      1033 },
  { "en-GB", N_("English (United Kingdom)"),     2057 },
  { "en-AU", N_("English (Australia)"),          3081 },
  { "en-CA", N_("English (Canada)"),             4105 },
  { "nb-NO", N_("Norwegian (Bokm\303\245l)"),    1044 },
  { "nn-NO", N_("Norwegian (Nynorsk)"),          2068 },
  { "da-DK", N_("Danish"),                       1030 },
  { "sv-SE", N_("Swedish"),                      1053 },
  { "fi-FI", N_("Finnish"),                      1035 },
  { "is-IS", N_("Icelandic"),                    1039 },
  { "de-DE", N_("German (Germany)"),             1031 },
  { "de-AT", N_("German (Austria)"),             3079 },
  { "de-CH", N_("German (Switzerland)"),         2055 },
  { "fr-FR", N_("French (France)"),              1036 },
  { "fr-CA", N_("French (Canada)"),              3084 },
  { "es-ES", N_("Spanish (Spain)"),              3082 },
  { "es-MX", N_("Spanish (Mexico)"),             2058 },
  { "pt-PT", N_("Portuguese (Portugal)"),        2070 },
  { "pt-BR", N_("Portuguese (Brazil)"),          1046 },
  { "it-IT", N_("Italian"),                      1040 },
  { "nl-NL", N_("Dutch (Netherlands)"),          1043 },
  { "nl-BE", N_("Dutch (Belgium)"),              2067 },
  { "pl-PL", N_("Polish"),                       1045 },
  { "cs-CZ", N_("Czech"),                        1029 },
  { "sk-SK", N_("Slovak"),                       1051 },
  { "hu-HU", N_("Hungarian"),                    1038 },
  { "ro-RO", N_("Romanian"),                     1048 },
  { "bg-BG", N_("Bulgarian"),                    1026 },
  { "el-GR", N_("Greek"),                        1032 },
  { "ru-RU", N_("Russian"),                      1049 },
  { "uk-UA", N_("Ukrainian"),                    1058 },
  { "tr-TR", N_("Turkish"),                      1055 },
  { "he-IL", N_("Hebrew"),                       1037 },
  { "ar-SA", N_("Arabic (Saudi Arabia)"),        1025 },
  { "fa-IR", N_("Persian"),                      1065 },
  { "hi-IN", N_("Hindi"),                        1081 },
  { "th-TH", N_("Thai"),                         1054 },
  { "vi-VN", N_("Vietnamese"),                   1066 },
  { "id-ID", N_("Indonesian"),                   1057 },
  { "ja-JP", N_("Japanese"),                     1041 },
  { "ko-KR", N_("Korean"),                       1042 },
  { "zh-CN", N_("Chinese (Simplified)"),         2052 },
  { "zh-TW", N_("Chinese (Traditional)"),        1028 },
  { "ca-ES", N_("Catalan"),                      1027 },
  { "et-EE", N_("Estonian"),                     1061 },
  { "lv-LV", N_("Latvian"),                      1062 },
  { "lt-LT", N_("Lithuanian"),                   1063 },
  { "sl-SI", N_("Slovenian"),                    1060 },
  { "hr-HR", N_("Croatian"),                     1050 },
  { "sr-RS", N_("Serbian"),                      2074 },
  { "af-ZA", N_("Afrikaans"),                    1078 },
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
  char canon[32];
  gsize n;

  if (tag == NULL || *tag == '\0')
    return 0;

  /* The C library spells a tag "nb_NO".  And "no" is Norwegian, which in
   * practice means Bokmål: it was 1044's own tag before Bokmål and
   * Nynorsk had tags of their own, and a desktop set to no_NO still
   * says it. */
  n = strlen (tag);
  if (n >= sizeof canon)
    return 0;
  for (gsize i = 0; i <= n; i++)
    canon[i] = tag[i] == '_' ? '-' : tag[i];
  if (g_ascii_strncasecmp (canon, "no", 2) == 0 &&
      (canon[2] == '\0' || canon[2] == '-'))
    {
      canon[0] = 'n';
      canon[1] = 'b';
    }

  for (guint i = 0; i < G_N_ELEMENTS (LANGUAGES); i++)
    if (g_ascii_strcasecmp (LANGUAGES[i].tag, canon) == 0)
      return LANGUAGES[i].lcid;

  /* "en" on its own: the first English there is. */
  for (guint i = 0; i < G_N_ELEMENTS (LANGUAGES); i++)
    if (g_ascii_strncasecmp (LANGUAGES[i].tag, canon, n) == 0 &&
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
    return _("(document default)");

  for (guint i = 0; i < G_N_ELEMENTS (LANGUAGES); i++)
    if (g_ascii_strcasecmp (LANGUAGES[i].tag, tag) == 0)
      return _(LANGUAGES[i].name);
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
