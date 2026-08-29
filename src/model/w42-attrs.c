/* w42-attrs.c - see w42-attrs.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-attrs.h"

#include <string.h>

struct _W42ApTable {
  GPtrArray  *records;   /* W42Fmt*, index == W42ApIdx */
  GHashTable *index;     /* W42Fmt* -> W42ApIdx + 1 (0 is "absent") */
  W42ApIdx    fallback;
};

void
w42_fmt_init_default (W42Fmt *fmt)
{
  g_return_if_fail (fmt != NULL);

  memset (fmt, 0, sizeof *fmt);

  fmt->ch.family = g_intern_static_string ("Times New Roman");
  fmt->ch.size   = 20;          /* 10pt, Word 6's default */
  fmt->ch.color  = 0x000000;
  fmt->ch.script = 0;

  fmt->pa.style        = g_intern_static_string ("Normal");
  fmt->pa.align        = W42_ALIGN_LEFT;
  fmt->pa.line_spacing = 0;     /* single */
  fmt->pa.widow_control = 1;
}

static guint
fmt_hash (gconstpointer key)
{
  const guint8 *bytes = key;
  guint hash = 5381;

  for (gsize i = 0; i < sizeof (W42Fmt); i++)
    hash = (hash << 5) + hash + bytes[i];

  return hash;
}

static gboolean
fmt_equal (gconstpointer a, gconstpointer b)
{
  return memcmp (a, b, sizeof (W42Fmt)) == 0;
}

W42ApTable *
w42_ap_table_new (void)
{
  W42ApTable *table = g_new0 (W42ApTable, 1);
  W42Fmt fallback;

  table->records = g_ptr_array_new_with_free_func (g_free);
  table->index   = g_hash_table_new (fmt_hash, fmt_equal);

  w42_fmt_init_default (&fallback);
  table->fallback = w42_ap_table_intern (table, &fallback);

  return table;
}

void
w42_ap_table_free (W42ApTable *table)
{
  if (table == NULL)
    return;

  g_hash_table_destroy (table->index);
  g_ptr_array_free (table->records, TRUE);
  g_free (table);
}

W42ApIdx
w42_ap_table_intern (W42ApTable *table, const W42Fmt *fmt)
{
  gpointer found;
  W42Fmt *copy;

  g_return_val_if_fail (table != NULL, 0);
  g_return_val_if_fail (fmt != NULL, 0);

  found = g_hash_table_lookup (table->index, fmt);
  if (found != NULL)
    return (W42ApIdx) (GPOINTER_TO_UINT (found) - 1);

  copy = g_memdup2 (fmt, sizeof *fmt);
  g_ptr_array_add (table->records, copy);

  /* The key is the stored record itself, so it stays valid for the life of
   * the table and needs no separate free func. */
  g_hash_table_insert (table->index, copy,
                       GUINT_TO_POINTER (table->records->len));

  return (W42ApIdx) (table->records->len - 1);
}

const W42Fmt *
w42_ap_table_get (W42ApTable *table, W42ApIdx idx)
{
  g_return_val_if_fail (table != NULL, NULL);

  if (idx >= table->records->len)
    idx = table->fallback;

  return g_ptr_array_index (table->records, idx);
}

W42ApIdx
w42_ap_table_default (W42ApTable *table)
{
  g_return_val_if_fail (table != NULL, 0);
  return table->fallback;
}

/* ---------------------------------------------------------------------- */
/* Tab stops                                                               */
/* ---------------------------------------------------------------------- */

void
w42_para_fmt_clear_tab (W42ParaFmt *pa, int pos)
{
  g_return_if_fail (pa != NULL);

  for (int i = 0; i < pa->n_tabs; i++)
    {
      if (pa->tab_pos[i] != pos)
        continue;

      for (int j = i; j + 1 < pa->n_tabs; j++)
        {
          pa->tab_pos[j]  = pa->tab_pos[j + 1];
          pa->tab_kind[j] = pa->tab_kind[j + 1];
        }
      pa->n_tabs--;
      pa->tab_pos[pa->n_tabs]  = 0;
      pa->tab_kind[pa->n_tabs] = 0;
      return;
    }
}

void
w42_para_fmt_set_tab_leader (W42ParaFmt *pa, int pos, W42TabKind kind,
                             W42TabLeader leader)
{
  int at;

  g_return_if_fail (pa != NULL);

  if (pos <= 0)
    return;

  w42_para_fmt_clear_tab (pa, pos);
  if (pa->n_tabs >= W42_MAX_TABS)
    return;

  /* Kept in order, so the layout can hand them to Pango as they are. */
  at = pa->n_tabs;
  while (at > 0 && pa->tab_pos[at - 1] > pos)
    {
      pa->tab_pos[at]  = pa->tab_pos[at - 1];
      pa->tab_kind[at] = pa->tab_kind[at - 1];
      at--;
    }
  pa->tab_pos[at]  = pos;
  pa->tab_kind[at] = W42_TAB_BYTE (kind, leader);
  pa->n_tabs++;
}

void
w42_para_fmt_set_tab (W42ParaFmt *pa, int pos, W42TabKind kind)
{
  w42_para_fmt_set_tab_leader (pa, pos, kind, W42_TAB_LEAD_NONE);
}

/* ---------------------------------------------------------------------- */
/* Highlight colours                                                       */
/* ---------------------------------------------------------------------- */

guint32
w42_highlight_rgb (int index)
{
  static const guint32 table[17] = {
    0xFFFFFF, 0x000000, 0x0000FF, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000,
    0xFFFF00, 0xFFFFFF, 0x000080, 0x008080, 0x008000, 0x800080, 0x800000,
    0x808000, 0x808080, 0xC0C0C0,
  };

  return (index > 0 && index < 17) ? table[index] : 0xFFFF00;
}

/* ---------------------------------------------------------------------- */
/* Roman numerals                                                          */
/* ---------------------------------------------------------------------- */

void
w42_roman_lower (int n, char *out, gsize size)
{
  static const int values[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
  static const char *glyphs[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl",
                                  "x", "ix", "v", "iv", "i" };
  GString *r;

  g_return_if_fail (out != NULL && size > 0);

  if (n <= 0 || n >= 4000)
    {
      g_snprintf (out, size, "%d", n);
      return;
    }
  r = g_string_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (values); i++)
    while (n >= values[i])
      {
        g_string_append (r, glyphs[i]);
        n -= values[i];
      }
  g_strlcpy (out, r->str, size);
  g_string_free (r, TRUE);
}

/* ---------------------------------------------------------------------- */
/* Lists                                                                   */
/* ---------------------------------------------------------------------- */

gboolean
w42_list_is_numbered (W42ListKind kind)
{
  return kind == W42_LIST_NUMBER || kind == W42_LIST_LOWER_LETTER ||
         kind == W42_LIST_UPPER_LETTER || kind == W42_LIST_LOWER_ROMAN ||
         kind == W42_LIST_UPPER_ROMAN;
}

gboolean
w42_list_is_bullet (W42ListKind kind)
{
  return kind == W42_LIST_BULLET || kind == W42_LIST_BULLET_CIRCLE ||
         kind == W42_LIST_BULLET_SQUARE || kind == W42_LIST_BULLET_DASH;
}

void
w42_list_marker (W42ListKind kind, int n, char *out, gsize size)
{
  g_return_if_fail (out != NULL && size > 0);

  n = MAX (n, 1);
  switch (kind)
    {
    case W42_LIST_NUMBER:
      g_snprintf (out, size, "%d.", n);
      break;
    case W42_LIST_LOWER_LETTER:
    case W42_LIST_UPPER_LETTER:
      {
        /* a..z, then aa..zz, as Word does. */
        char base = kind == W42_LIST_LOWER_LETTER ? 'a' : 'A';
        int reps = (n - 1) / 26 + 1;
        char letter = (char) (base + (n - 1) % 26);
        gsize i = 0;

        for (; i < (gsize) reps && i + 2 < size; i++)
          out[i] = letter;
        if (i + 1 < size)
          out[i++] = '.';
        out[i] = '\0';
        break;
      }
    case W42_LIST_LOWER_ROMAN:
    case W42_LIST_UPPER_ROMAN:
      w42_roman_lower (n, out, size);
      if (kind == W42_LIST_UPPER_ROMAN)
        for (char *p = out; *p; p++)
          *p = g_ascii_toupper (*p);
      g_strlcat (out, ".", size);
      break;
    case W42_LIST_BULLET_CIRCLE:
      g_strlcpy (out, "\342\227\246", size);      /* U+25E6 */
      break;
    case W42_LIST_BULLET_SQUARE:
      g_strlcpy (out, "\342\226\252", size);      /* U+25AA */
      break;
    case W42_LIST_BULLET_DASH:
      g_strlcpy (out, "\342\200\223", size);      /* U+2013 */
      break;
    case W42_LIST_BULLET:
    default:
      g_strlcpy (out, "\342\200\242", size);      /* U+2022 */
      break;
    }
}

/* ---------------------------------------------------------------------- */
/* Fields                                                                  */
/* ---------------------------------------------------------------------- */

const char *
w42_field_code (const char *instruction)
{
  static const char *codes[] = { "PAGE", "NUMPAGES", "DATE", "TIME", "FILENAME", "NUMWORDS", NULL };
  char *code;
  const char *found = NULL;

  if (instruction == NULL)
    return NULL;
  code = g_strstrip (g_strdup (instruction));

  /* An index entry: XE, or XE:term when the entry is filed under
   * something other than the words that are marked.  The term is part
   * of the code, so it is kept whole. */
  if (g_ascii_strncasecmp (code, "XE", 2) == 0 &&
      (code[2] == '\0' || code[2] == ':' || g_ascii_isspace (code[2])))
    {
      char *end = strchr (code, ' ');

      if (end != NULL)
        *end = '\0';
      found = g_intern_string (code);
      g_free (code);
      return found;
    }

  for (int i = 0; codes[i] != NULL && found == NULL; i++)
    {
      gsize n = strlen (codes[i]);

      if (g_ascii_strncasecmp (code, codes[i], n) == 0 &&
          (code[n] == '\0' || g_ascii_isspace (code[n]) || code[n] == '\\'))
        found = g_intern_static_string (codes[i]);
    }
  g_free (code);
  return found;
}
