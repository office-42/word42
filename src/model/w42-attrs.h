/* w42-attrs.h - character and paragraph formatting, interned in an AP table
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "w42-types.h"

G_BEGIN_DECLS

/* An index into a W42ApTable.  Every piece in the piece table carries one of
 * these instead of its own copy of the formatting, so a document formatted
 * with three fonts costs three formatting records no matter how long it is.
 * This mirrors AbiWord's Attribute/Property table. */
typedef guint32 W42ApIdx;

#define W42_AP_INVALID ((W42ApIdx) G_MAXUINT32)

/* How a run is underlined.  Word 6 offered None, Single, Words Only and
 * Double; the others are what the file formats can say, and what other
 * programs write. */
typedef enum {
  W42_UNDERLINE_NONE = 0,
  W42_UNDERLINE_SINGLE,
  W42_UNDERLINE_DOUBLE,
  W42_UNDERLINE_WORDS,      /* the spaces between words are not underlined */
  W42_UNDERLINE_DOTTED,
  W42_UNDERLINE_DASHED,
  W42_UNDERLINE_THICK,
  W42_UNDERLINE_WAVE
} W42Underline;

typedef struct {
  const char *family;      /* interned via g_intern_string(), never freed */
  int         size;        /* half-points */
  guint       bold      : 1;
  guint       italic    : 1;
  guint       underline : 3;   /* W42Underline: 0 none, 1 single, ... */
  guint       strikeout : 1;
  guint       overline  : 1;   /* AbiWord has it; Word never did */
  gint8       script;      /* -1 subscript, 0 normal, +1 superscript */
  guint8      smallcaps;
  guint8      allcaps;
  guint8      highlight;   /* 0 none, else Word's colour index (7 yellow) */
  gint16      spacing;     /* twips added between characters; may be < 0 */
  guint8      revision;    /* 0 none, 1 inserted, 2 deleted: a marked change */
  guint32     color;       /* 0x00RRGGBB */
  const char *link;        /* interned URL, or NULL: the run is a hyperlink */
  const char *bookmark;    /* interned name, or NULL: the run is bookmarked */
  const char *comment;     /* interned text, or NULL: an annotation on the run */
  const char *field;       /* interned code ("PAGE", "DATE"...), or NULL: the run
                            * is a field's cached result, Update Fields renews it */
  const char *lang;        /* interned BCP-47 tag ("en-GB", "nb-NO"), or NULL
                            * for the document's own; "none" is not checked */
} W42CharFmt;

/* A paragraph that is an item of a list.  The marker is not text: it is
 * painted in front of the first line, the way a heading's section number
 * is, and numbers are counted at layout time over consecutive numbered
 * paragraphs. */
typedef enum {
  W42_LIST_NONE = 0,
  W42_LIST_BULLET,          /* a round bullet */
  W42_LIST_NUMBER,          /* 1. 2. 3. */
  W42_LIST_LOWER_LETTER,    /* a. b. c. */
  W42_LIST_UPPER_LETTER,    /* A. B. C. */
  W42_LIST_LOWER_ROMAN,     /* i. ii. iii. */
  W42_LIST_UPPER_ROMAN,     /* I. II. III. */
  W42_LIST_BULLET_CIRCLE,   /* an open bullet */
  W42_LIST_BULLET_SQUARE,
  W42_LIST_BULLET_DASH,
  W42_LIST_KINDS
} W42ListKind;

/* Numbered kinds count; bullet kinds all show the same mark. */
gboolean w42_list_is_numbered (W42ListKind kind);
gboolean w42_list_is_bullet   (W42ListKind kind);
/* The marker for item `n` of a list of this kind: "3.", "c.", "iii.", or
 * the bullet character. */
void     w42_list_marker (W42ListKind kind, int n, char *out, gsize size);

/* Tab stops.  Positions are twips from the left margin, as Word measured
 * them, kept sorted; kinds say what lines up at the stop. */
#define W42_MAX_TABS 16

typedef enum {
  W42_TAB_LEFT = 0,
  W42_TAB_CENTER,
  W42_TAB_RIGHT,
  W42_TAB_DECIMAL
} W42TabKind;

/* What fills the gap in front of a tab stop.  Word 6 offered these four
 * on its Tabs dialog, and a table of contents is unreadable without the
 * dots. */
typedef enum {
  W42_TAB_LEAD_NONE = 0,
  W42_TAB_LEAD_DOT,        /* ......... */
  W42_TAB_LEAD_DASH,       /* --------- */
  W42_TAB_LEAD_LINE        /* _________ */
} W42TabLeader;

/* A stop's kind and its leader share one byte: the kind in the low four
 * bits, the leader in the high four, so that a stop is still one number
 * to store, sort and carry through a file. */
#define W42_TAB_KIND(byte)     ((W42TabKind) ((byte) & 0x0F))
#define W42_TAB_LEADER(byte)   ((W42TabLeader) (((byte) >> 4) & 0x0F))
#define W42_TAB_BYTE(kind, leader) \
  ((guint8) (((guint) (kind) & 0x0F) | (((guint) (leader) & 0x0F) << 4)))

/* A cell covered by the vertical merge of the cell above it. */
#define W42_CELL_COVERED 255

/* Which sides of a paragraph carry a border. */
typedef enum {
  W42_BORDER_TOP    = 1 << 0,
  W42_BORDER_BOTTOM = 1 << 1,
  W42_BORDER_LEFT   = 1 << 2,
  W42_BORDER_RIGHT  = 1 << 3,
  W42_BORDER_BOX    = 0xF,
  W42_BORDER_CELL_SET = 0x80   /* on a CELL mark: the cell's own sides, which
                                * override the table's borders setting */
} W42BorderSides;

/* The sides again, as indexes: W42_BORDER_TOP is bit 0, so the top edge
 * is edge[0], and so on down the enum. */
enum {
  W42_EDGE_TOP = 0,
  W42_EDGE_BOTTOM,
  W42_EDGE_LEFT,
  W42_EDGE_RIGHT,
  W42_EDGE_INSIDE_H,   /* a table's rules between its rows ... */
  W42_EDGE_INSIDE_V,   /* ... and between its columns */
  W42_N_EDGES
};

/* How a border's line is drawn.  Word XP's Borders and Shading dialog
 * offered two dozen; these are the ones every file format can say, and
 * the rest come in as the nearest of them. */
typedef enum {
  W42_BORDER_SINGLE = 0,
  W42_BORDER_DOUBLE,
  W42_BORDER_DASHED,
  W42_BORDER_DOTTED,
  W42_BORDER_NONE       /* on a table's edge: that side is not ruled, though
                         * the table is; a cell's sides say so with their
                         * bits instead */
} W42BorderStyle;

/* One edge of a border: its line.  A width of 0 is the hairline, Word's
 * 3/4 point; a colour of 0 is black. */
typedef struct {
  guint8  style;    /* W42BorderStyle */
  guint8  width;    /* twips: 15 is 3/4 pt, 120 is 6 pt */
  guint32 color;    /* 0x00RRGGBB */
} W42BorderEdge;

#define W42_BORDER_HAIRLINE 15

/* The edge's width, with the hairline standing in for none. */
#define W42_EDGE_WIDTH(edge) ((edge)->width > 0 ? (int) (edge)->width : W42_BORDER_HAIRLINE)

typedef struct {
  const char *style;         /* interned, e.g. "Normal", "Heading 1" */
  W42Align    align;
  int         indent_left;   /* twips */
  int         indent_right;  /* twips */
  int         indent_first;  /* twips, may be negative for a hanging indent */
  int         space_before;  /* twips */
  int         space_after;   /* twips */
  int         line_spacing;  /* twips, an exact leading; 0 means unset */
  int         line_spacing_pct; /* 100 single, 150, 200; 0 means unset.
                                 * Takes precedence over line_spacing. */
  guint       page_break_before : 1;   /* the paragraph starts a new page */
  guint       keep_next     : 1;   /* stays on a page with the next paragraph */
  guint       keep_together : 1;   /* its lines stay on one page */
  guint       rtl           : 1;   /* right-to-left paragraph: alignment is
                                    * relative to the right edge */
  guint       widow_control : 1;   /* no lone line at a page's top or foot;
                                    * on by default, as Word had it */
  guint8      list;          /* W42ListKind */
  guint8      list_start;    /* restart the numbering here at this; 0 continues */
  guint8      list_level;    /* 0 outermost .. 8; each level counts on its own */
  guint8      border;        /* W42BorderSides */
  guint8      shading;       /* percent of black behind the paragraph */
  guint8      has_shading_color;  /* the background is a colour, not a grey */
  guint8      cell_valign;   /* on a CELL mark: W42CellVAlign */
  guint32     shading_color; /* 0x00RRGGBB, when has_shading_color */
  W42BorderEdge edge[4];     /* the line of each side, by W42_EDGE_* */
  guint8      section_break; /* the paragraph starts a new section, on a new page */
  guint8      columns;       /* that section's newspaper columns; 0 or 1 is one */
  int         column_gap;    /* twips between them; 0 means a half inch */
  guint8      n_tabs;
  guint8      tab_kind[W42_MAX_TABS];   /* W42TabKind */
  int         tab_pos[W42_MAX_TABS];    /* twips, ascending */
  guint8      drop_cap;      /* lines the dropped first letter spans; 0 none */
  guint8      frame_side;    /* W42FrameSide: the paragraph is set in a frame
                              * at that side of the column, the text after it
                              * running down the other side */
  int         frame_width;   /* twips; 0 means a third of the column */
  guint8      cell_vspan;    /* on a CELL mark: the rows the cell covers.
                              * 0 and 1 are one row; W42_CELL_COVERED means
                              * the cell above covers this one. */
} W42ParaFmt;

typedef enum {
  W42_FRAME_NONE = 0,
  W42_FRAME_LEFT,
  W42_FRAME_RIGHT
} W42FrameSide;

/* Where a cell's text sits when the row is taller than it. */
typedef enum {
  W42_CELL_VALIGN_TOP = 0,
  W42_CELL_VALIGN_CENTER,
  W42_CELL_VALIGN_BOTTOM
} W42CellVAlign;

/* Every side's line at once, which is what the dialogs set. */
void w42_para_fmt_set_edges (W42ParaFmt *pa, int width, guint32 color,
                             W42BorderStyle style);
/* The widest of the sides that are on: what a dialog shows as "the"
 * width; likewise the first such side's colour and style. */
int            w42_para_fmt_border_width (const W42ParaFmt *pa);
/* The style's name as CSS and ODF spell it, "solid", "double", "dashed"
 * or "dotted"; and the style a line spec naming one of those means. */
const char    *w42_border_style_css  (W42BorderStyle style);
W42BorderStyle w42_border_style_from_css (const char *spec);
guint32        w42_para_fmt_border_color (const W42ParaFmt *pa);
W42BorderStyle w42_para_fmt_border_style (const W42ParaFmt *pa);

/* The field codes word42 knows, as Word spells them; NULL for anything
 * else.  A field's text is its cached result until updated. */
const char *w42_field_code (const char *instruction);

/* 1 -> "i", 4 -> "iv", and so on, for endnote marks. */
void w42_roman_lower (int n, char *out, gsize size);

/* Word's sixteen highlight colours, by the index RTF and .doc use. */
guint32 w42_highlight_rgb (int index);

/* Sets a stop at `pos`, replacing one already there; clears one. */
void w42_para_fmt_set_tab   (W42ParaFmt *pa, int pos, W42TabKind kind);
/* The same, with what fills the gap in front of the stop. */
void w42_para_fmt_set_tab_leader (W42ParaFmt *pa, int pos, W42TabKind kind,
                                  W42TabLeader leader);
void w42_para_fmt_clear_tab (W42ParaFmt *pa, int pos);

/* Character and paragraph formatting travel together in one record.  A text
 * piece only ever varies its W42CharFmt half; a block strux only ever varies
 * its W42ParaFmt half.  Keeping them in one struct means one intern table. */
typedef struct {
  W42CharFmt ch;
  W42ParaFmt pa;
} W42Fmt;

/* Interning relies on byte comparison, which is only sound because every
 * W42Fmt is zeroed by w42_fmt_init_default() before its fields are set --
 * that defines the padding bytes.  Always start from this function. */
void w42_fmt_init_default (W42Fmt *fmt);

typedef struct _W42ApTable W42ApTable;

W42ApTable    *w42_ap_table_new     (void);
void           w42_ap_table_free    (W42ApTable *table);
W42ApIdx       w42_ap_table_intern  (W42ApTable *table, const W42Fmt *fmt);
const W42Fmt  *w42_ap_table_get     (W42ApTable *table, W42ApIdx idx);
W42ApIdx       w42_ap_table_default (W42ApTable *table);

G_END_DECLS
