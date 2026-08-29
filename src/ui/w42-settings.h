/* w42-settings.h - the few settings word42 remembers between runs
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A key file in the user's configuration directory, read once at startup
 * and written whenever a setting changes.  Tools > Options edits them; the
 * View menu's toggles are remembered here too.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  W42_UNITS_INCHES = 0,
  W42_UNITS_CM
} W42Units;

void        w42_settings_load       (void);

W42Units    w42_settings_get_units  (void);
void        w42_settings_set_units  (W42Units units);

/* Twips to and from the user's unit, and how to write the unit. */
double      w42_settings_from_twips (int twips);
int         w42_settings_to_twips   (double value);
const char *w42_settings_unit_name  (void);

gboolean    w42_settings_get_bool   (const char *key, gboolean fallback);
void        w42_settings_set_bool   (const char *key, gboolean value);
int         w42_settings_get_int    (const char *key, int fallback);
void        w42_settings_set_int    (const char *key, int value);
char       *w42_settings_get_string (const char *key, const char *fallback);
char      **w42_settings_get_strv   (const char *key);   /* g_strfreev() */
void        w42_settings_set_strv   (const char *key, const char * const *value);
void        w42_settings_set_string (const char *key, const char *value);

G_END_DECLS
