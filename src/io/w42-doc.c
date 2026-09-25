/* w42-doc.c - see w42-doc.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-doc.h"

#include <string.h>

#include "w42-image.h"
#include "w42-shape.h"

/* ---------------------------------------------------------------------- */
/* Little-endian readers                                                   */
/* ---------------------------------------------------------------------- */

static inline guint16 rd16 (const guint8 *p) { return (guint16) (p[0] | (p[1] << 8)); }
static inline gint16  rd16s (const guint8 *p) { return (gint16) rd16 (p); }

/* Word's brcType as one of the four lines the model draws. */
static guint8
brc_style (guint kind)
{
  if (kind == 0)
    return W42_BORDER_NONE;
  if (kind == 3 || (kind >= 10 && kind <= 21))
    return W42_BORDER_DOUBLE;
  if (kind == 6)
    return W42_BORDER_DOTTED;
  if (kind == 7 || kind == 8 || kind == 9 || kind == 22 || kind == 23)
    return W42_BORDER_DASHED;
  return W42_BORDER_SINGLE;
}

/* A Word 97 BRC80: width in eighths of a point, the kind of line, then
 * one of the sixteen palette colours.  TRUE when it is a line.  All
 * zero is no line of its own -- the table's, if the table has one --
 * and all 0xFF is no line at all; the edge is left unset for the first
 * and set to none for the second. */
static gboolean
brc80_edge (const guint8 *brc, W42BorderEdge *edge)
{
  guint width = brc[0], kind = brc[1], ico = brc[2];

  if (width == 0xFF && kind == 0xFF)
    {
      edge->style = W42_BORDER_NONE;
      edge->width = 0;
      edge->color = 0;
      return FALSE;
    }
  if (width == 0 || kind == 0)
    {
      edge->style = W42_BORDER_SINGLE;
      edge->width = 0;
      edge->color = 0;
      return FALSE;
    }
  edge->style = brc_style (kind);
  edge->width = (guint8) CLAMP ((int) width * 20 / 8, 5, 255);
  edge->color = ico > 0 && ico < 17 ? w42_highlight_rgb ((int) ico) : 0;
  return TRUE;
}

/* The later BRC: four bytes of colour, red first, with 0xFF in the last
 * for "automatic"; then the width and the kind. */
static gboolean
brc_edge (const guint8 *brc, W42BorderEdge *edge)
{
  guint width = brc[4], kind = brc[5];

  if (width == 0 || kind == 0)
    {
      edge->style = W42_BORDER_NONE;
      edge->width = 0;
      edge->color = 0;
      return FALSE;
    }
  edge->style = brc_style (kind);
  edge->width = (guint8) CLAMP ((int) width * 20 / 8, 5, 255);
  edge->color = brc[3] != 0xFF ? (((guint32) brc[0] << 16) | ((guint32) brc[1] << 8) | brc[2]) : 0;
  return TRUE;
}
static inline guint32 rd32 (const guint8 *p)
{
  return (guint32) p[0] | ((guint32) p[1] << 8) | ((guint32) p[2] << 16) |
         ((guint32) p[3] << 24);
}

#define FAIL(err, ...) \
  G_STMT_START { \
    g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, __VA_ARGS__); \
    return FALSE; \
  } G_STMT_END

/* ---------------------------------------------------------------------- */
/* OLE2 compound files                                                     */
/* ---------------------------------------------------------------------- */

#define OLE_END_OF_CHAIN 0xFFFFFFFEu
#define OLE_FREE         0xFFFFFFFFu

typedef struct {
  const guint8 *data;
  gsize         len;
  guint         sector;        /* bytes per sector */
  guint         mini_sector;
  guint32       mini_cutoff;
  GArray       *fat;           /* guint32 per sector */
  GArray       *minifat;
  GByteArray   *directory;     /* all directory sectors, in chain order */
  GByteArray   *ministream;    /* the root entry's stream */
} Ole;

static gboolean
ole_read_chain (Ole *ole, guint32 start, GByteArray *out, GError **error)
{
  guint32 s = start;
  guint steps = 0;
  guint8 *seen = g_malloc0 (ole->fat->len / 8 + 1);

  while (s != OLE_END_OF_CHAIN && s != OLE_FREE)
    {
      gsize off = (gsize) (s + 1) * ole->sector;

      if (off >= ole->len)
        { g_free (seen); FAIL (error, "OLE sector %u is past the end of the file", s); }
      if (s >= ole->fat->len)
        { g_free (seen); FAIL (error, "OLE sector %u has no FAT entry", s); }
      if (++steps > ole->fat->len || (seen[s / 8] & (1 << (s % 8))))
        { g_free (seen); FAIL (error, "OLE sector chain loops"); }
      seen[s / 8] |= (guint8) (1 << (s % 8));

      if (off + ole->sector > ole->len)
        {
          /* The file ends inside its last sector: a mail gateway cut it,
           * or a writer did not pad.  What is there is the stream's
           * end, and the text before it is all present. */
          g_byte_array_append (out, ole->data + off, ole->len - off);
          break;
        }
      g_byte_array_append (out, ole->data + off, ole->sector);
      s = g_array_index (ole->fat, guint32, s);
    }

  g_free (seen);
  return TRUE;
}

static gboolean
ole_open (Ole *ole, const guint8 *data, gsize len, GError **error)
{
  static const guint8 magic[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
  guint sector_shift, mini_shift;
  guint32 n_fat, dir_start, minifat_start, n_minifat, difat_start, n_difat;
  GArray *fat_sectors;
  guint per_sector;

  memset (ole, 0, sizeof *ole);
  ole->data = data;
  ole->len = len;

  if (len < 512 || memcmp (data, magic, 8) != 0)
    FAIL (error, "This is not a Word document (no OLE2 header).");

  sector_shift  = rd16 (data + 0x1E);
  mini_shift    = rd16 (data + 0x20);
  n_fat         = rd32 (data + 0x2C);
  dir_start     = rd32 (data + 0x30);
  ole->mini_cutoff = rd32 (data + 0x38);
  minifat_start = rd32 (data + 0x3C);
  n_minifat     = rd32 (data + 0x40);
  difat_start   = rd32 (data + 0x44);
  n_difat       = rd32 (data + 0x48);

  if (sector_shift < 7 || sector_shift > 12 || mini_shift < 2 || mini_shift > 12)
    FAIL (error, "OLE sector size is out of range");

  ole->sector = 1u << sector_shift;
  ole->mini_sector = 1u << mini_shift;
  per_sector = ole->sector / 4;

  /* No more FAT sectors than the file has sectors at all: the count is
   * the attacker's, and every claimed sector is a sector's worth of
   * entries appended below before the size is put right. */
  if (n_fat > len / ole->sector + 1)
    n_fat = (guint32) (len / ole->sector) + 1;

  /* The DIFAT lists the FAT's sectors: 109 in the header, the rest in a
   * chain of DIFAT sectors. */
  fat_sectors = g_array_new (FALSE, FALSE, sizeof (guint32));
  for (guint i = 0; i < 109 && i < n_fat; i++)
    {
      guint32 s = rd32 (data + 0x4C + 4 * i);
      g_array_append_val (fat_sectors, s);
    }
  {
    guint32 s = difat_start;
    guint guard = 0;

    while (s != OLE_END_OF_CHAIN && s != OLE_FREE && guard++ < n_difat + 1 &&
           guard < len / ole->sector + 2)
      {
        gsize off = (gsize) (s + 1) * ole->sector;

        if (off + ole->sector > len)
          break;
        for (guint i = 0; i + 1 < per_sector && fat_sectors->len < n_fat; i++)
          {
            guint32 f = rd32 (data + off + 4 * i);
            g_array_append_val (fat_sectors, f);
          }
        s = rd32 (data + off + 4 * (per_sector - 1));
      }
  }

  ole->fat = g_array_new (FALSE, FALSE, sizeof (guint32));
  for (guint i = 0; i < fat_sectors->len && ole->fat->len <= len / ole->sector; i++)
    {
      guint32 s = g_array_index (fat_sectors, guint32, i);
      gsize off = (gsize) (s + 1) * ole->sector;

      if (off + ole->sector > len)
        {
          /* A FAT sector past the end of the file.  Its entries are
           * unknown, not absent: the table keeps their places, or every
           * chain through a later sector would read the wrong sectors. */
          for (guint j = 0; j < per_sector; j++)
            {
              guint32 e = OLE_FREE;
              g_array_append_val (ole->fat, e);
            }
          continue;
        }
      for (guint j = 0; j < per_sector; j++)
        {
          guint32 e = rd32 (data + off + 4 * j);
          g_array_append_val (ole->fat, e);
        }
    }
  g_array_free (fat_sectors, TRUE);
  /* No more FAT entries than the file has sectors: a FAT that names the
   * same sectors over and over cannot make a stream bigger than the file. */
  if (ole->fat->len > len / ole->sector)
    g_array_set_size (ole->fat, (guint) (len / ole->sector));

  ole->directory = g_byte_array_new ();
  if (!ole_read_chain (ole, dir_start, ole->directory, error))
    return FALSE;
  if (ole->directory->len < 128)
    FAIL (error, "OLE directory is empty");

  /* The mini FAT, and the mini stream it indexes, which is the root
   * entry's own stream. */
  ole->minifat = g_array_new (FALSE, FALSE, sizeof (guint32));
  {
    GByteArray *raw = g_byte_array_new ();

    if (n_minifat > 0 && !ole_read_chain (ole, minifat_start, raw, error))
      {
        g_byte_array_free (raw, TRUE);
        return FALSE;
      }
    for (guint i = 0; i + 4 <= raw->len; i += 4)
      {
        guint32 e = rd32 (raw->data + i);
        g_array_append_val (ole->minifat, e);
      }
    g_byte_array_free (raw, TRUE);
  }

  ole->ministream = g_byte_array_new ();
  {
    guint32 root_start = rd32 (ole->directory->data + 0x74);
    guint32 root_size = rd32 (ole->directory->data + 0x78);

    if (root_size > 0 && !ole_read_chain (ole, root_start, ole->ministream, error))
      return FALSE;
  }

  return TRUE;
}

static void
ole_close (Ole *ole)
{
  if (ole->fat) g_array_free (ole->fat, TRUE);
  if (ole->minifat) g_array_free (ole->minifat, TRUE);
  if (ole->directory) g_byte_array_free (ole->directory, TRUE);
  if (ole->ministream) g_byte_array_free (ole->ministream, TRUE);
}

/* The named stream's bytes, or NULL. */
static GByteArray *
ole_stream (Ole *ole, const char *name, GError **error)
{
  guint n = ole->directory->len / 128;

  for (guint i = 0; i < n; i++)
    {
      const guint8 *e = ole->directory->data + i * 128;
      guint name_len = rd16 (e + 0x40);
      guint8 type = e[0x42];
      char ascii[32];
      guint k = 0;

      if (type != 2 || name_len < 2 || name_len > 64)
        continue;

      for (guint j = 0; j + 1 < name_len && k < sizeof ascii - 1; j += 2)
        {
          guint16 c = rd16 (e + j);
          if (c == 0)
            break;
          ascii[k++] = (c < 128) ? (char) c : '?';
        }
      ascii[k] = '\0';

      if (!g_str_equal (ascii, name))
        continue;

      {
        guint32 start = rd32 (e + 0x74);
        guint32 size = rd32 (e + 0x78);
        GByteArray *out = g_byte_array_new ();

        if (size < ole->mini_cutoff)
          {
            guint32 s = start;
            guint steps = 0;

            while (s != OLE_END_OF_CHAIN && s != OLE_FREE && out->len < size)
              {
                gsize off = (gsize) s * ole->mini_sector;

                /* A chain that visits more links than the mini stream has
                 * sectors is going round in a circle. */
                if (off + ole->mini_sector > ole->ministream->len ||
                    s >= ole->minifat->len || ++steps > ole->minifat->len ||
                    steps > ole->ministream->len / ole->mini_sector + 1)
                  {
                    g_byte_array_free (out, TRUE);
                    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "OLE mini stream %s is damaged", name);
                    return NULL;
                  }
                g_byte_array_append (out, ole->ministream->data + off, ole->mini_sector);
                s = g_array_index (ole->minifat, guint32, s);
              }
          }
        else if (!ole_read_chain (ole, start, out, error))
          {
            g_byte_array_free (out, TRUE);
            return NULL;
          }

        if (out->len > size)
          g_byte_array_set_size (out, size);
        return out;
      }
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "The document has no %s stream.", name);
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* The Word file                                                           */
/* ---------------------------------------------------------------------- */

typedef struct {
  guint32  cp_start, cp_end;
  guint32  fc;
  gboolean compressed;    /* 8-bit cp1252 rather than UTF-16 */
} Piece;

typedef struct {
  guint16       sti;        /* built-in identity: 0 Normal, 1-9 headings */
  guint16       istd_base;
  guint8        sgc;        /* 1 paragraph style, 2 character style */
  const guint8 *papx;       /* grpprl, past the istd */
  guint         papx_len;
  const guint8 *chpx;
  guint         chpx_len;
} DocStyle;

/* A row's shape, from the table sprms on its row-end mark: its cells'
 * edges, sides, lines, merges and backgrounds, and the table's own
 * lines.  Nearly three kilobytes, and only a row-end mark has one, so a
 * paragraph gets it when a table sprm first comes along rather than
 * every paragraph carrying it: carried by all of them, a 268 KB file of
 * nothing but paragraph marks took 860 MB to open, where it takes 143
 * MB now. */
typedef struct {
  int      itc_mac;         /* columns, from a row-end TDefTable */
  int      cellx[64];
  guint8   cell_sides[64];  /* each cell's ruled sides, from its TC80 */
  W42BorderEdge cell_edge[64][4];  /* and their lines */
  guint16  cell_flags[64];  /* the TC80's flags: merges and alignment */
  guint32  cell_shade[64];  /* each cell's background, 0x00RRGGBB */
  guint8   has_cell_shade[64];
  W42BorderEdge tbl_edge[W42_N_EDGES];  /* sprmTTableBorders */
  gboolean has_tbl_edge;
  int      dya_row_height;  /* sprmTDyaRowHeight; negative is "exactly" */
  gboolean table_header;    /* sprmTTableHeader */
} RowShape;

/* Resolved paragraph properties, Word's names for them. */
typedef struct {
  int      istd;
  int      jc;
  int      dxa_left, dxa_right, dxa_left1;
  int      dya_before, dya_after;
  int      dya_line;
  int      f_mult;
  gboolean in_table, ttp, page_break;
  gboolean keep_next, keep_together, widow, bidi;
  int      ilfo, ilvl;
  RowShape *row;            /* owned; NULL until a table sprm */
  guint8   n_tabs;                    /* sprmPChgTabsPapx */
  int      tab_pos[W42_MAX_TABS];
  guint8   tab_kind[W42_MAX_TABS];    /* kind and leader, as the model packs them */
  guint8   border;                    /* sprmPBrcTop and its neighbours */
  W42BorderEdge edge[4];
  guint8   shading;                   /* sprmPShd */
  guint8   has_shading_color;
  guint32  shading_color;
} Para;

typedef struct {
  int      bold, italic, strike;   /* 0 or 1 */
  int      dstrike, shadow, outline, emboss, imprint;   /* Word 97's effects */
  int      kul;
  int      ico;
  guint32  rgb;                    /* 0 for none; sprmCCv */
  gboolean has_rgb;
  int      hps;
  int      ftc;
  int      iss;
  int      smallcaps, allcaps;
  int      highlight;
  int      dxa_space;
  gboolean spec;
  int      vanish;       /* hidden text: not shown, so not read */
  int      rmark_del;    /* a tracked deletion ... */
  int      rmark_ins;    /* ... and a tracked insertion */
  guint32  pic_fc;       /* sprmCPicLocation: into the Data stream */
  gboolean has_pic;
} Char;

typedef struct {
  const guint8 *wd;  gsize wd_len;
  const guint8 *tb;  gsize tb_len;
  const guint8 *dt;  gsize dt_len;     /* the Data stream: pictures */
  guint16   nfib;
  guint32   fc_min, fc_mac;
  gint32    ccp_text;
  gint32    ccp_ftn;
  gint32    ccp_hdd, ccp_mcr, ccp_atn, ccp_edn;   /* the stories after them */
  GArray   *edn_refs;     /* guint32: the cps of the endnote references */
  guint32   cp_max;      /* one past the last cp the pieces cover */
  GArray   *pieces;       /* Piece */
  guint     piece_cursor; /* where the last cp lookup landed */
  GArray   *chpx_fc, *chpx_pn;
  GArray   *papx_fc, *papx_pn;
  guint     chpx_cursor, papx_cursor;   /* where the last fc lookups landed */
  GArray   *styles;       /* DocStyle */
  GPtrArray *fonts;       /* char* */
  int       default_ftc;  /* the stylesheet's standard font */
  GArray   *lfo_lsid;     /* guint32: each LFO's list, by ilfo - 1 */
  GArray   *lst_lsid;     /* guint32: each list's id */
  GArray   *lst_nfc;      /* guint8[9]: each list's number format per level */
} Doc;

static gboolean
in_wd (Doc *doc, gsize off, gsize n)
{
  return off <= doc->wd_len && n <= doc->wd_len - off;
}

static gboolean
in_tb (Doc *doc, gsize off, gsize n)
{
  return off <= doc->tb_len && n <= doc->tb_len - off;
}

/* fc and lcb of the FIB's rgfclcb entry `i`. */
static void
fib_fclcb (Doc *doc, guint i, guint32 *fc, guint32 *lcb)
{
  gsize off = 0x9A + 8 * (gsize) i;

  *fc = *lcb = 0;
  if (in_wd (doc, off, 8))
    {
      *fc  = rd32 (doc->wd + off);
      *lcb = rd32 (doc->wd + off + 4);
    }
}

/* ---- pieces ------------------------------------------------------------ */

static gboolean
read_pieces (Doc *doc, GError **error)
{
  guint32 fc, lcb, p, lcb_pcd;
  guint n;
  guint64 total = 0;

  doc->pieces = g_array_new (FALSE, FALSE, sizeof (Piece));

  fib_fclcb (doc, 33, &fc, &lcb);
  if (lcb == 0 || !in_tb (doc, fc, lcb))
    FAIL (error, "The document has no piece table.");

  /* The CLX: any number of Prc (property modifiers we do not need), then
   * the Pcdt with the PlcPcd. */
  p = fc;
  while (p < fc + lcb && doc->tb[p] == 0x01)
    {
      if (!in_tb (doc, p, 3))
        FAIL (error, "Piece table is damaged");
      p += 3 + rd16 (doc->tb + p + 1);
    }
  if (!in_tb (doc, p, 5) || doc->tb[p] != 0x02)
    FAIL (error, "Piece table is damaged");

  lcb_pcd = rd32 (doc->tb + p + 1);
  p += 5;
  if (lcb_pcd < 4 || !in_tb (doc, p, lcb_pcd))
    FAIL (error, "Piece table is damaged");

  n = (lcb_pcd - 4) / 12;
  for (guint i = 0; i < n; i++)
    {
      Piece piece;
      const guint8 *pcd = doc->tb + p + 4 * (n + 1) + 8 * i;
      guint32 raw = rd32 (pcd + 2);

      piece.cp_start = rd32 (doc->tb + p + 4 * i);
      piece.cp_end   = rd32 (doc->tb + p + 4 * (i + 1));
      /* FcCompressed: thirty bits of offset, bit 30 for fCompressed, and
       * a reserved bit 31 that is not part of the offset either way. */
      piece.compressed = (raw & 0x40000000u) != 0;
      piece.fc = piece.compressed ? (raw & 0x3FFFFFFFu) / 2 : (raw & 0x3FFFFFFFu);
      /* A piece can hold no more characters than the file has bytes
       * after its start. */
      if (piece.fc >= doc->wd_len || piece.cp_end < piece.cp_start)
        continue;
      /* The piece table is contiguous from cp 0: a gap, an overlap, or a
       * first piece starting elsewhere is a broken file, and the pieces so
       * far are all there is.  Without the start check a single piece at a
       * huge cp makes every later walk to cp_max spin billions of times. */
      if (doc->pieces->len == 0 && piece.cp_start != 0)
        break;
      if (doc->pieces->len > 0 &&
          piece.cp_start != g_array_index (doc->pieces, Piece, doc->pieces->len - 1).cp_end)
        break;
      if (piece.cp_end - piece.cp_start > doc->wd_len)
        piece.cp_end = piece.cp_start + (guint32) doc->wd_len;
      {
        guint32 room = (guint32) ((doc->wd_len - piece.fc) / (piece.compressed ? 1 : 2));

        if (piece.cp_end - piece.cp_start > room)
          piece.cp_end = piece.cp_start + room;
      }
      /* Each character of a Word file is stored once, so the pieces
       * together hold no more than the stream has bytes: pieces that all
       * point at the same text made a 260 KB file a document of a hundred
       * million characters. */
      total += piece.cp_end - piece.cp_start;
      if (total > MAX ((guint64) doc->wd_len * 2, (guint64) 1 << 20))
        break;
      g_array_append_val (doc->pieces, piece);
    }

  if (doc->pieces->len == 0)
    FAIL (error, "The document has no text pieces.");

  return TRUE;
}

/* The file offset of character `cp`, and whether it is 8-bit there. */
static gboolean
cp_to_fc (Doc *doc, guint32 cp, guint32 *fc, gboolean *compressed)
{
  guint n = doc->pieces->len;

  /* The text is walked in order, so look first where the last cp was
   * found: a file of a million one-character pieces would otherwise cost
   * their square, one full scan per character. */
  for (guint k = 0; k < n; k++)
    {
      guint i = doc->piece_cursor + k < n ? doc->piece_cursor + k
                                          : doc->piece_cursor + k - n;
      const Piece *piece = &g_array_index (doc->pieces, Piece, i);

      if (cp >= piece->cp_start && cp < piece->cp_end)
        {
          doc->piece_cursor = i;
          *fc = piece->fc + (cp - piece->cp_start) * (piece->compressed ? 1 : 2);
          *compressed = piece->compressed;
          return TRUE;
        }
    }
  return FALSE;
}

/* Windows-1252's upper half, which the 8-bit pieces use. */
static gunichar
cp1252 (guint8 c)
{
  static const guint16 high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
  };

  if (c >= 0x80 && c < 0xA0)
    return high[c - 0x80];
  return c;
}

static gunichar
char_at (Doc *doc, guint32 cp)
{
  guint32 fc = 0;
  gboolean compressed = FALSE;

  if (!cp_to_fc (doc, cp, &fc, &compressed))
    return 0;

  if (compressed)
    return in_wd (doc, fc, 1) ? cp1252 (doc->wd[fc]) : 0;

  if (!in_wd (doc, fc, 2))
    return 0;
  {
    guint16 u = rd16 (doc->wd + fc);

    /* A surrogate pair is two cps; take the pair as one character and
     * let the low half come out as nothing. */
    if (u >= 0xD800 && u < 0xDC00 && in_wd (doc, fc + 2, 2))
      {
        guint16 lo = rd16 (doc->wd + fc + 2);
        if (lo >= 0xDC00 && lo < 0xE000)
          return 0x10000 + (((gunichar) u - 0xD800) << 10) + (lo - 0xDC00);
      }
    /* Either half on its own is not a character, and must not become
     * invalid UTF-8 in the model. */
    if (u >= 0xD800 && u < 0xE000)
      return 0;
    return u;
  }
}

/* ---- bins and FKPs ------------------------------------------------------ */

static void
read_bins (Doc *doc, guint index, GArray **fcs, GArray **pns)
{
  guint32 fc, lcb;
  guint n;

  *fcs = g_array_new (FALSE, FALSE, sizeof (guint32));
  *pns = g_array_new (FALSE, FALSE, sizeof (guint32));

  fib_fclcb (doc, index, &fc, &lcb);
  if (lcb < 8 || !in_tb (doc, fc, lcb))
    return;

  n = (lcb - 4) / 8;
  for (guint i = 0; i <= n; i++)
    {
      guint32 v = rd32 (doc->tb + fc + 4 * i);
      g_array_append_val (*fcs, v);
    }
  for (guint i = 0; i < n; i++)
    {
      guint32 v = rd32 (doc->tb + fc + 4 * (n + 1) + 4 * i) & 0x3FFFFF;
      g_array_append_val (*pns, v);
    }
}

static const guint8 *
fkp_page (Doc *doc, GArray *fcs, GArray *pns, guint32 fc, guint *cursor)
{
  guint n = pns->len;

  /* Like cp_to_fc: the callers walk forwards, so start where the last
   * lookup landed rather than scanning every bin every time. */
  for (guint k = 0; k < n; k++)
    {
      guint i = *cursor + k < n ? *cursor + k : *cursor + k - n;

      if (i + 1 < fcs->len &&
          fc >= g_array_index (fcs, guint32, i) &&
          fc < g_array_index (fcs, guint32, i + 1))
        {
          gsize off = (gsize) g_array_index (pns, guint32, i) * 512;

          *cursor = i;
          return in_wd (doc, off, 512) ? doc->wd + off : NULL;
        }
    }
  return NULL;
}

/* The run of the FKP page that contains `fc`: its index, or -1. */
static int
fkp_run (const guint8 *page, guint32 fc, guint32 *run_end, guint max_runs)
{
  guint crun = page[511];

  if (crun == 0 || crun > max_runs)
    return -1;

  for (guint i = 0; i < crun; i++)
    {
      guint32 a = rd32 (page + 4 * i), b = rd32 (page + 4 * (i + 1));

      if (fc >= a && fc < b)
        {
          *run_end = b;
          return (int) i;
        }
    }
  return -1;
}

/* The CHPX grpprl for the character at `fc`, and where its run ends. */
static const guint8 *
chpx_at (Doc *doc, guint32 fc, guint *len, guint32 *run_end)
{
  const guint8 *page = fkp_page (doc, doc->chpx_fc, doc->chpx_pn, fc, &doc->chpx_cursor);
  int i;
  guint crun, off;

  *len = 0;
  *run_end = fc + 1;
  if (page == NULL)
    return NULL;

  /* A full CHPX page has 0x65 runs, which is more than a PAPX page can. */
  i = fkp_run (page, fc, run_end, 0x65);
  if (i < 0)
    return NULL;

  crun = page[511];
  off = page[4 * (crun + 1) + i];
  if (off == 0 || (gsize) off * 2 + 1 > 511)
    return NULL;

  *len = page[off * 2];
  if ((gsize) off * 2 + 1 + *len > 511)
    {
      *len = 0;
      return NULL;
    }
  return page + off * 2 + 1;
}

/* The PAPX grpprl for the paragraph whose end is at `fc`: the istd comes
 * first, then the sprms. */
static const guint8 *
papx_at (Doc *doc, guint32 fc, guint *len)
{
  const guint8 *page = fkp_page (doc, doc->papx_fc, doc->papx_pn, fc, &doc->papx_cursor);
  guint32 end;
  int i;
  guint crun, off, cb;

  *len = 0;
  if (page == NULL)
    return NULL;

  i = fkp_run (page, fc, &end, 100);
  if (i < 0)
    return NULL;

  crun = page[511];
  if (crun > 29)
    return NULL;        /* a PAPX page holds 29 runs at most */
  off = page[4 * (crun + 1) + 13 * i];
  if (off == 0 || (gsize) off * 2 + 2 > 511)
    return NULL;

  cb = page[off * 2];
  if (cb == 0)
    {
      cb = page[off * 2 + 1];
      if ((gsize) off * 2 + 2 + cb * 2 > 512)
        return NULL;
      *len = cb * 2;
      return page + off * 2 + 2;
    }
  if ((gsize) off * 2 + 1 + cb * 2 - 1 > 512)
    return NULL;
  *len = cb * 2 - 1;
  return page + off * 2 + 1;
}

/* ---- sprms ------------------------------------------------------------ */

/* How many bytes the operand of `sprm` takes, given the bytes after it. */
/* ---------------------------------------------------------------------- */
/* Summary information                                                     */
/* ---------------------------------------------------------------------- */

/* One property's string value.  A property set holds either bytes in a
 * code page (VT_LPSTR) or UTF-16 (VT_LPWSTR); both turn into UTF-8. */
static char *
property_string (const guint8 *d, gsize len, gsize at, guint codepage)
{
  guint32 type, n;

  if (at + 8 > len)
    return NULL;
  type = rd32 (d + at);
  n = rd32 (d + at + 4);

  if (type == 0x1E)          /* VT_LPSTR */
    {
      char *out;

      if (n == 0 || at + 8 + n > len || n > (1u << 20))
        return NULL;
      while (n > 0 && d[at + 8 + n - 1] == '\0')
        n--;
      if (codepage == 1200)  /* the "bytes" are really UTF-16 */
        {
          /* Copied first: `at` is the file's own offset and can be odd,
           * and a gunichar2 read wants its alignment. */
          gunichar2 *aligned = g_memdup2 (d + at + 8, (gsize) (n / 2) * 2);
          char *s = g_utf16_to_utf8 (aligned, n / 2, NULL, NULL, NULL);

          g_free (aligned);
          return s;
        }
      if (codepage == 65001)
        out = g_convert ((const char *) d + at + 8, n, "UTF-8", "UTF-8", NULL, NULL, NULL);
      else
        {
          /* The property set says its code page; a Greek or Russian
           * title is in that, not in Western Windows. */
          char *from = g_strdup_printf ("CP%u", codepage);

          out = g_convert ((const char *) d + at + 8, n, "UTF-8", from, NULL, NULL, NULL);
          g_free (from);
          if (out == NULL)
            out = g_convert ((const char *) d + at + 8, n, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL);
        }
      if (out == NULL)
        {
          /* Whatever the bytes were, what leaves here must be UTF-8. */
          char *raw = g_strndup ((const char *) d + at + 8, n);

          out = g_utf8_make_valid (raw, -1);
          g_free (raw);
        }
      return out;
    }
  if (type == 0x1F)          /* VT_LPWSTR */
    {
      gunichar2 *aligned;
      char *s;

      if (n == 0 || at + 8 + n * 2 > len || n > (1u << 20))
        return NULL;
      aligned = g_memdup2 (d + at + 8, (gsize) n * 2);
      s = g_utf16_to_utf8 (aligned, n, NULL, NULL, NULL);
      g_free (aligned);
      return s;
    }
  return NULL;
}

/* What File > Summary Info shows, out of the \005SummaryInformation
 * stream that every Word document carries. */
static void
read_summary (Ole *ole, W42PieceTable *pt)
{
  GByteArray *stream = ole_stream (ole, "\005SummaryInformation", NULL);
  const guint8 *d;
  gsize len;
  guint32 sections, section_at, count;
  guint codepage = 1252;
  char *value[5] = { NULL, NULL, NULL, NULL, NULL };
  gboolean any = FALSE;

  if (stream == NULL)
    return;
  d = stream->data;
  len = stream->len;

  if (len < 48 || rd16 (d) != 0xFFFE)
    {
      g_byte_array_free (stream, TRUE);
      return;
    }
  sections = rd32 (d + 24);
  if (sections < 1)
    {
      g_byte_array_free (stream, TRUE);
      return;
    }
  section_at = rd32 (d + 44);           /* after the first FMTID */
  /* Widened before the sum: two 32-bit offsets wrap where the pointer
   * arithmetic below would not, and the check must see what it sees. */
  if ((gsize) section_at + 8 > len)
    {
      g_byte_array_free (stream, TRUE);
      return;
    }
  count = rd32 (d + section_at + 4);
  if (count > 4096)
    count = 4096;

  /* The code page comes first, since the strings are read in it. */
  for (guint32 i = 0; i < count; i++)
    {
      gsize at = (gsize) section_at + 8 + (gsize) i * 8;

      if (at + 8 > len)
        break;
      if (rd32 (d + at) == 1)
        {
          gsize value_at = (gsize) section_at + rd32 (d + at + 4);

          if (value_at + 6 <= len && rd32 (d + value_at) == 2)   /* VT_I2 */
            codepage = rd16 (d + value_at + 4);
        }
    }

  for (guint32 i = 0; i < count; i++)
    {
      gsize at = (gsize) section_at + 8 + (gsize) i * 8;
      guint32 pid;
      gsize value_at;
      int slot = -1;

      if (at + 8 > len)
        break;
      pid = rd32 (d + at);
      value_at = (gsize) section_at + rd32 (d + at + 4);

      switch (pid)
        {
        case 2: slot = 0; break;      /* title */
        case 3: slot = 1; break;      /* subject */
        case 4: slot = 2; break;      /* author */
        case 5: slot = 3; break;      /* keywords */
        case 6: slot = 4; break;      /* comments */
        default: break;
        }
      if (slot < 0 || value[slot] != NULL || value_at >= len)
        continue;
      value[slot] = property_string (d, len, value_at, codepage);
      if (value[slot] != NULL && *value[slot] != '\0')
        any = TRUE;
    }

  if (any)
    {
      W42DocInfo info;

      memset (&info, 0, sizeof info);
      info.title    = value[0];
      info.subject  = value[1];
      info.author   = value[2];
      info.keywords = value[3];
      info.comments = value[4];
      w42_pt_set_info (pt, &info);
    }

  for (guint i = 0; i < G_N_ELEMENTS (value); i++)
    g_free (value[i]);
  g_byte_array_free (stream, TRUE);
}

/* Which of Word's sixteen highlights a colour is nearest to.  White, and
 * no colour at all, mean no highlight. */
static int
doc_nearest_highlight (guint32 want)
{
  int best = 0;
  long best_away = 0;

  if (want == 0xFFFFFF)
    return 0;
  for (int i = 1; i <= 16; i++)
    {
      guint32 c = w42_highlight_rgb (i);
      long dr = (long) ((c >> 16) & 0xFF) - (long) ((want >> 16) & 0xFF);
      long dg = (long) ((c >> 8) & 0xFF) - (long) ((want >> 8) & 0xFF);
      long db = (long) (c & 0xFF) - (long) (want & 0xFF);
      long away = dr * dr + dg * dg + db * db;

      if (best == 0 || away < best_away)
        {
          best = i;
          best_away = away;
        }
    }
  return best;
}

static guint
sprm_operand_len (guint16 sprm, const guint8 *p, guint avail)
{
  switch (sprm >> 13)
    {
    case 0: case 1: return 1;
    case 2: case 4: case 5: return 2;
    case 3: return 4;
    case 7: return 3;
    default:
      if (avail == 0)
        return 0;
      if (sprm == 0xD608 || sprm == 0xD606)
        return avail >= 2 ? (guint) rd16 (p) + 1 : avail;
      return (guint) p[0] + 1;
    }
}

static void
apply_toggle (int *flag, guint8 v)
{
  if (v == 0 || v == 1)
    *flag = v;
  else if (v == 129)
    *flag = !*flag;
}

/* The paragraph's row shape, made when a table sprm first needs it. */
static RowShape *
para_row (Para *pa)
{
  if (pa->row == NULL)
    pa->row = g_new0 (RowShape, 1);
  return pa->row;
}

static void
apply_papx (const guint8 *grpprl, guint len, Para *pa)
{
  guint p = 0;

  while (p + 2 <= len)
    {
      guint16 sprm = rd16 (grpprl + p);
      const guint8 *op = grpprl + p + 2;
      guint avail = len - p - 2;
      guint olen = sprm_operand_len (sprm, op, avail);

      if (olen > avail)
        break;

      switch (sprm)
        {
        case 0x2403: case 0x2461: pa->jc = op[0]; break;
        case 0x840F: case 0x845E: pa->dxa_left = rd16s (op); break;
        case 0x840E: case 0x845D: pa->dxa_right = rd16s (op); break;
        case 0x8411: case 0x8460: pa->dxa_left1 = rd16s (op); break;
        case 0xA413: pa->dya_before = rd16 (op); break;
        case 0xA414: pa->dya_after = rd16 (op); break;
        case 0x6412:
          pa->dya_line = rd16s (op);
          pa->f_mult = rd16s (op + 2);
          break;
        case 0x2416: pa->in_table = op[0] != 0; break;
        case 0x2417: pa->ttp = op[0] != 0; break;
        case 0x2407: pa->page_break = op[0] != 0; break;
        case 0x2406: pa->keep_next = op[0] != 0; break;
        case 0x2405: pa->keep_together = op[0] != 0; break;
        case 0x2431: pa->widow = op[0] != 0; break;
        case 0x2441: pa->bidi = op[0] != 0; break;
        case 0x4600: pa->istd = rd16 (op); break;
        case 0x460B: pa->ilfo = rd16s (op); break;
        case 0x260A: pa->ilvl = op[0]; break;

        case 0xC60D:
          /* sprmPChgTabsPapx: a count of stops to delete and their
           * positions, then a count to add, their positions, and one
           * TBD byte each -- the kind in the low three bits, the leader
           * in the next three. */
          if (olen >= 3)
            {
              guint del = op[1];
              guint add_at = 2 + del * 2;

              if (add_at < olen)
                {
                  guint add = op[add_at];
                  guint pos_at = add_at + 1;
                  guint tbd_at = pos_at + add * 2;

                  /* A change to the stops there are -- the style's --
                   * not a list of its own: the deleted ones go, then the
                   * added ones go in where they sort, a stop at the same
                   * place replaced. */
                  for (guint d = 0; d < del && 2 + d * 2 + 1 < olen; d++)
                    {
                      int gone = rd16s (op + 2 + d * 2);

                      for (guint t = 0; t < pa->n_tabs; t++)
                        if (pa->tab_pos[t] == gone)
                          {
                            memmove (pa->tab_pos + t, pa->tab_pos + t + 1,
                                     (pa->n_tabs - t - 1) * sizeof pa->tab_pos[0]);
                            memmove (pa->tab_kind + t, pa->tab_kind + t + 1,
                                     (pa->n_tabs - t - 1) * sizeof pa->tab_kind[0]);
                            pa->n_tabs--;
                            break;
                          }
                    }
                  for (guint i = 0; i < add &&
                                    tbd_at + i < olen && pos_at + i * 2 + 1 < olen; i++)
                    {
                      int at = rd16s (op + pos_at + i * 2);
                      guint8 tbd = op[tbd_at + i];
                      W42TabKind kind;
                      W42TabLeader leader;
                      guint t;

                      switch (tbd & 0x07)
                        {
                        case 1:  kind = W42_TAB_CENTER;  break;
                        case 2:  kind = W42_TAB_RIGHT;   break;
                        case 3:  kind = W42_TAB_DECIMAL; break;
                        default: kind = W42_TAB_LEFT;    break;
                        }
                      switch ((tbd >> 3) & 0x07)
                        {
                        case 1:  leader = W42_TAB_LEAD_DOT;  break;
                        case 2:  leader = W42_TAB_LEAD_DASH; break;
                        case 3: case 4: leader = W42_TAB_LEAD_LINE; break;
                        default: leader = W42_TAB_LEAD_NONE; break;
                        }
                      if (at <= 0)
                        continue;
                      for (t = 0; t < pa->n_tabs && pa->tab_pos[t] < at; t++)
                        ;
                      if (t < pa->n_tabs && pa->tab_pos[t] == at)
                        {
                          pa->tab_kind[t] = W42_TAB_BYTE (kind, leader);
                          continue;
                        }
                      if (pa->n_tabs >= W42_MAX_TABS)
                        continue;
                      memmove (pa->tab_pos + t + 1, pa->tab_pos + t,
                               (pa->n_tabs - t) * sizeof pa->tab_pos[0]);
                      memmove (pa->tab_kind + t + 1, pa->tab_kind + t,
                               (pa->n_tabs - t) * sizeof pa->tab_kind[0]);
                      pa->tab_pos[t] = at;
                      pa->tab_kind[t] = W42_TAB_BYTE (kind, leader);
                      pa->n_tabs++;
                    }
                }
            }
          break;

        case 0xC64E: case 0xC64F: case 0xC650: case 0xC651:
          /* sprmPBrcTop, Left, Bottom and Right: a BRC each, whose second
           * half holds the line's width in eighths of a point.  A width of
           * nothing, or the "no border" line style, means no border. */
          {
            static const guint8 EDGES[] = { W42_EDGE_TOP, W42_EDGE_LEFT,
                                            W42_EDGE_BOTTOM, W42_EDGE_RIGHT };
            const guint8 *brc = olen > 8 ? op + 1 : op;   /* cb, then the BRC */
            guint brc_len = olen > 8 ? olen - 1 : olen;
            int e = EDGES[sprm - 0xC64E];

            if (brc_len >= 8 && brc_edge (brc, &pa->edge[e]))
              pa->border |= (guint8) (1 << e);
            else if (brc_len >= 8)
              pa->border &= (guint8) ~(1 << e);
          }
          break;

        case 0x6424: case 0x6425: case 0x6426: case 0x6427:
          /* sprmPBrcTop80 and its neighbours: the Word 97 form, a BRC80. */
          {
            static const guint8 EDGES[] = { W42_EDGE_TOP, W42_EDGE_LEFT,
                                            W42_EDGE_BOTTOM, W42_EDGE_RIGHT };
            int e = EDGES[sprm - 0x6424];

            if (olen >= 4 && brc80_edge (op, &pa->edge[e]))
              pa->border |= (guint8) (1 << e);
            else if (olen >= 4)
              pa->border &= (guint8) ~(1 << e);
          }
          break;

        case 0x9407:
          /* sprmTDyaRowHeight: the row's height, negative for "exactly". */
          if (olen >= 2)
            para_row (pa)->dya_row_height = rd16s (op);
          break;
        case 0x3404:
          /* sprmTTableHeader: the row repeats at the top of every page. */
          if (olen >= 1)
            para_row (pa)->table_header = op[0] != 0;
          break;
        case 0xD605:
          /* sprmTTableBorders80: six BRC80s, top, left, bottom, right,
           * then between the rows and between the columns. */
          if (olen >= 25)
            {
              static const int EDGES[6] = { W42_EDGE_TOP, W42_EDGE_LEFT, W42_EDGE_BOTTOM,
                                            W42_EDGE_RIGHT, W42_EDGE_INSIDE_H, W42_EDGE_INSIDE_V };
              RowShape *row = para_row (pa);

              for (int e = 0; e < 6; e++)
                brc80_edge (op + 1 + 4 * e, &row->tbl_edge[EDGES[e]]);
              row->has_tbl_edge = TRUE;
            }
          break;
        case 0xD613:
          /* sprmTTableBorders: the same, as the later eight-byte BRC. */
          if (olen >= 49)
            {
              static const int EDGES[6] = { W42_EDGE_TOP, W42_EDGE_LEFT, W42_EDGE_BOTTOM,
                                            W42_EDGE_RIGHT, W42_EDGE_INSIDE_H, W42_EDGE_INSIDE_V };
              RowShape *row = para_row (pa);

              for (int e = 0; e < 6; e++)
                brc_edge (op + 1 + 8 * e, &row->tbl_edge[EDGES[e]]);
              row->has_tbl_edge = TRUE;
            }
          break;
        case 0xD620: case 0xD62F:
          /* sprmTSetBrc80 and sprmTSetBrc: one line for the sides named
           * in grfbrc of the cells itcFirst to itcLim. */
          if (olen >= 4)
            {
              int first = op[1], lim = op[2];
              guint grf = op[3];
              RowShape *row = para_row (pa);
              W42BorderEdge edge;
              gboolean on = sprm == 0xD620 ? (olen >= 8 && brc80_edge (op + 4, &edge))
                                           : (olen >= 12 && brc_edge (op + 4, &edge));

              for (int c = first; c < lim && c < 64; c++)
                for (int side = 0; side < 4; side++)
                  {
                    /* grfbrc: top, left, bottom, right. */
                    static const int EDGES[4] = { W42_EDGE_TOP, W42_EDGE_LEFT, W42_EDGE_BOTTOM, W42_EDGE_RIGHT };

                    if (!(grf & (1u << side)))
                      continue;
                    if (on)
                      {
                        row->cell_sides[c] |= (guint8) (1 << EDGES[side]);
                        row->cell_edge[c][EDGES[side]] = edge;
                      }
                    else
                      row->cell_sides[c] &= (guint8) ~(1 << EDGES[side]);
                  }
            }
          break;

        case 0xC64D: case 0x442D:
          /* sprmPShd, and its Word 97 form sprmPShd80: the paragraph's
           * background, as a shading percentage or a colour.  Either way
           * what the model wants is the grey. */
          if (sprm == 0xC64D && olen >= 11)
            {
              guint32 back = rd32 (op + 5);

              if ((back >> 24) != 0xFF)
                {
                  pa->shading_color = ((back & 0xFF) << 16) | (back & 0xFF00) |
                                      ((back >> 16) & 0xFF);
                  pa->has_shading_color = 1;
                }
            }
          else if (olen >= 2)
            {
              /* The SHD80: a pattern, which for Word's plain shades is
               * a percentage by this table, and a background colour
               * index for the clear pattern.  ipat / 50 made every one
               * of them nought. */
              static const guint8 PCT[14] = { 0, 100, 5, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90 };
              guint shd = rd16 (op);
              guint ipat = shd >> 10, back = (shd >> 5) & 0x1F;

              if (ipat < G_N_ELEMENTS (PCT))
                pa->shading = PCT[ipat];
              if (ipat == 0 && back > 0 && back <= 16)
                {
                  pa->shading_color = w42_highlight_rgb ((int) back);
                  pa->has_shading_color = 1;
                }
            }
          break;
        case 0xD608:
          /* TDefTable: cb, itcMac, then itcMac+1 column edges, then one
           * twenty-byte TC80 per cell.  A TC80 is two bytes of flags, two
           * unused, and the cell's four BRC80 borders. */
          if (olen >= 3)
            {
              int itc = op[2];
              guint edges;
              RowShape *row = para_row (pa);

              if (itc > 63) itc = 63;
              edges = 3u + 2u * ((guint) itc + 1);
              if (olen >= edges)
                {
                  row->itc_mac = itc;
                  for (int c = 0; c <= itc; c++)
                    row->cellx[c] = rd16s (op + 3 + 2 * c);
                }
              for (int c = 0; c < itc && olen >= edges + 20u * (guint) (c + 1); c++)
                {
                  static const int EDGES[] = { W42_EDGE_TOP, W42_EDGE_LEFT,
                                               W42_EDGE_BOTTOM, W42_EDGE_RIGHT };
                  const guint8 *tc = op + edges + 20 * c;

                  row->cell_flags[c] = rd16 (tc);
                  for (int side = 0; side < 4; side++)
                    {
                      const guint8 *brc = tc + 4 + 4 * side;

                      if (brc80_edge (brc, &row->cell_edge[c][EDGES[side]]))
                        row->cell_sides[c] |= (guint8) (1 << EDGES[side]);
                    }
                }
            }
          break;
        case 0xD612:
          /* sprmTDefTableShd: cb, then one ten-byte SHD per cell in column
           * order.  A background of "automatic" leaves the cell as the
           * page; anything else is the colour it is filled with.  Word 97's
           * own sprmTDefTableShd80 (0xD609) could only name one of sixteen
           * palette colours, so it is left alone: a file that has it also
           * has this. */
          {
            guint n = olen > 0 ? (guint) (olen - 1) / 10 : 0;

            for (guint c = 0; c < n && c < 64; c++)
              {
                const guint8 *shd = op + 1 + 10 * c;
                guint32 back = rd32 (shd + 4);

                /* A COLORREF is red, green, blue, then 0xFF for
                 * "automatic"; the model wants 0x00RRGGBB. */
                if ((back >> 24) != 0xFF)
                  {
                    RowShape *row = para_row (pa);

                    row->cell_shade[c] = ((back & 0xFF) << 16) |
                                         (back & 0xFF00) |
                                         ((back >> 16) & 0xFF);
                    row->has_cell_shade[c] = 1;
                  }
              }
          }
          break;
        default:
          break;
        }

      p += 2 + olen;
    }
}

static void resolve_style (Doc *doc, int istd, Para *pa, Char *ch, int depth);

static void
apply_chpx (Doc *doc, const guint8 *grpprl, guint len, Char *ch, int depth)
{
  guint p = 0;

  while (p + 2 <= len)
    {
      guint16 sprm = rd16 (grpprl + p);
      const guint8 *op = grpprl + p + 2;
      guint avail = len - p - 2;
      guint olen = sprm_operand_len (sprm, op, avail);

      if (olen > avail)
        break;

      switch (sprm)
        {
        case 0x0835: apply_toggle (&ch->bold, op[0]); break;
        case 0x0836: apply_toggle (&ch->italic, op[0]); break;
        case 0x0837: apply_toggle (&ch->strike, op[0]); break;
        case 0x0838: apply_toggle (&ch->outline, op[0]); break;
        case 0x0839: apply_toggle (&ch->shadow, op[0]); break;
        case 0x0854: apply_toggle (&ch->imprint, op[0]); break;
        case 0x0858: apply_toggle (&ch->emboss, op[0]); break;
        case 0x2A53: ch->dstrike = op[0] != 0; break;   /* sprmCFDStrike: a byte, not a toggle */
        case 0x083A: apply_toggle (&ch->smallcaps, op[0]); break;
        case 0x083B: apply_toggle (&ch->allcaps, op[0]); break;
        case 0x083C: apply_toggle (&ch->vanish, op[0]); break;
        case 0x0800: apply_toggle (&ch->rmark_del, op[0]); break;
        case 0x0801: apply_toggle (&ch->rmark_ins, op[0]); break;
        case 0x4A30:
          /* sprmCIstd: the run wears a character style -- Emphasis,
           * Strong, Hyperlink -- and the sprms after this one are its
           * differences from it. */
          if (depth == 0)
            resolve_style (doc, rd16 (op), NULL, ch, 1);
          break;
        case 0xCA71:
          /* sprmCShd: a run's background as a colour, which is what a
           * highlight is when it is not one of Word's sixteen names.
           * The operand is its count, then the Shd: the foreground
           * COLORREF, the background COLORREF -- red, green, blue,
           * fAuto -- and the pattern.  An automatic background is none. */
          if (ch->highlight == 0 && olen >= 11 && op[8] != 0xFF)
            {
              guint32 rgb = ((guint32) op[5] << 16) | ((guint32) op[6] << 8) | op[7];

              ch->highlight = (guint8) doc_nearest_highlight (rgb);
            }
          break;
        case 0x4866:
          /* sprmCShd80: the old Shd80, its background one of the sixteen
           * colours by index in bits 5 to 9, 0 for automatic. */
          if (ch->highlight == 0 && olen >= 2)
            {
              int ico = (rd16 (op) >> 5) & 0x1F;

              if (ico > 0 && ico <= 16)
                ch->highlight = ico;
            }
          break;
        case 0x2A0C:
          /* sprmCHighlight: Word's highlighter, one of its sixteen by
           * index; a shading read into the highlight gives way to it. */
          ch->highlight = op[0] <= 16 ? op[0] : 0;
          break;
        case 0x8840: ch->dxa_space = rd16s (op); break;
        case 0x2A3E: ch->kul = op[0]; break;
        case 0x2A42: ch->ico = op[0]; ch->has_rgb = FALSE; break;
        case 0x6870:
          if (op[3] == 0xFF)      /* fAuto */
            ch->has_rgb = FALSE;
          else
            {
              ch->rgb = ((guint32) op[0] << 16) | ((guint32) op[1] << 8) | op[2];
              ch->has_rgb = TRUE;
            }
          break;
        case 0x4A43: ch->hps = rd16 (op); break;
        case 0x4A4F: ch->ftc = rd16 (op); break;
        case 0x2A48: ch->iss = op[0]; break;
        case 0x0855: ch->spec = op[0] != 0; break;
        case 0x6A03: ch->pic_fc = rd32 (op); ch->has_pic = TRUE; break;
        default:
          break;
        }

      p += 2 + olen;
    }
}

/* ---- styles ----------------------------------------------------------- */

static void
read_styles (Doc *doc)
{
  guint32 fc, lcb;
  guint cb_stshi, cstd, cb_base, p;

  doc->styles = g_array_new (FALSE, TRUE, sizeof (DocStyle));

  fib_fclcb (doc, 1, &fc, &lcb);
  if (lcb < 6 || !in_tb (doc, fc, lcb))
    return;

  cb_stshi = rd16 (doc->tb + fc);
  cstd     = rd16 (doc->tb + fc + 2);
  cb_base  = rd16 (doc->tb + fc + 4);
  p = fc + 2 + cb_stshi;

  /* The stylesheet names the document's standard font, which is what
   * text gets when no style or run says otherwise. */
  if (cb_stshi >= 14 && in_tb (doc, fc + 14, 2))
    doc->default_ftc = rd16 (doc->tb + fc + 2 + 12);

  for (guint i = 0; i < cstd && i < 4096; i++)
    {
      DocStyle style;
      guint cb, name_len, q, upx_end;
      const guint8 *std;

      memset (&style, 0, sizeof style);
      style.istd_base = 0xFFF;

      if (!in_tb (doc, p, 2) || p + 2 > fc + lcb)
        break;
      cb = rd16 (doc->tb + p);
      p += 2;
      if (cb == 0)
        {
          g_array_append_val (doc->styles, style);
          continue;
        }
      if (!in_tb (doc, p, cb))
        break;
      if (cb < MAX (cb_base + 2, 4))
        {
          /* Too short to be a style; its place is kept, since every
           * paragraph names its style by its number. */
          g_array_append_val (doc->styles, style);
          p += cb;
          continue;
        }

      std = doc->tb + p;
      style.sti       = rd16 (std) & 0xFFF;
      style.sgc       = rd16 (std + 2) & 0xF;
      style.istd_base = (rd16 (std + 2) >> 4) & 0xFFF;

      name_len = rd16 (std + cb_base);
      q = cb_base + 2 + 2 * name_len + 2;   /* past the name and its null */
      q = (q + 1) & ~1u;

      if (style.sgc == 1 && q + 2 <= cb)
        {
          guint cb_upx = rd16 (std + q);

          upx_end = q + 2 + cb_upx;
          if (upx_end <= cb && cb_upx >= 2)
            {
              style.papx = std + q + 2 + 2;   /* past the istd */
              style.papx_len = cb_upx - 2;
            }
          q = (upx_end + 1) & ~1u;
        }
      if (q + 2 <= cb)
        {
          guint cb_upx = rd16 (std + q);

          if (q + 2 + cb_upx <= cb)
            {
              style.chpx = std + q + 2;
              style.chpx_len = cb_upx;
            }
        }

      g_array_append_val (doc->styles, style);
      p += cb;
    }
}

/* The paragraph style's own and inherited properties. */
static void
resolve_style (Doc *doc, int istd, Para *pa, Char *ch, int depth)
{
  const DocStyle *style;

  if (istd < 0 || (guint) istd >= doc->styles->len || depth > 12)
    return;

  style = &g_array_index (doc->styles, DocStyle, istd);
  if (style->istd_base != 0xFFF && style->istd_base != istd)
    resolve_style (doc, style->istd_base, pa, ch, depth + 1);

  if (pa != NULL && style->papx != NULL)
    apply_papx (style->papx, style->papx_len, pa);
  if (ch != NULL && style->chpx != NULL)
    apply_chpx (doc, style->chpx, style->chpx_len, ch, depth + 1);
}

static const char *
style_name_for (Doc *doc, int istd)
{
  const DocStyle *style;

  if (istd < 0 || (guint) istd >= doc->styles->len)
    return "Normal";

  style = &g_array_index (doc->styles, DocStyle, istd);
  switch (style->sti)
    {
    case 1: return "Heading 1";
    case 2: return "Heading 2";
    case 3: case 4: case 5: case 6: case 7: case 8: case 9: return "Heading 3";
    case 62: return "Title";
    default: return "Normal";
    }
}

/* ---- fonts ------------------------------------------------------------ */

static void
read_fonts (Doc *doc)
{
  guint32 fc, lcb;
  guint n, p;

  doc->fonts = g_ptr_array_new_with_free_func (g_free);

  fib_fclcb (doc, 15, &fc, &lcb);
  if (lcb < 4 || !in_tb (doc, fc, lcb))
    return;

  n = rd16 (doc->tb + fc);
  p = fc + 4;
  for (guint i = 0; i < n && i < 1024; i++)
    {
      guint cb;
      GString *name = g_string_new (NULL);

      if (!in_tb (doc, p, 1))
        break;
      cb = doc->tb[p];
      if (!in_tb (doc, p + 1, cb))
        break;

      /* ffid, weight, charset, alternate index, PANOSE and font signature
       * come to 39 bytes; the name follows, in UTF-16. */
      for (guint j = 39; j + 1 < cb; j += 2)
        {
          guint16 c = rd16 (doc->tb + p + 1 + j);
          if (c == 0)
            break;
          if (c >= 0xD800 && c < 0xE000)   /* half a pair is not a character */
            continue;
          g_string_append_unichar (name, c);
        }
      g_ptr_array_add (doc->fonts, g_string_free (name, FALSE));
      p += 1 + cb;
    }
}

/* ---- page setup ------------------------------------------------------- */

static void
read_page_setup (Doc *doc, W42PageSetup *page)
{
  guint32 fc, lcb, fc_sepx;
  guint n, cb, p;

  if (page == NULL)
    return;

  fib_fclcb (doc, 6, &fc, &lcb);
  if (lcb < 4 + 4 + 12 || !in_tb (doc, fc, lcb))
    return;

  n = (lcb - 4) / 16;
  if (n == 0)
    return;

  /* The first section's SEPX: fcSepx sits 2 bytes into its Sed. */
  fc_sepx = rd32 (doc->tb + fc + 4 * (n + 1) + 2);
  if (fc_sepx == 0xFFFFFFFFu || !in_wd (doc, fc_sepx, 2))
    return;

  cb = rd16 (doc->wd + fc_sepx);
  if (!in_wd (doc, fc_sepx + 2, cb))
    return;

  p = 0;
  while (p + 2 <= cb)
    {
      const guint8 *g = doc->wd + fc_sepx + 2;
      guint16 sprm = rd16 (g + p);
      const guint8 *op = g + p + 2;
      guint avail = cb - p - 2;
      guint olen = sprm_operand_len (sprm, op, avail);

      if (olen > avail)
        break;

      switch (sprm)
        {
        case 0xB01F: page->width = rd16 (op); break;
        case 0xB020: page->height = rd16 (op); break;
        case 0xB021: page->margin_left = rd16 (op); break;
        case 0xB022: page->margin_right = rd16 (op); break;
        case 0x9023: page->margin_top = rd16s (op); break;
        case 0x9024: page->margin_bottom = rd16s (op); break;
        case 0x500B: page->columns = rd16 (op) + 1; break;     /* ccolM1 */
        case 0x900C: page->column_gap = rd16 (op); break;
        default: break;
        }
      p += 2 + olen;
    }

  if (page->width < 1440 || page->height < 1440)
    {
      page->width = 12240;
      page->height = 15840;
    }
}

/* ---------------------------------------------------------------------- */
/* Building the document                                                   */
/* ---------------------------------------------------------------------- */

typedef struct {
  guint32 cp_start;      /* first character */
  guint32 cp_end;        /* the paragraph mark or cell mark */
  Para    pa;
  int     istd;
} DocPara;

static void
doc_para_clear (gpointer data)
{
  g_free (((DocPara *) data)->pa.row);
}

static void
para_defaults (Para *pa)
{
  memset (pa, 0, sizeof *pa);
  pa->f_mult = 1;
  pa->dya_line = 240;
  pa->widow = TRUE;
}

static void
char_defaults (Char *ch)
{
  memset (ch, 0, sizeof *ch);
  ch->hps = 20;
  ch->ftc = -1;      /* the document's standard font, unless a sprm says */
}

static guint32
word_colour (int ico)
{
  static const guint32 table[17] = {
    0x000000, 0x000000, 0x0000FF, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000,
    0xFFFF00, 0xFFFFFF, 0x000080, 0x008080, 0x008000, 0x800080, 0x800000,
    0x808000, 0x808080, 0xC0C0C0,
  };
  return (ico >= 0 && ico < 17) ? table[ico] : 0;
}

static void
fill_char_fmt (Doc *doc, const Char *ch, W42CharFmt *out)
{
  const char *family = "Times New Roman";
  int ftc = ch->ftc >= 0 ? ch->ftc : doc->default_ftc;

  if (ftc >= 0 && (guint) ftc < doc->fonts->len)
    {
      const char *name = g_ptr_array_index (doc->fonts, ftc);
      if (name != NULL && *name != '\0')
        family = name;
    }

  out->family    = g_intern_string (family);
  out->size      = ch->hps > 0 ? CLAMP (ch->hps, 2, 3276) : 20;
  out->bold      = ch->bold ? 1 : 0;
  out->italic    = ch->italic ? 1 : 0;
  /* Word's kul: 1 single, 2 words only, 3 double, 4 dotted, 6 thick,
   * 7 and 9 dashed, 11 wave. */
  switch (ch->kul)
    {
    case 0:  out->underline = W42_UNDERLINE_NONE;   break;
    case 2:  out->underline = W42_UNDERLINE_WORDS;  break;
    case 3:  out->underline = W42_UNDERLINE_DOUBLE; break;
    case 4:  out->underline = W42_UNDERLINE_DOTTED; break;
    case 6:  out->underline = W42_UNDERLINE_THICK;  break;
    case 7: case 9: case 10: out->underline = W42_UNDERLINE_DASHED; break;
    case 11: case 27: out->underline = W42_UNDERLINE_WAVE; break;
    default: out->underline = W42_UNDERLINE_SINGLE; break;
    }
  out->strikeout = ch->strike ? 1 : 0;
  out->dstrike   = ch->dstrike ? 1 : 0;
  out->shadow    = ch->shadow ? 1 : 0;
  out->outline   = ch->outline ? 1 : 0;
  out->emboss    = ch->emboss ? 1 : 0;
  out->engrave   = ch->imprint ? 1 : 0;
  out->script    = ch->iss == 1 ? 1 : ch->iss == 2 ? -1 : 0;
  out->smallcaps = ch->smallcaps ? 1 : 0;
  out->allcaps   = ch->allcaps ? 1 : 0;
  out->highlight = (guint8) CLAMP (ch->highlight, 0, 16);
  out->spacing   = (gint16) CLAMP (ch->dxa_space, -720, 720);
  out->color     = ch->has_rgb ? ch->rgb : word_colour (ch->ico);
}

/* ---- lists -------------------------------------------------------------- */

/* Word 97's lists: PlfLst holds the list definitions (LSTF, then each
 * one's levels, LVL), PlfLfo the list format overrides paragraphs refer
 * to by ilfo, each naming a list by its id.  What word42 wants from all
 * that is whether a level is numbered or bulleted: nfc 23 is a bullet. */
static void
read_lists (Doc *doc)
{
  guint32 fc, lcb;

  doc->lfo_lsid = g_array_new (FALSE, FALSE, sizeof (guint32));
  doc->lst_lsid = g_array_new (FALSE, FALSE, sizeof (guint32));
  doc->lst_nfc  = g_array_new (FALSE, TRUE, 9);

  fib_fclcb (doc, 73, &fc, &lcb);          /* fcPlfLst */
  if (lcb >= 2 && in_tb (doc, fc, lcb))
    {
      const guint8 *p = doc->tb + fc, *end = p + lcb;
      int n_lst = (gint16) rd16 (p);
      GArray *simple = g_array_new (FALSE, FALSE, sizeof (gboolean));

      p += 2;
      for (int i = 0; i < n_lst && p + 28 <= end; i++)
        {
          guint32 lsid = rd32 (p);
          gboolean is_simple = (p[26] & 0x01) != 0;
          guint8 nfc[9] = { 0 };

          g_array_append_val (doc->lst_lsid, lsid);
          g_array_append_vals (doc->lst_nfc, nfc, 1);
          g_array_append_val (simple, is_simple);
          p += 28;
        }
      /* The levels follow all the LSTFs, in the same order -- and past
       * lcbPlfLst, which counts the LSTFs only. */
      end = doc->tb + doc->tb_len;
      for (guint i = 0; i < doc->lst_lsid->len && p < end; i++)
        {
          int levels = g_array_index (simple, gboolean, i) ? 1 : 9;
          guint8 *nfc = &g_array_index (doc->lst_nfc, guint8, i * 9);

          for (int l = 0; l < levels && p + 28 <= end; l++)
            {
              guint8 cb_chpx = p[24], cb_papx = p[25];
              guint16 cch;

              nfc[l] = p[4];
              p += 28 + cb_papx + cb_chpx;
              if (p + 2 > end)
                break;
              cch = rd16 (p);
              p += 2 + 2 * (gsize) cch;
            }
          if (levels == 1)
            for (int l = 1; l < 9; l++)
              nfc[l] = nfc[0];
        }
      g_array_free (simple, TRUE);
    }

  fib_fclcb (doc, 74, &fc, &lcb);          /* fcPlfLfo */
  if (lcb >= 4 && in_tb (doc, fc, lcb))
    {
      const guint8 *p = doc->tb + fc, *end = p + lcb;
      gint32 n_lfo = (gint32) rd32 (p);

      p += 4;
      for (gint32 i = 0; i < n_lfo && p + 16 <= end; i++)
        {
          guint32 lsid = rd32 (p);

          g_array_append_val (doc->lfo_lsid, lsid);
          p += 16;
        }
    }
}

static W42ListKind
list_kind_for (Doc *doc, int ilfo, int ilvl)
{
  guint32 lsid;

  if (ilfo <= 0)
    return W42_LIST_NONE;
  if (doc->lfo_lsid == NULL || (guint) ilfo > doc->lfo_lsid->len)
    return W42_LIST_BULLET;        /* a list whose definition is missing */
  lsid = g_array_index (doc->lfo_lsid, guint32, ilfo - 1);
  for (guint i = 0; i < doc->lst_lsid->len; i++)
    if (g_array_index (doc->lst_lsid, guint32, i) == lsid)
      {
        guint8 nfc = g_array_index (doc->lst_nfc, guint8, i * 9 + CLAMP (ilvl, 0, 8));

        switch (nfc)
          {
          case 1:  return W42_LIST_UPPER_ROMAN;
          case 2:  return W42_LIST_LOWER_ROMAN;
          case 3:  return W42_LIST_UPPER_LETTER;
          case 4:  return W42_LIST_LOWER_LETTER;
          case 23: return W42_LIST_BULLET;
          default: return W42_LIST_NUMBER;
          }
      }
  return W42_LIST_BULLET;
}

static void
fill_para_fmt (Doc *doc, const DocPara *dp, W42ParaFmt *out)
{
  const Para *pa = &dp->pa;

  out->style = g_intern_string (style_name_for (doc, dp->istd));
  switch (pa->jc)
    {
    case 1: out->align = W42_ALIGN_CENTER; break;
    case 2: out->align = W42_ALIGN_RIGHT; break;
    case 3: case 4: out->align = W42_ALIGN_JUSTIFY; break;
    default: out->align = W42_ALIGN_LEFT; break;
    }
  out->indent_left  = pa->dxa_left;
  out->indent_right = pa->dxa_right;
  out->indent_first = pa->dxa_left1;
  out->space_before = pa->dya_before;
  out->space_after  = pa->dya_after;
  out->line_spacing = 0;
  out->line_spacing_pct = 0;
  if (pa->f_mult && pa->dya_line > 0 && pa->dya_line != 240)
    out->line_spacing_pct = pa->dya_line * 100 / 240;
  else if (!pa->f_mult && pa->dya_line != 0)
    out->line_spacing = ABS (pa->dya_line);
  out->page_break_before = pa->page_break ? 1 : 0;
  out->keep_next     = pa->keep_next ? 1 : 0;
  out->keep_together = pa->keep_together ? 1 : 0;
  out->widow_control = pa->widow ? 1 : 0;
  out->rtl = pa->bidi ? 1 : 0;
  out->n_tabs = MIN (pa->n_tabs, W42_MAX_TABS);
  memcpy (out->tab_pos, pa->tab_pos, sizeof out->tab_pos);
  memcpy (out->tab_kind, pa->tab_kind, sizeof out->tab_kind);
  out->border = pa->border;
  memcpy (out->edge, pa->edge, sizeof out->edge);
  out->shading = pa->shading;
  out->shading_color = pa->shading_color;
  out->has_shading_color = pa->has_shading_color;
  out->list = list_kind_for (doc, pa->ilfo, pa->ilvl);
  out->list_level = (guint8) CLAMP (pa->ilvl, 0, 8);

  /* Word keeps a list item's hanging indent in the list definition, which
   * is not read; without one the marker would sit on the text.  Word's own
   * default level is a quarter inch hung, so that is what the item gets. */
  if (out->list != W42_LIST_NONE && out->indent_first >= 0)
    {
      out->indent_left = MAX (out->indent_left, 360);
      out->indent_first = -360;
    }
}

static gboolean in_dt (Doc *doc, gsize off, gsize n);

/* sprmPHugePapx: a PAPX too big for its FKP -- a row end with many cells
 * -- keeps its sprms in the Data stream, and the FKP only says where. */
static void
apply_huge_papx (Doc *doc, const guint8 *grpprl, guint len, Para *pa)
{
  guint p = 0;

  while (p + 2 <= len)
    {
      guint16 sprm = rd16 (grpprl + p);
      guint avail = len - p - 2;
      guint olen = sprm_operand_len (sprm, grpprl + p + 2, avail);

      if (olen > avail)
        break;
      if ((sprm == 0x6646 || sprm == 0x6645) && olen == 4)
        {
          guint32 at = rd32 (grpprl + p + 2);

          if (in_dt (doc, at, 2) && in_dt (doc, (gsize) at + 2, rd16 (doc->dt + at)))
            apply_papx (doc->dt + at + 2, rd16 (doc->dt + at), pa);
        }
      p += 2 + olen;
    }
}

/* The paragraphs of the main text, with their properties resolved. */
static GArray *
collect_paragraphs (Doc *doc)
{
  GArray *paras = g_array_new (FALSE, FALSE, sizeof (DocPara));
  guint32 start = 0;
  gboolean break_here = FALSE;    /* the paragraph being read starts a page */
  gboolean break_next = FALSE;    /* and the one after it will */

  g_array_set_clear_func (paras, doc_para_clear);
  for (guint32 cp = 0; (gint32) cp < doc->ccp_text; cp++)
    {
      gunichar c = char_at (doc, cp);
      gboolean ends = c == 0x0D || c == 0x07 || (gint32) cp == doc->ccp_text - 1;

      /* A section mark or a page break, 0x0C, ends its paragraph as a
       * paragraph mark does, and what follows starts a new page: two
       * sections' text was run together into one paragraph.  At the
       * start of a paragraph it is that paragraph's break; before a
       * paragraph mark, the mark ends the paragraph. */
      if (c == 0x0C)
        {
          if (cp == start)
            {
              break_here = TRUE;
              if (!ends)
                continue;
            }
          else
            {
              break_next = TRUE;
              if ((gint32) cp + 1 < doc->ccp_text && char_at (doc, cp + 1) == 0x0D)
                continue;
              ends = TRUE;
            }
        }

      if (ends)
        {
          DocPara dp;
          guint32 fc = 0;
          gboolean compressed = FALSE;
          const guint8 *papx;
          guint len;

          dp.cp_start = start;
          dp.cp_end = cp;
          para_defaults (&dp.pa);
          dp.istd = 0;

          if (cp_to_fc (doc, cp, &fc, &compressed))
            {
              papx = papx_at (doc, fc, &len);
              if (papx != NULL && len >= 2)
                {
                  dp.istd = rd16 (papx);
                  resolve_style (doc, dp.istd, &dp.pa, NULL, 0);
                  dp.pa.istd = dp.istd;
                  apply_papx (papx + 2, len - 2, &dp.pa);
                  apply_huge_papx (doc, papx + 2, len - 2, &dp.pa);
                }
              else
                resolve_style (doc, 0, &dp.pa, NULL, 0);
            }
          if (break_here)
            dp.pa.page_break = TRUE;
          break_here = break_next;
          break_next = FALSE;

          g_array_append_val (paras, dp);
          start = cp + 1;

          /* A file that is nothing but paragraph marks buys one per
           * byte: past any document's worth of them, the rest run on as
           * the last one. */
          if (paras->len >= 262144)
            {
              g_array_index (paras, DocPara, paras->len - 1).cp_end =
                doc->ccp_text > 0 ? (guint32) doc->ccp_text - 1 : 0;
              break;
            }
        }
    }

  return paras;
}

/* ---- pictures --------------------------------------------------------- */

static gboolean
in_dt (Doc *doc, gsize off, gsize n)
{
  return doc->dt != NULL && off <= doc->dt_len && n <= doc->dt_len - off;
}

/* A metafile blip's bytes are deflated in the zlib framing; the header
 * before them says how long they were. */
static GBytes *
inflate_zlib (const guint8 *in, gsize in_len, gsize expect)
{
  GZlibDecompressor *dec = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_ZLIB);
  GByteArray *out = g_byte_array_new ();
  guint8 buf[65536];
  gsize in_pos = 0;
  gsize limit = expect > 0 ? MIN (expect, 64u << 20) : 64u << 20;
  gboolean done = FALSE, ok = TRUE;

  while (!done)
    {
      gsize read = 0, written = 0;
      GConverterResult res = g_converter_convert (G_CONVERTER (dec), in + in_pos, in_len - in_pos,
                                                  buf, sizeof buf,
                                                  in_pos >= in_len ? G_CONVERTER_INPUT_AT_END : G_CONVERTER_NO_FLAGS,
                                                  &read, &written, NULL);

      if (res == G_CONVERTER_ERROR || out->len + written > limit)
        {
          ok = FALSE;
          break;
        }
      in_pos += read;
      g_byte_array_append (out, buf, written);
      if (res == G_CONVERTER_FINISHED || (read == 0 && written == 0 && in_pos >= in_len))
        done = TRUE;
    }
  g_object_unref (dec);
  if (!ok || out->len == 0)
    {
      g_byte_array_free (out, TRUE);
      return NULL;
    }
  return g_byte_array_free_to_bytes (out);
}

/* The picture at `fc` in the Data stream: a PICF header, then OfficeArt
 * records down to a blip-store entry holding the bytes of a PNG or JPEG,
 * or of a Windows metafile, which comes back inflated with `ext` saying
 * "emf" or "wmf" -- nothing here can draw one, but it keeps its place
 * and its bytes.  DIBs are passed over.  On success the bytes and the
 * size Word showed it at, in twips. */
static GBytes *
find_picture (Doc *doc, guint32 fc, int *width, int *height, const char **ext)
{
  guint32 lcb, cb_header, p, end;
  int dxa, dya, mx, my;

  if (!in_dt (doc, fc, 68))
    return NULL;

  lcb = rd32 (doc->dt + fc);
  cb_header = rd16 (doc->dt + fc + 4);
  dxa = rd16s (doc->dt + fc + 28);
  dya = rd16s (doc->dt + fc + 30);
  mx  = rd16s (doc->dt + fc + 32);
  my  = rd16s (doc->dt + fc + 34);
  if (lcb < cb_header || cb_header < 68 || !in_dt (doc, fc, lcb))
    return NULL;

  *width  = (mx > 0) ? (int) ((gint64) dxa * mx / 1000) : dxa;
  *height = (my > 0) ? (int) ((gint64) dya * my / 1000) : dya;

  /* Walk the records; containers are entered, atoms stepped over. */
  p = fc + cb_header;
  end = fc + lcb;
  while (p + 8 <= end)
    {
      guint16 verinst = rd16 (doc->dt + p);
      guint16 type = rd16 (doc->dt + p + 2);
      guint32 len = rd32 (doc->dt + p + 4);

      if ((verinst & 0xF) == 0xF)
        {
          p += 8;                      /* a container: look inside */
          continue;
        }

      if (type == 0xF007 && len > 36 + 8 && len <= end - p - 8)
        {
          /* The blip-store entry: a 36-byte FBSE, then the blip record
           * with its header, one or two 16-byte UIDs, a tag byte, and
           * the picture file's bytes. */
          guint32 b = p + 8 + 36;
          guint16 binst, btype;
          guint32 blen, skip;
          const char *want = NULL;

          if (b + 8 > end)
            return NULL;
          binst = rd16 (doc->dt + b) >> 4;
          btype = rd16 (doc->dt + b + 2);
          blen  = rd32 (doc->dt + b + 4);

          if (btype == 0xF01D)
            {
              want = "jpeg";
              skip = (binst == 0x46B || binst == 0x6E3) ? 32 : 16;
            }
          else if (btype == 0xF01E)
            {
              want = "png";
              skip = (binst == 0x6E1) ? 32 : 16;
            }
          else if (btype == 0xF01A || btype == 0xF01B)
            {
              /* EMF or WMF: after the UIDs, a header of its own -- the
               * uncompressed size first, the stored size and the
               * compression byte at the end -- then the bytes. */
              guint32 h, cb_size, cb_save;
              guint8 compression;

              want = btype == 0xF01A ? "emf" : "wmf";
              skip = (binst == 0x3D5 || binst == 0x217) ? 32 : 16;
              if (blen > end - b - 8 || blen < skip + 34)
                return NULL;
              h = b + 8 + skip;
              cb_size = rd32 (doc->dt + h);
              cb_save = rd32 (doc->dt + h + 28);
              compression = doc->dt[h + 32];
              if (cb_save == 0 || cb_save > blen - skip - 34)
                return NULL;
              *ext = want;
              if (compression == 0xFE)
                return g_bytes_new (doc->dt + h + 34, cb_save);
              return inflate_zlib (doc->dt + h + 34, cb_save, cb_size);
            }
          else
            return NULL;

          skip += 1;                   /* the tag byte */
          if (blen <= skip || blen > end - b - 8)
            return NULL;

          *ext = want;
          return g_bytes_new (doc->dt + b + 8 + skip, blen - skip);
        }

      /* A length that wraps would leave p where it is, for ever. */
      if (len > end - p - 8)
        return NULL;
      p += 8 + len;
    }

  return NULL;
}

typedef struct {
  W42PieceTable *pt;
  Doc           *doc;
  gsize          pos;
  int            table;
  int            row, col;
  gboolean       in_cell;
  gboolean       skip_cell;   /* the cell is merged into the one before */
  gsize          last_cell_pos;
  int            last_cell_span;
  int            cell_index;  /* the cell's place in its row's own shape */
  int            n_cols;      /* the table's grid: every row's edges, together */
  int            grid[65];
  gboolean       table_before_block;
  GString       *run;
  W42ApIdx       run_ap;
  GArray        *note_ids;    /* the footnotes made, in reference order */
  GArray        *end_ids;     /* and the endnotes */
  /* What row_shape found last, and the paragraphs it looked through to
   * find it, [shape_from, shape_to): asked from any of them, the answer
   * is the same. */
  guint          shape_from, shape_to;
  const RowShape *shape;
} Builder;

static void
flush_run (Builder *b)
{
  if (b->run->len > 0)
    {
      w42_pt_insert_text (b->pt, b->pos, b->run->str, b->run_ap);
      b->pos += g_utf8_strlen (b->run->str, -1);
      g_string_truncate (b->run, 0);
    }
}

/* The text of one paragraph, run by run of character formatting. */
static void
emit_text (Builder *b, const DocPara *dp)
{
  Doc *doc = b->doc;
  Char style_ch;
  guint32 run_end = 0;
  int field_depth = 0;        /* fields open around this character */
  int code_depth = 0;         /* the field whose code is being read, or 0 */
  GString *field_code = g_string_new (NULL);
  const char *link = NULL;
  W42ApIdx current = ((W42ApIdx) G_MAXUINT32);

  char_defaults (&style_ch);
  resolve_style (doc, dp->istd, NULL, &style_ch, 0);

  for (guint32 cp = dp->cp_start; cp < dp->cp_end; cp++)
    {
      gunichar c = char_at (doc, cp);
      guint32 fc = 0;
      gboolean compressed = FALSE;
      Char ch = style_ch;
      W42Fmt fmt;
      W42ApIdx ap;

      if (!cp_to_fc (doc, cp, &fc, &compressed))
        continue;

      {
        const guint8 *chpx;
        guint len;

        chpx = chpx_at (doc, fc, &len, &run_end);
        if (chpx != NULL)
          apply_chpx (doc, chpx, len, &ch, 0);
      }

      /* Fields: the code between 0x13 and 0x14 is not text; the result
       * between 0x14 and 0x15 is.  The three are marks only on a run
       * flagged special -- elsewhere they are characters -- and a field
       * inside another's result is a field of its own, while one inside
       * another's code is part of that code. */
      if (ch.spec && c == 0x13)
        {
          field_depth++;
          if (code_depth == 0)
            {
              code_depth = field_depth;
              g_string_truncate (field_code, 0);
            }
          continue;
        }
      if (ch.spec && c == 0x14)
        {
          if (code_depth == field_depth && field_depth > 0)
            {
              /* HYPERLINK "url": the result is the link. */
              code_depth = 0;
              link = NULL;
              if (g_ascii_strncasecmp (g_strstrip (field_code->str), "HYPERLINK", 9) == 0)
                {
                  const char *q = strchr (field_code->str, '"');
                  const char *e = q != NULL ? strchr (q + 1, '"') : NULL;
                  if (q != NULL && e != NULL && e > q + 1)
                    {
                      char *url = g_strndup (q + 1, (gsize) (e - q - 1));
                      link = g_intern_string (url);
                      g_free (url);
                    }
                }
            }
          continue;
        }
      if (ch.spec && c == 0x15)
        {
          if (field_depth > 0)
            field_depth--;
          if (code_depth > field_depth)
            code_depth = 0;
          link = NULL;
          continue;
        }
      if (code_depth > 0)
        {
          if (c >= 0x20)
            g_string_append_unichar (field_code, c);
          continue;
        }

      if (ch.spec && c == 0x02 && b->note_ids != NULL)
        {
          /* A note's reference: the note's text comes later, from the
           * footnote story or the endnote story, which PlcfendRef tells
           * apart; the mark and an empty note go in now.  Notes are
           * numbered by the piece table as they are made, both kinds
           * together. */
          W42Fmt pfmt;
          int id = (int) (b->note_ids->len + b->end_ids->len);
          gboolean endnote = FALSE;
          W42ApIdx mark_ap;

          for (guint k = 0; doc->edn_refs != NULL && k < doc->edn_refs->len && !endnote; k++)
            endnote = g_array_index (doc->edn_refs, guint32, k) == cp;
          w42_fmt_init_default (&pfmt);
          fill_char_fmt (doc, &ch, &pfmt.ch);
          flush_run (b);
          mark_ap = w42_ap_table_intern (w42_pt_ap_table (b->pt), &pfmt);
          if (endnote)
            w42_pt_insert_endnote (b->pt, b->pos, mark_ap);
          else
            w42_pt_insert_footnote (b->pt, b->pos, mark_ap);
          b->pos += 1;
          current = ((W42ApIdx) G_MAXUINT32);
          g_array_append_val (endnote ? b->end_ids : b->note_ids, id);
          continue;
        }

      if (ch.spec)
        {
          /* A picture, if that is what the special character is. */
          if (c == 0x01 && ch.has_pic)
            {
              int width = 0, height = 0, pw = 0, ph = 0;
              const char *ext = NULL;
              GBytes *data = find_picture (doc, ch.pic_fc, &width, &height, &ext);
              const char *format = NULL;
              W42ObjectIdx idx = W42_OBJECT_NONE;
              W42ObjectTable *objects = w42_pt_object_table (b->pt);

              if (data != NULL && w42_image_probe (data, &pw, &ph, &format))
                {
                  if (width <= 0)  width = pw * 15;
                  if (height <= 0) height = ph * 15;
                  /* No bigger than the largest page, whatever the PICF
                   * says. */
                  width = CLAMP (width, 15, 31680);
                  height = CLAMP (height, 15, 31680);
                  idx = w42_object_table_add (objects, data, format, pw, ph, width, height);
                }
              else if (data != NULL && ext != NULL &&
                       (g_str_equal (ext, "emf") || g_str_equal (ext, "wmf")))
                {
                  /* A metafile: a box its size with its kind written in
                   * it, as Word shows a picture it cannot draw, and the
                   * bytes kept for the file the document is saved as. */
                  char *label = g_strdup_printf ("%s picture", g_str_equal (ext, "emf") ? "EMF" : "WMF");
                  GBytes *png;

                  if (width <= 0)  width = 2880;
                  if (height <= 0) height = 1440;
                  /* The box is drawn at its size: a PICF's word for a
                   * size of forty feet was a 400 MB bitmap. */
                  width = CLAMP (width, 15, 31680);
                  height = CLAMP (height, 15, 31680);
                  pw = MAX (width / 15, 2);
                  ph = MAX (height / 15, 2);
                  png = w42_shape_render (W42_SHAPE_RECTANGLE, pw, ph, 0.75, 0x999999, FALSE, 0xFFFFFF, label);
                  if (png != NULL)
                    {
                      idx = w42_object_table_add (objects, png, g_intern_static_string ("png"),
                                                  pw, ph, width, height);
                      w42_object_table_set_shape (objects, idx, W42_SHAPE_RECTANGLE, 0.75, 0x999999,
                                                  FALSE, 0xFFFFFF, label);
                      w42_object_table_set_original (objects, idx, data, ext);
                      g_bytes_unref (png);
                    }
                  g_free (label);
                }
              if (idx != W42_OBJECT_NONE)
                {
                  W42Fmt pfmt;

                  w42_fmt_init_default (&pfmt);
                  fill_char_fmt (doc, &ch, &pfmt.ch);
                  flush_run (b);
                  w42_pt_insert_object (b->pt, b->pos, idx,
                                        w42_ap_table_intern (w42_pt_ap_table (b->pt), &pfmt));
                  b->pos += 1;
                  current = ((W42ApIdx) G_MAXUINT32);
                }
              if (data != NULL)
                g_bytes_unref (data);
            }
          continue;               /* marks and such */
        }
      if (c == 0x0C || c == 0x0E)
        continue;               /* page and column breaks */
      if (c == 0x0B) c = 0x2028;
      if (c == 0x1E) c = 0x2011;
      if (c == 0x1F) continue;   /* optional hyphen */
      if (c == 0 || (c < 0x20 && c != '\t'))
        continue;
      if (ch.vanish)
        continue;               /* hidden text: Word does not show it either */

      w42_fmt_init_default (&fmt);
      fill_char_fmt (doc, &ch, &fmt.ch);
      fmt.ch.link = link;
      if (ch.rmark_del)
        fmt.ch.revision = 2;    /* a tracked change: kept, and marked */
      else if (ch.rmark_ins)
        fmt.ch.revision = 1;
      ap = w42_ap_table_intern (w42_pt_ap_table (b->pt), &fmt);

      if (ap != current)
        {
          flush_run (b);
          current = ap;
          b->run_ap = ap;
        }
      g_string_append_unichar (b->run, c);
    }

  flush_run (b);
  g_string_free (field_code, TRUE);
}

static W42ApIdx
para_ap (Builder *b, const DocPara *dp)
{
  W42Fmt fmt;
  Char ch;

  char_defaults (&ch);
  resolve_style (b->doc, dp->istd, NULL, &ch, 0);

  w42_fmt_init_default (&fmt);
  fill_char_fmt (b->doc, &ch, &fmt.ch);
  fill_para_fmt (b->doc, dp, &fmt.pa);

  return w42_ap_table_intern (w42_pt_ap_table (b->pt), &fmt);
}

/* The paragraph properties of a .doc sit on the paragraph mark, so they are
 * known only once the paragraph's text has been emitted.  They belong to the
 * BLOCK strux in front of that text, which is the one w42_pt_apply_para_fmt
 * finds by widening backwards -- but from `b->pos - 1`, the last thing
 * emitted, not from `b->pos`: a document that ends in a table still has its
 * final empty paragraph's BLOCK sitting at `b->pos`, and widening from there
 * finds that one and leaves every paragraph wearing its predecessor's
 * formatting.  An empty paragraph puts its own BLOCK at `b->pos - 1`, which
 * is what it should get. */
static void
apply_para (Builder *b, const DocPara *dp)
{
  W42Fmt fmt;

  w42_fmt_init_default (&fmt);
  fill_para_fmt (b->doc, dp, &fmt.pa);
  w42_pt_apply_para_fmt (b->pt, b->pos > 0 ? b->pos - 1 : 0, 0,
                         W42_PARA_ALL, &fmt.pa);
}

static gboolean
has_row_shape (const Para *pa)
{
  return pa->ttp && pa->row != NULL && pa->row->itc_mac > 0;
}

/* A row's shape -- its columns, their widths and each cell's borders and
 * background -- is in the row-end paragraph that follows its cells, so it
 * has to be looked ahead for.  It is asked for at every cell, so what the
 * last look found is kept: every paragraph it passed over has the same
 * answer.  Looked for afresh each time, cells marked as in a table with
 * no row end after them were each a walk to the end of the document, and
 * 40 000 of them took eleven seconds to open; they take 20 ms now. */
static const RowShape *
row_shape (Builder *b, GArray *paras, guint index)
{
  guint k;

  if (index >= b->shape_from && index < b->shape_to)
    return b->shape;

  b->shape = NULL;
  for (k = index; k < paras->len; k++)
    {
      const DocPara *q = &g_array_index (paras, DocPara, k);

      if (has_row_shape (&q->pa))
        {
          b->shape = q->pa.row;
          break;
        }
      if (!q->pa.in_table)
        break;
    }
  b->shape_from = index;
  b->shape_to = k + 1;
  return b->shape;
}

/* The column a row's cell edge falls on in the table's grid: the
 * nearest, since Word's rows do not always agree to the twip. */
static int
grid_column (const Builder *b, int edge)
{
  int best = 0;

  for (int c = 0; c <= b->n_cols; c++)
    if (ABS (b->grid[c] - edge) < ABS (b->grid[best] - edge))
      best = c;
  return best;
}

static void
open_table (Builder *b, GArray *paras, guint index)
{
  const RowShape *shape = row_shape (b, paras, index);
  int n_cols = 1;
  int widths[64] = { 0 };

  /* Every row has a shape of its own, and a row with fewer, wider cells
   * is how Word merges across.  The table's grid is every row's edges
   * together; a cell spans the columns between its two. */
  {
    int n_edges = 0;

    for (guint k = index; k < paras->len && n_edges < 65; k++)
      {
        const DocPara *q = &g_array_index (paras, DocPara, k);

        if (!q->pa.in_table)
          break;
        if (!has_row_shape (&q->pa))
          continue;
        for (int c = 0; c <= q->pa.row->itc_mac && c < 65; c++)
          {
            int edge = q->pa.row->cellx[c];
            int at = 0;

            while (at < n_edges && b->grid[at] < edge - 10)
              at++;
            if (at < n_edges && ABS (b->grid[at] - edge) <= 10)
              continue;
            if (n_edges >= 65)
              break;
            memmove (&b->grid[at + 1], &b->grid[at], (n_edges - at) * sizeof (int));
            b->grid[at] = edge;
            n_edges++;
          }
      }
    /* The columns are one fewer than the edges. */
    b->n_cols = MAX (n_edges - 1, 0);
  }

  if (b->n_cols > 0)
    {
      n_cols = b->n_cols;
      for (int c = 0; c < n_cols; c++)
        widths[c] = b->grid[c + 1] - b->grid[c];
    }
  else if (shape != NULL && shape->itc_mac > 0)
    {
      n_cols = MIN (shape->itc_mac, 64);
      for (int c = 0; c < n_cols; c++)
        widths[c] = shape->cellx[c + 1] - shape->cellx[c];
      b->n_cols = n_cols;
      for (int c = 0; c <= n_cols; c++)
        b->grid[c] = shape->cellx[c];
    }
  else
    {
      /* No row says what shape it is: a column for each cell of the first
       * row, sharing the width, rather than no columns and every cell's
       * text dropped. */
      int cells = 0;

      for (guint k = index; k < paras->len; k++)
        {
          const DocPara *q = &g_array_index (paras, DocPara, k);

          if (!q->pa.in_table || q->pa.ttp)
            break;
          if (char_at (b->doc, q->cp_end) == 0x07)
            cells++;
        }
      n_cols = CLAMP (cells, 1, 64);
      b->n_cols = n_cols;
      for (int c = 0; c <= n_cols; c++)
        b->grid[c] = c;
    }

  /* The body ends where the notes begin, once there are notes: the
   * document's length would make every table after the first footnote
   * gain an empty paragraph before it. */
  b->table_before_block = FALSE;
  if (b->pos >= 2 && b->pos == (w42_pt_notes_start (b->pt) != (gsize) -1
                                ? w42_pt_notes_start (b->pt) : w42_pt_length (b->pt)))
    {
      char *tail = w42_pt_get_text (b->pt, b->pos - 1, 1);
      b->table_before_block = (tail != NULL && *tail == '\n');
      g_free (tail);
    }
  if (b->table_before_block)
    b->pos -= 1;

  b->table = w42_pt_insert_table_start (b->pt, b->pos, n_cols, widths);
  /* Word keeps every rule in the cells themselves, so the table's own
   * "ruled" flag -- which the model has for a table drawn by hand -- is
   * off and each cell says what it wants.  Its own lines, where it has
   * them, stand in for the cells that name none. */
  w42_pt_table_set_borders (b->pt, b->table, FALSE);
  if (shape != NULL && shape->has_tbl_edge)
    for (int e = 0; e < W42_N_EDGES; e++)
      w42_pt_table_set_edge (b->pt, b->table, e, &shape->tbl_edge[e]);
  b->pos += 1;
  b->row = b->col = 0;
  b->cell_index = 0;
  b->in_cell = FALSE;
}

/* Whether the row that paragraph `index` is in is its table's last:
 * the paragraph after its row-end mark is not in a table. */
static gboolean
row_is_last (GArray *paras, guint index)
{
  for (guint k = index; k < paras->len; k++)
    {
      const DocPara *q = &g_array_index (paras, DocPara, k);

      if (q->pa.ttp)
        return k + 1 >= paras->len ||
               !((const DocPara *) &g_array_index (paras, DocPara, k + 1))->pa.in_table;
      if (!q->pa.in_table)
        break;
    }
  return TRUE;
}

/* What the row's shape says about the cell just opened at `cell_pos`:
 * its sides and their lines, its background, where its text sits, and
 * the merge downwards it starts or carries on. */
static void
doc_apply_cell (Builder *b, GArray *paras, guint index, const RowShape *shape, gsize cell_pos)
{
  int col = b->cell_index;
  guint8 sides = shape->cell_sides[col];
  W42BorderEdge edges[4];
  guint flags = shape->cell_flags[col];
  int valign = (flags >> 7) & 3;

  memcpy (edges, shape->cell_edge[col], sizeof edges);

  /* A side the cell names no line for takes the table's, where the table
   * has one: its outer line round the outside, its inside one between
   * the cells. */
  if (shape->has_tbl_edge)
    {
      gboolean first_row = b->row == 0, last_row = row_is_last (paras, index);
      gboolean first_col = b->col == 0, last_col = b->col + b->last_cell_span >= b->n_cols;

      for (int e = 0; e < 4; e++)
        {
          gboolean outer;
          const W42BorderEdge *te;

          if ((sides & (1 << e)) || edges[e].style == W42_BORDER_NONE || edges[e].width != 0)
            continue;
          outer = (e == W42_EDGE_TOP && first_row) || (e == W42_EDGE_BOTTOM && last_row) ||
                  (e == W42_EDGE_LEFT && first_col) || (e == W42_EDGE_RIGHT && last_col);
          te = &shape->tbl_edge[outer ? e : (e <= W42_EDGE_BOTTOM ? W42_EDGE_INSIDE_H : W42_EDGE_INSIDE_V)];
          if (te->style != W42_BORDER_NONE && te->width > 0)
            {
              sides |= (guint8) (1 << e);
              edges[e] = *te;
            }
        }
    }
  for (int e = 0; e < 4; e++)
    if (edges[e].style == W42_BORDER_NONE)
      edges[e].style = W42_BORDER_SINGLE;   /* the side is off; its line is moot */

  w42_pt_cell_set_borders_at (b->pt, cell_pos, sides | W42_BORDER_CELL_SET);
  w42_pt_cell_set_edges_at (b->pt, cell_pos, edges);
  w42_pt_cell_set_fill_at (b->pt, cell_pos, shape->has_cell_shade[col], shape->cell_shade[col]);
  if (valign == 1)
    w42_pt_cell_set_valign_at (b->pt, cell_pos, W42_CELL_VALIGN_CENTER);
  else if (valign == 2)
    w42_pt_cell_set_valign_at (b->pt, cell_pos, W42_CELL_VALIGN_BOTTOM);
  if (flags & 0x0020)
    w42_pt_set_cell_vspan (b->pt, cell_pos, (flags & 0x0040) ? 2 : W42_CELL_COVERED);

  if (b->col == 0)
    {
      if (shape->dya_row_height != 0)
        w42_pt_table_set_row_height (b->pt, b->table, b->row, ABS (shape->dya_row_height));
      if (shape->table_header)
        w42_pt_table_set_header_rows (b->pt, b->table, b->row + 1);
    }
}

static void
close_table (Builder *b, W42ApIdx ap)
{
  w42_pt_resolve_vmerges (b->pt, b->table);
  if (b->table_before_block)
    {
      w42_pt_insert_table_end_only (b->pt, b->pos);
      b->pos += 2;
    }
  else
    {
      w42_pt_insert_table_end (b->pt, b->pos, ap);
      b->pos += 2;
    }
  b->table = -1;
}

/* The text of one kind of note, from its story: the PLC at FIB entry
 * `plc` holds each note's start in the story that begins at `base` and
 * runs `ccp`, and `ids` the notes made for the references, in order.
 * The notes' own marks are left out, and so is a field's code, as in the
 * body. */
static void
fill_notes (Doc *doc, W42PieceTable *pt, GArray *ids, guint plc, guint32 base, guint32 ccp)
{
  guint32 fc, lcb;

  if (ids->len == 0)
    return;
  fib_fclcb (doc, plc, &fc, &lcb);
  if (lcb < 8 || !in_tb (doc, fc, lcb))
    return;

  for (guint i = 0, n = lcb / 4 - 1; i < n && i < ids->len; i++)
    {
      guint32 a = rd32 (doc->tb + fc + 4 * i);
      guint32 e = rd32 (doc->tb + fc + 4 * (i + 1));
      gsize pos = w42_pt_note_body (pt, g_array_index (ids, int, i));
      W42ApIdx ap = w42_ap_table_default (w42_pt_ap_table (pt));
      GString *text;
      gboolean first = TRUE;
      int field_depth = 0, code_depth = 0;

      if (pos == (gsize) -1 || e <= a || e > ccp)
        continue;               /* a note the file does not really have */
      text = g_string_new (NULL);

      /* Word's note text starts with the mark and a space; the space
       * goes with the mark. */
#define NOTE_INSERT() G_STMT_START { \
        if (first) { g_strchug (text->str); g_string_set_size (text, strlen (text->str)); first = FALSE; } \
        if (text->len > 0) { w42_pt_insert_text (pt, pos, text->str, ap); pos += g_utf8_strlen (text->str, -1); } \
        g_string_truncate (text, 0); } G_STMT_END

      for (guint32 cp = base + a; cp < base + e; cp++)
        {
          gunichar c = char_at (doc, cp);

          if (c == 0x13)
            {
              field_depth++;
              if (code_depth == 0)
                code_depth = field_depth;
              continue;
            }
          if (c == 0x14)
            {
              if (code_depth == field_depth && field_depth > 0)
                code_depth = 0;
              continue;
            }
          if (c == 0x15)
            {
              if (field_depth > 0)
                field_depth--;
              if (code_depth > field_depth)
                code_depth = 0;
              continue;
            }
          if (code_depth > 0 || c == 0x02)
            continue;
          if (c == 0x0D)
            {
              /* A paragraph of the note ends: put what we have, then a
               * new paragraph, unless this is the story's final mark. */
              NOTE_INSERT ();
              if (cp + 1 < base + e)
                {
                  w42_pt_insert_block (pt, pos, ap);
                  pos += 1;
                }
            }
          else if (c == '\t' || c >= 0x20)
            g_string_append_unichar (text, c);
        }
      NOTE_INSERT ();
#undef NOTE_INSERT
      g_string_free (text, TRUE);
    }
}

static void
build_document (Doc *doc, W42PieceTable *pt)
{
  GArray *paras = collect_paragraphs (doc);
  Builder b;

  memset (&b, 0, sizeof b);
  b.pt = pt;
  b.doc = doc;
  b.pos = w42_pt_first_caret_pos (pt);
  b.table = -1;
  b.last_cell_pos = (gsize) -1;
  b.run = g_string_new (NULL);
  b.run_ap = w42_ap_table_default (w42_pt_ap_table (pt));
  b.note_ids = g_array_new (FALSE, FALSE, sizeof (int));
  b.end_ids = g_array_new (FALSE, FALSE, sizeof (int));

  for (guint i = 0; i < paras->len; i++)
    {
      const DocPara *dp = &g_array_index (paras, DocPara, i);
      gunichar mark = char_at (doc, dp->cp_end);
      W42ApIdx ap = para_ap (&b, dp);

      if (dp->pa.ttp)
        {
          /* The row-end mark: its own paragraph, with nothing to show. */
          if (b.table >= 0)
            {
              b.row++;
              b.col = 0;
              b.cell_index = 0;
              b.in_cell = FALSE;
            }
          continue;
        }

      if (dp->pa.in_table)
        {
          if (b.table < 0)
            open_table (&b, paras, i);
          if (!b.in_cell)
            {
              const RowShape *shape = row_shape (&b, paras, i);
              gsize cell_pos = b.pos;

              /* A cell merged into the one before it: that one grows
               * over its column, and what it holds -- nothing, in a
               * file Word wrote -- is left out. */
              if (shape != NULL && b.cell_index > 0 && b.cell_index < 64 &&
                  (shape->cell_flags[b.cell_index] & 0x0002) && b.last_cell_pos != (gsize) -1)
                {
                  b.last_cell_span = MIN (b.last_cell_span + 1, 1023);
                  w42_pt_set_cell_span (pt, b.last_cell_pos, b.last_cell_span);
                  b.in_cell = TRUE;
                  b.skip_cell = TRUE;
                }
              else if (b.col >= b.n_cols || b.col > 1023 || b.row >= W42_TABLE_MAX_ROWS)
                {
                  /* More cells than the grid has columns, or rows than a
                   * table can number: dropped, as the other readers drop
                   * them. */
                  b.in_cell = TRUE;
                  b.skip_cell = TRUE;
                }
              else
                {
                  int span = 1;

                  if (shape != NULL && b.cell_index < 64 && b.cell_index < shape->itc_mac)
                    {
                      int c0 = grid_column (&b, shape->cellx[b.cell_index]);
                      int c1 = grid_column (&b, shape->cellx[b.cell_index + 1]);

                      b.col = CLAMP (c0, 0, b.n_cols - 1);
                      span = CLAMP (c1 - c0, 1, b.n_cols - b.col);
                    }
                  w42_pt_insert_cell (pt, b.pos, b.table, b.row, b.col, ap);
                  if (span > 1)
                    w42_pt_set_cell_span (pt, cell_pos, span);
                  b.pos += 2;
                  b.in_cell = TRUE;
                  b.skip_cell = FALSE;
                  b.last_cell_pos = cell_pos;
                  b.last_cell_span = span;
                  if (shape != NULL && b.cell_index >= 0 && b.cell_index < 64)
                    doc_apply_cell (&b, paras, i, shape, cell_pos);
                }
            }

          if (b.skip_cell)
            {
              if (mark == 0x07)
                {
                  b.in_cell = FALSE;
                  b.cell_index++;
                  b.col += b.last_cell_span;
                }
              continue;
            }

          emit_text (&b, dp);
          apply_para (&b, dp);

          if (mark == 0x07)
            {
              b.in_cell = FALSE;
              b.cell_index++;
              b.col += b.last_cell_span;
            }
          else
            {
              w42_pt_insert_block (pt, b.pos, ap);
              b.pos += 1;
            }
          continue;
        }

      if (b.table >= 0)
        close_table (&b, ap);

      emit_text (&b, dp);
      apply_para (&b, dp);

      /* The last paragraph's mark is the document's own final mark. */
      if (i + 1 < paras->len)
        {
          w42_pt_insert_block (pt, b.pos, ap);
          b.pos += 1;
        }
    }

  if (b.table >= 0)
    close_table (&b, w42_ap_table_default (w42_pt_ap_table (pt)));

  /* The notes' text: the footnote story follows the main text, the
   * endnote story comes after the headers' and the comments', and
   * PlcffndTxt and PlcfendTxt say where each note's paragraphs are. */
  fill_notes (doc, pt, b.note_ids, 3, (guint32) doc->ccp_text, (guint32) doc->ccp_ftn);
  fill_notes (doc, pt, b.end_ids, 47,
              (guint32) doc->ccp_text + (guint32) doc->ccp_ftn + (guint32) doc->ccp_hdd +
              (guint32) doc->ccp_mcr + (guint32) doc->ccp_atn,
              (guint32) doc->ccp_edn);

  g_array_free (b.note_ids, TRUE);
  g_array_free (b.end_ids, TRUE);
  g_string_free (b.run, TRUE);
  g_array_free (paras, TRUE);
}

/* ---- headers and footers ----------------------------------------------- */

/* The text of a header story, with its fields spelt the way word42 spells
 * them: {PAGE}, {NUMPAGES}, {DATE}. */
static char *
story_text (Doc *doc, guint32 cp_start, guint32 cp_end)
{
  cp_end = MIN (cp_end, doc->cp_max);
  if (cp_start > cp_end)
    cp_start = cp_end;
  GString *text = g_string_new (NULL);
  GString *code = NULL;
  gboolean in_result = FALSE;

  for (guint32 cp = cp_start; cp < cp_end; cp++)
    {
      gunichar c = char_at (doc, cp);

      if (c == 0x13)
        {
          if (code != NULL)
            g_string_free (code, TRUE);
          code = g_string_new (NULL);
          continue;
        }
      if (c == 0x14 && code != NULL)
        {
          char *up = g_ascii_strup (code->str, -1);

          if (strstr (up, "NUMPAGES") != NULL)   g_string_append (text, "{NUMPAGES}");
          else if (strstr (up, "PAGE") != NULL)  g_string_append (text, "{PAGE}");
          else if (strstr (up, "DATE") != NULL || strstr (up, "TIME") != NULL)
            g_string_append (text, "{DATE}");
          g_free (up);
          g_string_free (code, TRUE);
          code = NULL;
          in_result = TRUE;
          continue;
        }
      if (c == 0x15)
        {
          in_result = FALSE;
          continue;
        }
      if (code != NULL)
        {
          g_string_append_unichar (code, c);
          continue;
        }
      if (in_result)
        continue;

      if (c == 0x0D || c == 0x07)
        g_string_append_c (text, ' ');
      else if (c == '\t')
        g_string_append_c (text, ' ');
      else if (c >= 0x20)
        g_string_append_unichar (text, c);
    }

  if (code != NULL)
    g_string_free (code, TRUE);

  return g_strstrip (g_string_free (text, FALSE));
}

/* How a header or footer story's first paragraph is aligned: its PAPX,
 * over its style, as a body paragraph's would be. */
static W42Align
story_align (Doc *doc, guint32 cp)
{
  Para pa;
  guint32 fc = 0;
  gboolean compressed = FALSE;
  const guint8 *papx;
  guint len = 0;

  para_defaults (&pa);
  if (cp_to_fc (doc, cp, &fc, &compressed) &&
      (papx = papx_at (doc, fc, &len)) != NULL && len >= 2)
    {
      resolve_style (doc, rd16 (papx), &pa, NULL, 0);
      apply_papx (papx + 2, len - 2, &pa);
    }
  g_free (pa.row);
  switch (pa.jc)
    {
    case 1:  return W42_ALIGN_CENTER;
    case 2:  return W42_ALIGN_RIGHT;
    default: return W42_ALIGN_LEFT;
    }
}

static void
read_headers (Doc *doc, W42PieceTable *pt)
{
  guint32 fc, lcb, base;
  guint n;

  fib_fclcb (doc, 11, &fc, &lcb);
  if (lcb < 4 || !in_tb (doc, fc, lcb))
    return;

  n = lcb / 4;
  base = (guint32) doc->ccp_text + (guint32) doc->ccp_ftn;

  /* Six separator stories come first, then per section: even header, odd
   * header, even footer, odd footer, first-page header, first-page
   * footer.  The odd ones are what every page gets without settings. */
  /* Word puts a document's only header in the first-page story often
   * enough -- "different first page" with one page -- that it is read
   * when the odd pages' story is empty. */
  {
    static const guint ODD_FIRST[2][2] = { { 7, 10 }, { 9, 11 } };

    for (int which = 0; which < 2; which++)
      for (int try = 0; try < 2; try++)
        {
          guint i = ODD_FIRST[which][try];
          guint32 a, b;
          char *text;

          if (n <= i + 1)
            continue;
          a = rd32 (doc->tb + fc + 4 * i);
          b = rd32 (doc->tb + fc + 4 * (i + 1));
          if (b <= a)
            continue;
          text = story_text (doc, base + a, base + b);
          if (*text != '\0')
            {
              W42Align align = story_align (doc, base + a);

              if (which == 0)
                w42_pt_set_header (pt, text, align);
              else
                w42_pt_set_footer (pt, text, align);
              g_free (text);
              break;
            }
          g_free (text);
        }
  }
}

/* ---------------------------------------------------------------------- */
/* The .doc formats before Word 97: the text and nothing else              */
/* ---------------------------------------------------------------------- */

static gboolean
load_word6_text (Doc *doc, W42PieceTable *pt, GError **error)
{
  GString *text = g_string_new (NULL);

  if (doc->fc_mac <= doc->fc_min || !in_wd (doc, doc->fc_min, doc->fc_mac - doc->fc_min))
    {
      g_string_free (text, TRUE);
      FAIL (error, "The document's text is out of reach.");
    }

  {
    /* The main text is ccpText characters, one byte each: what follows
     * up to fcMac is the footnotes' and the headers' stories, which ran
     * on into the body. */
    guint32 ccp_text = in_wd (doc, 0x34, 4) ? rd32 (doc->wd + 0x34) : 0;

    if (ccp_text > 0 && ccp_text < doc->fc_mac - doc->fc_min)
      doc->fc_mac = doc->fc_min + ccp_text;
  }
  for (guint32 fc = doc->fc_min; fc < doc->fc_mac; fc++)
    {
      guint8 c = doc->wd[fc];

      if (c == 0x0D || c == 0x07)
        g_string_append_c (text, '\n');
      else if (c == '\t' || c >= 0x20)
        g_string_append_unichar (text, cp1252 (c));
    }

  w42_pt_load_text (pt, text->str);
  g_string_free (text, TRUE);
  return TRUE;
}

/* ---------------------------------------------------------------------- */

gboolean
w42_doc_load (W42PieceTable *pt, W42PageSetup *page, GFile *file, GError **error)
{
  char *contents = NULL;
  gsize length = 0;
  Ole ole;
  GByteArray *wd = NULL, *tb = NULL, *dt = NULL;
  Doc doc;
  gboolean ok = FALSE;
  guint16 flags;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (!g_file_load_contents (file, NULL, &contents, &length, NULL, error))
    return FALSE;

  if (!ole_open (&ole, (const guint8 *) contents, length, error))
    {
      ole_close (&ole);
      g_free (contents);
      return FALSE;
    }

  memset (&doc, 0, sizeof doc);

  wd = ole_stream (&ole, "WordDocument", error);
  if (wd == NULL || wd->len < 0x200)
    {
      if (wd != NULL)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "The WordDocument stream is too short to be one.");
      goto out;
    }

  doc.wd = wd->data;
  doc.wd_len = wd->len;

  if (rd16 (doc.wd) != 0xA5EC && rd16 (doc.wd) != 0xA5DC)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "This is not a Word document.");
      goto out;
    }

  doc.nfib = rd16 (doc.wd + 2);
  flags = rd16 (doc.wd + 0x0A);
  doc.fc_min = rd32 (doc.wd + 0x18);
  doc.fc_mac = rd32 (doc.wd + 0x1C);

  if (flags & 0x0100)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "The document is encrypted, and Word42 cannot open "
                   "encrypted documents.");
      goto out;
    }

  w42_pt_load_text (pt, "");

  if (doc.nfib < 193)
    {
      /* A file from before Word 97 keeps its tables elsewhere in the FIB;
       * the text is where it says, and that is what we take. */
      ok = load_word6_text (&doc, pt, error);
      goto out;
    }

  /* fWhichTblStm names the table stream; a file some tool has rewritten
   * can carry the other one only, and that one is better than nothing. */
  tb = ole_stream (&ole, (flags & 0x0200) ? "1Table" : "0Table", NULL);
  if (tb == NULL)
    tb = ole_stream (&ole, (flags & 0x0200) ? "0Table" : "1Table", error);
  if (tb == NULL)
    goto out;
  doc.tb = tb->data;
  doc.tb_len = tb->len;

  doc.ccp_text = (gint32) rd32 (doc.wd + 0x4C);
  doc.ccp_ftn  = (gint32) rd32 (doc.wd + 0x50);
  doc.ccp_hdd  = (gint32) rd32 (doc.wd + 0x54);
  doc.ccp_mcr  = (gint32) rd32 (doc.wd + 0x58);
  doc.ccp_atn  = (gint32) rd32 (doc.wd + 0x5C);
  doc.ccp_edn  = (gint32) rd32 (doc.wd + 0x60);
  if (doc.ccp_text < 0)
    doc.ccp_text = 0;
  if (doc.ccp_ftn < 0)
    doc.ccp_ftn = 0;

  /* The Data stream holds the pictures; a document without one is fine. */
  dt = ole_stream (&ole, "Data", NULL);
  read_summary (&ole, pt);
  if (dt != NULL)
    {
      doc.dt = dt->data;
      doc.dt_len = dt->len;
    }

  if (!read_pieces (&doc, error))
    goto out;
  /* The stories can reach no further than the pieces do. */
  {
    guint32 cp_max = 0;

    for (guint i = 0; i < doc.pieces->len; i++)
      cp_max = MAX (cp_max, g_array_index (doc.pieces, Piece, i).cp_end);
    doc.cp_max = cp_max;
    if ((guint32) doc.ccp_text > cp_max)
      doc.ccp_text = (gint32) cp_max;
    if ((guint32) doc.ccp_ftn > cp_max - (guint32) doc.ccp_text)
      doc.ccp_ftn = (gint32) (cp_max - (guint32) doc.ccp_text);
    /* The stories that follow, each within what is left. */
    {
      gint32 *later[] = { &doc.ccp_hdd, &doc.ccp_mcr, &doc.ccp_atn, &doc.ccp_edn };
      guint32 used = (guint32) doc.ccp_text + (guint32) doc.ccp_ftn;

      for (guint i = 0; i < G_N_ELEMENTS (later); i++)
        {
          if (*later[i] < 0)
            *later[i] = 0;
          if ((guint32) *later[i] > cp_max - used)
            *later[i] = (gint32) (cp_max - used);
          used += (guint32) *later[i];
        }
    }
  }

  read_bins (&doc, 12, &doc.chpx_fc, &doc.chpx_pn);
  read_bins (&doc, 13, &doc.papx_fc, &doc.papx_pn);
  read_styles (&doc);
  read_fonts (&doc);
  read_lists (&doc);
  read_page_setup (&doc, page);
  {
    /* The endnote references' cps, first in PlcfendRef; the references
     * themselves are 0x02 marks in the text like a footnote's. */
    guint32 fc, lcb;

    doc.edn_refs = g_array_new (FALSE, FALSE, sizeof (guint32));
    fib_fclcb (&doc, 46, &fc, &lcb);
    if (lcb >= 10 && in_tb (&doc, fc, lcb))
      for (guint i = 0, n = (lcb - 4) / 6; i < n; i++)
        {
          guint32 cp = rd32 (doc.tb + fc + 4 * i);

          g_array_append_val (doc.edn_refs, cp);
        }
  }

  build_document (&doc, pt);
  read_headers (&doc, pt);
  ok = TRUE;

out:
  if (doc.pieces) g_array_free (doc.pieces, TRUE);
  if (doc.edn_refs) g_array_free (doc.edn_refs, TRUE);
  if (doc.chpx_fc) g_array_free (doc.chpx_fc, TRUE);
  if (doc.chpx_pn) g_array_free (doc.chpx_pn, TRUE);
  if (doc.papx_fc) g_array_free (doc.papx_fc, TRUE);
  if (doc.papx_pn) g_array_free (doc.papx_pn, TRUE);
  if (doc.styles) g_array_free (doc.styles, TRUE);
  if (doc.fonts) g_ptr_array_free (doc.fonts, TRUE);
  if (doc.lfo_lsid) g_array_free (doc.lfo_lsid, TRUE);
  if (doc.lst_lsid) g_array_free (doc.lst_lsid, TRUE);
  if (doc.lst_nfc) g_array_free (doc.lst_nfc, TRUE);
  if (wd) g_byte_array_free (wd, TRUE);
  if (tb) g_byte_array_free (tb, TRUE);
  if (dt) g_byte_array_free (dt, TRUE);
  ole_close (&ole);
  g_free (contents);

  if (ok)
    w42_pt_clear_undo (pt);
  return ok;
}
