/* w42-omml.c - see w42-omml.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The two languages say the same things differently in two places worth
 * a note.  Office Math's n-ary operator holds what it is of -- the sum
 * holds its terms -- where MathML's is followed by them; read, the terms
 * come out after the operator, and written, the terms up to the next
 * relation go in.  And Office Math's runs are text, letters and digits
 * and signs together, which MathML has as tokens each; read, a run is
 * split up the way TeX would have typed it.
 */

#include "w42-omml.h"
#include "w42-math.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* Reading the element                                                     */
/* ---------------------------------------------------------------------- */

typedef struct {
  W42MathNode *root;
  GPtrArray   *stack;      /* W42MathNode*, not owned */
} Reader;

static const char *
local_name (const char *name)
{
  const char *colon = strchr (name, ':');

  return colon != NULL ? colon + 1 : name;
}

static void
on_start (GMarkupParseContext *ctx, const char *element, const char **names,
          const char **values, gpointer data, GError **error)
{
  Reader *r = data;
  W42MathNode *node;

  (void) ctx; (void) error;
  if (r->stack->len == 0 && r->root != NULL)
    return;
  node = w42_math_node_new (local_name (element), NULL);
  for (int i = 0; names[i] != NULL; i++)
    if (!g_str_has_prefix (names[i], "xmlns"))
      w42_math_node_set_attr (node, local_name (names[i]), values[i]);
  if (r->stack->len == 0)
    r->root = node;
  else
    w42_math_node_add (g_ptr_array_index (r->stack, r->stack->len - 1), node);
  g_ptr_array_add (r->stack, node);
}

static void
on_end (GMarkupParseContext *ctx, const char *element, gpointer data, GError **error)
{
  Reader *r = data;

  (void) ctx; (void) element; (void) error;
  if (r->stack->len > 0)
    g_ptr_array_set_size (r->stack, r->stack->len - 1);
}

static void
on_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  Reader *r = data;
  W42MathNode *top;

  (void) ctx; (void) error;
  if (r->stack->len == 0)
    return;
  top = g_ptr_array_index (r->stack, r->stack->len - 1);
  if (g_str_equal (top->name, "t"))
    {
      char *more = g_strndup (text, len);
      char *joined = g_strconcat (top->text != NULL ? top->text : "", more, NULL);

      g_free (top->text);
      top->text = joined;
      g_free (more);
    }
}

static W42MathNode *
om_parse (const char *xml, gsize len)
{
  static const GMarkupParser PARSER = { on_start, on_end, on_text, NULL, NULL };
  Reader r = { NULL, g_ptr_array_new () };
  GMarkupParseContext *ctx = g_markup_parse_context_new (&PARSER, 0, &r, NULL);
  gboolean ok = g_markup_parse_context_parse (ctx, xml, (gssize) len, NULL) &&
                g_markup_parse_context_end_parse (ctx, NULL);

  g_markup_parse_context_free (ctx);
  g_ptr_array_free (r.stack, TRUE);
  if (!ok)
    {
      w42_math_node_free (r.root);
      return NULL;
    }
  return r.root;
}

/* ---------------------------------------------------------------------- */
/* Office Math to MathML                                                   */
/* ---------------------------------------------------------------------- */

static const W42MathNode *
child (const W42MathNode *n, const char *name)
{
  if (n == NULL)
    return NULL;
  for (guint i = 0; i < n->children->len; i++)
    {
      const W42MathNode *k = g_ptr_array_index (n->children, i);

      if (g_str_equal (k->name, name))
        return k;
    }
  return NULL;
}

/* A property's value: `props`'s child `name`, its m:val.  "" for one that
 * is there with no value, NULL for one that is not there. */
static const char *
prop (const W42MathNode *props, const char *name)
{
  const W42MathNode *k = child (props, name);
  const char *v;

  if (k == NULL)
    return NULL;
  v = w42_math_node_attr (k, "val");
  return v != NULL ? v : "";
}

/* An on/off property: on when there, unless its value says off. */
static gboolean
flag (const W42MathNode *props, const char *name)
{
  const char *v = prop (props, name);

  return v != NULL && !g_str_equal (v, "0") && !g_str_equal (v, "off") && !g_str_equal (v, "false");
}

typedef struct {
  int size;                /* the half-points a run said, or 0 */
} Conv;

static void om_items (Conv *c, const W42MathNode *om, W42MathNode *row);

static W42MathNode *
mk (const char *name, const char *text)
{
  return w42_math_node_new (name, text);
}

/* What `om` holds as one node: its only one, or a row of them. */
static W42MathNode *
om_row (Conv *c, const W42MathNode *om)
{
  W42MathNode *row = mk ("mrow", NULL);

  if (om != NULL)
    om_items (c, om, row);
  if (row->children->len == 1)
    {
      W42MathNode *only = g_ptr_array_steal_index (row->children, 0);

      w42_math_node_free (row);
      return only;
    }
  return row;
}

static W42MathNode *
pair (const char *name, W42MathNode *a, W42MathNode *b)
{
  W42MathNode *n = mk (name, NULL);

  w42_math_node_add (n, a);
  if (b != NULL)
    w42_math_node_add (n, b);
  return n;
}

static W42MathNode *
op (const char *text)
{
  return mk ("mo", text);
}

/* A combining accent as Office Math has it, as the character MathML sets
 * over the letter. */
static const char *
spacing_accent (const char *chr)
{
  static const struct { gunichar combining; const char *spacing; } ACCENTS[] = {
    { 0x0302, "^" }, { 0x0303, "~" }, { 0x0304, "¯" }, { 0x0305, "¯" },
    { 0x20D7, "→" }, { 0x20D6, "←" }, { 0x20E1, "↔" }, { 0x0307, "˙" },
    { 0x0308, "¨" }, { 0x0301, "´" }, { 0x0300, "`" }, { 0x0306, "˘" },
    { 0x030C, "ˇ" }, { 0x033F, "¯" },
  };
  gunichar c;

  if (chr == NULL || *chr == '\0')
    return "^";
  c = g_utf8_get_char (chr);
  for (guint i = 0; i < G_N_ELEMENTS (ACCENTS); i++)
    if (ACCENTS[i].combining == c)
      return ACCENTS[i].spacing;
  return chr;
}

/* The letters of an Office Math alphabet, as MathML's mathvariant. */
static const char *
script_variant (const char *scr)
{
  if (scr == NULL) return NULL;
  if (g_str_equal (scr, "double-struck")) return "double-struck";
  if (g_str_equal (scr, "script")) return "script";
  if (g_str_equal (scr, "fraktur")) return "fraktur";
  if (g_str_equal (scr, "sans-serif")) return "sans-serif";
  if (g_str_equal (scr, "monospace")) return "monospace";
  return NULL;
}

/* A run's text as MathML's tokens: numbers, letters and signs.  Upright
 * letters together are a name -- sin, lim, d -- and italic ones each a
 * variable, as TeX would have them. */
static void
run_tokens (W42MathNode *row, const char *text, const char *sty, gboolean normal_text,
            const char *scr)
{
  const char *variant = script_variant (scr);
  gboolean plain = sty != NULL && g_str_equal (sty, "p") && variant == NULL;
  GString *name = g_string_new (NULL), *number = g_string_new (NULL);

  if (variant == NULL && sty != NULL)
    variant = g_str_equal (sty, "b") ? "bold" : g_str_equal (sty, "bi") ? "bold-italic" : NULL;
  if (normal_text)
    {
      if (*text != '\0')
        w42_math_node_add (row, mk ("mtext", text));
      g_string_free (name, TRUE);
      g_string_free (number, TRUE);
      return;
    }

#define FLUSH_NUMBER() G_STMT_START {                                          \
    if (number->len > 0)                                                       \
      {                                                                        \
        W42MathNode *mn = mk ("mn", number->str);                              \
        if (variant != NULL)                                                   \
          w42_math_node_set_attr (mn, "mathvariant", variant);                 \
        w42_math_node_add (row, mn);                                           \
        g_string_truncate (number, 0);                                         \
      }                                                                        \
  } G_STMT_END
#define FLUSH_NAME() G_STMT_START {                                            \
    if (name->len > 0)                                                         \
      {                                                                        \
        W42MathNode *mi = mk ("mi", name->str);                                \
        if (g_utf8_strlen (name->str, -1) == 1)                                \
          w42_math_node_set_attr (mi, "mathvariant", "normal");                \
        w42_math_node_add (row, mi);                                           \
        g_string_truncate (name, 0);                                           \
      }                                                                        \
  } G_STMT_END

  for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (g_unichar_isdigit (c) ||
          (c == '.' && number->len > 0 && g_unichar_isdigit (g_utf8_get_char (g_utf8_next_char (p)))))
        {
          FLUSH_NAME ();
          g_string_append_unichar (number, c);
          continue;
        }
      FLUSH_NUMBER ();
      if (g_unichar_isalpha (c))
        {
          if (plain)
            g_string_append_unichar (name, c);
          else
            {
              char utf8[8] = { 0 };
              W42MathNode *mi;

              g_unichar_to_utf8 (c, utf8);
              mi = mk ("mi", utf8);
              if (variant != NULL)
                w42_math_node_set_attr (mi, "mathvariant", variant);
              w42_math_node_add (row, mi);
            }
          continue;
        }
      FLUSH_NAME ();
      /* A space, and the & that aligns an equation array, take no room. */
      if (c == ' ' || c == '&' || c == 0xA0)
        continue;
      {
        char utf8[8] = { 0 };
        W42MathNode *mo;

        g_unichar_to_utf8 (c, utf8);
        mo = op (utf8);
        /* A bracket typed in a run keeps its size; m:d's grow. */
        if (strchr ("()[]{}|", (int) c) != NULL && c < 0x80)
          w42_math_node_set_attr (mo, "stretchy", "false");
        w42_math_node_add (row, mo);
      }
    }
  FLUSH_NUMBER ();
  FLUSH_NAME ();
#undef FLUSH_NUMBER
#undef FLUSH_NAME
  g_string_free (name, TRUE);
  g_string_free (number, TRUE);
}

static void
om_run (Conv *c, const W42MathNode *r, W42MathNode *row)
{
  GString *text = g_string_new (NULL);
  const char *sty = NULL, *scr = NULL;
  gboolean normal_text = FALSE;

  for (guint i = 0; i < r->children->len; i++)
    {
      const W42MathNode *k = g_ptr_array_index (r->children, i);

      if (g_str_equal (k->name, "t") && k->text != NULL)
        g_string_append (text, k->text);
      else if (g_str_equal (k->name, "rPr"))
        {
          /* m:rPr says how the math is set; w:rPr the font and size. */
          if (child (k, "sty") != NULL) sty = prop (k, "sty");
          if (child (k, "scr") != NULL) scr = prop (k, "scr");
          if (flag (k, "nor")) normal_text = TRUE;
          if (child (k, "sz") != NULL && c->size == 0)
            c->size = (int) g_ascii_strtoll (prop (k, "sz"), NULL, 10);
        }
    }
  run_tokens (row, text->str, sty, normal_text, scr);
  g_string_free (text, TRUE);
}

static const char *const INTEGRALS[] = { "∫", "∬", "∭", "∮", "∯", "∰", NULL };

static void
om_item (Conv *c, const W42MathNode *n, W42MathNode *row)
{
  const char *name = n->name;

  if (g_str_equal (name, "r"))
    om_run (c, n, row);
  else if (g_str_equal (name, "f"))
    {
      const char *type = prop (child (n, "fPr"), "type");
      W42MathNode *num = om_row (c, child (n, "num")), *den = om_row (c, child (n, "den"));

      if (type != NULL && g_str_equal (type, "lin"))
        {
          w42_math_node_add (row, num);
          w42_math_node_add (row, op ("/"));
          w42_math_node_add (row, den);
          return;
        }
      {
        W42MathNode *frac = pair ("mfrac", num, den);

        if (type != NULL && g_str_equal (type, "noBar"))
          w42_math_node_set_attr (frac, "linethickness", "0");
        else if (type != NULL && g_str_equal (type, "skw"))
          w42_math_node_set_attr (frac, "bevelled", "true");
        w42_math_node_add (row, frac);
      }
    }
  else if (g_str_equal (name, "sSup"))
    w42_math_node_add (row, pair ("msup", om_row (c, child (n, "e")), om_row (c, child (n, "sup"))));
  else if (g_str_equal (name, "sSub"))
    w42_math_node_add (row, pair ("msub", om_row (c, child (n, "e")), om_row (c, child (n, "sub"))));
  else if (g_str_equal (name, "sSubSup"))
    {
      W42MathNode *s = pair ("msubsup", om_row (c, child (n, "e")), om_row (c, child (n, "sub")));

      w42_math_node_add (s, om_row (c, child (n, "sup")));
      w42_math_node_add (row, s);
    }
  else if (g_str_equal (name, "sPre"))
    {
      W42MathNode *s = pair ("mmultiscripts", om_row (c, child (n, "e")), mk ("mprescripts", NULL));

      w42_math_node_add (s, om_row (c, child (n, "sub")));
      w42_math_node_add (s, om_row (c, child (n, "sup")));
      w42_math_node_add (row, s);
    }
  else if (g_str_equal (name, "rad"))
    {
      const W42MathNode *deg = child (n, "deg");

      if (flag (child (n, "radPr"), "degHide") || deg == NULL || deg->children->len == 0)
        w42_math_node_add (row, pair ("msqrt", om_row (c, child (n, "e")), NULL));
      else
        w42_math_node_add (row, pair ("mroot", om_row (c, child (n, "e")), om_row (c, deg)));
    }
  else if (g_str_equal (name, "d"))
    {
      const W42MathNode *pr = child (n, "dPr");
      const char *beg = prop (pr, "begChr"), *end = prop (pr, "endChr"), *sep = prop (pr, "sepChr");
      W42MathNode *d = mk ("mrow", NULL);
      guint k = 0;

      if (beg == NULL) beg = "(";
      if (end == NULL) end = ")";
      if (sep == NULL) sep = "|";
      if (*beg != '\0')
        w42_math_node_add (d, op (beg));
      for (guint i = 0; i < n->children->len; i++)
        {
          const W42MathNode *e = g_ptr_array_index (n->children, i);

          if (!g_str_equal (e->name, "e"))
            continue;
          if (k++ > 0 && *sep != '\0')
            w42_math_node_add (d, op (sep));
          w42_math_node_add (d, om_row (c, e));
        }
      if (*end != '\0')
        w42_math_node_add (d, op (end));
      w42_math_node_add (row, d);
    }
  else if (g_str_equal (name, "nary"))
    {
      const W42MathNode *pr = child (n, "naryPr");
      const char *chr = prop (pr, "chr"), *loc = prop (pr, "limLoc");
      const W42MathNode *sub = child (n, "sub"), *sup = child (n, "sup");
      gboolean integral, under;
      W42MathNode *base, *sb = NULL, *sp = NULL;

      if (chr == NULL || *chr == '\0')
        chr = "∫";
      integral = g_strv_contains (INTEGRALS, chr);
      under = loc != NULL ? g_str_equal (loc, "undOvr") : !integral;
      if (!flag (pr, "subHide") && sub != NULL && sub->children->len > 0)
        sb = om_row (c, sub);
      if (!flag (pr, "supHide") && sup != NULL && sup->children->len > 0)
        sp = om_row (c, sup);
      base = op (chr);
      if (sb != NULL && sp != NULL)
        {
          base = pair (under ? "munderover" : "msubsup", base, sb);
          w42_math_node_add (base, sp);
        }
      else if (sb != NULL)
        base = pair (under ? "munder" : "msub", base, sb);
      else if (sp != NULL)
        base = pair (under ? "mover" : "msup", base, sp);
      w42_math_node_add (row, base);
      /* What it is of follows it, in MathML. */
      if (child (n, "e") != NULL)
        om_items (c, child (n, "e"), row);
    }
  else if (g_str_equal (name, "func"))
    {
      om_items (c, child (n, "fName"), row);
      w42_math_node_add (row, op ("\342\201\241"));
      w42_math_node_add (row, om_row (c, child (n, "e")));
    }
  else if (g_str_equal (name, "limLow") || g_str_equal (name, "limUpp"))
    w42_math_node_add (row, pair (name[3] == 'L' ? "munder" : "mover",
                                  om_row (c, child (n, "e")), om_row (c, child (n, "lim"))));
  else if (g_str_equal (name, "acc"))
    {
      W42MathNode *a = pair ("mover", om_row (c, child (n, "e")),
                             op (spacing_accent (prop (child (n, "accPr"), "chr"))));

      w42_math_node_set_attr (a, "accent", "true");
      w42_math_node_add (row, a);
    }
  else if (g_str_equal (name, "bar"))
    {
      const char *pos = prop (child (n, "barPr"), "pos");
      gboolean top = pos != NULL && g_str_equal (pos, "top");
      W42MathNode *b = pair (top ? "mover" : "munder", om_row (c, child (n, "e")), op (top ? "¯" : "_"));

      w42_math_node_set_attr (b, top ? "accent" : "accentunder", "true");
      w42_math_node_add (row, b);
    }
  else if (g_str_equal (name, "groupChr"))
    {
      const W42MathNode *pr = child (n, "groupChrPr");
      const char *chr = prop (pr, "chr"), *pos = prop (pr, "pos");
      gboolean top = pos != NULL && g_str_equal (pos, "top");

      if (chr == NULL || *chr == '\0')
        chr = "⏟";
      w42_math_node_add (row, pair (top ? "mover" : "munder", om_row (c, child (n, "e")), op (chr)));
    }
  else if (g_str_equal (name, "m") || g_str_equal (name, "eqArr"))
    {
      W42MathNode *table = mk ("mtable", NULL);
      gboolean array = g_str_equal (name, "eqArr");

      for (guint i = 0; i < n->children->len; i++)
        {
          const W42MathNode *r = g_ptr_array_index (n->children, i);
          W42MathNode *tr;

          if (array && g_str_equal (r->name, "e"))
            {
              W42MathNode *td = mk ("mtd", NULL);

              om_items (c, r, td);
              tr = pair ("mtr", td, NULL);
              w42_math_node_add (table, tr);
              continue;
            }
          if (!g_str_equal (r->name, "mr"))
            continue;
          tr = mk ("mtr", NULL);
          for (guint j = 0; j < r->children->len; j++)
            {
              const W42MathNode *e = g_ptr_array_index (r->children, j);
              W42MathNode *td;

              if (!g_str_equal (e->name, "e"))
                continue;
              td = mk ("mtd", NULL);
              om_items (c, e, td);
              w42_math_node_add (tr, td);
            }
          w42_math_node_add (table, tr);
        }
      if (array)
        w42_math_node_set_attr (table, "displaystyle", "true");
      w42_math_node_add (row, table);
    }
  else if (g_str_equal (name, "borderBox"))
    {
      W42MathNode *box = pair ("menclose", om_row (c, child (n, "e")), NULL);

      w42_math_node_set_attr (box, "notation", "box");
      w42_math_node_add (row, box);
    }
  else if (g_str_equal (name, "phant"))
    {
      const char *show = prop (child (n, "phantPr"), "show");

      if (show != NULL && (g_str_equal (show, "0") || g_str_equal (show, "off")))
        w42_math_node_add (row, pair ("mphantom", om_row (c, child (n, "e")), NULL));
      else
        om_items (c, child (n, "e"), row);
    }
  else if (g_str_equal (name, "box") || g_str_equal (name, "e") || g_str_equal (name, "oMath") ||
           g_str_equal (name, "num") || g_str_equal (name, "den") || g_str_equal (name, "sdtContent") ||
           g_str_equal (name, "sdt") || g_str_equal (name, "ins") || g_str_equal (name, "smartTag"))
    om_items (c, g_str_equal (name, "box") ? child (n, "e") : n, row);
  /* Properties, bookmarks, proofing marks: nothing to set. */
}

static void
om_items (Conv *c, const W42MathNode *om, W42MathNode *row)
{
  if (om == NULL)
    return;
  for (guint i = 0; i < om->children->len; i++)
    om_item (c, g_ptr_array_index (om->children, i), row);
}

char *
w42_omml_to_mathml (const char *xml, gsize len, gboolean display, int *size)
{
  W42MathNode *om = xml != NULL ? om_parse (xml, len) : NULL;
  W42MathNode *math, *row;
  Conv c = { 0 };
  char *out;

  if (size != NULL)
    *size = 0;
  if (om == NULL)
    return NULL;
  if (!g_str_equal (om->name, "oMath") && !g_str_equal (om->name, "oMathPara"))
    {
      w42_math_node_free (om);
      return NULL;
    }
  math = mk ("math", NULL);
  if (display)
    w42_math_node_set_attr (math, "display", "block");
  row = mk ("mrow", NULL);
  om_items (&c, om, row);
  w42_math_node_add (math, row);
  out = w42_math_node_to_string (math);
  w42_math_node_free (math);
  w42_math_node_free (om);
  if (size != NULL)
    *size = c.size;
  return out;
}

/* ---------------------------------------------------------------------- */
/* MathML to Office Math                                                   */
/* ---------------------------------------------------------------------- */

static void to_om (GString *out, const W42MathNode *n);
static void to_om_items (GString *out, const W42MathNode *n, guint from, guint to);

static void
esc (GString *out, const char *s)
{
  for (const char *p = s != NULL ? s : ""; *p != '\0'; p++)
    switch (*p)
      {
      case '<': g_string_append (out, "&lt;"); break;
      case '>': g_string_append (out, "&gt;"); break;
      case '&': g_string_append (out, "&amp;"); break;
      case '"': g_string_append (out, "&quot;"); break;
      default:  g_string_append_c (out, *p);
      }
}

/* A run of math text: `sty` "p" for upright, "b" bold, "bi" bold italic,
 * NULL for Word's own choice (italic letters); `scr` an alphabet; as
 * ordinary text, not mathematics, when `normal`.  In Cambria Math, as
 * Word sets its equations. */
static void
om_text (GString *out, const char *text, const char *sty, const char *scr, gboolean normal)
{
  GString *clean = g_string_new (NULL);

  /* Function application and invisible times are MathML's to say. */
  for (const char *p = text != NULL ? text : ""; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c != 0x2061 && c != 0x2062 && c != 0x2063 && c != 0x2064)
        g_string_append_unichar (clean, c);
    }
  if (clean->len == 0)
    {
      g_string_free (clean, TRUE);
      return;
    }
  g_string_append (out, "<m:r>");
  if (sty != NULL || scr != NULL || normal)
    {
      g_string_append (out, "<m:rPr>");
      if (scr != NULL)
        g_string_append_printf (out, "<m:scr m:val=\"%s\"/>", scr);
      if (sty != NULL)
        g_string_append_printf (out, "<m:sty m:val=\"%s\"/>", sty);
      if (normal)
        g_string_append (out, "<m:nor/>");
      g_string_append (out, "</m:rPr>");
    }
  g_string_append (out, "<w:rPr><w:rFonts w:ascii=\"Cambria Math\" w:hAnsi=\"Cambria Math\"/></w:rPr>"
                        "<m:t xml:space=\"preserve\">");
  esc (out, clean->str);
  g_string_append (out, "</m:t></m:r>");
  g_string_free (clean, TRUE);
}

static const W42MathNode *
kid (const W42MathNode *n, guint i)
{
  return n != NULL && i < n->children->len ? g_ptr_array_index (n->children, i) : NULL;
}

/* An argument: the element Office Math wants round it, and what goes in. */
static void
om_arg (GString *out, const char *tag, const W42MathNode *n)
{
  g_string_append_printf (out, "<m:%s>", tag);
  if (n != NULL)
    to_om (out, n);
  g_string_append_printf (out, "</m:%s>", tag);
}

static gboolean
is_large (const W42MathNode *n)
{
  static const char *const LARGE[] = { "∑", "∏", "∐", "⋃", "⋂", "⨁", "⨂", "⨀", "⋀", "⋁", "⨄",
                                       "∫", "∬", "∭", "∮", "∯", "∰", NULL };

  return n != NULL && g_str_equal (n->name, "mo") && n->text != NULL && g_strv_contains (LARGE, n->text);
}

/* A large operator, with the limits it has: the mo, and its under or
 * lower and over or upper limits. */
static const W42MathNode *
big_operator (const W42MathNode *n, const W42MathNode **lower, const W42MathNode **upper,
              gboolean *under)
{
  const char *name = n->name;

  *lower = *upper = NULL;
  *under = FALSE;
  if (is_large (n))
    return n;
  if (!is_large (kid (n, 0)))
    return NULL;
  *under = g_str_has_prefix (name, "munder") || g_str_equal (name, "mover");
  if (g_str_equal (name, "msub") || g_str_equal (name, "munder"))
    *lower = kid (n, 1);
  else if (g_str_equal (name, "msup") || g_str_equal (name, "mover"))
    *upper = kid (n, 1);
  else if (g_str_equal (name, "msubsup") || g_str_equal (name, "munderover"))
    {
      *lower = kid (n, 1);
      *upper = kid (n, 2);
    }
  else
    return NULL;
  return kid (n, 0);
}

static gboolean
is_relation (const W42MathNode *n)
{
  static const char *const REL[] = { "=", "<", ">", "≤", "≥", "≠", "≈", "≡", "∼", "≃", "≅", "∝",
                                     "∈", "∉", "⊂", "⊃", "⊆", "⊇", "→", "←", "↔", "⇒", "⇐", "⇔",
                                     ",", ";", NULL };

  return n != NULL && g_str_equal (n->name, "mo") && n->text != NULL && g_strv_contains (REL, n->text);
}

static gboolean
is_fence (const W42MathNode *n)
{
  static const char *const FENCES[] = { "(", ")", "[", "]", "{", "}", "|", "‖", "⟨", "⟩",
                                        "⌈", "⌉", "⌊", "⌋", "⟦", "⟧", NULL };
  const char *stretchy;

  if (n == NULL || !g_str_equal (n->name, "mo") || n->text == NULL ||
      !g_strv_contains (FENCES, n->text))
    return FALSE;
  stretchy = w42_math_node_attr (n, "stretchy");
  return stretchy == NULL || !g_str_equal (stretchy, "false");
}

/* The combining accent Office Math sets for a character over a letter,
 * or NULL. */
static const char *
combining_accent (const char *mark)
{
  static const struct { const char *spacing; const char *combining; } ACCENTS[] = {
    { "^", "\314\202" }, { "ˆ", "\314\202" }, { "~", "\314\203" }, { "˜", "\314\203" },
    { "¯", "\314\205" }, { "‾", "\314\205" }, { "→", "\342\203\227" }, { "←", "\342\203\226" },
    { "↔", "\342\203\241" }, { "˙", "\314\207" }, { "¨", "\314\210" }, { "´", "\314\201" },
    { "`", "\314\200" }, { "˘", "\314\206" }, { "ˇ", "\314\214" },
  };

  if (mark == NULL)
    return NULL;
  for (guint i = 0; i < G_N_ELEMENTS (ACCENTS); i++)
    if (g_str_equal (ACCENTS[i].spacing, mark))
      return ACCENTS[i].combining;
  return NULL;
}

static void
to_om_items (GString *out, const W42MathNode *n, guint from, guint to)
{
  for (guint i = from; i < to; i++)
    {
      const W42MathNode *k = kid (n, i);
      const W42MathNode *lower, *upper, *opn;
      gboolean under;

      if ((opn = big_operator (k, &lower, &upper, &under)) != NULL)
        {
          /* An n-ary operator holds what it is of: what follows it, up
           * to a relation. */
          guint j = i + 1;
          gboolean integral = strstr ("∫∬∭∮∯∰", opn->text) != NULL;

          while (j < to && !is_relation (kid (n, j)))
            j++;
          g_string_append (out, "<m:nary><m:naryPr><m:chr m:val=\"");
          esc (out, opn->text);
          g_string_append_printf (out, "\"/><m:limLoc m:val=\"%s\"/>",
                                  under || (!integral && lower == NULL && upper == NULL) ? "undOvr" : "subSup");
          if (lower == NULL)
            g_string_append (out, "<m:subHide m:val=\"1\"/>");
          if (upper == NULL)
            g_string_append (out, "<m:supHide m:val=\"1\"/>");
          g_string_append (out, "</m:naryPr>");
          om_arg (out, "sub", lower);
          om_arg (out, "sup", upper);
          g_string_append (out, "<m:e>");
          to_om_items (out, n, i + 1, j);
          g_string_append (out, "</m:e></m:nary>");
          i = j - 1;
          continue;
        }
      to_om (out, k);
    }
}

static void
om_delimited (GString *out, const char *beg, const char *end, const W42MathNode *n,
              guint from, guint to)
{
  g_string_append (out, "<m:d><m:dPr><m:begChr m:val=\"");
  esc (out, beg);
  g_string_append (out, "\"/><m:endChr m:val=\"");
  esc (out, end);
  g_string_append (out, "\"/></m:dPr><m:e>");
  to_om_items (out, n, from, to);
  g_string_append (out, "</m:e></m:d>");
}

static void
to_om (GString *out, const W42MathNode *n)
{
  const char *name = n->name;

  if (g_str_equal (name, "mi"))
    {
      const char *variant = w42_math_node_attr (n, "mathvariant");
      const char *scr = NULL, *sty = NULL;

      if (variant != NULL)
        {
          if (g_str_equal (variant, "normal")) sty = "p";
          else if (g_str_equal (variant, "bold")) sty = "b";
          else if (g_str_equal (variant, "bold-italic")) sty = "bi";
          else if (g_str_equal (variant, "italic")) sty = "i";
          else if (g_str_equal (variant, "double-struck") || g_str_equal (variant, "script") ||
                   g_str_equal (variant, "fraktur") || g_str_equal (variant, "sans-serif") ||
                   g_str_equal (variant, "monospace"))
            {
              scr = variant;
              sty = "p";
            }
        }
      else if (n->text != NULL && g_utf8_strlen (n->text, -1) > 1)
        sty = "p";                  /* a name: sin, max */
      om_text (out, n->text, sty, scr, FALSE);
      return;
    }
  if (g_str_equal (name, "mn") || g_str_equal (name, "mo"))
    {
      om_text (out, n->text, "p", NULL, FALSE);
      return;
    }
  if (g_str_equal (name, "mtext") || g_str_equal (name, "ms"))
    {
      om_text (out, n->text, "p", NULL, TRUE);
      return;
    }
  if (g_str_equal (name, "mspace"))
    {
      const char *w = w42_math_node_attr (n, "width");

      if (w != NULL && g_ascii_strtod (w, NULL) >= 0.2)
        om_text (out, " ", "p", NULL, TRUE);
      return;
    }
  if (g_str_equal (name, "msup") || g_str_equal (name, "msub") || g_str_equal (name, "msubsup"))
    {
      const W42MathNode *lower, *upper;
      gboolean under;

      /* A large operator alone, whose terms are not there to hold. */
      if (big_operator (n, &lower, &upper, &under) != NULL)
        {
          GPtrArray *one = g_ptr_array_new ();
          W42MathNode holder = { g_intern_static_string ("mrow"), NULL, one, NULL };

          g_ptr_array_add (one, (gpointer) n);
          to_om_items (out, &holder, 0, 1);
          g_ptr_array_free (one, TRUE);
          return;
        }
      if (g_str_equal (name, "msup"))
        {
          g_string_append (out, "<m:sSup>");
          om_arg (out, "e", kid (n, 0));
          om_arg (out, "sup", kid (n, 1));
          g_string_append (out, "</m:sSup>");
        }
      else if (g_str_equal (name, "msub"))
        {
          g_string_append (out, "<m:sSub>");
          om_arg (out, "e", kid (n, 0));
          om_arg (out, "sub", kid (n, 1));
          g_string_append (out, "</m:sSub>");
        }
      else
        {
          g_string_append (out, "<m:sSubSup>");
          om_arg (out, "e", kid (n, 0));
          om_arg (out, "sub", kid (n, 1));
          om_arg (out, "sup", kid (n, 2));
          g_string_append (out, "</m:sSubSup>");
        }
      return;
    }
  if (g_str_equal (name, "mfrac"))
    {
      const char *lt = w42_math_node_attr (n, "linethickness");
      const char *bevelled = w42_math_node_attr (n, "bevelled");

      g_string_append (out, "<m:f>");
      if (lt != NULL && g_ascii_strtod (lt, NULL) == 0.0 && g_ascii_isdigit (lt[0]))
        g_string_append (out, "<m:fPr><m:type m:val=\"noBar\"/></m:fPr>");
      else if (bevelled != NULL && g_str_equal (bevelled, "true"))
        g_string_append (out, "<m:fPr><m:type m:val=\"skw\"/></m:fPr>");
      om_arg (out, "num", kid (n, 0));
      om_arg (out, "den", kid (n, 1));
      g_string_append (out, "</m:f>");
      return;
    }
  if (g_str_equal (name, "msqrt"))
    {
      g_string_append (out, "<m:rad><m:radPr><m:degHide m:val=\"1\"/></m:radPr><m:deg/><m:e>");
      to_om_items (out, n, 0, n->children->len);
      g_string_append (out, "</m:e></m:rad>");
      return;
    }
  if (g_str_equal (name, "mroot"))
    {
      g_string_append (out, "<m:rad>");
      om_arg (out, "deg", kid (n, 1));
      om_arg (out, "e", kid (n, 0));
      g_string_append (out, "</m:rad>");
      return;
    }
  if (g_str_equal (name, "munder") || g_str_equal (name, "mover") || g_str_equal (name, "munderover"))
    {
      const W42MathNode *base = kid (n, 0);
      const W42MathNode *mark = kid (n, 1);
      const W42MathNode *lower, *upper;
      gboolean under;

      if (big_operator (n, &lower, &upper, &under) != NULL)
        {
          GPtrArray *one = g_ptr_array_new ();
          W42MathNode holder = { g_intern_static_string ("mrow"), NULL, one, NULL };

          g_ptr_array_add (one, (gpointer) n);
          to_om_items (out, &holder, 0, 1);
          g_ptr_array_free (one, TRUE);
          return;
        }
      if (!g_str_equal (name, "munderover") && mark != NULL && g_str_equal (mark->name, "mo") &&
          mark->text != NULL)
        {
          gboolean over = g_str_equal (name, "mover");
          gboolean single = base != NULL && (g_str_equal (base->name, "mi") || g_str_equal (base->name, "mn")) &&
                            base->text != NULL && g_utf8_strlen (base->text, -1) == 1;
          const char *comb = combining_accent (mark->text);

          if ((g_str_equal (mark->text, "⏞") && over) || (g_str_equal (mark->text, "⏟") && !over))
            {
              g_string_append_printf (out, "<m:groupChr><m:groupChrPr><m:chr m:val=\"%s\"/>"
                                      "<m:pos m:val=\"%s\"/><m:vertJc m:val=\"%s\"/></m:groupChrPr>",
                                      mark->text, over ? "top" : "bot", over ? "bot" : "top");
              om_arg (out, "e", base);
              g_string_append (out, "</m:groupChr>");
              return;
            }
          if ((over && (g_str_equal (mark->text, "¯") || g_str_equal (mark->text, "‾")) && !single) ||
              (!over && (g_str_equal (mark->text, "_") || g_str_equal (mark->text, "¯"))))
            {
              g_string_append_printf (out, "<m:bar><m:barPr><m:pos m:val=\"%s\"/></m:barPr>",
                                      over ? "top" : "bot");
              om_arg (out, "e", base);
              g_string_append (out, "</m:bar>");
              return;
            }
          if (over && comb != NULL)
            {
              g_string_append_printf (out, "<m:acc><m:accPr><m:chr m:val=\"%s\"/></m:accPr>", comb);
              om_arg (out, "e", base);
              g_string_append (out, "</m:acc>");
              return;
            }
        }
      if (g_str_equal (name, "munderover"))
        {
          g_string_append (out, "<m:limUpp><m:e><m:limLow>");
          om_arg (out, "e", base);
          om_arg (out, "lim", kid (n, 1));
          g_string_append (out, "</m:limLow></m:e>");
          om_arg (out, "lim", kid (n, 2));
          g_string_append (out, "</m:limUpp>");
          return;
        }
      g_string_append_printf (out, "<m:%s>", g_str_equal (name, "munder") ? "limLow" : "limUpp");
      om_arg (out, "e", base);
      om_arg (out, "lim", mark);
      g_string_append_printf (out, "</m:%s>", g_str_equal (name, "munder") ? "limLow" : "limUpp");
      return;
    }
  if (g_str_equal (name, "mtable"))
    {
      guint cols = 1;

      for (guint r = 0; r < n->children->len; r++)
        cols = MAX (cols, kid (n, r)->children->len);
      g_string_append_printf (out, "<m:m><m:mPr><m:mcs><m:mc><m:mcPr><m:count m:val=\"%u\"/>"
                                   "<m:mcJc m:val=\"center\"/></m:mcPr></m:mc></m:mcs></m:mPr>", cols);
      for (guint r = 0; r < n->children->len; r++)
        {
          const W42MathNode *row = kid (n, r);

          g_string_append (out, "<m:mr>");
          if (!g_str_equal (row->name, "mtr") && !g_str_equal (row->name, "mlabeledtr"))
            om_arg (out, "e", row);
          else
            for (guint c = g_str_equal (row->name, "mlabeledtr") ? 1 : 0; c < row->children->len; c++)
              {
                const W42MathNode *cell = kid (row, c);

                g_string_append (out, "<m:e>");
                to_om_items (out, cell, 0, cell->children->len);
                g_string_append (out, "</m:e>");
              }
          /* Word wants every row as wide as the matrix. */
          for (guint c = row->children->len; c < cols; c++)
            g_string_append (out, "<m:e/>");
          g_string_append (out, "</m:mr>");
        }
      g_string_append (out, "</m:m>");
      return;
    }
  if (g_str_equal (name, "menclose"))
    {
      const char *notation = w42_math_node_attr (n, "notation");

      if (notation != NULL && strstr (notation, "radical") != NULL)
        {
          g_string_append (out, "<m:rad><m:radPr><m:degHide m:val=\"1\"/></m:radPr><m:deg/><m:e>");
          to_om_items (out, n, 0, n->children->len);
          g_string_append (out, "</m:e></m:rad>");
          return;
        }
      g_string_append (out, "<m:borderBox><m:e>");
      to_om_items (out, n, 0, n->children->len);
      g_string_append (out, "</m:e></m:borderBox>");
      return;
    }
  if (g_str_equal (name, "mphantom"))
    {
      g_string_append (out, "<m:phant><m:phantPr><m:show m:val=\"0\"/></m:phantPr><m:e>");
      to_om_items (out, n, 0, n->children->len);
      g_string_append (out, "</m:e></m:phant>");
      return;
    }
  if (g_str_equal (name, "mfenced"))
    {
      const char *open = w42_math_node_attr (n, "open"), *close = w42_math_node_attr (n, "close");
      const char *seps = w42_math_node_attr (n, "separators");

      g_string_append (out, "<m:d><m:dPr><m:begChr m:val=\"");
      esc (out, open != NULL ? open : "(");
      g_string_append (out, "\"/><m:sepChr m:val=\"");
      esc (out, seps != NULL ? seps : ",");
      g_string_append (out, "\"/><m:endChr m:val=\"");
      esc (out, close != NULL ? close : ")");
      g_string_append (out, "\"/></m:dPr>");
      for (guint i = 0; i < n->children->len; i++)
        om_arg (out, "e", kid (n, i));
      g_string_append (out, "</m:d>");
      return;
    }
  if (g_str_equal (name, "mmultiscripts"))
    {
      guint split = n->children->len;

      for (guint i = 1; i < n->children->len; i++)
        if (g_str_equal (kid (n, i)->name, "mprescripts"))
          split = i;
      if (split + 2 < n->children->len)
        {
          g_string_append (out, "<m:sPre>");
          om_arg (out, "sub", g_str_equal (kid (n, split + 1)->name, "none") ? NULL : kid (n, split + 1));
          om_arg (out, "sup", g_str_equal (kid (n, split + 2)->name, "none") ? NULL : kid (n, split + 2));
          g_string_append (out, "<m:e>");
        }
      if (split >= 3)
        {
          g_string_append (out, "<m:sSubSup>");
          om_arg (out, "e", kid (n, 0));
          om_arg (out, "sub", g_str_equal (kid (n, 1)->name, "none") ? NULL : kid (n, 1));
          om_arg (out, "sup", g_str_equal (kid (n, 2)->name, "none") ? NULL : kid (n, 2));
          g_string_append (out, "</m:sSubSup>");
        }
      else if (kid (n, 0) != NULL)
        to_om (out, kid (n, 0));
      if (split + 2 < n->children->len)
        g_string_append (out, "</m:e></m:sPre>");
      return;
    }
  if (g_str_equal (name, "semantics"))
    {
      if (kid (n, 0) != NULL)
        to_om (out, kid (n, 0));
      return;
    }
  if (g_str_equal (name, "annotation") || g_str_equal (name, "annotation-xml") ||
      g_str_equal (name, "none") || g_str_equal (name, "mprescripts"))
    return;
  if (g_str_equal (name, "mrow") && n->children->len >= 2 && is_fence (kid (n, 0)))
    {
      /* A bracketed row, as \left and \right make: Office Math's
       * delimiter, which grows with what it holds. */
      gboolean closed = is_fence (kid (n, n->children->len - 1));

      om_delimited (out, kid (n, 0)->text, closed ? kid (n, n->children->len - 1)->text : "",
                    n, 1, closed ? n->children->len - 1 : n->children->len);
      return;
    }
  to_om_items (out, n, 0, n->children->len);
}

gboolean
w42_omml_from_mathml (GString *out, const char *mathml, gboolean para)
{
  W42MathNode *root = mathml != NULL ? w42_math_parse (mathml, -1, NULL) : NULL;

  if (root == NULL)
    return FALSE;
  if (para)
    g_string_append (out, "<m:oMathPara xmlns:m=\"" W42_OMML_NS "\"><m:oMath>");
  else
    g_string_append (out, "<m:oMath xmlns:m=\"" W42_OMML_NS "\">");
  to_om_items (out, root, 0, root->children->len);
  g_string_append (out, para ? "</m:oMath></m:oMathPara>" : "</m:oMath>");
  w42_math_node_free (root);
  return TRUE;
}
