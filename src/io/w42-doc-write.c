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
  FIB_FND_REF    = 2,
  FIB_FND_TXT    = 3,
  FIB_PLCF_SED   = 6,
  FIB_PLCF_HDD   = 11,
  FIB_BTE_CHPX   = 12,
  FIB_BTE_PAPX   = 13,
  FIB_STTBF_FFN  = 15,
  FIB_DOP        = 31,
  FIB_FLD_MOM    = 16,
  FIB_FLD_HDR    = 17,
  FIB_FLD_FTN    = 18,
  FIB_CLX        = 33,
  FIB_PLF_LST    = 73,
  FIB_PLF_LFO    = 74,
  FIB_END_REF    = 46,
  FIB_END_TXT    = 47,
  FIB_FLD_EDN    = 48,
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

/* A field's mark in a story: where it is, and its FLD. */
typedef struct {
  guint32 cp;
  guint8  ch;               /* 0x13 begin, 0x14 separator, 0x15 end */
  guint8  flt;              /* the begin's field type, the end's flags */
} FieldMark;

/* A section: where it ends, and its columns. */
typedef struct {
  guint32 cp_end;
  int     columns;
  int     column_gap;
} Section;

/* The stories after the main text, in the order Word keeps them. */
enum { STORY_MAIN = 0, STORY_FTN, STORY_HDD, STORY_EDN, N_STORIES };

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

  guint32     story_start[N_STORIES];
  guint32     ccp[N_STORIES];
  int         story;        /* the one being written */
  GArray     *fields[N_STORIES];   /* FieldMark, cps from the story's start */
  GArray     *note_ref[2];  /* guint32: footnotes' and endnotes' reference cps */
  GArray     *note_id[2];   /* int: their ids, in the same order */
  GArray     *note_txt[2];  /* guint32: where each one's text starts in its story */
  GArray     *hdd;          /* guint32: where each header story starts */
  GArray     *sections;     /* Section */
  GByteArray *data;         /* the Data stream: big PAPXs, pictures */
  GArray     *lists;        /* ListDef: one per list, its LFO the same place + 1 */
  int         cur_list[2];  /* the numbered and the bulleted list going on, or -1 */
  GArray     *row_cells;    /* CellInfo: the cells of the row being written */
} Writer;

/* A list, as Word defines one: nine levels, each with its kind and the
 * number it starts at. */
typedef struct {
  guint32 lsid;
  guint8  kind[9];          /* W42ListKind; 0 where the level is unused */
  int     start[9];
} ListDef;

/* A cell of a table row: its first column, the columns it covers, and
 * what its mark carries. */
typedef struct {
  int      col;
  int      span;
  W42ApIdx cell_ap;
} CellInfo;

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

/* Word's built-in styles past the headings, by their identity (sti):
 * a file names them by it, and Word shows them in its own language. */
static const struct { const char *name; guint sti; } BUILTIN_STYLES[] = {
  { "TOC 1", 19 }, { "TOC 2", 20 }, { "TOC 3", 21 }, { "TOC 4", 22 }, { "TOC 5", 23 },
  { "TOC 6", 24 }, { "TOC 7", 25 }, { "TOC 8", 26 }, { "TOC 9", 27 },
  { "Footnote Text", 29 }, { "Header", 31 }, { "Footer", 32 }, { "Caption", 34 },
  { "Footnote Reference", 38 }, { "Endnote Reference", 42 }, { "Endnote Text", 43 },
  { "Title", 62 }, { "Body Text", 66 }, { "Subtitle", 74 },
  { "Hyperlink", 85 }, { "Strong", 87 }, { "Emphasis", 88 },
  { "List Paragraph", 179 }, { "Quote", 180 }, { "Intense Quote", 181 },
};

static guint
builtin_sti (const char *name)
{
  for (guint i = 0; i < G_N_ELEMENTS (BUILTIN_STYLES); i++)
    if (g_ascii_strcasecmp (name, BUILTIN_STYLES[i].name) == 0)
      return BUILTIN_STYLES[i].sti;
  return STI_USER;
}

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
    {
      /* A heading the document has no style for is still Word's. */
      static const char *HEADINGS[ISTD_DEFAULT_FONT] = {
        NULL, "heading 1", "heading 2", "heading 3", "heading 4",
        "heading 5", "heading 6", "heading 7", "heading 8", "heading 9",
      };

      sti = istd;
      name = style != NULL ? style->name : HEADINGS[istd];
    }
  else
    sti = builtin_sti (style->name), name = style->name;

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

/* The style a paragraph wears, and its place. */
static const W42Style *
para_style (Writer *w, const char *name, guint *istd)
{
  const W42Style *style;

  *istd = istd_for (w, name);
  style = *istd < ISTD_FIRST_USER ? w->istd_style[*istd]
                                  : g_ptr_array_index (w->user_styles, *istd - ISTD_FIRST_USER);
  if (style == NULL)
    {
      *istd = ISTD_NORMAL;
      style = w->istd_style[ISTD_NORMAL];
    }
  return style;
}

/* Ends a paragraph at the text's end: its mark, and its PAPX. */
static void
end_para (Writer *w, gunichar mark, const W42CharFmt *mark_ch, const W42CharFmt *style_ch,
          const W42ParaFmt *pa, const W42ParaFmt *style_pa, guint istd)
{
  GByteArray *grpprl = g_byte_array_new ();
  ParaRun para;

  add_char (w, mark);
  if (mark_ch != NULL)
    chp_sprms (w, grpprl, mark_ch, style_ch);
  end_run (w, grpprl);

  para.cp_end = w->text->len;
  para.istd = istd;
  para.grpprl = g_byte_array_new ();
  if (pa != NULL)
    pap_sprms (para.grpprl, pa, style_pa);
  g_array_append_val (w->pap, para);
}

/* A note's reference mark, 2 as Word has it: a special character,
 * superscript, over the formatting of the text around it. */
static void
add_note_mark (Writer *w, const W42CharFmt *ch, const W42CharFmt *style_ch)
{
  GByteArray *grpprl = g_byte_array_new ();
  W42CharFmt super = *ch;

  super.script = 1;
  add_char (w, 0x02);
  sprm8 (grpprl, 0x0855, 1);        /* sprmCFSpec */
  chp_sprms (w, grpprl, &super, style_ch);
  end_run (w, grpprl);
}

/* A field, as a story holds one: its begin, its code, its separator, its
 * result and its end, the three marks special characters. */
static void
add_field (Writer *w, const char *code, guint flt, const char *result,
           const W42CharFmt *ch, const W42CharFmt *style_ch)
{
  GArray *marks = w->fields[w->story];
  guint32 base = w->story_start[w->story];
  const char *parts[] = { NULL, code, NULL, result, NULL };
  static const guint8 CH[] = { 0x13, 0, 0x14, 0, 0x15 };

  for (guint i = 0; i < G_N_ELEMENTS (parts); i++)
    {
      GByteArray *grpprl = g_byte_array_new ();

      if (CH[i] != 0)
        {
          FieldMark m = { w->text->len - base, CH[i], 0 };

          m.flt = CH[i] == 0x13 ? (guint8) flt : CH[i] == 0x15 ? 0x80 : 0xFF;   /* fHasSep */
          g_array_append_val (marks, m);
          add_char (w, CH[i]);
          sprm8 (grpprl, 0x0855, 1);
        }
      else
        add_text (w, parts[i], strlen (parts[i]));
      chp_sprms (w, grpprl, ch, style_ch);
      end_run (w, grpprl);
    }
}

/* Appends sprms to the paragraph just ended. */
static void
para_extra (Writer *w, const GByteArray *sprms)
{
  ParaRun *last = &g_array_index (w->pap, ParaRun, w->pap->len - 1);

  g_byte_array_append (last->grpprl, sprms->data, sprms->len);
}

/* The list a list paragraph belongs to, as Word's ilfo.  Word42 counts a
 * run of numbered paragraphs on until a plain paragraph, a change of kind
 * or a restart; Word counts a list on through the document.  So each run
 * is a list of its own, a restart or a new kind at the top level starts
 * another, and bullets are a list beside the numbers, which count on
 * across them as Word42's do. */
static int
list_for (Writer *w, const W42ParaFmt *pa)
{
  int bullet = w42_list_is_bullet ((W42ListKind) pa->list) ? 1 : 0;
  int lv = MIN (pa->list_level, 8);
  int cur = w->cur_list[bullet];
  ListDef *def = cur >= 0 ? &g_array_index (w->lists, ListDef, cur) : NULL;

  if (def == NULL || (def->kind[lv] != 0 && def->kind[lv] != pa->list && lv == 0) ||
      (!bullet && lv == 0 && pa->list_start > 0))
    {
      ListDef fresh;

      memset (&fresh, 0, sizeof fresh);
      fresh.lsid = 0x2A420000u + w->lists->len;
      g_array_append_val (w->lists, fresh);
      cur = w->cur_list[bullet] = (int) w->lists->len - 1;
      def = &g_array_index (w->lists, ListDef, cur);
    }
  if (def->kind[lv] == 0)
    {
      def->kind[lv] = pa->list;
      def->start[lv] = pa->list_start > 0 ? pa->list_start : 1;
    }
  return cur + 1;
}

/* A list paragraph's sprms: its list and level, and its indents said
 * outright, since without them Word takes the list's. */
static void
list_sprms (Writer *w, GByteArray *o, const W42ParaFmt *pa)
{
  if (pa->list == W42_LIST_NONE || pa->list >= W42_LIST_KINDS)
    {
      w->cur_list[0] = w->cur_list[1] = -1;
      return;
    }
  sprm16 (o, 0x460B, (guint) list_for (w, pa));        /* sprmPIlfo */
  sprm8 (o, 0x260A, MIN (pa->list_level, 8));          /* sprmPIlvl */
  sprm16 (o, 0x840F, (guint16) pa->indent_left);
  sprm16 (o, 0x8411, (guint16) pa->indent_first);
}

/* One of the model's paragraphs: its runs, then its mark.  `note_mark`
 * puts a note's reference mark and a space in front, as Word begins a
 * note's text. */
static void
write_block (Writer *w, const W42Block *block, gunichar mark, gboolean note_mark)
{
  const W42Fmt *fmt = w42_ap_table_get (w->aps, block->ap);
  guint istd;
  const W42Style *style = para_style (w, fmt->pa.style, &istd);
  const W42CharFmt *mark_ch = &fmt->ch;

  if (note_mark)
    {
      GByteArray *grpprl = g_byte_array_new ();

      add_note_mark (w, block->runs->len > 0
                        ? &w42_ap_table_get (w->aps, g_array_index (block->runs, W42Run, 0).ap)->ch
                        : &fmt->ch, &style->ch);
      add_char (w, ' ');
      chp_sprms (w, grpprl, &fmt->ch, &style->ch);
      end_run (w, grpprl);
    }

  for (guint r = 0; r < block->runs->len; r++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, r);
      const W42Fmt *rf = w42_ap_table_get (w->aps, run->ap);
      GByteArray *grpprl;

      if (run->footnote > 0 && w->story == STORY_MAIN)
        {
          int kind = run->endnote ? 1 : 0;
          guint32 cp = w->text->len;

          g_array_append_val (w->note_ref[kind], cp);
          g_array_append_val (w->note_id[kind], run->footnote_id);
          add_note_mark (w, &rf->ch, &style->ch);
          mark_ch = &rf->ch;
          continue;
        }
      if (run->object != W42_OBJECT_NONE || run->footnote > 0)
        continue;
      grpprl = g_byte_array_new ();
      add_text (w, block->text->str + run->byte_offset, run->n_bytes);
      chp_sprms (w, grpprl, &rf->ch, &style->ch);
      end_run (w, grpprl);
      mark_ch = &rf->ch;
    }

  /* The paragraph mark: an empty paragraph's is its own, which says
   * how tall the blank line is; any other's is its last run's, as a
   * \par in RTF takes the formatting before it. */
  end_para (w, mark, mark_ch, &style->ch, &fmt->pa, &style->pa, istd);
  {
    GByteArray *extra = g_byte_array_new ();

    list_sprms (w, extra, &fmt->pa);
    if (block->table >= 0 && w->story == STORY_MAIN)
      sprm8 (extra, 0x2416, 1);                        /* sprmPFInTable */
    para_extra (w, extra);
    g_byte_array_free (extra, TRUE);
  }
}

/* A cell's side as a BRC80: its own line or none when it says its sides,
 * else the table's outer or inside line when the table is ruled. */
static guint32
cell_brc (const W42TableProps *props, const W42ParaFmt *cell, int edge, gboolean outer)
{
  if (cell->border & W42_BORDER_CELL_SET)
    return (cell->border & (1 << edge)) ? brc80 (&cell->edge[edge]) : 0xFFFFFFFFu;
  if (props == NULL || !props->borders)
    return 0;
  {
    const W42BorderEdge *te = &props->edge[outer ? edge : (edge <= W42_EDGE_BOTTOM ? W42_EDGE_INSIDE_H
                                                                             : W42_EDGE_INSIDE_V)];

    return te->style == W42_BORDER_NONE ? 0 : brc80 (te);
  }
}

/* The mark that ends a table row, 7 in a paragraph of its own, whose
 * PAPX carries the row's shape: the gap between cells, its height, the
 * edges of its cells and each one's TC80, their backgrounds, and the
 * table's own lines. */
static void
end_row (Writer *w, int table, int row)
{
  const W42TableProps *props = w42_pt_table_props (w->pt, table);
  GByteArray *s = g_byte_array_new ();
  int n = (int) MIN (w->row_cells->len, 63);
  int n_cols = props != NULL ? MAX (props->n_cols, 1) : MAX (n, 1);
  int rows = w42_pt_table_rows (w->pt, table);
  int text_w;
  int x = -108;
  gboolean any_fill = FALSE;

  {
    const W42PageSetup *page = w->page;
    int width = page != NULL && page->width > 0 ? page->width : 12240;

    text_w = width - (page != NULL ? page->margin_left + page->margin_right : 3600);
    if (text_w < 1440)
      text_w = 1440;
  }

  end_para (w, 0x07, NULL, NULL, NULL, NULL, ISTD_NORMAL);

  sprm8 (s, 0x2416, 1);                                  /* sprmPFInTable */
  sprm8 (s, 0x2417, 1);                                  /* sprmPFTtp */
  sprm16 (s, 0x9602, 108);                               /* sprmTDxaGapHalf */
  if (props != NULL && props->row_heights != NULL && (guint) row < props->row_heights->len &&
      g_array_index (props->row_heights, int, row) > 0)
    sprm16 (s, 0x9407, (guint) g_array_index (props->row_heights, int, row));
  if (props != NULL && row < props->header_rows)
    sprm8 (s, 0x3404, 1);                                /* sprmTTableHeader */
  if (props != NULL && props->borders)
    {
      static const int ORDER[6] = { W42_EDGE_TOP, W42_EDGE_LEFT, W42_EDGE_BOTTOM,
                                    W42_EDGE_RIGHT, W42_EDGE_INSIDE_H, W42_EDGE_INSIDE_V };

      put16 (s, 0xD605);                                 /* sprmTTableBorders80 */
      put8 (s, 24);
      for (int e = 0; e < 6; e++)
        put32 (s, props->edge[ORDER[e]].style == W42_BORDER_NONE ? 0 : brc80 (&props->edge[ORDER[e]]));
    }

  /* sprmTDefTable: the cells' edges, then a TC80 each. */
  put16 (s, 0xD608);
  put16 (s, (guint) (2 + 2 * (n + 1) + 20 * n));
  put8 (s, (guint) n);
  put16 (s, (guint16) x);
  for (int i = 0; i < n; i++)
    {
      const CellInfo *c = &g_array_index (w->row_cells, CellInfo, i);

      for (int k = 0; k < c->span; k++)
        {
          int col = c->col + k;
          int cw = props != NULL && props->widths != NULL && (guint) col < props->widths->len
                   ? g_array_index (props->widths, int, col) : 0;

          x += cw > 0 ? cw : text_w / n_cols;
        }
      put16 (s, (guint16) x);
    }
  for (int i = 0; i < n; i++)
    {
      const CellInfo *c = &g_array_index (w->row_cells, CellInfo, i);
      const W42ParaFmt *pa = &w42_ap_table_get (w->aps, c->cell_ap)->pa;
      guint flags = 0;

      if (pa->cell_vspan == W42_CELL_COVERED)
        flags |= 0x0020;                                 /* fVertMerge */
      else if (pa->cell_vspan > 1)
        flags |= 0x0060;                                 /* fVertMerge, fVertRestart */
      if (pa->cell_valign == W42_CELL_VALIGN_CENTER)
        flags |= 1u << 7;
      else if (pa->cell_valign == W42_CELL_VALIGN_BOTTOM)
        flags |= 2u << 7;
      put16 (s, flags);
      put16 (s, 0);
      put32 (s, cell_brc (props, pa, W42_EDGE_TOP, row == 0));
      put32 (s, cell_brc (props, pa, W42_EDGE_LEFT, c->col == 0));
      put32 (s, cell_brc (props, pa, W42_EDGE_BOTTOM, row + 1 >= rows));
      put32 (s, cell_brc (props, pa, W42_EDGE_RIGHT, c->col + c->span >= n_cols));
      any_fill |= pa->has_shading_color || pa->shading > 0;
    }
  if (any_fill)
    {
      /* sprmTDefTableShd: a SHD per cell, the background its colour or
       * its grey. */
      put16 (s, 0xD612);
      put8 (s, (guint) (10 * n));
      for (int i = 0; i < n; i++)
        {
          const W42ParaFmt *pa = &w42_ap_table_get (w->aps, g_array_index (w->row_cells, CellInfo, i).cell_ap)->pa;
          guint32 rgb = pa->has_shading_color ? pa->shading_color
                        : pa->shading > 0 ? 0x010101u * (guint32) (255 * (100 - MIN (pa->shading, 100)) / 100)
                        : 0;

          put32 (s, 0xFF000000u);
          if (pa->has_shading_color || pa->shading > 0)
            put32 (s, ((rgb >> 16) & 0xFF) | (rgb & 0xFF00) | ((rgb & 0xFF) << 16));
          else
            put32 (s, 0xFF000000u);
          put16 (s, 0);
        }
    }

  para_extra (w, s);
  g_byte_array_free (s, TRUE);
  g_array_set_size (w->row_cells, 0);
}

static void
begin_story (Writer *w, int story)
{
  w->story = story;
  w->story_start[story] = w->text->len;
}

static void
end_story (Writer *w, int story)
{
  w->ccp[story] = w->text->len - w->story_start[story];
}

/* The main text, and the sections it falls into.  A paragraph that
 * starts a section ends the one before with a section mark, 12, in
 * place of its paragraph mark. */
static void
write_main_text (Writer *w, GPtrArray *blocks)
{
  const W42Block *prev = NULL;
  Section sect = { 0, w42_page_columns (w->page), w42_page_column_gap (w->page) };

  begin_story (w, STORY_MAIN);
  for (guint i = 0; i < blocks->len; i++)
    {
      const W42Block *block = g_ptr_array_index (blocks, i);
      const W42Block *next = NULL;
      gunichar mark = 0x0D;

      if (block->note >= 0)
        continue;
      for (guint j = i + 1; j < blocks->len && next == NULL; j++)
        if (((const W42Block *) g_ptr_array_index (blocks, j))->note < 0)
          next = g_ptr_array_index (blocks, j);

      if (next != NULL && block->table < 0 &&
          w42_ap_table_get (w->aps, next->ap)->pa.section_break)
        mark = 0x0C;

      if (block->table >= 0)
        {
          gboolean cell_start = prev == NULL || prev->table != block->table ||
                                prev->row != block->row || prev->col != block->col;
          gboolean cell_end = next == NULL || next->table != block->table ||
                              next->row != block->row || next->col != block->col;
          gboolean row_end = next == NULL || next->table != block->table || next->row != block->row;

          if (cell_start)
            {
              CellInfo c = { block->col, MAX (block->span, 1), block->cell_ap };
              g_array_append_val (w->row_cells, c);
            }
          write_block (w, block, cell_end ? 0x07 : 0x0D, FALSE);
          if (row_end)
            end_row (w, block->table, block->row);
          prev = block;
          continue;
        }
      write_block (w, block, mark, FALSE);
      prev = block;

      if (mark == 0x0C)
        {
          const W42ParaFmt *npa = &w42_ap_table_get (w->aps, next->ap)->pa;

          sect.cp_end = w->text->len;
          g_array_append_val (w->sections, sect);
          sect.columns = npa->columns > 1 ? MIN (npa->columns, 6) : 1;
          sect.column_gap = npa->column_gap > 0 ? npa->column_gap : 720;
        }
    }
  (void) prev;
  sect.cp_end = w->text->len;     /* put right when the stories are in */
  g_array_append_val (w->sections, sect);
  end_story (w, STORY_MAIN);
}

/* The notes of one kind, in the order of their references, each opening
 * with its mark and a space, then one more paragraph mark for the story. */
static void
write_notes (Writer *w, GPtrArray *blocks, int kind)
{
  int story = kind == 0 ? STORY_FTN : STORY_EDN;

  begin_story (w, story);
  if (w->note_ref[kind]->len == 0)
    {
      end_story (w, story);
      return;
    }
  for (guint n = 0; n < w->note_id[kind]->len; n++)
    {
      int id = g_array_index (w->note_id[kind], int, n);
      guint32 at = w->text->len - w->story_start[story];
      gboolean first = TRUE;

      g_array_append_val (w->note_txt[kind], at);
      for (guint i = 0; i < blocks->len; i++)
        {
          const W42Block *block = g_ptr_array_index (blocks, i);

          if (block->note != id)
            continue;
          write_block (w, block, 0x0D, first);
          first = FALSE;
        }
      if (first)
        {
          /* A note with no paragraphs still has its mark. */
          add_note_mark (w, &w->istd_style[ISTD_NORMAL]->ch, &w->istd_style[ISTD_NORMAL]->ch);
          end_para (w, 0x0D, NULL, NULL, NULL, NULL, ISTD_NORMAL);
        }
    }
  end_para (w, 0x0D, NULL, NULL, NULL, NULL, ISTD_NORMAL);
  end_story (w, story);
}

/* A header or footer: its text, the fields in it made fields, as one
 * paragraph aligned as it says, and the story's closing mark. */
static void
write_page_text (Writer *w, const W42PageText *pt_text)
{
  guint32 at = w->text->len - w->story_start[STORY_HDD];
  const W42CharFmt *normal = &w->istd_style[ISTD_NORMAL]->ch;
  W42ParaFmt pa = w->istd_style[ISTD_NORMAL]->pa;
  const char *p;

  g_array_append_val (w->hdd, at);
  if (pt_text == NULL || pt_text->text == NULL || *pt_text->text == '\0')
    return;

  p = pt_text->text;
  while (*p != '\0')
    {
      static const struct { const char *name; const char *code; guint flt; } FIELDS[] = {
        { "{PAGE}", " PAGE ", 33 }, { "{NUMPAGES}", " NUMPAGES ", 26 }, { "{DATE}", " DATE ", 31 },
      };
      const char *brace = strchr (p, '{');
      gboolean field = FALSE;

      if (brace == NULL)
        brace = p + strlen (p);
      if (brace > p)
        {
          GByteArray *grpprl = g_byte_array_new ();

          add_text (w, p, (gsize) (brace - p));
          end_run (w, grpprl);
          p = brace;
          continue;
        }
      for (guint i = 0; i < G_N_ELEMENTS (FIELDS); i++)
        if (g_str_has_prefix (p, FIELDS[i].name))
          {
            char *result;

            if (FIELDS[i].flt == 31)
              {
                GDateTime *now = g_date_time_new_now_local ();
                result = g_date_time_format (now, "%x");
                g_date_time_unref (now);
              }
            else
              result = g_strdup ("1");
            add_field (w, FIELDS[i].code, FIELDS[i].flt, result, normal, normal);
            g_free (result);
            p += strlen (FIELDS[i].name);
            field = TRUE;
            break;
          }
      if (!field)
        {
          GByteArray *grpprl = g_byte_array_new ();

          add_text (w, p, 1);
          end_run (w, grpprl);
          p++;
        }
    }
  pa.align = pt_text->align;
  end_para (w, 0x0D, NULL, NULL, &pa, &w->istd_style[ISTD_NORMAL]->pa, ISTD_NORMAL);
  end_para (w, 0x0D, NULL, NULL, NULL, NULL, ISTD_NORMAL);
}

/* The headers and footers: six separator stories, left empty for Word's
 * own, then for each section its even header, odd header, even footer,
 * odd footer, first-page header and first-page footer.  The first
 * section says them; the others follow it. */
static void
write_headers (Writer *w)
{
  gboolean title = w42_pt_get_title_page (w->pt);
  gboolean facing = w42_pt_get_facing_pages (w->pt);
  const W42PageText *stories[6];
  gboolean any = FALSE;

  stories[0] = facing ? w42_pt_get_header_kind (w->pt, W42_PAGE_TEXT_EVEN) : NULL;
  stories[1] = w42_pt_get_header (w->pt);
  stories[2] = facing ? w42_pt_get_footer_kind (w->pt, W42_PAGE_TEXT_EVEN) : NULL;
  stories[3] = w42_pt_get_footer (w->pt);
  stories[4] = title ? w42_pt_get_header_kind (w->pt, W42_PAGE_TEXT_FIRST) : NULL;
  stories[5] = title ? w42_pt_get_footer_kind (w->pt, W42_PAGE_TEXT_FIRST) : NULL;
  for (guint i = 0; i < 6; i++)
    any |= stories[i] != NULL && stories[i]->text != NULL && *stories[i]->text != '\0';

  begin_story (w, STORY_HDD);
  if (!any && w->note_ref[0]->len == 0 && w->note_ref[1]->len == 0)
    {
      end_story (w, STORY_HDD);
      return;
    }
  for (guint i = 0; i < 6; i++)
    {
      /* The notes' separators, as Word writes them whether there are
       * notes or not: its line, 3, and the longer line when a note runs
       * on, 4.  The continuation notices stay empty. */
      static const gunichar SEP[6] = { 0x03, 0x04, 0, 0x03, 0x04, 0 };
      guint32 at = w->text->len - w->story_start[STORY_HDD];

      if (SEP[i] == 0)
        {
          write_page_text (w, NULL);
          continue;
        }
      g_array_append_val (w->hdd, at);
      {
        GByteArray *grpprl = g_byte_array_new ();

        add_char (w, SEP[i]);
        sprm8 (grpprl, 0x0855, 1);
        end_run (w, grpprl);
      }
      end_para (w, 0x0D, NULL, NULL, NULL, NULL, ISTD_NORMAL);
      end_para (w, 0x0D, NULL, NULL, NULL, NULL, ISTD_NORMAL);
    }
  for (guint i = 0; i < 6; i++)
    write_page_text (w, stories[i]);
  for (guint s = 1; s < w->sections->len; s++)
    for (guint i = 0; i < 6; i++)
      write_page_text (w, NULL);
  /* One more mark closes the story, and the table says where it is;
   * the table's last cp is two past it, as Word writes. */
  {
    guint32 at = w->text->len - w->story_start[STORY_HDD];

    g_array_append_val (w->hdd, at);
    end_para (w, 0x0D, NULL, NULL, NULL, NULL, ISTD_NORMAL);
    end_story (w, STORY_HDD);
    at = w->ccp[STORY_HDD] + 2;
    g_array_append_val (w->hdd, at);
  }
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
          ParaRun *para = &g_array_index (w->pap, ParaRun, i);
          guint len;

          /* Too big for a page: the sprms go to the Data stream, and
           * the page holds sprmPHugePapx saying where. */
          if (para->grpprl->len > 300)
            {
              guint32 at = w->data->len;

              put16 (w->data, para->grpprl->len);
              g_byte_array_append (w->data, para->grpprl->data, para->grpprl->len);
              g_byte_array_set_size (para->grpprl, 0);
              sprm32 (para->grpprl, 0x6646, at);
            }
          len = 2 + para->grpprl->len;            /* istd, then the sprms */
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

/* A section's SEPX: the page, its margins and columns; the first
 * section also whether its first page is a title page, and where its
 * numbers begin. */
static void
write_sepx (Writer *w, GByteArray *wd, guint index)
{
  const Section *sect = &g_array_index (w->sections, Section, index);
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
  if (sect->columns > 1)
    {
      sprm16 (s, 0x500B, (guint) sect->columns - 1);           /* sprmSCcolumns */
      sprm16 (s, 0x900C, (guint) sect->column_gap);            /* sprmSDxaColumns */
    }
  if (index == 0)
    {
      if (w42_pt_get_title_page (w->pt))
        sprm8 (s, 0x300A, 1);                                  /* sprmSFTitlePage */
      w42_pt_get_page_numbering (w->pt, &from, &start);
      if (from > 1 || start != 1)
        {
          sprm8 (s, 0x3011, 1);                                /* sprmSFPgnRestart */
          sprm16 (s, 0x501C, (guint) MAX (start - (from - 1), 0)); /* sprmSPgnStart97 */
        }
    }

  put16 (wd, s->len);
  g_byte_array_append (wd, s->data, s->len);
  g_byte_array_free (s, TRUE);
}

/* ---- lists ------------------------------------------------------------ */

/* Word's number format for a kind of list. */
static guint
nfc_for (guint kind)
{
  switch (kind)
    {
    case W42_LIST_UPPER_ROMAN:  return 1;
    case W42_LIST_LOWER_ROMAN:  return 2;
    case W42_LIST_UPPER_LETTER: return 3;
    case W42_LIST_LOWER_LETTER: return 4;
    case W42_LIST_NUMBER:       return 0;
    default:                    return 23;     /* a bullet */
    }
}

/* One LVL: the LVLF, the level's indents, its marker's font for a
 * bullet, and the marker's text -- the level's own number and a stop, or
 * the bullet character. */
static void
write_lvl (Writer *w, GByteArray *o, guint kind, int level, int start)
{
  GByteArray *papx = g_byte_array_new ();
  GByteArray *chpx = g_byte_array_new ();
  guint nfc = nfc_for (kind);
  gunichar2 text[2];
  guint cch;

  sprm16 (papx, 0x840F, (guint) (360 * (level + 1)));
  sprm16 (papx, 0x8411, (guint16) -360);
  if (nfc == 23)
    {
      const char *font = "Symbol";
      gunichar2 bullet = 0xF0B7;

      switch (kind)
        {
        case W42_LIST_BULLET_CIRCLE: font = "Courier New"; bullet = 'o'; break;
        case W42_LIST_BULLET_SQUARE: font = "Wingdings";   bullet = 0xF0A7; break;
        case W42_LIST_BULLET_DASH:   font = "Times New Roman"; bullet = 0x2013; break;
        default: break;
        }
      {
        guint ftc = font_index (w, font);

        sprm16 (chpx, 0x4A4F, ftc);
        sprm16 (chpx, 0x4A51, ftc);
      }
      text[0] = bullet;
      cch = 1;
    }
  else
    {
      text[0] = (gunichar2) level;       /* the placeholder for this level's number */
      text[1] = '.';
      cch = 2;
    }

  put32 (o, (guint32) start);            /* iStartAt */
  put8 (o, nfc);
  put8 (o, 0);                           /* left aligned */
  put8 (o, nfc == 23 ? 0 : 1);           /* rgbxchNums: where the number is */
  pad_to (o, o->len + 8);
  put8 (o, 0);                           /* ixchFollow: a tab */
  put32 (o, 0);                          /* dxaIndentSav */
  put32 (o, 0);
  put8 (o, chpx->len);
  put8 (o, papx->len);
  put8 (o, 0);                           /* ilvlRestartLim */
  put8 (o, 0);
  g_byte_array_append (o, papx->data, papx->len);
  g_byte_array_append (o, chpx->data, chpx->len);
  put16 (o, cch);
  for (guint i = 0; i < cch; i++)
    put16 (o, text[i]);

  g_byte_array_free (papx, TRUE);
  g_byte_array_free (chpx, TRUE);
}

/* PlfLst: the lists' LSTFs, and after them -- outside the count, as
 * Word has it -- their levels.  Returns the LSTFs' length. */
static guint
write_lists (Writer *w, GByteArray *tb)
{
  guint at = tb->len, lcb;

  put16 (tb, w->lists->len);
  for (guint i = 0; i < w->lists->len; i++)
    {
      const ListDef *def = &g_array_index (w->lists, ListDef, i);

      put32 (tb, def->lsid);
      put32 (tb, 0xFFFFFFFF);            /* tplc */
      for (int l = 0; l < 9; l++)
        put16 (tb, 0x0FFF);              /* no style for any level */
      put8 (tb, 0);                      /* nine levels */
      put8 (tb, 0);
    }
  lcb = tb->len - at;
  for (guint i = 0; i < w->lists->len; i++)
    {
      const ListDef *def = &g_array_index (w->lists, ListDef, i);
      guint fallback = W42_LIST_NUMBER;

      /* A level no paragraph used is like the first one used. */
      for (int l = 0; l < 9; l++)
        if (def->kind[l] != 0)
          {
            fallback = w42_list_is_bullet ((W42ListKind) def->kind[l]) ? W42_LIST_BULLET : W42_LIST_NUMBER;
            break;
          }
      for (int l = 0; l < 9; l++)
        write_lvl (w, tb, def->kind[l] != 0 ? def->kind[l] : fallback, l,
                   def->start[l] > 0 ? def->start[l] : 1);
    }
  return lcb;
}

/* PlfLfo: an LFO per list, overriding nothing, then each one's LFOData. */
static void
write_lfos (Writer *w, GByteArray *tb)
{
  put32 (tb, w->lists->len);
  for (guint i = 0; i < w->lists->len; i++)
    {
      put32 (tb, g_array_index (w->lists, ListDef, i).lsid);
      put32 (tb, 0);
      put32 (tb, 0);
      put32 (tb, 0);                     /* no overrides */
    }
  for (guint i = 0; i < w->lists->len; i++)
    put32 (tb, 0xFFFFFFFF);
}

/* ---- summary information ---------------------------------------------- */

/* \005DocumentSummaryInformation, for its second section: the custom
 * properties, which is where the page the numbers begin on is kept --
 * Word numbers every page -- with the number that page gets.  NULL when
 * there is nothing to keep. */
static GByteArray *
doc_summary_info (W42PieceTable *pt)
{
  static const guint8 FMTID_DOC[16] = {
    0x02, 0xD5, 0xCD, 0xD5, 0x9C, 0x2E, 0x1B, 0x10,
    0x93, 0x97, 0x08, 0x00, 0x2B, 0x2C, 0xF9, 0xAE,
  };
  static const guint8 FMTID_USER[16] = {
    0x05, 0xD5, 0xCD, 0xD5, 0x9C, 0x2E, 0x1B, 0x10,
    0x93, 0x97, 0x08, 0x00, 0x2B, 0x2C, 0xF9, 0xAE,
  };
  static const char * const NAMES[2] = { "Word42PageNumbersFrom", "Word42PageNumbersStart" };
  GByteArray *o, *first, *user;
  int from, start, values[2];

  w42_pt_get_page_numbering (pt, &from, &start);
  if (from <= 1)
    return NULL;
  values[0] = from;
  values[1] = start;

  /* The first section: the code page and nothing else. */
  first = g_byte_array_new ();
  put32 (first, 8 + 8 + 8);
  put32 (first, 1);
  put32 (first, 1);
  put32 (first, 16);
  put32 (first, 0x02);
  put16 (first, 1252);
  put16 (first, 0);

  /* The second: the dictionary naming properties 2 and 3, the code
   * page, and the two numbers. */
  user = g_byte_array_new ();
  {
    GByteArray *dict = g_byte_array_new ();
    guint32 at_dict, at_cp, at_v[2], n = 4;
    GByteArray *body = g_byte_array_new ();

    put32 (dict, 2);
    for (guint i = 0; i < 2; i++)
      {
        put32 (dict, i + 2);
        put32 (dict, (guint32) strlen (NAMES[i]) + 1);
        g_byte_array_append (dict, (const guint8 *) NAMES[i], (guint) strlen (NAMES[i]) + 1);
      }
    while (dict->len % 4)
      put8 (dict, 0);

    at_dict = 8 + 8 * n;
    g_byte_array_append (body, dict->data, dict->len);
    at_cp = at_dict + body->len;
    put32 (body, 0x02);
    put16 (body, 1252);
    put16 (body, 0);
    for (guint i = 0; i < 2; i++)
      {
        at_v[i] = at_dict + body->len;
        put32 (body, 0x03);            /* VT_I4 */
        put32 (body, (guint32) values[i]);
      }

    put32 (user, 8 + 8 * n + body->len);
    put32 (user, n);
    put32 (user, 0);  put32 (user, at_dict);
    put32 (user, 1);  put32 (user, at_cp);
    put32 (user, 2);  put32 (user, at_v[0]);
    put32 (user, 3);  put32 (user, at_v[1]);
    g_byte_array_append (user, body->data, body->len);
    g_byte_array_free (dict, TRUE);
    g_byte_array_free (body, TRUE);
  }

  o = g_byte_array_new ();
  put16 (o, 0xFFFE);
  put16 (o, 0);
  put32 (o, 0x00020006);
  pad_to (o, 8 + 16);
  put32 (o, 2);
  g_byte_array_append (o, FMTID_DOC, 16);
  put32 (o, 68);                            /* after the two FMTID/offset pairs */
  g_byte_array_append (o, FMTID_USER, 16);
  put32 (o, 68 + first->len);
  g_byte_array_append (o, first->data, first->len);
  g_byte_array_append (o, user->data, user->len);
  g_byte_array_free (first, TRUE);
  g_byte_array_free (user, TRUE);
  return o;
}

/* \005SummaryInformation: File > Summary Info, as a property set of
 * strings -- in Western Windows when they fit it, else in UTF-16, which
 * the set says by its code page. */
static GByteArray *
summary_info (W42PieceTable *pt)
{
  static const guint8 FMTID[16] = {
    0xE0, 0x85, 0x9F, 0xF2, 0xF9, 0x4F, 0x68, 0x10,
    0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9,
  };
  const W42DocInfo *info = w42_pt_get_info (pt);
  const char *values[5] = { NULL };
  static const guint32 PIDS[5] = { 2, 3, 4, 5, 6 };   /* title, subject, author, keywords, comments */
  GByteArray *o = g_byte_array_new ();
  GByteArray *props = g_byte_array_new ();
  GArray *offsets = g_array_new (FALSE, FALSE, sizeof (guint32));
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (guint32));
  gboolean unicode = FALSE;
  guint n;

  if (info != NULL)
    {
      values[0] = info->title;
      values[1] = info->subject;
      values[2] = info->author;
      values[3] = info->keywords;
      values[4] = info->comments;
    }
  for (guint i = 0; i < 5; i++)
    if (values[i] != NULL)
      {
        char *test = g_convert (values[i], -1, "WINDOWS-1252", "UTF-8", NULL, NULL, NULL);

        unicode |= test == NULL;
        g_free (test);
      }

  /* The code page first. */
  {
    guint32 id = 1, at = 0;

    g_array_append_val (ids, id);
    g_array_append_val (offsets, at);
    put32 (props, 0x02);                    /* VT_I2 */
    put16 (props, unicode ? 1200 : 1252);
    put16 (props, 0);
  }
  for (guint i = 0; i < 5; i++)
    {
      guint32 at = props->len;

      if (values[i] == NULL || *values[i] == '\0')
        continue;
      g_array_append_val (ids, PIDS[i]);
      g_array_append_val (offsets, at);
      put32 (props, 0x1E);                  /* VT_LPSTR */
      if (unicode)
        {
          glong n16 = 0;
          gunichar2 *u = g_utf8_to_utf16 (values[i], -1, NULL, &n16, NULL);

          put32 (props, (guint32) (n16 + 1) * 2);
          for (glong c = 0; c < n16; c++)
            put16 (props, u[c]);
          put16 (props, 0);
          g_free (u);
        }
      else
        {
          gsize len = 0;
          char *cp = g_convert (values[i], -1, "WINDOWS-1252", "UTF-8", NULL, &len, NULL);

          put32 (props, (guint32) len + 1);
          g_byte_array_append (props, (const guint8 *) cp, (guint) len);
          put8 (props, 0);
          g_free (cp);
        }
      while (props->len % 4)
        put8 (props, 0);
    }

  n = ids->len;
  put16 (o, 0xFFFE);
  put16 (o, 0);
  put32 (o, 0x00020006);                    /* the OS it was written on */
  pad_to (o, 8 + 16);                       /* no class */
  put32 (o, 1);
  g_byte_array_append (o, FMTID, 16);
  put32 (o, 48);
  put32 (o, 8 + 8 * n + props->len);        /* the section's size */
  put32 (o, n);
  for (guint i = 0; i < n; i++)
    {
      put32 (o, g_array_index (ids, guint32, i));
      put32 (o, 8 + 8 * n + g_array_index (offsets, guint32, i));
    }
  g_byte_array_append (o, props->data, props->len);

  g_byte_array_free (props, TRUE);
  g_array_free (offsets, TRUE);
  g_array_free (ids, TRUE);
  return o;
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
  guint32 ccp_all;
  GArray *fc_sepx = g_array_new (FALSE, FALSE, sizeof (guint32));
  GByteArray *si, *dsi;
  GPtrArray *blocks;
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
  for (int s = 0; s < N_STORIES; s++)
    w.fields[s] = g_array_new (FALSE, FALSE, sizeof (FieldMark));
  for (int k = 0; k < 2; k++)
    {
      w.note_ref[k] = g_array_new (FALSE, FALSE, sizeof (guint32));
      w.note_id[k] = g_array_new (FALSE, FALSE, sizeof (int));
      w.note_txt[k] = g_array_new (FALSE, FALSE, sizeof (guint32));
    }
  w.hdd = g_array_new (FALSE, FALSE, sizeof (guint32));
  w.sections = g_array_new (FALSE, FALSE, sizeof (Section));
  w.data = g_byte_array_new ();
  w.lists = g_array_new (FALSE, FALSE, sizeof (ListDef));
  w.cur_list[0] = w.cur_list[1] = -1;
  w.row_cells = g_array_new (FALSE, FALSE, sizeof (CellInfo));

  collect_styles (&w);
  blocks = w42_pt_snapshot_blocks (pt);
  write_main_text (&w, blocks);
  write_notes (&w, blocks, 0);
  write_headers (&w);
  write_notes (&w, blocks, 1);
  g_ptr_array_free (blocks, TRUE);

  /* With any story after the main text, one more paragraph mark ends
   * them all, and the last section runs to it. */
  if (w.ccp[STORY_FTN] + w.ccp[STORY_HDD] + w.ccp[STORY_EDN] > 0)
    end_para (&w, 0x0D, NULL, NULL, NULL, NULL, ISTD_NORMAL);
  ccp_all = w.text->len;
  if (ccp_all > w.ccp[STORY_MAIN])
    g_array_index (w.sections, Section, w.sections->len - 1).cp_end = ccp_all;

  /* WordDocument: the FIB's place, the text, the pages of properties. */
  pad_to (wd, TEXT_FC);
  for (guint i = 0; i < w.text->len; i++)
    put16 (wd, g_array_index (w.text, gunichar2, i));
  pad_to (wd, (wd->len + 511) / 512 * 512);
  write_chpx_fkps (&w, wd, chp_fc, chp_pn);
  write_papx_fkps (&w, wd, pap_fc, pap_pn);
  for (guint s = 0; s < w.sections->len; s++)
    {
      guint32 at = wd->len;

      g_array_append_val (fc_sepx, at);
      write_sepx (&w, wd, s);
      if (wd->len % 2)
        put8 (wd, 0);
    }

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

    /* The notes: where each reference is, with an FRD saying it is
     * numbered, then where each one's text starts, the end of the last,
     * and two past the story's end, as Word writes. */
    for (int k = 0; k < 2; k++)
      {
        int story = k == 0 ? STORY_FTN : STORY_EDN;
        GArray *refs = w.note_ref[k];

        if (refs->len == 0)
          continue;
        BEGIN (k == 0 ? FIB_FND_REF : FIB_END_REF);
        for (guint i = 0; i < refs->len; i++)
          put32 (tb, g_array_index (refs, guint32, i));
        put32 (tb, ccp_all);
        for (guint i = 0; i < refs->len; i++)
          put16 (tb, 1);
        END (k == 0 ? FIB_FND_REF : FIB_END_REF);

        BEGIN (k == 0 ? FIB_FND_TXT : FIB_END_TXT);
        for (guint i = 0; i < w.note_txt[k]->len; i++)
          put32 (tb, g_array_index (w.note_txt[k], guint32, i));
        put32 (tb, w.ccp[story] - 1);
        put32 (tb, w.ccp[story] + 2);
        END (k == 0 ? FIB_FND_TXT : FIB_END_TXT);
      }

    /* PlcfSed: the sections' cps, then a SED each. */
    BEGIN (FIB_PLCF_SED);
    put32 (tb, 0);
    for (guint s = 0; s < w.sections->len; s++)
      put32 (tb, g_array_index (w.sections, Section, s).cp_end);
    for (guint s = 0; s < w.sections->len; s++)
      {
        put16 (tb, 0);
        put32 (tb, g_array_index (fc_sepx, guint32, s));
        put16 (tb, 0);
        put32 (tb, 0xFFFFFFFF);
      }
    END (FIB_PLCF_SED);

    if (w.hdd->len > 0)
      {
        BEGIN (FIB_PLCF_HDD);
        for (guint i = 0; i < w.hdd->len; i++)
          put32 (tb, g_array_index (w.hdd, guint32, i));
        END (FIB_PLCF_HDD);
      }

    /* The fields of each story: where their marks are, then an FLD
     * each; the table's last cp is two past the story's end. */
    {
      static const int PLC[N_STORIES] = { FIB_FLD_MOM, FIB_FLD_FTN, FIB_FLD_HDR, FIB_FLD_EDN };

      for (int s = 0; s < N_STORIES; s++)
        {
          GArray *marks = w.fields[s];

          if (marks->len == 0)
            continue;
          BEGIN (PLC[s]);
          for (guint i = 0; i < marks->len; i++)
            put32 (tb, g_array_index (marks, FieldMark, i).cp);
          put32 (tb, w.ccp[s] + (s == STORY_MAIN ? 1 : 2));
          for (guint i = 0; i < marks->len; i++)
            {
              put8 (tb, g_array_index (marks, FieldMark, i).ch);
              put8 (tb, g_array_index (marks, FieldMark, i).flt);
            }
          END (PLC[s]);
        }
    }

    BEGIN (FIB_BTE_CHPX);
    write_bin_table (tb, chp_fc, chp_pn);
    END (FIB_BTE_CHPX);
    BEGIN (FIB_BTE_PAPX);
    write_bin_table (tb, pap_fc, pap_pn);
    END (FIB_BTE_PAPX);

    if (w.lists->len > 0)
      {
        BEGIN (FIB_PLF_LST);
        fclcb[FIB_PLF_LST][1] = write_lists (&w, tb);
        BEGIN (FIB_PLF_LFO);
        write_lfos (&w, tb);
        END (FIB_PLF_LFO);
      }

    /* The fonts last of the tables that name them: the styles, the text
     * and the lists have added theirs by now. */
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
    set32 (wd, 0x4C, w.ccp[STORY_MAIN]);
    set32 (wd, 0x50, w.ccp[STORY_FTN]);
    set32 (wd, 0x54, w.ccp[STORY_HDD]);
    set32 (wd, 0x60, w.ccp[STORY_EDN]);
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
  si = summary_info (pt);
  dsi = doc_summary_info (pt);
  {
    OleStream streams[] = {
      { "WordDocument", wd, 0 },
      { "1Table", tb, 0 },
      { "\001CompObj", co, 0 },
      { "\005SummaryInformation", si, 0 },
      { "Data", w.data, 0 },
      { "\005DocumentSummaryInformation", dsi, 0 },
    };
    guint n = G_N_ELEMENTS (streams) - (dsi == NULL ? 1 : 0);

    if (w.data->len == 0)
      {
        /* No Data stream: the summary moves up into its place. */
        streams[4] = streams[5];
        n--;
      }
    else
      pad_to (w.data, OLE_MINI_CUTOFF);
    ole = ole_build (streams, n, CLSID_WORD);
  }

  ok = g_file_replace_contents (file, (const char *) ole->data, ole->len, NULL, FALSE,
                                G_FILE_CREATE_NONE, NULL, NULL, error);

  g_byte_array_free (ole, TRUE);
  g_byte_array_free (co, TRUE);
  g_byte_array_free (si, TRUE);
  if (dsi != NULL)
    g_byte_array_free (dsi, TRUE);
  g_array_free (fc_sepx, TRUE);
  for (int s = 0; s < N_STORIES; s++)
    g_array_free (w.fields[s], TRUE);
  for (int k = 0; k < 2; k++)
    {
      g_array_free (w.note_ref[k], TRUE);
      g_array_free (w.note_id[k], TRUE);
      g_array_free (w.note_txt[k], TRUE);
    }
  g_array_free (w.hdd, TRUE);
  g_array_free (w.sections, TRUE);
  g_byte_array_free (w.data, TRUE);
  g_array_free (w.lists, TRUE);
  g_array_free (w.row_cells, TRUE);
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
