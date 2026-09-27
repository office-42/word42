/* w42-doc-write.c - writing Word 97 .doc files
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The file is what Word 97 wrote: an OLE2 compound file holding a
 * WordDocument stream -- the File Information Block, the text, and the
 * "formatted disk pages" of character and paragraph properties -- and a
 * 1Table stream with the stylesheet, the font table, the section table,
 * the document properties and the piece table.  The text is one piece of
 * UTF-16, so every character Word has survives.
 */

#include "w42-doc.h"

#include <string.h>
#include <glib/gi18n.h>

#include "w42-lang.h"

/* ---------------------------------------------------------------------- */
/* Little-endian writers                                                   */
/* ---------------------------------------------------------------------- */

static void
put8 (GByteArray *out, guint v)
{
  guint8 b = (guint8) v;
  g_byte_array_append (out, &b, 1);
}

static void
put16 (GByteArray *out, guint v)
{
  guint8 b[2] = { (guint8) v, (guint8) (v >> 8) };
  g_byte_array_append (out, b, 2);
}

static void
put32 (GByteArray *out, guint32 v)
{
  guint8 b[4] = { (guint8) v, (guint8) (v >> 8), (guint8) (v >> 16), (guint8) (v >> 24) };
  g_byte_array_append (out, b, 4);
}

static void
set16 (GByteArray *out, gsize at, guint v)
{
  out->data[at] = (guint8) v;
  out->data[at + 1] = (guint8) (v >> 8);
}

static void
set32 (GByteArray *out, gsize at, guint32 v)
{
  for (int i = 0; i < 4; i++)
    out->data[at + i] = (guint8) (v >> (8 * i));
}

static void
pad_to (GByteArray *out, gsize size)
{
  static const guint8 zero[512] = { 0 };

  while (out->len < size)
    g_byte_array_append (out, zero, MIN (size - out->len, sizeof zero));
}

/* ---------------------------------------------------------------------- */
/* OLE2 compound files                                                     */
/* ---------------------------------------------------------------------- */

#define OLE_SECTOR      512
#define OLE_MINI_SECTOR 64
#define OLE_MINI_CUTOFF 4096
#define OLE_FREE        0xFFFFFFFFu
#define OLE_END         0xFFFFFFFEu
#define OLE_FAT_SECTOR  0xFFFFFFFDu
#define OLE_DIF_SECTOR  0xFFFFFFFCu
#define OLE_NO_STREAM   0xFFFFFFFFu

typedef struct {
  const char *name;       /* ASCII, as the streams of a Word file are */
  GByteArray *data;
  guint32     start;      /* its first sector, or mini sector */
} OleStream;

/* The order the directory's tree sorts names in: shorter first, then by
 * the characters upper-cased. */
static int
ole_name_cmp (const char *a, const char *b)
{
  gsize la = strlen (a), lb = strlen (b);

  if (la != lb)
    return la < lb ? -1 : 1;
  for (gsize i = 0; i < la; i++)
    {
      int ca = g_ascii_toupper (a[i]), cb = g_ascii_toupper (b[i]);

      if (ca != cb)
        return ca < cb ? -1 : 1;
    }
  return 0;
}

/* A balanced tree over the sorted entries lo..hi, all black: readers walk
 * the tree and never check its colours. */
static guint32
ole_tree (guint32 *left, guint32 *right, const guint *order, int lo, int hi)
{
  int mid;

  if (lo > hi)
    return OLE_NO_STREAM;
  mid = (lo + hi) / 2;
  left[order[mid]] = ole_tree (left, right, order, lo, mid - 1);
  right[order[mid]] = ole_tree (left, right, order, mid + 1, hi);
  return order[mid] + 1;    /* directory entry 0 is the root */
}

static void
ole_chain (GArray *fat, guint32 first, guint32 count)
{
  for (guint32 i = 0; i < count; i++)
    {
      guint32 next = i + 1 < count ? first + i + 1 : OLE_END;
      g_array_index (fat, guint32, first + i) = next;
    }
}

static GByteArray *
ole_build (OleStream *streams, guint n, const guint8 clsid[16])
{
  GByteArray *mini = g_byte_array_new ();
  GArray *minifat = g_array_new (FALSE, FALSE, sizeof (guint32));
  GArray *fat;
  GByteArray *out;
  guint32 next = 0, mini_start, n_mini, minifat_start, n_minifat, dir_start, n_dir;
  guint32 n_fat = 0, n_dif = 0, used, total;
  guint32 *left = g_new0 (guint32, n), *right = g_new0 (guint32, n);
  guint *order = g_new (guint, n);
  guint32 root_child;

  /* The small streams go in the mini stream, 64 bytes a sector. */
  for (guint i = 0; i < n; i++)
    {
      guint len = streams[i].data->len;

      if (len == 0 || len >= OLE_MINI_CUTOFF)
        continue;
      streams[i].start = mini->len / OLE_MINI_SECTOR;
      g_byte_array_append (mini, streams[i].data->data, len);
      pad_to (mini, (mini->len + OLE_MINI_SECTOR - 1) / OLE_MINI_SECTOR * OLE_MINI_SECTOR);
      for (guint32 s = streams[i].start; s < mini->len / OLE_MINI_SECTOR; s++)
        {
          guint32 e = s + 1 < mini->len / OLE_MINI_SECTOR ? s + 1 : OLE_END;
          g_array_append_val (minifat, e);
        }
    }

  /* The sectors: the big streams, the mini stream, the mini FAT, the
   * directory, then the FAT and the DIFAT that say where they all are. */
  for (guint i = 0; i < n; i++)
    if (streams[i].data->len >= OLE_MINI_CUTOFF)
      {
        streams[i].start = next;
        next += (streams[i].data->len + OLE_SECTOR - 1) / OLE_SECTOR;
      }
  mini_start = next;
  n_mini = (mini->len + OLE_SECTOR - 1) / OLE_SECTOR;
  next += n_mini;
  minifat_start = next;
  n_minifat = (minifat->len * 4 + OLE_SECTOR - 1) / OLE_SECTOR;
  next += n_minifat;
  dir_start = next;
  n_dir = ((n + 1) * 128 + OLE_SECTOR - 1) / OLE_SECTOR;
  next += n_dir;
  used = next;

  for (;;)
    {
      guint32 fat_need = (used + n_fat + n_dif + 127) / 128;
      guint32 dif_need = fat_need > 109 ? (fat_need - 109 + 126) / 127 : 0;

      if (fat_need == n_fat && dif_need == n_dif)
        break;
      n_fat = fat_need;
      n_dif = dif_need;
    }
  total = used + n_fat + n_dif;

  fat = g_array_new (FALSE, FALSE, sizeof (guint32));
  g_array_set_size (fat, n_fat * 128);
  for (guint i = 0; i < fat->len; i++)
    g_array_index (fat, guint32, i) = OLE_FREE;
  for (guint i = 0; i < n; i++)
    if (streams[i].data->len >= OLE_MINI_CUTOFF)
      ole_chain (fat, streams[i].start, (streams[i].data->len + OLE_SECTOR - 1) / OLE_SECTOR);
  ole_chain (fat, mini_start, n_mini);
  ole_chain (fat, minifat_start, n_minifat);
  ole_chain (fat, dir_start, n_dir);
  for (guint32 i = 0; i < n_fat; i++)
    g_array_index (fat, guint32, used + i) = OLE_FAT_SECTOR;
  for (guint32 i = 0; i < n_dif; i++)
    g_array_index (fat, guint32, used + n_fat + i) = OLE_DIF_SECTOR;

  /* The header. */
  out = g_byte_array_sized_new (OLE_SECTOR * (total + 1));
  {
    static const guint8 magic[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };

    g_byte_array_append (out, magic, 8);
    pad_to (out, 0x18);
    put16 (out, 0x003E);             /* minor version */
    put16 (out, 0x0003);             /* major version: 512-byte sectors */
    put16 (out, 0xFFFE);             /* little-endian */
    put16 (out, 9);                  /* sector shift */
    put16 (out, 6);                  /* mini sector shift */
    pad_to (out, 0x2C);
    put32 (out, n_fat);
    put32 (out, dir_start);
    put32 (out, 0);                  /* transaction signature */
    put32 (out, OLE_MINI_CUTOFF);
    put32 (out, n_minifat > 0 ? minifat_start : OLE_END);
    put32 (out, n_minifat);
    put32 (out, n_dif > 0 ? used + n_fat : OLE_END);
    put32 (out, n_dif);
    for (guint32 i = 0; i < 109; i++)
      put32 (out, i < n_fat ? used + i : OLE_FREE);
  }

  /* The streams' sectors. */
  for (guint i = 0; i < n; i++)
    if (streams[i].data->len >= OLE_MINI_CUTOFF)
      {
        g_byte_array_append (out, streams[i].data->data, streams[i].data->len);
        pad_to (out, (out->len + OLE_SECTOR - 1) / OLE_SECTOR * OLE_SECTOR);
      }
  g_byte_array_append (out, mini->data, mini->len);
  pad_to (out, (out->len + OLE_SECTOR - 1) / OLE_SECTOR * OLE_SECTOR);
  for (guint i = 0; i < minifat->len; i++)
    put32 (out, g_array_index (minifat, guint32, i));
  while (out->len % OLE_SECTOR != 0)
    put32 (out, OLE_FREE);

  /* The directory: the root, then a stream each, in a tree by name. */
  for (guint i = 0; i < n; i++)
    order[i] = i;
  for (guint i = 1; i < n; i++)
    for (guint j = i; j > 0 && ole_name_cmp (streams[order[j - 1]].name, streams[order[j]].name) > 0; j--)
      {
        guint t = order[j];
        order[j] = order[j - 1];
        order[j - 1] = t;
      }
  root_child = ole_tree (left, right, order, 0, (int) n - 1);

  for (guint e = 0; e < n_dir * (OLE_SECTOR / 128); e++)
    {
      gsize at = out->len;
      const char *name = e == 0 ? "Root Entry" : e <= n ? streams[e - 1].name : NULL;

      pad_to (out, at + 128);
      if (name == NULL)
        {
          set32 (out, at + 0x44, OLE_NO_STREAM);
          set32 (out, at + 0x48, OLE_NO_STREAM);
          set32 (out, at + 0x4C, OLE_NO_STREAM);
          continue;
        }
      for (gsize c = 0; name[c] != '\0' && c < 31; c++)
        set16 (out, at + 2 * c, (guchar) name[c]);
      set16 (out, at + 0x40, (guint) (MIN (strlen (name), 31) + 1) * 2);
      out->data[at + 0x42] = e == 0 ? 5 : 2;         /* root, or a stream */
      out->data[at + 0x43] = 1;                      /* black */
      if (e == 0)
        {
          set32 (out, at + 0x44, OLE_NO_STREAM);
          set32 (out, at + 0x48, OLE_NO_STREAM);
          set32 (out, at + 0x4C, root_child);
          memcpy (out->data + at + 0x50, clsid, 16);
          set32 (out, at + 0x74, n_mini > 0 ? mini_start : OLE_END);
          set32 (out, at + 0x78, mini->len);
        }
      else
        {
          const OleStream *s = &streams[e - 1];

          set32 (out, at + 0x44, left[e - 1]);
          set32 (out, at + 0x48, right[e - 1]);
          set32 (out, at + 0x4C, OLE_NO_STREAM);
          set32 (out, at + 0x74, s->data->len > 0 ? s->start : OLE_END);
          set32 (out, at + 0x78, s->data->len);
        }
    }

  /* The FAT, and the DIFAT for the FAT sectors past the header's 109. */
  for (guint i = 0; i < fat->len; i++)
    put32 (out, g_array_index (fat, guint32, i));
  for (guint32 d = 0; d < n_dif; d++)
    {
      for (guint32 k = 0; k < 127; k++)
        {
          guint32 f = 109 + d * 127 + k;
          put32 (out, f < n_fat ? used + f : OLE_FREE);
        }
      put32 (out, d + 1 < n_dif ? used + n_fat + d + 1 : OLE_END);
    }

  g_array_free (fat, TRUE);
  g_array_free (minifat, TRUE);
  g_byte_array_free (mini, TRUE);
  g_free (left);
  g_free (right);
  g_free (order);
  return out;
}

/* ---------------------------------------------------------------------- */
/* The Word file                                                           */
/* ---------------------------------------------------------------------- */

/* The FIB's rgFcLcb entries this writes. */
enum {
  FIB_STSHF_ORIG = 0,
  FIB_STSHF      = 1,
  FIB_PLCF_SED   = 6,
  FIB_BTE_CHPX   = 12,
  FIB_BTE_PAPX   = 13,
  FIB_STTBF_FFN  = 15,
  FIB_DOP        = 31,
  FIB_CLX        = 33,
  FIB_N_FCLCB    = 93      /* Word 97's FibRgFcLcb97 */
};

#define FIB_RGFCLCB 0x9A
#define TEXT_FC     0x800   /* where the text starts in WordDocument */

/* The built-in styles' fixed places, as Word 97 kept them. */
#define ISTD_NORMAL       0
#define ISTD_HEADING1     1
#define ISTD_DEFAULT_FONT 10
#define ISTD_FIRST_USER   15

#define STI_NORMAL       0
#define STI_TITLE        62
#define STI_DEFAULT_FONT 65
#define STI_USER         0xFFE

typedef struct {
  guint32     cp_end;       /* one past the run's last character */
  GByteArray *grpprl;       /* owned; empty for none */
} Run;

typedef struct {
  guint32     cp_end;       /* one past the paragraph mark */
  guint       istd;
  GByteArray *grpprl;       /* owned: the sprms after the istd */
} ParaRun;

typedef struct {
  W42PieceTable     *pt;
  const W42PageSetup *page;
  W42ApTable        *aps;
  W42StyleSheet     *sheet;

  GArray     *text;         /* gunichar2: every story, one after another */
  GArray     *chp;          /* Run */
  GArray     *pap;          /* ParaRun */
  GPtrArray  *fonts;        /* the font table's names, interned */
  const W42Style *istd_style[ISTD_FIRST_USER];   /* the fixed places' styles */
  GPtrArray  *user_styles;  /* W42Style*, from istd 15 on */
  guint32     ccp_text;
} Writer;

static void
run_clear (gpointer data)
{
  g_byte_array_free (((Run *) data)->grpprl, TRUE);
}

static void
para_run_clear (gpointer data)
{
  g_byte_array_free (((ParaRun *) data)->grpprl, TRUE);
}

/* ---- fonts ------------------------------------------------------------ */

static guint
font_index (Writer *w, const char *family)
{
  const char *name = g_intern_string (family != NULL && *family != '\0' ? family : "Times New Roman");

  for (guint i = 0; i < w->fonts->len; i++)
    if (g_ptr_array_index (w->fonts, i) == name)
      return i;
  g_ptr_array_add (w->fonts, (gpointer) name);
  return w->fonts->len - 1;
}

/* SttbfFfn: a count, then an FFN each -- a length, the pitch and family
 * byte, the weight, the character set, the alternate's place, a PANOSE
 * and a font signature, which are left empty, and the name. */
static void
write_fonts (Writer *w, GByteArray *tb)
{
  put16 (tb, w->fonts->len);
  put16 (tb, 0);
  for (guint i = 0; i < w->fonts->len; i++)
    {
      const char *name = g_ptr_array_index (w->fonts, i);
      glong n16 = 0;
      gunichar2 *u = g_utf8_to_utf16 (name, -1, NULL, &n16, NULL);

      n16 = MIN (n16, 31);
      put8 (tb, 39 + 2 * (guint) (n16 + 1));   /* the bytes after this one */
      put8 (tb, g_str_equal (name, "Symbol") ? 0x16 : 0x06);   /* variable pitch, TrueType */
      put16 (tb, 400);
      put8 (tb, g_str_equal (name, "Symbol") ? 2 : 0);          /* ANSI, or the symbol set */
      put8 (tb, 0);
      pad_to (tb, tb->len + 10 + 24);
      for (glong c = 0; c < n16; c++)
        put16 (tb, u[c]);
      put16 (tb, 0);
      g_free (u);
    }
}

/* ---- sprms ------------------------------------------------------------ */

static void
sprm8 (GByteArray *o, guint16 sprm, guint v)
{
  put16 (o, sprm);
  put8 (o, v);
}

static void
sprm16 (GByteArray *o, guint16 sprm, guint v)
{
  put16 (o, sprm);
  put16 (o, v);
}

static void
sprm32 (GByteArray *o, guint16 sprm, guint32 v)
{
  put16 (o, sprm);
  put32 (o, v);
}

/* Word's sixteen colours, by the index .doc files name them with. */
static int
nearest_ico (guint32 rgb)
{
  return (rgb & 0xFFFFFF) == 0xFFFFFF ? 8 : w42_highlight_nearest (rgb);
}

static guint
kul_for (guint underline)
{
  switch (underline)
    {
    case W42_UNDERLINE_SINGLE: return 1;
    case W42_UNDERLINE_WORDS:  return 2;
    case W42_UNDERLINE_DOUBLE: return 3;
    case W42_UNDERLINE_DOTTED: return 4;
    case W42_UNDERLINE_THICK:  return 6;
    case W42_UNDERLINE_DASHED: return 7;
    case W42_UNDERLINE_WAVE:   return 11;
    default:                   return 0;
    }
}

/* The sprms that make `base` into `ch`; with no base, every one. */
static void
chp_sprms (Writer *w, GByteArray *o, const W42CharFmt *ch, const W42CharFmt *base)
{
  static const W42CharFmt none;
  const W42CharFmt *b = base != NULL ? base : &none;

  if (base == NULL || g_strcmp0 (ch->family, b->family) != 0)
    {
      guint ftc = font_index (w, ch->family);

      sprm16 (o, 0x4A4F, ftc);     /* sprmCRgFtc0: the ASCII font */
      sprm16 (o, 0x4A50, ftc);     /* sprmCRgFtc1: the East Asian */
      sprm16 (o, 0x4A51, ftc);     /* sprmCRgFtc2: everything else */
    }
  if (base == NULL || ch->size != b->size)
    {
      guint hps = ch->size > 0 ? (guint) ch->size : 20;

      sprm16 (o, 0x4A43, hps);     /* sprmCHps */
      sprm16 (o, 0x4A61, hps);     /* sprmCHpsBi */
    }
  if (ch->bold != b->bold)           sprm8 (o, 0x0835, ch->bold);
  if (ch->italic != b->italic)       sprm8 (o, 0x0836, ch->italic);
  if (ch->strikeout != b->strikeout) sprm8 (o, 0x0837, ch->strikeout);
  if (ch->outline != b->outline)     sprm8 (o, 0x0838, ch->outline);
  if (ch->shadow != b->shadow)       sprm8 (o, 0x0839, ch->shadow);
  if (ch->smallcaps != b->smallcaps) sprm8 (o, 0x083A, ch->smallcaps != 0);
  if (ch->allcaps != b->allcaps)     sprm8 (o, 0x083B, ch->allcaps != 0);
  if (ch->engrave != b->engrave)     sprm8 (o, 0x0854, ch->engrave);
  if (ch->emboss != b->emboss)       sprm8 (o, 0x0858, ch->emboss);
  if (ch->dstrike != b->dstrike)     sprm8 (o, 0x2A53, ch->dstrike);
  if (ch->underline != b->underline) sprm8 (o, 0x2A3E, kul_for (ch->underline));
  if (ch->script != b->script)       sprm8 (o, 0x2A48, ch->script > 0 ? 1 : ch->script < 0 ? 2 : 0);
  if (ch->highlight != b->highlight) sprm8 (o, 0x2A0C, MIN (ch->highlight, 16));
  if (ch->spacing != b->spacing)     sprm16 (o, 0x8840, (guint16) ch->spacing);
  if (base == NULL ? ch->color != 0 : ch->color != b->color)
    {
      /* The palette's nearest for Word 97, then the colour itself. */
      if ((ch->color & 0xFFFFFF) == 0)
        sprm8 (o, 0x2A42, 0);        /* automatic, which is black */
      else
        {
          sprm8 (o, 0x2A42, (guint) nearest_ico (ch->color));
          sprm32 (o, 0x6870, ((ch->color >> 16) & 0xFF) | (ch->color & 0xFF00) |
                             ((ch->color & 0xFF) << 16));
        }
    }
  if (ch->lang != b->lang && ch->lang != NULL)
    {
      int lcid = g_str_equal (ch->lang, "none") ? 0x0400 : w42_lang_to_lcid (ch->lang);

      if (lcid > 0)
        {
          sprm16 (o, 0x486D, (guint) lcid);   /* sprmCRgLid0_80 */
          sprm16 (o, 0x4873, (guint) lcid);   /* sprmCRgLid0 */
        }
    }
}

/* A BRC80: the width in eighths of a point, the kind of line, and one of
 * the sixteen colours. */
static guint32
brc80 (const W42BorderEdge *edge)
{
  guint width = (guint) CLAMP (W42_EDGE_WIDTH (edge) * 8 / 20, 2, 255);
  guint kind;

  switch (edge->style)
    {
    case W42_BORDER_DOUBLE: kind = 3; break;
    case W42_BORDER_DOTTED: kind = 6; break;
    case W42_BORDER_DASHED: kind = 7; break;
    case W42_BORDER_NONE:   return 0;
    default:                kind = 1; break;
    }
  return width | (kind << 8) | ((guint32) ((edge->color & 0xFFFFFF) != 0 ? nearest_ico (edge->color) : 0) << 16);
}

static guint
jc_for (W42Align align)
{
  switch (align)
    {
    case W42_ALIGN_CENTER:  return 1;
    case W42_ALIGN_RIGHT:   return 2;
    case W42_ALIGN_JUSTIFY: return 3;
    default:                return 0;
    }
}

/* The LSPD: single spacing, a multiple, or an exact height. */
static void
line_spacing (const W42ParaFmt *pa, int *dya, int *mult)
{
  if (pa->line_spacing_pct > 0)
    {
      *dya = 240 * pa->line_spacing_pct / 100;
      *mult = 1;
    }
  else if (pa->line_spacing > 0)
    {
      *dya = -pa->line_spacing;    /* negative is "exactly" */
      *mult = 0;
    }
  else
    {
      *dya = 240;
      *mult = 1;
    }
}

/* The sprms that make `base` into `pa`; with no base, the ones that
 * differ from Word's defaults. */
static void
pap_sprms (GByteArray *o, const W42ParaFmt *pa, const W42ParaFmt *base)
{
  static W42ParaFmt word_default;
  const W42ParaFmt *b = base != NULL ? base : &word_default;
  int dya, mult, bdya, bmult;

  if (jc_for (pa->align) != jc_for (b->align))  sprm8 (o, 0x2403, jc_for (pa->align));
  if (pa->rtl != b->rtl)                        sprm8 (o, 0x2441, pa->rtl);
  if (pa->indent_left != b->indent_left)        sprm16 (o, 0x840F, (guint16) pa->indent_left);
  if (pa->indent_right != b->indent_right)      sprm16 (o, 0x840E, (guint16) pa->indent_right);
  if (pa->indent_first != b->indent_first)      sprm16 (o, 0x8411, (guint16) pa->indent_first);
  if (pa->space_before != b->space_before)      sprm16 (o, 0xA413, (guint) MAX (pa->space_before, 0));
  if (pa->space_after != b->space_after)        sprm16 (o, 0xA414, (guint) MAX (pa->space_after, 0));
  line_spacing (pa, &dya, &mult);
  line_spacing (b, &bdya, &bmult);
  if (dya != bdya || mult != bmult)
    {
      put16 (o, 0x6412);
      put16 (o, (guint16) dya);
      put16 (o, (guint16) mult);
    }
  if (pa->page_break_before != b->page_break_before) sprm8 (o, 0x2407, pa->page_break_before);
  if (pa->keep_together != b->keep_together)         sprm8 (o, 0x2405, pa->keep_together);
  if (pa->keep_next != b->keep_next)                 sprm8 (o, 0x2406, pa->keep_next);
  if (base == NULL || pa->widow_control != b->widow_control)
    sprm8 (o, 0x2431, pa->widow_control);

  if (pa->n_tabs != b->n_tabs ||
      memcmp (pa->tab_pos, b->tab_pos, sizeof pa->tab_pos[0] * pa->n_tabs) != 0 ||
      memcmp (pa->tab_kind, b->tab_kind, pa->n_tabs) != 0)
    {
      /* sprmPChgTabsPapx: the base's stops go, these come. */
      guint del = b->n_tabs, add = MIN (pa->n_tabs, W42_MAX_TABS);

      put16 (o, 0xC60D);
      put8 (o, 1 + 2 * del + 1 + 3 * add);
      put8 (o, del);
      for (guint i = 0; i < del; i++)
        put16 (o, (guint16) b->tab_pos[i]);
      put8 (o, add);
      for (guint i = 0; i < add; i++)
        put16 (o, (guint16) pa->tab_pos[i]);
      for (guint i = 0; i < add; i++)
        {
          static const guint8 JC[] = { 0, 1, 2, 3 };
          static const guint8 TLC[] = { 0, 1, 2, 3 };

          put8 (o, JC[W42_TAB_KIND (pa->tab_kind[i]) & 3] |
                   (TLC[W42_TAB_LEADER (pa->tab_kind[i]) & 3] << 3));
        }
    }

  {
    /* The sides, top, left, bottom and right as the sprms go. */
    static const struct { guint16 sprm; int edge; } SIDES[] = {
      { 0x6424, W42_EDGE_TOP }, { 0x6425, W42_EDGE_LEFT },
      { 0x6426, W42_EDGE_BOTTOM }, { 0x6427, W42_EDGE_RIGHT },
    };

    for (guint i = 0; i < G_N_ELEMENTS (SIDES); i++)
      {
        int e = SIDES[i].edge;
        gboolean on = (pa->border & (1 << e)) != 0;
        gboolean was = (b->border & (1 << e)) != 0;

        if (on != was || (on && memcmp (&pa->edge[e], &b->edge[e], sizeof pa->edge[e]) != 0))
          sprm32 (o, SIDES[i].sprm, on ? brc80 (&pa->edge[e]) : 0);
      }
  }

  if (pa->shading != b->shading || pa->has_shading_color != b->has_shading_color ||
      (pa->has_shading_color && pa->shading_color != b->shading_color))
    {
      if (pa->has_shading_color)
        {
          guint32 c = pa->shading_color;

          /* sprmPShd80 with the nearest of the sixteen, then sprmPShd
           * with the colour. */
          sprm16 (o, 0x442D, (guint) nearest_ico (c) << 5);
          put16 (o, 0xC64D);
          put8 (o, 10);
          put32 (o, 0xFF000000u);                           /* automatic foreground */
          put32 (o, ((c >> 16) & 0xFF) | (c & 0xFF00) | ((c & 0xFF) << 16));
          put16 (o, 0);                                     /* clear */
        }
      else
        {
          /* A grey: the pattern nearest the percentage. */
          static const guint8 PCT[14] = { 0, 100, 5, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90 };
          guint best = 0;

          for (guint i = 0; i < G_N_ELEMENTS (PCT); i++)
            if (ABS ((int) PCT[i] - (int) pa->shading) < ABS ((int) PCT[best] - (int) pa->shading))
              best = i;
          sprm16 (o, 0x442D, (guint) (best << 10) | 1);    /* black on automatic */
        }
    }
}

/* ---- styles ----------------------------------------------------------- */

static const W42Style *
style_base (Writer *w, const W42Style *style)
{
  const W42Style *base = style->based_on != NULL ? w42_stylesheet_find (w->sheet, style->based_on) : NULL;

  if (base == style || (base == NULL && !g_str_equal (style->name, "Normal")))
    base = w42_stylesheet_find (w->sheet, "Normal");
  return base != style ? base : NULL;
}

static guint
istd_for (Writer *w, const char *name)
{
  if (name == NULL)
    return ISTD_NORMAL;
  for (guint i = 0; i < ISTD_FIRST_USER; i++)
    if (w->istd_style[i] != NULL && g_str_equal (w->istd_style[i]->name, name))
      return i;
  for (guint i = 0; i < w->user_styles->len; i++)
    if (g_str_equal (((const W42Style *) g_ptr_array_index (w->user_styles, i))->name, name))
      return ISTD_FIRST_USER + i;
  return ISTD_NORMAL;
}

static void
collect_styles (Writer *w)
{
  guint n = w42_stylesheet_size (w->sheet);

  w->user_styles = g_ptr_array_new ();
  w->istd_style[ISTD_NORMAL] = w42_stylesheet_find (w->sheet, "Normal");
  for (int h = 1; h <= 9; h++)
    {
      char name[16];

      g_snprintf (name, sizeof name, "Heading %d", h);
      w->istd_style[ISTD_HEADING1 + h - 1] = w42_stylesheet_find (w->sheet, name);
    }
  for (guint i = 0; i < n; i++)
    {
      const W42Style *s = w42_stylesheet_get (w->sheet, i);

      if (istd_for (w, s->name) == ISTD_NORMAL && s != w->istd_style[ISTD_NORMAL])
        g_ptr_array_add (w->user_styles, (gpointer) s);
    }
}

/* One STD: its header, name, and the paragraph and character UPXs. */
static void
write_std (Writer *w, GByteArray *tb, const W42Style *style, guint istd)
{
  GByteArray *std = g_byte_array_new ();
  gboolean character = style != NULL && style->character;
  const W42Style *base = style != NULL && !character ? style_base (w, style) : NULL;
  guint sti, istd_base;
  const char *name;
  glong n16 = 0;
  gunichar2 *u;

  if (istd == ISTD_DEFAULT_FONT)
    {
      sti = STI_DEFAULT_FONT;
      name = "Default Paragraph Font";
      character = TRUE;
    }
  else if (istd == ISTD_NORMAL)
    sti = STI_NORMAL, name = style != NULL ? style->name : "Normal";
  else if (istd < ISTD_DEFAULT_FONT)
    sti = istd, name = style != NULL ? style->name : NULL;
  else
    sti = g_str_equal (style->name, "Title") ? STI_TITLE : STI_USER, name = style->name;

  if (name == NULL)
    {
      static const char *HEADINGS[] = { NULL, "heading 1", "heading 2", "heading 3", "heading 4",
                                        "heading 5", "heading 6", "heading 7", "heading 8", "heading 9" };
      name = HEADINGS[istd];
    }

  istd_base = istd == ISTD_NORMAL || istd == ISTD_DEFAULT_FONT ? 0xFFF
              : character ? ISTD_DEFAULT_FONT
              : base != NULL ? istd_for (w, base->name) : ISTD_NORMAL;

  put16 (std, sti | (1u << 14));                     /* sti, fHasUpe */
  put16 (std, (character ? 2u : 1u) | (istd_base << 4));
  put16 (std, (character ? 1u : 2u) | ((character ? istd : ISTD_NORMAL) << 4));   /* cupx, istdNext */
  put16 (std, 0);                                    /* bchUpe, set below */
  put16 (std, 0);
  u = g_utf8_to_utf16 (name, -1, NULL, &n16, NULL);
  put16 (std, (guint) n16);
  for (glong c = 0; c < n16; c++)
    put16 (std, u[c]);
  put16 (std, 0);
  g_free (u);

  if (!character)
    {
      GByteArray *papx = g_byte_array_new ();

      if (style != NULL)
        pap_sprms (papx, &style->pa, base != NULL ? &base->pa : NULL);
      put16 (std, 2 + papx->len);
      put16 (std, istd);
      g_byte_array_append (std, papx->data, papx->len);
      if (std->len % 2)
        put8 (std, 0);
      g_byte_array_free (papx, TRUE);
    }
  {
    GByteArray *chpx = g_byte_array_new ();

    if (style != NULL && istd != ISTD_DEFAULT_FONT)
      chp_sprms (w, chpx, &style->ch,
                 character ? &w->istd_style[ISTD_NORMAL]->ch : base != NULL ? &base->ch : NULL);
    put16 (std, chpx->len);
    g_byte_array_append (std, chpx->data, chpx->len);
    if (std->len % 2)
      put8 (std, 0);
    g_byte_array_free (chpx, TRUE);
  }
  set16 (std, 6, std->len);

  put16 (tb, std->len);
  g_byte_array_append (tb, std->data, std->len);
  g_byte_array_free (std, TRUE);
}

/* STSH: the STSHI -- how many styles, the size of an STD's fixed part,
 * and the standard fonts -- then an STD each, the empty places as 0. */
static void
write_styles (Writer *w, GByteArray *tb)
{
  guint cstd = ISTD_FIRST_USER + w->user_styles->len;
  guint ftc = font_index (w, w->istd_style[ISTD_NORMAL]->ch.family);

  put16 (tb, 18);            /* cbStshi */
  put16 (tb, cstd);
  put16 (tb, 10);            /* cbSTDBaseInFile: Word 97's */
  put16 (tb, 1);             /* fStdStylenamesWritten */
  put16 (tb, 0x5B);          /* stiMaxWhenSaved */
  put16 (tb, ISTD_FIRST_USER);
  put16 (tb, 0);             /* nVerBuiltInNamesWhenSaved */
  put16 (tb, ftc);
  put16 (tb, ftc);
  put16 (tb, ftc);

  for (guint istd = 0; istd < cstd; istd++)
    {
      if (istd > ISTD_DEFAULT_FONT && istd < ISTD_FIRST_USER)
        put16 (tb, 0);             /* reserved places, empty */
      else if (istd < ISTD_FIRST_USER)
        write_std (w, tb, istd < ISTD_DEFAULT_FONT ? w->istd_style[istd] : NULL, istd);
      else
        write_std (w, tb, g_ptr_array_index (w->user_styles, istd - ISTD_FIRST_USER), istd);
    }
}

/* ---- text ------------------------------------------------------------- */

static void
add_char (Writer *w, gunichar c)
{
  gunichar2 u[2];

  if (c >= 0x10000)
    {
      c -= 0x10000;
      u[0] = (gunichar2) (0xD800 + (c >> 10));
      u[1] = (gunichar2) (0xDC00 + (c & 0x3FF));
      g_array_append_vals (w->text, u, 2);
    }
  else
    {
      u[0] = (gunichar2) c;
      g_array_append_val (w->text, u[0]);
    }
}

/* Ends a character run at the text's end, with the sprms in `grpprl`,
 * which it takes; a run like the one before joins it. */
static void
end_run (Writer *w, GByteArray *grpprl)
{
  Run *last = w->chp->len > 0 ? &g_array_index (w->chp, Run, w->chp->len - 1) : NULL;
  Run run;

  if ((last != NULL ? last->cp_end : 0) == w->text->len)
    {
      g_byte_array_free (grpprl, TRUE);
      return;
    }
  if (last != NULL && last->grpprl->len == grpprl->len &&
      memcmp (last->grpprl->data, grpprl->data, grpprl->len) == 0)
    {
      last->cp_end = w->text->len;
      g_byte_array_free (grpprl, TRUE);
      return;
    }
  run.cp_end = w->text->len;
  run.grpprl = grpprl;
  g_array_append_val (w->chp, run);
}

/* A run's text as Word has it: a line break is 11, a soft hyphen 31, a
 * non-breaking hyphen 30, and the other control characters, which mean
 * something else to Word, are left out. */
static void
add_text (Writer *w, const char *text, gsize n_bytes)
{
  for (const char *p = text; p < text + n_bytes; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      switch (c)
        {
        case 0x2028: add_char (w, 0x0B); break;
        case 0x00AD: add_char (w, 0x1F); break;
        case 0x2011: add_char (w, 0x1E); break;
        case 0xFFFC: break;
        case '\t':   add_char (w, 0x09); break;
        default:
          if (c >= 0x20)
            add_char (w, c);
          break;
        }
    }
}

static void
write_main_text (Writer *w)
{
  GPtrArray *blocks = w42_pt_snapshot_blocks (w->pt);

  for (guint i = 0; i < blocks->len; i++)
    {
      const W42Block *block = g_ptr_array_index (blocks, i);
      const W42Fmt *fmt = w42_ap_table_get (w->aps, block->ap);
      guint istd;
      const W42Style *style;
      const W42ParaFmt *style_pa;
      const W42CharFmt *style_ch;
      const W42CharFmt *mark_ch;
      ParaRun para;

      if (block->note >= 0)
        continue;

      istd = istd_for (w, fmt->pa.style);
      style = istd < ISTD_FIRST_USER ? w->istd_style[istd]
                                     : g_ptr_array_index (w->user_styles, istd - ISTD_FIRST_USER);
      if (style == NULL)
        style = w->istd_style[ISTD_NORMAL];
      style_pa = &style->pa;
      style_ch = &style->ch;
      mark_ch = &fmt->ch;

      for (guint r = 0; r < block->runs->len; r++)
        {
          const W42Run *run = &g_array_index (block->runs, W42Run, r);
          const W42Fmt *rf = w42_ap_table_get (w->aps, run->ap);
          GByteArray *grpprl = g_byte_array_new ();

          if (run->object != W42_OBJECT_NONE || run->footnote > 0)
            {
              g_byte_array_free (grpprl, TRUE);
              continue;
            }
          add_text (w, block->text->str + run->byte_offset, run->n_bytes);
          chp_sprms (w, grpprl, &rf->ch, style_ch);
          end_run (w, grpprl);
          mark_ch = &rf->ch;
        }

      /* The paragraph mark: an empty paragraph's is its own, which says
       * how tall the blank line is; any other's is its last run's, as a
       * \par in RTF takes the formatting before it. */
      add_char (w, 0x0D);
      {
        GByteArray *grpprl = g_byte_array_new ();

        chp_sprms (w, grpprl, mark_ch, style_ch);
        end_run (w, grpprl);
      }

      para.cp_end = w->text->len;
      para.istd = istd;
      para.grpprl = g_byte_array_new ();
      pap_sprms (para.grpprl, &fmt->pa, style_pa);
      g_array_append_val (w->pap, para);
    }

  g_ptr_array_free (blocks, TRUE);
  w->ccp_text = w->text->len;
}

/* ---- formatted disk pages --------------------------------------------- */

/* The character FKPs: each page a run of fcs, an offset byte each, and
 * the CHPXs packed from the page's end.  Returns the bin table's fcs and
 * page numbers through `bins`, pairs of fc and pn. */
static void
write_chpx_fkps (Writer *w, GByteArray *wd, GArray *bin_fc, GArray *bin_pn)
{
  guint i = 0;
  guint32 cp_start = 0;

  while (i < w->chp->len)
    {
      guint8 page[512];
      guint first = i, crun = 0;
      guint top = 511;               /* where the next CHPX ends */
      guint8 offsets[0x65];
      guint32 run_start = cp_start;

      memset (page, 0, sizeof page);
      while (i < w->chp->len && crun < 0x65)
        {
          const Run *run = &g_array_index (w->chp, Run, i);
          guint need = run->grpprl->len > 0 ? (1 + run->grpprl->len + 1) & ~1u : 0;
          guint header = 4 * (crun + 2) + (crun + 1);

          if (header + need > top && crun > 0)
            break;
          if (run->grpprl->len > 0)
            {
              top = (top - (1 + run->grpprl->len)) & ~1u;
              page[top] = (guint8) run->grpprl->len;
              memcpy (page + top + 1, run->grpprl->data, run->grpprl->len);
              offsets[crun] = (guint8) (top / 2);
            }
          else
            offsets[crun] = 0;
          crun++;
          i++;
        }

      for (guint k = 0; k <= crun; k++)
        {
          guint32 cp = k == 0 ? run_start : g_array_index (w->chp, Run, first + k - 1).cp_end;
          guint32 fc = TEXT_FC + 2 * cp;

          page[4 * k] = (guint8) fc;
          page[4 * k + 1] = (guint8) (fc >> 8);
          page[4 * k + 2] = (guint8) (fc >> 16);
          page[4 * k + 3] = (guint8) (fc >> 24);
        }
      memcpy (page + 4 * (crun + 1), offsets, crun);
      page[511] = (guint8) crun;

      {
        guint32 fc = TEXT_FC + 2 * run_start, pn = wd->len / 512;

        g_array_append_val (bin_fc, fc);
        g_array_append_val (bin_pn, pn);
      }
      g_byte_array_append (wd, page, 512);
      cp_start = g_array_index (w->chp, Run, i - 1).cp_end;
    }
  {
    guint32 fc = TEXT_FC + 2 * cp_start;
    g_array_append_val (bin_fc, fc);
  }
}

/* The paragraph FKPs: each page a run of fcs, a BX of 13 bytes each --
 * the offset and an empty PHE -- and the PAPXs, istd first. */
static void
write_papx_fkps (Writer *w, GByteArray *wd, GArray *bin_fc, GArray *bin_pn)
{
  guint i = 0;
  guint32 cp_start = 0;

  while (i < w->pap->len)
    {
      guint8 page[512];
      guint first = i, crun = 0;
      guint top = 511;
      guint8 offsets[29];
      guint32 run_start = cp_start;

      memset (page, 0, sizeof page);
      while (i < w->pap->len && crun < 29)
        {
          const ParaRun *para = &g_array_index (w->pap, ParaRun, i);
          guint len = 2 + para->grpprl->len;            /* istd, then the sprms */
          guint size = (len % 2 ? 1 : 2) + len;
          guint header = 4 * (crun + 2) + 13 * (crun + 1);

          if (header + size + 1 > top && crun > 0)
            break;
          top = (top - size) & ~1u;
          if (len % 2)
            {
              page[top] = (guint8) ((len + 1) / 2);
              page[top + 1] = (guint8) para->istd;
              page[top + 2] = (guint8) (para->istd >> 8);
              memcpy (page + top + 3, para->grpprl->data, para->grpprl->len);
            }
          else
            {
              page[top] = 0;
              page[top + 1] = (guint8) (len / 2);
              page[top + 2] = (guint8) para->istd;
              page[top + 3] = (guint8) (para->istd >> 8);
              memcpy (page + top + 4, para->grpprl->data, para->grpprl->len);
            }
          offsets[crun] = (guint8) (top / 2);
          crun++;
          i++;
        }

      for (guint k = 0; k <= crun; k++)
        {
          guint32 cp = k == 0 ? run_start : g_array_index (w->pap, ParaRun, first + k - 1).cp_end;
          guint32 fc = TEXT_FC + 2 * cp;

          page[4 * k] = (guint8) fc;
          page[4 * k + 1] = (guint8) (fc >> 8);
          page[4 * k + 2] = (guint8) (fc >> 16);
          page[4 * k + 3] = (guint8) (fc >> 24);
        }
      for (guint k = 0; k < crun; k++)
        page[4 * (crun + 1) + 13 * k] = offsets[k];
      page[511] = (guint8) crun;

      {
        guint32 fc = TEXT_FC + 2 * run_start, pn = wd->len / 512;

        g_array_append_val (bin_fc, fc);
        g_array_append_val (bin_pn, pn);
      }
      g_byte_array_append (wd, page, 512);
      cp_start = g_array_index (w->pap, ParaRun, i - 1).cp_end;
    }
  {
    guint32 fc = TEXT_FC + 2 * cp_start;
    g_array_append_val (bin_fc, fc);
  }
}

static void
write_bin_table (GByteArray *tb, GArray *bin_fc, GArray *bin_pn)
{
  for (guint i = 0; i < bin_fc->len; i++)
    put32 (tb, g_array_index (bin_fc, guint32, i));
  for (guint i = 0; i < bin_pn->len; i++)
    put32 (tb, g_array_index (bin_pn, guint32, i));
}

/* ---- sections --------------------------------------------------------- */

/* The one section's SEPX: the page, its margins and columns, and where
 * its numbers begin. */
static void
write_sepx (Writer *w, GByteArray *wd)
{
  GByteArray *s = g_byte_array_new ();
  W42PageSetup page;
  int from, start;

  if (w->page != NULL)
    page = *w->page;
  else
    {
      memset (&page, 0, sizeof page);
      page.width = 12240;
      page.height = 15840;
      page.margin_left = page.margin_right = 1800;
      page.margin_top = page.margin_bottom = 1440;
    }

  sprm8 (s, 0x3009, 2);                                        /* sprmSBkc: a new page */
  if (page.width > page.height)
    sprm8 (s, 0x301D, 2);                                      /* sprmSBOrientation */
  sprm16 (s, 0xB01F, (guint) page.width);
  sprm16 (s, 0xB020, (guint) page.height);
  sprm16 (s, 0xB021, (guint) page.margin_left);
  sprm16 (s, 0xB022, (guint) page.margin_right);
  sprm16 (s, 0x9023, (guint) page.margin_top);
  sprm16 (s, 0x9024, (guint) page.margin_bottom);
  sprm16 (s, 0xB017, 720);                                     /* sprmSDyaHdrTop */
  sprm16 (s, 0xB018, 720);                                     /* sprmSDyaHdrBottom */
  if (w42_page_columns (&page) > 1)
    {
      sprm16 (s, 0x500B, (guint) w42_page_columns (&page) - 1); /* sprmSCcolumns */
      sprm16 (s, 0x900C, (guint) w42_page_column_gap (&page));  /* sprmSDxaColumns */
    }
  if (w42_pt_get_title_page (w->pt))
    sprm8 (s, 0x300A, 1);                                      /* sprmSFTitlePage */
  w42_pt_get_page_numbering (w->pt, &from, &start);
  if (from > 1 || start != 1)
    {
      sprm8 (s, 0x3011, 1);                                    /* sprmSFPgnRestart */
      sprm16 (s, 0x501C, (guint) MAX (start - (from - 1), 0)); /* sprmSPgnStart97 */
    }

  put16 (wd, s->len);
  g_byte_array_append (wd, s->data, s->len);
  g_byte_array_free (s, TRUE);
}

/* ---- the document properties ------------------------------------------ */

/* Word 97's Dop, as Word writes it for a new document: widow control,
 * footnotes at the foot of the page and numbered from 1, endnotes at the
 * end of the document, half-inch tab stops. */
static const guint8 DOP97[] = {
  0x22, 0x00, 0x04, 0x00, 0x31, 0x08, 0x80, 0x18, 0x00, 0xf0, 0xd0, 0x02,
  0x00, 0x00, 0xa9, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x03, 0x90, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
  0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x03,
  0x00, 0xf0, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00,
};

static void
write_dop (Writer *w, GByteArray *tb)
{
  gsize at = tb->len;

  g_byte_array_append (tb, DOP97, sizeof DOP97);
  pad_to (tb, at + 500);
  /* The Word 97 part's grid and compatibility settings, as Word's own. */
  {
    static const guint8 GRID[] = { 0x89, 0x05, 0x89, 0x05, 0xB4, 0x00, 0xB4, 0x00,
                                   0x81, 0x81, 0x12, 0x30 };

    memcpy (tb->data + at + 0x190, GRID, sizeof GRID);
    tb->data[at + 0x1EE] = 0x02;
  }
  if (w42_pt_get_facing_pages (w->pt))
    tb->data[at] |= 0x01;          /* fFacingPages */
}

/* ---- the compound object ---------------------------------------------- */

static GByteArray *
compobj (void)
{
  static const guint8 CLSID_WORD[16] = {
    0x06, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
  };
  static const char * const strings[] = {
    "Microsoft Word 97-2003 Document", "MSWordDoc", "Word.Document.8",
  };
  GByteArray *o = g_byte_array_new ();

  put16 (o, 0x0001);
  put16 (o, 0xFFFE);
  put32 (o, 0x00000A03);
  put32 (o, 0xFFFFFFFF);
  g_byte_array_append (o, CLSID_WORD, 16);
  for (guint i = 0; i < G_N_ELEMENTS (strings); i++)
    {
      put32 (o, (guint32) strlen (strings[i]) + 1);
      g_byte_array_append (o, (const guint8 *) strings[i], (guint) strlen (strings[i]) + 1);
    }
  put32 (o, 0x71B239F4);          /* the Unicode marker, and nothing after it */
  put32 (o, 0);
  put32 (o, 0);
  put32 (o, 0);
  return o;
}

/* ---------------------------------------------------------------------- */

gboolean
w42_doc_save (W42PieceTable *pt, const W42PageSetup *page, GFile *file, GError **error)
{
  static const guint8 CLSID_WORD[16] = {
    0x06, 0x09, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
  };
  Writer w;
  GByteArray *wd = g_byte_array_new ();
  GByteArray *tb = g_byte_array_new ();
  GByteArray *co, *ole;
  GArray *chp_fc = g_array_new (FALSE, FALSE, sizeof (guint32));
  GArray *chp_pn = g_array_new (FALSE, FALSE, sizeof (guint32));
  GArray *pap_fc = g_array_new (FALSE, FALSE, sizeof (guint32));
  GArray *pap_pn = g_array_new (FALSE, FALSE, sizeof (guint32));
  guint32 fclcb[FIB_N_FCLCB][2];
  guint32 fc_sepx, ccp_all;
  gboolean ok;

  g_return_val_if_fail (pt != NULL, FALSE);

  memset (&w, 0, sizeof w);
  memset (fclcb, 0, sizeof fclcb);
  w.pt = pt;
  w.page = page;
  w.aps = w42_pt_ap_table (pt);
  w.sheet = w42_pt_stylesheet (pt);
  w.text = g_array_new (FALSE, FALSE, sizeof (gunichar2));
  w.chp = g_array_new (FALSE, FALSE, sizeof (Run));
  g_array_set_clear_func (w.chp, run_clear);
  w.pap = g_array_new (FALSE, FALSE, sizeof (ParaRun));
  g_array_set_clear_func (w.pap, para_run_clear);
  w.fonts = g_ptr_array_new ();
  font_index (&w, "Times New Roman");     /* the first font, as Word's is */

  collect_styles (&w);
  write_main_text (&w);
  ccp_all = w.text->len;

  /* WordDocument: the FIB's place, the text, the pages of properties. */
  pad_to (wd, TEXT_FC);
  for (guint i = 0; i < w.text->len; i++)
    put16 (wd, g_array_index (w.text, gunichar2, i));
  pad_to (wd, (wd->len + 511) / 512 * 512);
  write_chpx_fkps (&w, wd, chp_fc, chp_pn);
  write_papx_fkps (&w, wd, pap_fc, pap_pn);
  fc_sepx = wd->len;
  write_sepx (&w, wd);

  /* 1Table: the stylesheet first, as Word has it. */
#define BEGIN(i) (fclcb[i][0] = tb->len)
#define END(i)   (fclcb[i][1] = tb->len - fclcb[i][0])
  {
    GByteArray *fonts = g_byte_array_new ();

    BEGIN (FIB_STSHF);
    write_styles (&w, tb);
    END (FIB_STSHF);
    fclcb[FIB_STSHF_ORIG][0] = fclcb[FIB_STSHF][0];
    fclcb[FIB_STSHF_ORIG][1] = fclcb[FIB_STSHF][1];

    /* PlcfSed: the section's cps, then its SED. */
    BEGIN (FIB_PLCF_SED);
    put32 (tb, 0);
    put32 (tb, w.ccp_text);
    put16 (tb, 0);
    put32 (tb, fc_sepx);
    put16 (tb, 0);
    put32 (tb, 0xFFFFFFFF);
    END (FIB_PLCF_SED);

    BEGIN (FIB_BTE_CHPX);
    write_bin_table (tb, chp_fc, chp_pn);
    END (FIB_BTE_CHPX);
    BEGIN (FIB_BTE_PAPX);
    write_bin_table (tb, pap_fc, pap_pn);
    END (FIB_BTE_PAPX);

    /* The fonts last of the tables that name them: the styles and the
     * text have added theirs by now. */
    write_fonts (&w, fonts);
    BEGIN (FIB_STTBF_FFN);
    g_byte_array_append (tb, fonts->data, fonts->len);
    END (FIB_STTBF_FFN);
    g_byte_array_free (fonts, TRUE);

    BEGIN (FIB_DOP);
    write_dop (&w, tb);
    END (FIB_DOP);

    /* The piece table: all the text, one piece of UTF-16. */
    BEGIN (FIB_CLX);
    put8 (tb, 0x02);
    put32 (tb, 4 * 2 + 8);
    put32 (tb, 0);
    put32 (tb, ccp_all);
    put16 (tb, 0);
    put32 (tb, TEXT_FC);
    put16 (tb, 0);
    END (FIB_CLX);
  }
#undef BEGIN
#undef END

  /* The FIB. */
  {
    const char *lang = w42_stylesheet_language (w.sheet);
    int lid = lang != NULL ? w42_lang_to_lcid (lang) : 0;
    guint32 cb_mac = wd->len;

    if (lid <= 0)
      lid = 0x0409;
    set16 (wd, 0x00, 0xA5EC);                 /* wIdent */
    set16 (wd, 0x02, 0x00C1);                 /* nFib: Word 97 */
    set16 (wd, 0x06, (guint) lid);
    set16 (wd, 0x0A, 0x1200);                 /* fWhichTblStm, fExtChar */
    set16 (wd, 0x0C, 0x00BF);                 /* nFibBack */
    set32 (wd, 0x18, TEXT_FC);                /* fcMin */
    set32 (wd, 0x1C, TEXT_FC + 2 * ccp_all);  /* fcMac */
    set16 (wd, 0x20, 14);                     /* csw */
    set16 (wd, 0x22, 0x6A62);
    set16 (wd, 0x24, 0x6A62);
    set16 (wd, 0x3E, 22);                     /* cslw */
    set32 (wd, 0x40, cb_mac);
    set32 (wd, 0x4C, w.ccp_text);
    set32 (wd, 0x40 + 4 * 11, 0xFFFFF);       /* pnFbpChpFirst */
    set32 (wd, 0x40 + 4 * 14, 0xFFFFF);       /* pnFbpPapFirst */
    set32 (wd, 0x40 + 4 * 17, 0xFFFFF);       /* pnFbpLvcFirst */
    set16 (wd, 0x98, FIB_N_FCLCB);
    for (guint i = 0; i < FIB_N_FCLCB; i++)
      {
        set32 (wd, FIB_RGFCLCB + 8 * i, fclcb[i][0]);
        set32 (wd, FIB_RGFCLCB + 8 * i + 4, fclcb[i][1]);
      }
    set16 (wd, FIB_RGFCLCB + 8 * FIB_N_FCLCB, 0);   /* cswNew */
  }

  /* Big enough to stay out of the mini stream, which Word never puts
   * these in. */
  pad_to (wd, OLE_MINI_CUTOFF);
  pad_to (tb, OLE_MINI_CUTOFF);

  co = compobj ();
  {
    OleStream streams[] = {
      { "WordDocument", wd, 0 },
      { "1Table", tb, 0 },
      { "\001CompObj", co, 0 },
    };

    ole = ole_build (streams, G_N_ELEMENTS (streams), CLSID_WORD);
  }

  ok = g_file_replace_contents (file, (const char *) ole->data, ole->len, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);

  g_byte_array_free (ole, TRUE);
  g_byte_array_free (co, TRUE);
  g_byte_array_free (wd, TRUE);
  g_byte_array_free (tb, TRUE);
  g_array_free (chp_fc, TRUE);
  g_array_free (chp_pn, TRUE);
  g_array_free (pap_fc, TRUE);
  g_array_free (pap_pn, TRUE);
  g_array_free (w.text, TRUE);
  g_array_free (w.chp, TRUE);
  g_array_free (w.pap, TRUE);
  g_ptr_array_free (w.fonts, TRUE);
  g_ptr_array_free (w.user_styles, TRUE);
  return ok;
}
