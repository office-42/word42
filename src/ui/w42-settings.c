/* w42-settings.c - see w42-settings.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-settings.h"

#include <math.h>

#define GROUP "word42"

static GKeyFile *keyfile = NULL;

static char *
settings_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "word42", "settings.ini", NULL);
}

static void
settings_save (void)
{
  char *path = settings_path ();
  char *dir = g_path_get_dirname (path);

  if (keyfile != NULL)
    {
      g_mkdir_with_parents (dir, 0700);
      g_key_file_save_to_file (keyfile, path, NULL);
    }

  g_free (dir);
  g_free (path);
}

void
w42_settings_load (void)
{
  char *path = settings_path ();

  if (keyfile == NULL)
    keyfile = g_key_file_new ();

  /* A missing file is the ordinary first run; nothing to say about it. */
  g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, NULL);
  g_free (path);
}

static void
ensure (void)
{
  if (keyfile == NULL)
    w42_settings_load ();
}

/* ---- units ------------------------------------------------------------- */

W42Units
w42_settings_get_units (void)
{
  char *units = w42_settings_get_string ("units", "in");
  W42Units result = g_str_equal (units, "cm") ? W42_UNITS_CM : W42_UNITS_INCHES;

  g_free (units);
  return result;
}

void
w42_settings_set_units (W42Units units)
{
  w42_settings_set_string ("units", units == W42_UNITS_CM ? "cm" : "in");
}

/* A centimetre is 567 twips, near enough; an inch is exactly 1440. */
static double
twips_per_unit (void)
{
  return w42_settings_get_units () == W42_UNITS_CM ? 566.929 : 1440.0;
}

double
w42_settings_from_twips (int twips)
{
  return twips / twips_per_unit ();
}

int
w42_settings_to_twips (double value)
{
  return (int) lround (value * twips_per_unit ());
}

const char *
w42_settings_unit_name (void)
{
  return w42_settings_get_units () == W42_UNITS_CM ? "cm" : "\"";
}

/* ---- the rest ---------------------------------------------------------- */

gboolean
w42_settings_get_bool (const char *key, gboolean fallback)
{
  GError *error = NULL;
  gboolean value;

  ensure ();
  value = g_key_file_get_boolean (keyfile, GROUP, key, &error);
  if (error != NULL)
    {
      g_error_free (error);
      return fallback;
    }
  return value;
}

void
w42_settings_set_bool (const char *key, gboolean value)
{
  ensure ();
  g_key_file_set_boolean (keyfile, GROUP, key, value);
  settings_save ();
}

int
w42_settings_get_int (const char *key, int fallback)
{
  GError *error = NULL;
  int value;

  ensure ();
  value = g_key_file_get_integer (keyfile, GROUP, key, &error);
  if (error != NULL)
    {
      g_error_free (error);
      return fallback;
    }
  return value;
}

void
w42_settings_set_int (const char *key, int value)
{
  ensure ();
  g_key_file_set_integer (keyfile, GROUP, key, value);
  settings_save ();
}

char *
w42_settings_get_string (const char *key, const char *fallback)
{
  char *value;

  ensure ();
  value = g_key_file_get_string (keyfile, GROUP, key, NULL);
  return value != NULL ? value : g_strdup (fallback);
}

void
w42_settings_set_string (const char *key, const char *value)
{
  ensure ();
  g_key_file_set_string (keyfile, GROUP, key, value != NULL ? value : "");
  settings_save ();
}

char **
w42_settings_get_strv (const char *key)
{
  char **value;

  ensure ();
  value = g_key_file_get_string_list (keyfile, GROUP, key, NULL, NULL);
  return value != NULL ? value : g_new0 (char *, 1);
}

void
w42_settings_set_strv (const char *key, const char * const *value)
{
  gsize n = 0;

  ensure ();
  while (value != NULL && value[n] != NULL)
    n++;
  g_key_file_set_string_list (keyfile, GROUP, key, value, n);
  settings_save ();
}
