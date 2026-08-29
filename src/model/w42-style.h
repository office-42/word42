/* w42-style.h - the stylesheet: named formatting, and heading numbers
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A style is a name for a set of formatting.  Word 6 shipped with Normal,
 * three headings and a handful of others, and the Style box at the left of
 * the Formatting toolbar applied them.  Headings carry an outline level,
 * which is what lets the document number its sections: a Heading 2 after
 * the third Heading 1 is section 3.1, and it is the layout engine that says
 * so, not the text.
 */

#pragma once

#include "w42-attrs.h"

G_BEGIN_DECLS

typedef struct {
  const char *name;      /* interned */
  W42CharFmt  ch;
  W42ParaFmt  pa;        /* pa.style is always name */
  int         outline;   /* 0 for body text; 1..9 for a heading of that level */
  guint8      character; /* a character style: only `ch` counts, and it is
                          * applied to a selection rather than a paragraph */
  const char *based_on;  /* interned name of the style this one was made
                          * from, or NULL */
  /* Which settings are the style's own: W42_STYLE_PA_* and W42_STYLE_CH_*
   * bits.  The rest follow the base -- `pa` and `ch` always hold the
   * effective values, recomputed by w42_stylesheet_follow when a base
   * changes. */
  guint32     pa_own;
  guint32     ch_own;
} W42Style;

/* The settings a style can own, one bit each. */
enum {
  W42_STYLE_PA_ALIGN        = 1 << 0,
  W42_STYLE_PA_INDENT_LEFT  = 1 << 1,
  W42_STYLE_PA_INDENT_RIGHT = 1 << 2,
  W42_STYLE_PA_INDENT_FIRST = 1 << 3,
  W42_STYLE_PA_SPACE_BEFORE = 1 << 4,
  W42_STYLE_PA_SPACE_AFTER  = 1 << 5,
  W42_STYLE_PA_LINE_SPACING = 1 << 6,
  W42_STYLE_PA_FLOW         = 1 << 7,
  W42_STYLE_PA_ALL          = (1 << 8) - 1,

  W42_STYLE_CH_FAMILY    = 1 << 0,
  W42_STYLE_CH_SIZE      = 1 << 1,
  W42_STYLE_CH_BOLD      = 1 << 2,
  W42_STYLE_CH_ITALIC    = 1 << 3,
  W42_STYLE_CH_UNDERLINE = 1 << 4,
  W42_STYLE_CH_COLOR     = 1 << 5,
  W42_STYLE_CH_ALL       = (1 << 6) - 1
};

typedef struct _W42StyleSheet W42StyleSheet;

/* A new sheet holds Word 6's defaults: Normal, Heading 1 to 3, Title. */
W42StyleSheet   *w42_stylesheet_new  (void);
void             w42_stylesheet_free (W42StyleSheet *sheet);

guint            w42_stylesheet_size (W42StyleSheet *sheet);
const W42Style  *w42_stylesheet_get  (W42StyleSheet *sheet, guint index);
const W42Style  *w42_stylesheet_find (W42StyleSheet *sheet, const char *name);

/* Adds a style, or replaces the one with the same name.  The stored copy
 * has pa.style set to the name whatever was passed in. */
void             w42_stylesheet_set  (W42StyleSheet *sheet, const W42Style *style);
/* Takes a style out; Normal stays.  FALSE when there is no such style.
 * Styles based on it are based on its base afterwards. */
gboolean         w42_stylesheet_remove (W42StyleSheet *sheet, const char *name);

/* Recomputes `name` from its base and its own settings, then every style
 * based on it, and so on down.  Call after a style has changed. */
void             w42_stylesheet_follow (W42StyleSheet *sheet, const char *name);

/* Which of `style`'s settings differ from its base's: the own-settings
 * masks a style edited to these values should carry. */
void             w42_style_own_from_base (const W42Style *style, const W42Style *base,
                                          guint32 *pa_own, guint32 *ch_own);

/* The names of the styles based (at any depth) on `name`, NULL-ended;
 * free with g_free (the names are interned). */
const char     **w42_stylesheet_descendants (W42StyleSheet *sheet, const char *name);

/* The outline level of a style by name; 0 when the style is unknown. */
int              w42_stylesheet_outline (W42StyleSheet *sheet, const char *name);

/* Whether headings show their section number.  A document-wide switch,
 * since Word 6 had exactly one: Format > Heading Numbering. */
gboolean         w42_stylesheet_get_number_headings (W42StyleSheet *sheet);
void             w42_stylesheet_set_number_headings (W42StyleSheet *sheet, gboolean on);

G_END_DECLS
